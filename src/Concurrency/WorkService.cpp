//
// Created by Geenz on 7/7/25.
//

#include "WorkService.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include <tracy/Tracy.hpp>  // main-thread work-queue profiling (the per-frame "gap")

#include "../Debug/CpuZoneProfiler.h"  // headless `cpu.zones` mirror
#include "AdaptiveRankingScheduler.h"
#include "WorkContractGroup.h"
#include "WorkGraph.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{
thread_local size_t WorkService::stSoftFailureCount = 0;
thread_local size_t WorkService::stThreadId = 0;
thread_local WorkService* WorkService::stOwner = nullptr;

/// Park deadline of a worker with no timed work to wait for.
static constexpr auto NO_DEADLINE = std::chrono::steady_clock::time_point::max();

/// _parkDeadlineNs while no parked worker waits for a shared due time.
static constexpr int64_t NO_PARK_DEADLINE = std::numeric_limits<int64_t>::max();

WorkService::WorkService(Config config, std::unique_ptr<IWorkScheduler> scheduler) : _config(config) {
    // Always clamp to a range of 1 to hardware concurrency.
    if (_config.threadCount == 0) {
        _config.threadCount = std::thread::hardware_concurrency();
    }
    _config.threadCount = std::clamp(_config.threadCount, (uint32_t)1, std::thread::hardware_concurrency());

    // Update scheduler config with thread count
    _config.schedulerConfig.threadCount = _config.threadCount;

    // Create scheduler if not provided
    if (!scheduler) {
        _scheduler = std::make_unique<AdaptiveRankingScheduler>(_config.schedulerConfig);
    } else {
        _scheduler = std::move(scheduler);
    }

    // Sized here, not in start(): notifiers index it without a lock, so it must exist
    // and never move for the service's whole lifetime.
    _laneWake = std::vector<LaneWake>(_config.threadCount);
    _parkedMaskCoversPool = _config.threadCount <= 64;
}

WorkService::~WorkService() {
    stop();
    clear();
}

void WorkService::start() {
    // exchange, not check-then-set: two concurrent start() calls would both see
    // false and spawn a double set of worker threads.
    if (_running.exchange(true)) {
        return;  // Already running
    }

    for (uint32_t i = 0; i < _config.threadCount; i++) {
        _threads.emplace_back([this, threadId = i](const std::stop_token& stoken) {
            stThreadId = threadId;
            stOwner = this;
            executeWork(stoken, _laneWake[threadId]);
            stOwner = nullptr;
        });
    }
}

void WorkService::requestStop() {
    for (auto& thread : _threads) {
        thread.request_stop();
    }

    // The empty critical section cannot complete while a worker holds the mutex on its
    // way into wait, so the notify below cannot precede that worker's registration.
    // The park predicate reads the stop token.
    { std::lock_guard<std::mutex> handshake(_workAvailableMutex); }
    _workAvailableCV.notify_all();

    // Releases a waitForMainThreadWork() caller.
    notifyMainThreadWorkAvailable();
}

