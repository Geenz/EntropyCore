//
// Created by Geenz on 8/8/25.
//


#define NOMINMAX
#include "WorkContractExample.h"
#include <EntropyCore.h>
#include <format>
using namespace EntropyEngine;
using namespace Core;
using namespace Core::Logging;
using namespace Concurrency;

int main() {
    WorkService service{WorkService::Config()};
    service.start();
    WorkContractGroup group(1000);

    for (int i = 0; i < 1000; i++) {
        group.createContract([=]() {
            ENTROPY_LOG_INFO_CAT("WorkContractExample", std::format("WorkContractGroup createContract {}", i));
        }).schedule();
    }
    service.addWorkContractGroup(&group);
    group.wait();

    service.stop();
    return 0;
}
