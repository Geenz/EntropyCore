#include <catch2/catch_test_macros.hpp>
#include "Concurrency/WorkGraph.h"
#include "Concurrency/WorkContractGroup.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>

using namespace EntropyEngine::Core::Concurrency;
using namespace std::chrono_literals;

TEST_CASE("WorkGraph debugging", "[WorkGraph][Regression]") {
    WorkContractGroup group(256);
    WorkGraph graph(&group);

    std::atomic<bool> node1Complete = false;
    std::atomic<bool> node2Complete = false;

    auto node1 = graph.addNode([&node1Complete]() {
        node1Complete.store(true);
    }, "Node1");

    auto node2 = graph.addNode([&node2Complete]() {
        node2Complete.store(true);
    }, "Node2");

    graph.addDependency(node1, node2);

    graph.execute();

    while (!graph.isComplete()) {
        group.executeAllBackgroundWork();
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(node1Complete.load());
    REQUIRE(node2Complete.load());
}

TEST_CASE("WorkGraph isolated hang", "[WorkGraph][Regression]") {
    WorkContractGroup group(256);
    WorkGraph graph(&group);

    std::atomic<int> counter = 0;

    auto node1 = graph.addNode([&counter]() {
        counter.fetch_add(1);
    }, "node1");

    auto node2 = graph.addNode([&counter]() {
        counter.fetch_add(1);
    }, "node2");

    auto node3 = graph.addNode([&counter]() {
        counter.fetch_add(1);
    }, "node3");

    graph.addDependency(node1, node3);
    graph.addDependency(node2, node3);

    graph.execute();

    while (!graph.isComplete()) {
        group.executeAllBackgroundWork();
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(counter.load() == 3);
}

TEST_CASE("WorkGraph simple hang investigation", "[WorkGraph][Regression]") {
    SECTION("Independent nodes") {
        WorkContractGroup group(256);
        WorkGraph graph(&group);

        std::atomic<int> counter = 0;

        auto node1 = graph.addNode([&counter]() {
            counter.fetch_add(1);
        }, "Node1");

        auto node2 = graph.addNode([&counter]() {
            counter.fetch_add(1);
        }, "Node2");

        graph.execute();

        while (!graph.isComplete()) {
            group.executeAllBackgroundWork();
            std::this_thread::sleep_for(1ms);
        }

        REQUIRE(counter.load() == 2);
    }

    SECTION("Simple dependency chain") {
        WorkContractGroup group(256);
        WorkGraph graph(&group);

        std::atomic<int> counter = 0;

        auto node1 = graph.addNode([&counter]() {
            counter.fetch_add(1);
        }, "Node1");

        auto node2 = graph.addNode([&counter]() {
            counter.fetch_add(1);
        }, "Node2");

        graph.addDependency(node1, node2);

        graph.execute();

        while (!graph.isComplete()) {
            group.executeAllBackgroundWork();
            std::this_thread::sleep_for(1ms);
        }

        REQUIRE(counter.load() == 2);
    }

    SECTION("Diamond dependency") {
        WorkContractGroup group(256);
        WorkGraph graph(&group);

        std::atomic<int> counter = 0;

        auto node1 = graph.addNode([&counter]() {
            counter.fetch_add(1);
        }, "Node1");

        auto node2 = graph.addNode([&counter]() {
            counter.fetch_add(1);
        }, "Node2");

        auto node3 = graph.addNode([&counter]() {
            counter.fetch_add(1);
        }, "Node3");

        auto node4 = graph.addNode([&counter]() {
            counter.fetch_add(1);
        }, "Node4");

        graph.addDependency(node1, node2);
        graph.addDependency(node1, node3);
        graph.addDependency(node2, node4);
        graph.addDependency(node3, node4);

        graph.execute();

        while (!graph.isComplete()) {
            group.executeAllBackgroundWork();
            std::this_thread::sleep_for(1ms);
        }

        REQUIRE(counter.load() == 4);
    }
}

TEST_CASE("WorkGraph vector destruction", "[WorkGraph][Regression]") {
    WorkContractGroup group(256);

    {
        std::vector<std::unique_ptr<WorkGraph>> graphs;
        std::vector<std::atomic<int>> counters(10);

        for (int i = 0; i < 10; ++i) {
            auto graph = std::make_unique<WorkGraph>(&group);

            auto node1 = graph->addNode([&counter = counters[i]]() {
                counter.fetch_add(1);
            }, "node1");

            auto node2 = graph->addNode([&counter = counters[i]]() {
                counter.fetch_add(1);
            }, "node2");

            graph->addDependency(node1, node2);
            graph->execute();

            graphs.push_back(std::move(graph));
        }

        // Wait for all graphs to complete
        bool allComplete = false;
        while (!allComplete) {
            group.executeAllBackgroundWork();
            allComplete = true;
            for (auto& graph : graphs) {
                if (!graph->isComplete()) {
                    allComplete = false;
                    break;
                }
            }
            if (!allComplete) {
                std::this_thread::sleep_for(1ms);
            }
        }

        // Verify all counters
        for (int i = 0; i < 10; ++i) {
            REQUIRE(counters[i].load() == 2);
        }
    }
}
