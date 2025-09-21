//
// Created by Geenz on 8/8/25.
//


#define NOMINMAX
#include <EntropyCore.h>
#include <format>
using namespace EntropyEngine;
using namespace Core;
using namespace Core::Logging;
using namespace Concurrency;

int main() {
    // Optionally increase verbosity for this example
    Logger::global().setMinLevel(Logging::LogLevel::Info);

    WorkService service{WorkService::Config()};
    service.start();
    WorkContractGroup group(1000);

    ENTROPY_LOG_INFO_CAT("WorkContractExample", std::format("Group created: {}", group.debugString()));

    // Submit a bunch of contracts. Log a few handle debug strings and periodic group state.
    for (int i = 0; i < 1000; i++) {
        auto h = group.createContract([=]() {
            ENTROPY_LOG_INFO_CAT("WorkContractExample", std::format("Executing contract {}", i));
        });

        if (i < 3) {
            ENTROPY_LOG_INFO_CAT("WorkContractExample", std::format("Created handle: {}", h.debugString()));
        }

        h.schedule();

        if ((i + 1) % 250 == 0) {
            ENTROPY_LOG_INFO_CAT("WorkContractExample", std::format("After scheduling {}: {}", i + 1, group.debugString()));
        }
    }

    service.addWorkContractGroup(&group);
    ENTROPY_LOG_INFO_CAT("WorkContractExample", std::format("Group added to service: {}", group.debugString()));

    group.wait();
    ENTROPY_LOG_INFO_CAT("WorkContractExample", std::format("All work complete: {}", group.debugString()));

    service.stop();
    return 0;
}
