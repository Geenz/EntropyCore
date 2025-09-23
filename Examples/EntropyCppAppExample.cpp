#include <Core/EntropyApplication.h>
#include <Concurrency/WorkService.h>
#include <Concurrency/WorkContractGroup.h>
#include <Logging/Logger.h>
#include <thread>
#include <chrono>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;

class MyDelegate : public EntropyAppDelegate {
    std::shared_ptr<WorkService> work_;
    std::unique_ptr<WorkContractGroup> group_;

public:
    void applicationWillFinishLaunching() override {
        ENTROPY_LOG_INFO("[EntropyCppAppExample] applicationWillFinishLaunching");
    }

    void applicationDidFinishLaunching() override {
        ENTROPY_LOG_INFO("[EntropyCppAppExample] applicationDidFinishLaunching");

        // Acquire the WorkService registered by EntropyApplication
        auto base = EntropyApplication::shared().services().get("com.entropy.core.work");
        work_ = std::dynamic_pointer_cast<WorkService>(base);
        if (!work_) {
            ENTROPY_LOG_ERROR("[EntropyCppAppExample] WorkService not available!");
            EntropyApplication::shared().terminate(1);
            return;
        }

        // Create a work group and register it with the work service
        group_ = std::make_unique<WorkContractGroup>(64, "ExampleGroup");
        auto status = work_->addWorkContractGroup(group_.get());
        if (status != WorkService::GroupOperationStatus::Added &&
            status != WorkService::GroupOperationStatus::Exists) {
            ENTROPY_LOG_ERROR("[EntropyCppAppExample] Failed to register WorkContractGroup");
            EntropyApplication::shared().terminate(1);
            return;
        }

        // Schedule some background work
        for (int i = 0; i < 16; ++i) {
            auto handle = group_->createContract([i]() noexcept {
                // Simulate compute
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
                ENTROPY_LOG_DEBUG(std::format("  [Work] item {} executed on thread {}", i, tid));
            });
            if (handle.valid()) {
                handle.schedule();
            }
        }

        // When all work completes, request application termination from a helper thread
        std::thread([grp = group_.get()]() {
            grp->wait();
            ENTROPY_LOG_INFO("[EntropyCppAppExample] All work completed; requesting terminate");
            EntropyApplication::shared().terminate(0);
        }).detach();
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
};

int main() {
    auto& app = EntropyApplication::shared();

    EntropyApplicationConfig cfg;
    cfg.workerThreads = 0; // auto
    cfg.installSignalHandlers = false; // left off in this example
    cfg.shutdownDeadline = std::chrono::milliseconds(3000);

    app.configure(cfg);

    MyDelegate delegate;
    app.setDelegate(&delegate);

    ENTROPY_LOG_INFO("[EntropyCppAppExample] Starting app.run()");
    int rc = app.run();
    ENTROPY_LOG_INFO(std::format("[EntropyCppAppExample] app.run() exited with code {}", rc));
    return rc;
}