void WorkService::waitForStop() {
    for (auto& thread : _threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    _threads.clear();
    _running = false;

    // Reset thread-local state after all threads have stopped
    resetThreadLocalState();
}

void WorkService::stop() {
    requestStop();
    waitForStop();
}

bool WorkService::isRunning() const {
    return _running;
}

void WorkService::clear() {
    std::unique_lock<std::shared_mutex> lock(_workContractGroupsMutex);

    // Release and disconnect all groups we retained on add
    for (auto* group : _workContractGroups) {
        if (group) {
            // Clear back-reference so the group won't call back into us during destruction
            group->setConcurrencyProvider(nullptr);
            // Release the retain acquired in addWorkContractGroup
            group->release();
        }
    }

    _workContractGroups.clear();
    _workContractGroupCount = 0;

    // Notify scheduler
    _scheduler->notifyGroupsChanged({});
    _scheduler->reset();
}

WorkService::GroupOperationStatus WorkService::addWorkContractGroup(WorkContractGroup* contractGroup) {
    // Work scheduled while the group had no provider sent no notify; it is counted
    // before the provider read in scheduleContract(), so the counts below include it.
    size_t sharedWork = 0;
    bool mainThreadWork = false;
    std::vector<uint32_t> pinnedLanes;
    {
        std::unique_lock<std::shared_mutex> lock(_workContractGroupsMutex);

        // Check for existence to prevent duplicates
        if (std::find(_workContractGroups.begin(), _workContractGroups.end(), contractGroup) !=
            _workContractGroups.end()) {
            return GroupOperationStatus::Exists;
        }

        // Add the group
        _workContractGroups.push_back(contractGroup);
        _workContractGroupCount++;

        // Retain the group while registered with the service (ref-counted semantics)
        if (contractGroup) {
            contractGroup->retain();
        }

        // Notify scheduler of group change
        _scheduler->notifyGroupsChanged(_workContractGroups);

        // Set ourselves as the concurrency provider for this group
        contractGroup->setConcurrencyProvider(this);

        sharedWork = std::min<size_t>(contractGroup->scheduledCount(), _config.threadCount);
        mainThreadWork = contractGroup->mainThreadScheduledCount() > 0;
        for (uint32_t lane = 0; lane < _laneWake.size(); ++lane) {
            if (contractGroup->hasPinnedWork(lane)) {
                pinnedLanes.push_back(lane);
            }
        }
    }

    // Outside the lock: woken workers take it shared to select.
    for (size_t i = 0; i < sharedWork; ++i) {
        notifyWorkAvailable(contractGroup);
    }
    for (uint32_t lane : pinnedLanes) {
        notifyPinnedWorkAvailable(contractGroup, lane);
    }
    if (mainThreadWork) {
        notifyMainThreadWorkAvailable(contractGroup);
    }

    return GroupOperationStatus::Added;
}

WorkService::GroupOperationStatus WorkService::removeWorkContractGroup(WorkContractGroup* contractGroup) {
    // First, stop the group to prevent new work selection
    // Workers will skip this group via isStopping() checks
    contractGroup->stop();

    {
        std::unique_lock<std::shared_mutex> lock(_workContractGroupsMutex);

        auto it = std::find(_workContractGroups.begin(), _workContractGroups.end(), contractGroup);
        if (it == _workContractGroups.end()) {
            return GroupOperationStatus::NotFound;
        }

        // Remove the group from the list
        _workContractGroups.erase(it);
        _workContractGroupCount--;

        // Notify scheduler of group change
        _scheduler->notifyGroupsChanged(_workContractGroups);

        // Clear the concurrency provider for this group
        contractGroup->setConcurrencyProvider(nullptr);
    }
    // Lock released here

    // Wait for any in-flight contract executions to complete
    // This ensures no worker is actively using the group
    contractGroup->wait();

    // Now safe to release our reference
    contractGroup->release();

    return GroupOperationStatus::Removed;
}

size_t WorkService::getWorkContractGroupCount() const {
    std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
    return _workContractGroupCount;
}

size_t WorkService::getThreadCount() const {
    return _config.threadCount;
}

size_t WorkService::getSoftFailureCount() const {
    return _config.maxSoftFailureCount;
}

size_t WorkService::setSoftFailureCount(size_t softFailureCount) {
    if (softFailureCount != _config.maxSoftFailureCount) {
        _config.maxSoftFailureCount = softFailureCount;
    }
    return _config.maxSoftFailureCount;
}

size_t WorkService::getFailureSleepTime() const {
    return _config.failureSleepTime;
}

size_t WorkService::setFailureSleepTime(size_t failureSleepTime) {
    if (failureSleepTime != _config.failureSleepTime) {
        _config.failureSleepTime = failureSleepTime;
    }

    return _config.failureSleepTime;
}

void WorkService::executeWork(const std::stop_token& token, LaneWake& wake) {
    WorkContractGroup* lastExecutedGroup = nullptr;
    // Earliest due time among contracts this worker skipped as not due since it last parked.
    auto pinnedDue = NO_DEADLINE;
    auto sharedDue = NO_DEADLINE;
    // Woken before a shared due time it was watching; handed off once it claims work.
    bool leftSharedDeadline = false;
    // Per-worker selection cursor. Seeded by worker id (golden-ratio spread) so workers start
    // their scans in different parts of a queue; each claim advances it past the claimed slot.
    uint64_t sharedCursor = static_cast<uint64_t>(stThreadId) * 0x9E3779B97F4A7C15ull;
    uint64_t pinnedCursor = 0;

    while (!token.stop_requested()) {
        WorkContractGroup* selectedGroup = nullptr;
        WorkContractHandle contract;

        // Read BEFORE the poll below: see parkUntilWork().
        const uint64_t wakeSnapshot = wake.seq.load(std::memory_order_acquire);

        // Hold the shared_lock across BOTH the scheduler pick AND the claim
        // (selectForExecution). The claim registers this thread in the group's
        // own counters (selecting, then executing) before the lock is dropped,
        // so from the instant the pointer can outlive this scope the group's
        // destruction protocol (stop + wait on those counters) can see us.
        // Claiming after releasing the lock reopens the window where a worker
        // holds a raw group pointer protected by nothing, and
        // removeWorkContractGroup()/~WorkContractGroup can free the group under
        // it. The unique_lock side (add/remove/notifyGroupDestroyed) is the
        // quiescence barrier that makes this sound.
        {
            std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);

            if (!_workContractGroups.empty()) {
                // Drain this worker's pinned lane first (lane == worker id), then the
                // shared queue. hasPinnedWork is a single atomic read, so unpinned
                // workloads pay nothing measurable.
                auto claimFrom = [&](WorkContractGroup* group) {
                    WorkContractHandle claimed;
                    if (group->hasPinnedWork(stThreadId)) {
                        claimed = group->selectForPinnedExecution(stThreadId, std::ref(pinnedCursor), &pinnedDue);
                    }
                    if (!claimed.valid()) {
                        claimed = group->selectForExecution(std::ref(sharedCursor), &sharedDue);
                    }
                    return claimed;
                };

                // Ask scheduler for next group - reads directly from _workContractGroups
                auto scheduleResult = _scheduler->selectNextGroup(_workContractGroups);

                // Select group if valid and not stopping
                if (scheduleResult.group && !scheduleResult.group->isStopping()) {
                    selectedGroup = scheduleResult.group;
                    contract = claimFrom(selectedGroup);

                    // Nothing claimable there (its scheduled contracts may not be due yet):
                    // move on to the other groups before counting a soft failure.
                    for (auto* group : _workContractGroups) {
                        if (contract.valid()) break;
                        if (!group || group == scheduleResult.group || group->isStopping()) continue;
                        contract = claimFrom(group);
                        if (contract.valid()) selectedGroup = group;
                    }
                }
            }
        }
        // Shared lock released here; a claimed contract keeps the group alive
        // via its executing count until executeContract()'s final decrement.

        if (!selectedGroup) {
            // No work found - check for ready timers before sleeping
            checkTimedDeferrals();

            leftSharedDeadline = parkUntilWork(token, wake, wakeSnapshot, pinnedDue, sharedDue);
            pinnedDue = sharedDue = NO_DEADLINE;
            continue;
        }

        if (contract.valid()) {
            // Check stop token again before executing work to prevent deadlocks during shutdown
            if (token.stop_requested()) {
                // Abort without executing: transition Executing -> Free safely during shutdown
                selectedGroup->abortExecution(contract);
                break;
            }

            // Busy now: the shared due time this worker left would go unwatched.
            if (leftSharedDeadline) {
                handOffSharedDeadline();
                leftSharedDeadline = false;
            }

            // Execute the work (includes all cleanup)
            selectedGroup->executeContract(contract);

            // Notify scheduler of successful execution
            _scheduler->notifyWorkExecuted(selectedGroup, stThreadId);

            // Update tracking
            lastExecutedGroup = selectedGroup;
            stSoftFailureCount = 0;
        } else {
            stSoftFailureCount++;
            if (stSoftFailureCount >= _config.maxSoftFailureCount) {
                // Check for ready timers before sleeping
                checkTimedDeferrals();

                leftSharedDeadline = parkUntilWork(token, wake, wakeSnapshot, pinnedDue, sharedDue);
                pinnedDue = sharedDue = NO_DEADLINE;
            } else {
                std::this_thread::yield();
            }
        }
    }
}

