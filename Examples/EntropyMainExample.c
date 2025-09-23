#include <Core/entropy_main.h>
#include <Logging/Logger.h>

static void will_finish(EntropyApp* app, void* userdata) {
    (void)app; (void)userdata;
    ENTROPY_LOG_INFO("[EntropyMainExample] will_finish_launching");
}

static void did_finish(EntropyApp* app, void* userdata) {
    (void)app; (void)userdata;
    ENTROPY_LOG_INFO("[EntropyMainExample] did_finish_launching");
    // Request termination directly; no main-thread posting API
    entropy_main_terminate(0);
}

static bool should_terminate(EntropyApp* app, void* userdata) {
    (void)app; (void)userdata;
    // Allow termination when requested
    return true;
}

static void will_terminate(EntropyApp* app, void* userdata) {
    (void)app; (void)userdata;
    ENTROPY_LOG_INFO("[EntropyMainExample] will_terminate");
}

static void did_catch(EntropyApp* app, void* userdata) {
    (void)app; (void)userdata;
    ENTROPY_LOG_WARNING("[EntropyMainExample] did_catch_unhandled_exception (if any)");
}

int main(void) {
    EntropyMainConfig cfg = {0};
    cfg.worker_threads = 0;            // auto
    cfg.install_signal_handlers = true;
    cfg.shutdown_deadline_ms = 3000;

    EntropyAppDelegateC del = {0};
    del.will_finish_launching = will_finish;
    del.did_finish_launching = did_finish;
    del.should_terminate = should_terminate;
    del.will_terminate = will_terminate;
    del.did_catch_unhandled_exception = did_catch;
    del.userdata = NULL;

    ENTROPY_LOG_INFO("[EntropyMainExample] Starting entropy_main_run...");
    int rc = entropy_main_run(&cfg, &del);
    ENTROPY_LOG_INFO(std::format("[EntropyMainExample] Exited with code {}", rc));
    return rc;
}
