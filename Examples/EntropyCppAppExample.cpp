#include <Concurrency/WorkContractGroup.h>
#include <Concurrency/WorkService.h>
#include <Core/EntropyApplication.h>
#include <Logging/Logger.h>

#include <chrono>
#include <thread>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;

class MyDelegate : public EntropyAppDelegate
{
    std::shared_ptr<WorkService> work_;
    std::unique_ptr<WorkContractGroup> group_;
    std::atomic<int> remaining_{0};

public:
    void applicationWillFinishLaunching() override {
        ENTROPY_LOG_INFO("[EntropyCppAppExample] applicationWillFinishLaunching");
    }

    void applicationDidFinishLaunching() override {
        ENTROPY_LOG_INFO("[EntropyCppAppExample] applicationDidFinishLaunching");

        // Acquire the WorkService registered by EntropyApplication using type-based lookup
        work_ = EntropyApplication::shared().services().get<WorkService>();
        if (!work_) {
            ENTROPY_LOG_ERROR("[EntropyCppAppExample] WorkService not available!");
            EntropyApplication::shared().terminate(1);
            return;
        }

        // Create a work group and register it with the work service
        group_ = std::make_unique<WorkContractGroup>(64, "ExampleGroup");
        auto status = work_->addWorkContractGroup(group_.get());
        if (status != WorkService::GroupOperationStatus::Added && status != WorkService::GroupOperationStatus::Exists) {
            ENTROPY_LOG_ERROR("[EntropyCppAppExample] Failed to register WorkContractGroup");
            EntropyApplication::shared().terminate(1);
            return;
        }

        // Schedule some background work using contracts only; no detached threads
        int scheduled = 0;
        for (int i = 0; i < 16; ++i) {
            auto handle = group_->createContract([this, i]() noexcept {
                // Simulate compute
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
                ENTROPY_LOG_DEBUG(std::format("  [Work] item {} executed on thread {}", i, tid));
                // If this was the last contract to finish, request app termination
                if (--remaining_ == 0) {
                    ENTROPY_LOG_INFO("[EntropyCppAppExample] All work completed; requesting terminate");
                    // EntropyApplication::shared().terminate(0);
                }
            });
            if (handle.valid()) {
                handle.schedule();
                ++scheduled;
            }
        }
        remaining_.store(scheduled, std::memory_order_relaxed);
        if (scheduled == 0) {
            // Nothing scheduled; terminate immediately
        }
    }

    bool applicationShouldTerminate() override {
        ENTROPY_LOG_INFO("[EntropyCppAppExample] applicationShouldTerminate -> true");
        return true;
    }

    void applicationWillTerminate() override {
        ENTROPY_LOG_INFO("[EntropyCppAppExample] applicationWillTerminate");
        if (work_ && group_) {
            work_->removeWorkContractGroup(group_.get());
        }
        group_.reset();
        work_.reset();
    }

    void applicationDidCatchUnhandledException(std::exception_ptr) override {
        ENTROPY_LOG_ERROR("[EntropyCppAppExample] Unhandled exception observed");
    }

    void applicationMainLoop() override {
        ENTROPY_LOG_INFO("[EntropyCppAppExample] applicationMainLoop");
        EntropyApplication::shared().terminate(0);
    }
};

int main() {
    auto& app = EntropyApplication::shared();

    EntropyApplicationConfig cfg;
    cfg.workerThreads = 0;  // auto
    cfg.shutdownDeadline = std::chrono::milliseconds(3000);

    app.configure(cfg);

    MyDelegate delegate;
    app.setDelegate(&delegate);

    ENTROPY_LOG_INFO("[EntropyCppAppExample] Starting app.run()");
    int rc = app.run();
    ENTROPY_LOG_INFO(std::format("[EntropyCppAppExample] app.run() exited with code {}", rc));
    return rc;
}