bool WorkService::parkUntilWork(const std::stop_token& token, LaneWake& wake, uint64_t wakeSnapshot,
                                std::chrono::steady_clock::time_point pinnedDue,
                                std::chrono::steady_clock::time_point sharedDue) {
    // A timed contract pinned to this lane has no other drainer: always wake for it.
    // A shared due time wakes one parked worker: this one only if it lowers the
    // earliest shared due time any parked worker waits for.
    auto deadline = pinnedDue;
    int64_t sharedNs = NO_PARK_DEADLINE;
    if (sharedDue < deadline) {
        const int64_t dueNs = sharedDue.time_since_epoch().count();
        int64_t current = _parkDeadlineNs.load(std::memory_order_seq_cst);
        while (dueNs < current &&
               !_parkDeadlineNs.compare_exchange_weak(current, dueNs, std::memory_order_seq_cst)) {
        }
        if (dueNs < current) {
            sharedNs = dueNs;
            deadline = sharedDue;
        }
    }
    const bool timed = deadline != NO_DEADLINE;

    const uint64_t laneBit = stThreadId < 64 ? (uint64_t(1) << stThreadId) : 0;
    wake.parked.store(1, std::memory_order_seq_cst);
    _parkedMask.fetch_or(laneBit, std::memory_order_seq_cst);
    {
        std::unique_lock<std::mutex> lock(_workAvailableMutex);
        // Tokens and wake.seq are read seq_cst after the mask bit is set: the Dekker pair
        // with notifyWorkAvailable()'s mask read and notifyPinnedWorkAvailable()'s parked read.
        auto woken = [this, &wake, wakeSnapshot, &token]() {
            return tryConsumeWakeToken() || wake.seq.load(std::memory_order_seq_cst) != wakeSnapshot ||
                   token.stop_requested();
        };
        if (timed) {
            _workAvailableCV.wait_until(lock, deadline, woken);
        } else {
            _workAvailableCV.wait(lock, woken);
        }
    }
    _parkedMask.fetch_and(~laneBit, std::memory_order_seq_cst);
    wake.parked.store(0, std::memory_order_release);
    bool leftSharedDeadline = false;
    if (sharedNs != NO_PARK_DEADLINE) {
        // Unchanged since this worker set it: no parked worker now waits for a shared due time
        // it knows of. A later parker sets its own.
        const bool released =
            _parkDeadlineNs.compare_exchange_strong(sharedNs, NO_PARK_DEADLINE, std::memory_order_seq_cst);
        leftSharedDeadline = released && std::chrono::steady_clock::now() < deadline;
    }
    stSoftFailureCount = 0;
    return leftSharedDeadline;
}

