#define NOMINMAX
#include <EntropyCore.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <format>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Logging;
using namespace EntropyEngine::Core::Concurrency;
using namespace std::chrono_literals;

int main() {
    // Setup work service and contract group (like WorkContractExample)
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    service.start();
    
    WorkContractGroup group(1000);
    service.addWorkContractGroup(&group);

    ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Group added to service: {}", group.debugString()));
    
    // Example 1: Basic work graph with dependencies and main thread work
    {
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "\n=== Example 1: Basic Work Graph with Dependencies ===");
        WorkGraph graph(&group);
        
        // Create nodes
        auto task1 = graph.addNode([]() {
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Task 1: Background work");
            std::this_thread::sleep_for(100ms);
        }, "task1");
        
        auto task2 = graph.addNode([]() {
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Task 2: More background work");
            std::this_thread::sleep_for(100ms);
        }, "task2");
        
        auto mainThreadTask = graph.addNode([]() {
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Main Thread Task: UI Update");
            std::this_thread::sleep_for(50ms);
        }, "main-thread-task", nullptr, ExecutionType::MainThread);
        
        auto finalTask = graph.addNode([]() {
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Final Task: Cleanup");
        }, "final");
        
        // Set dependencies: task1 -> task2 -> mainThreadTask -> finalTask
        graph.addDependency(task1, task2);
        graph.addDependency(task2, mainThreadTask);
        graph.addDependency(mainThreadTask, finalTask);
        
        // Execute
        graph.execute();
        
        // Pump main thread work
        while (!graph.isComplete()) {
            group.executeMainThreadWork(10);
            std::this_thread::sleep_for(10ms);
        }
        
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Graph 1 complete");
    }
    
    // Example 2: Yieldable node that waits for atomic value
    {
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "\n=== Example 2: Yieldable Node Waiting for Atomic ===");
        WorkGraph graph(&group);
        
        std::atomic<bool> ready{false};
        
        // Producer sets the atomic after 500ms
        auto producer = graph.addNode([&ready]() {
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Producer: Working...");
            std::this_thread::sleep_for(1000ms);
            ready = true;
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Producer: Data ready!");
        }, "producer");
        
        // Consumer yields until atomic is true
        auto consumer = graph.addYieldableNode([&ready]() -> WorkResultContext {
            static int attempts = 0;
            attempts++;
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Consumer: Attempt {} - checking...", attempts));

            if (!ready.load()) {
                std::this_thread::sleep_for(100ms);
                return WorkResultContext::yield();
            }

            ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Consumer: Got data after {} attempts!", attempts));
            return WorkResultContext::complete();
        }, "consumer", nullptr, ExecutionType::AnyThread, 20); // Max 20 attempts
        
        // Execute (no dependency - they run in parallel)
        graph.execute();
        graph.wait();
        
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Graph 2 complete");
    }
    
    // Example 3: Suspend/Resume functionality
    {
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "\n=== Example 3: Suspend and Resume Graph ===");
        WorkGraph graph(&group);
        
        std::atomic<int> counter{0};
        
        // Create several nodes that increment counter
        auto node1 = graph.addNode([&counter]() {
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Node 1: Working...");
            std::this_thread::sleep_for(200ms);
            counter++;
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Node 1: Done (counter={})", counter.load()));
        }, "node1");
        
        auto node2 = graph.addNode([&counter]() {
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Node 2: Working...");
            std::this_thread::sleep_for(200ms);
            counter++;
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Node 2: Done (counter={})", counter.load()));
        }, "node2");
        
        // Yieldable node that increments counter multiple times
        auto yieldNode = graph.addYieldableNode([&counter]() -> WorkResultContext {
            static int iterations = 0;
            iterations++;
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Yield Node: Iteration {}", iterations));
            counter++;
            std::this_thread::sleep_for(100ms);

            if (iterations < 5) {
                return WorkResultContext::yield();
            }
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Yield Node: Complete (counter={})", counter.load()));
            return WorkResultContext::complete();
        }, "yield-node");
        
        auto node3 = graph.addNode([&counter]() {
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Node 3: Working...");
            std::this_thread::sleep_for(200ms);
            counter++;
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Node 3: Done (counter={})", counter.load()));
        }, "node3");
        
        // Set up dependencies: node1 -> node2 -> yieldNode -> node3
        graph.addDependency(node1, node2);
        graph.addDependency(node2, yieldNode);
        graph.addDependency(yieldNode, node3);
        
        // Start execution
        graph.execute();
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Graph started");
        
        // Let it run for a bit
        std::this_thread::sleep_for(300ms);
        
        // Suspend the graph
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "\n>>> SUSPENDING GRAPH <<<");
        graph.suspend();
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Graph suspended (counter={})", counter.load()));
        
        // Wait while suspended - nothing new should schedule
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Waiting 1 second while suspended...");
        std::this_thread::sleep_for(1000ms);
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Counter after suspension wait: {}", counter.load()));
        
        // Resume the graph
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "\n>>> RESUMING GRAPH <<<");
        graph.resume();
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Graph resumed");
        
        // Wait for completion
        auto result = graph.wait();
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", std::format("Graph 3 complete (final counter={})", counter.load()));
    }
    
    // Example 4: Timed Yield - Sleep until specific time (NEW!)
    {
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "\n=== Example 4: Timed Yield - Zero-CPU Waiting ===");
        WorkGraph graph(&group);

        std::atomic<int> pollCount{0};
        std::atomic<bool> dataReady{false};

        // Simulated async operation that completes after 500ms
        auto dataProvider = graph.addNode([&dataReady]() {
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Data Provider: Starting async operation...");
            std::this_thread::sleep_for(500ms);
            dataReady.store(true);
            ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Data Provider: Data is ready!");
        }, "data-provider");

        // Poller using timed yields - checks every 100ms without busy-waiting
        auto poller = graph.addYieldableNode([&pollCount, &dataReady]() -> WorkResultContext {
            pollCount++;
            auto now = std::chrono::steady_clock::now();
            ENTROPY_LOG_INFO_CAT("WorkGraphExample",
                std::format("Poller: Check #{} - data ready: {}", pollCount.load(), dataReady.load()));

            if (!dataReady.load()) {
                // NOT READY: Yield until 100ms from now (NO CPU USAGE!)
                auto wakeTime = now + 100ms;
                ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Poller: Sleeping for 100ms...");
                return WorkResultContext::yieldUntil(wakeTime);
            }

            // READY: Process and complete
            ENTROPY_LOG_INFO_CAT("WorkGraphExample",
                std::format("Poller: Data ready after {} polls!", pollCount.load()));
            return WorkResultContext::complete();
        }, "poller");

        // Execute (nodes run in parallel)
        graph.execute();
        graph.wait();

        ENTROPY_LOG_INFO_CAT("WorkGraphExample",
            std::format("Graph 4 complete - Poller checked {} times (expected ~5)", pollCount.load()));
        ENTROPY_LOG_INFO_CAT("WorkGraphExample", "Note: Zero CPU usage while waiting - timer sleeps passively!");
    }

    service.stop();
    return 0;
}