void WorkService::handOffSharedDeadline() {
    // Another parked worker pulls, finds the contract not due, and parks with its due time.
    if (!_parkedMaskCoversPool || _parkedMask.load(std::memory_order_seq_cst) != 0) {
        notifyWorkAvailable();
    }
}

void WorkService::checkTimedDeferrals() {
    // Check all work contract groups for ready timed deferrals
    // WorkGraph overrides checkTimedDeferrals() to check its timer queue,
    // while base WorkContractGroup returns 0 (no timers)
    size_t totalScheduled = 0;
    {
        std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
        for (auto* group : _workContractGroups) {
            totalScheduled += group->checkTimedDeferrals();
        }
    }

    // If any timers were scheduled, wake up waiting worker threads
    if (totalScheduled > 0) {
        notifyWorkAvailable();
    }
}

bool WorkService::tryConsumeWakeToken() noexcept {
    uint32_t tokens = _wakeTokens.load(std::memory_order_seq_cst);
    while (tokens > 0) {
        if (_wakeTokens.compare_exchange_weak(tokens, tokens - 1, std::memory_order_seq_cst)) {
            return true;
        }
    }
    return false;
}

void WorkService::notifyWorkAvailable([[maybe_unused]] WorkContractGroup* group) {
    // One token per notify, so N schedules release N parked workers. At the cap every
    // worker already has a token to consume, so none is added.
    uint32_t tokens = _wakeTokens.load(std::memory_order_seq_cst);
    while (tokens < _config.threadCount &&
           !_wakeTokens.compare_exchange_weak(tokens, tokens + 1, std::memory_order_seq_cst)) {
    }
    // Dekker pair with parkUntilWork, which sets its bit before reading the tokens: an
    // empty mask proves every worker still on its way into the wait sees the token.
    if (_parkedMaskCoversPool && _parkedMask.load(std::memory_order_seq_cst) == 0) {
        return;
    }
    { std::lock_guard<std::mutex> handshake(_workAvailableMutex); }  // see requestStop()
    _workAvailableCV.notify_one();
}

void WorkService::notifyWorkAvailableFor(WorkContractGroup* group, ExecutionType type, uint32_t lane) {
    if (type == ExecutionType::PinnedThread) {
        notifyPinnedWorkAvailable(group, lane);
        return;
    }
    notifyWorkAvailable(group);
}

// Out of line so the AnyThread path above never touches _laneWake, whose pointers
// share a cache line with the notifier-written _wakeTokens.
void WorkService::notifyPinnedWorkAvailable(WorkContractGroup* group, uint32_t lane) {
    // Lanes this service does not own are drained by whatever external thread pumps
    // them; fall back to the shared wake, which is what they see today.
    if (lane >= _laneWake.size()) {
        notifyWorkAvailable(group);
        return;
    }

    // Lane L's only drainer here is worker L, and when it is itself the scheduling
    // thread it is running, not parked, and reaches the lane on its next iteration.
    if (stOwner == this && lane == stThreadId) {
        return;
    }

    // Bump after the ready bit is published. _wakeTokens is deliberately left alone:
    // only lane L can claim this, so any other woken worker would just re-park.
    LaneWake& wake = _laneWake[lane];
    wake.seq.fetch_add(1, std::memory_order_acq_rel);
    if (wake.parked.load(std::memory_order_acquire) != 0) {
        { std::lock_guard<std::mutex> handshake(_workAvailableMutex); }  // see requestStop()
        _workAvailableCV.notify_all();
    }
}

void WorkService::notifyMainThreadWorkAvailable([[maybe_unused]] WorkContractGroup* group) {
    // Dekker pair with waitForMainThreadWork(): bump, then read parked; the waiter
    // raises parked, then reads seq.
    _mainThreadWake.seq.fetch_add(1, std::memory_order_seq_cst);
    if (_mainThreadWake.parked.load(std::memory_order_seq_cst) != 0) {
        { std::lock_guard<std::mutex> handshake(_workAvailableMutex); }  // see requestStop()
        _mainThreadWorkCV.notify_all();
    }
}

void WorkService::waitForMainThreadWork(uint64_t snapshot, std::optional<std::chrono::steady_clock::time_point> deadline) {
    _mainThreadWake.parked.fetch_add(1, std::memory_order_seq_cst);
    {
        std::unique_lock<std::mutex> lock(_workAvailableMutex);
        auto woken = [this, snapshot]() { return _mainThreadWake.seq.load(std::memory_order_seq_cst) != snapshot; };
        if (deadline) {
            _mainThreadWorkCV.wait_until(lock, *deadline, woken);
        } else {
            _mainThreadWorkCV.wait(lock, woken);
        }
    }
    _mainThreadWake.parked.fetch_sub(1, std::memory_order_release);
}

void WorkService::notifyGroupDestroyed(WorkContractGroup* group) {
    // When a group is destroyed, remove it without touching refcount to avoid
    // releasing during its destructor.
    std::unique_lock<std::shared_mutex> lock(_workContractGroupsMutex);

    auto it = std::find(_workContractGroups.begin(), _workContractGroups.end(), group);
    if (it != _workContractGroups.end()) {
        _workContractGroups.erase(it);
        _workContractGroupCount--;

        // Notify scheduler of group change
        _scheduler->notifyGroupsChanged(_workContractGroups);

        // Clear the concurrency provider for this group (no release here)
        group->setConcurrencyProvider(nullptr);
    }
}

void WorkService::resetThreadLocalState() {
    // This only resets the thread-local state in the calling thread,
    // not in the worker threads. The worker threads reset their own
    // state when they exit in the lambda function in start().
    stSoftFailureCount = 0;
    stThreadId = 0;
    stOwner = nullptr;
}

WorkService::MainThreadWorkResult WorkService::executeMainThreadWork(size_t maxContracts) {
    // This is the per-frame "gap" — run from EntropyApplication's loop BEFORE the
    // render delegate. CpuZoneScope feeds the (now EntropyCore-resident)
    // CpuZoneProfiler so this shows in `state get cpu.zones` headlessly, and the
    // Tracy zone attributes it in the GUI timeline.
    ZoneScopedN("WorkService::executeMainThreadWork");
    ::EntropyEngine::Core::Debug::CpuZoneScope _cpuz("WorkService::executeMainThreadWork");
    MainThreadWorkResult result{0, 0, false, std::nullopt};

    // Claim under the registry shared_lock, execute outside it. An unlocked
    // snapshot of raw group pointers would dangle if a group is removed and
    // destroyed mid-loop; claiming under the lock hands protection to the
    // group's own main-thread executing count before the pointer escapes
    // (same protocol as executeWork()).
    size_t remaining = maxContracts;
    std::vector<WorkContractGroup*> countedGroups;  // distinct groups drained (registry is small)
    auto nextDue = NO_DEADLINE;

    while (remaining > 0) {
        WorkContractGroup* group = nullptr;
        WorkContractHandle handle;
        {
            std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
            for (auto* candidate : _workContractGroups) {
                // Any scheduled contract, due or not: a pull reports the due time of those that are not.
                if (candidate && candidate->mainThreadScheduledCount() > 0) {
                    handle = candidate->selectForMainThreadExecution(std::nullopt, &nextDue);
                    if (handle.valid()) {
                        group = candidate;
                        break;
                    }
                }
            }
        }

        if (!group) {
            break;  // No claimable main thread work anywhere
        }

        if (std::find(countedGroups.begin(), countedGroups.end(), group) == countedGroups.end()) {
            countedGroups.push_back(group);
            result.groupsWithWork++;
        }

        // Outside the lock: the claim's executing count keeps the group alive.
        group->executeContract(handle);
        result.contractsExecuted++;
        remaining--;
    }

    // Check if there's more work available
    {
        std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
        for (auto* group : _workContractGroups) {
            if (group && group->hasMainThreadWork()) {
                result.moreWorkAvailable = true;
                break;
            }
        }
    }
    if (nextDue != NO_DEADLINE) {
        result.nextDue = nextDue;
    }

    return result;
}

size_t WorkService::executeMainThreadWork(WorkContractGroup* group, size_t maxContracts) {
    if (!group) {
        return 0;
    }

    return group->executeMainThreadWork(maxContracts);
}

bool WorkService::hasMainThreadWork() const {
    std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);

    for (auto* group : _workContractGroups) {
        if (group && group->hasMainThreadWork()) {
            return true;
        }
    }

    return false;
}

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
