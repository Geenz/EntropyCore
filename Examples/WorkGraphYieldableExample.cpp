#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include "../src/Concurrency/WorkContractGroup.h"
#include "../src/Concurrency/WorkService.h"
#include "../src/Concurrency/WorkGraph.h"

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
    
    // Example 1: Basic work graph with dependencies and main thread work
    {
        std::cout << "\n=== Example 1: Basic Work Graph with Dependencies ===\n";
        WorkGraph graph(&group);
        
        // Create nodes
        auto task1 = graph.addNode([]() {
            std::cout << "Task 1: Background work\n";
            std::this_thread::sleep_for(100ms);
        }, "task1");
        
        auto task2 = graph.addNode([]() {
            std::cout << "Task 2: More background work\n";
            std::this_thread::sleep_for(100ms);
        }, "task2");
        
        auto mainThreadTask = graph.addNode([]() {
            std::cout << "Main Thread Task: UI Update\n";
            std::this_thread::sleep_for(50ms);
        }, "main-thread-task", nullptr, ExecutionType::MainThread);
        
        auto finalTask = graph.addNode([]() {
            std::cout << "Final Task: Cleanup\n";
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
        
        std::cout << "Graph 1 complete\n";
    }
    
    // Example 2: Yieldable node that waits for atomic value
    {
        std::cout << "\n=== Example 2: Yieldable Node Waiting for Atomic ===\n";
        WorkGraph graph(&group);
        
        std::atomic<bool> ready{false};
        
        // Producer sets the atomic after 500ms
        auto producer = graph.addNode([&ready]() {
            std::cout << "Producer: Working...\n";
            std::this_thread::sleep_for(1000ms);
            ready = true;
            std::cout << "Producer: Data ready!\n";
        }, "producer");
        
        // Consumer yields until atomic is true
        auto consumer = graph.addYieldableNode([&ready]() -> WorkResult {
            static int attempts = 0;
            attempts++;
            std::cout << "Consumer: Attempt " << attempts << " - checking...\n";
            
            if (!ready.load()) {
                std::this_thread::sleep_for(100ms);
                return WorkResult::Yield;
            }
            
            std::cout << "Consumer: Got data after " << attempts << " attempts!\n";
            return WorkResult::Complete;
        }, "consumer", nullptr, ExecutionType::AnyThread, 20); // Max 20 attempts
        
        // Execute (no dependency - they run in parallel)
        graph.execute();
        graph.wait();
        
        std::cout << "Graph 2 complete\n";
    }
    
    // Example 3: Suspend/Resume functionality
    {
        std::cout << "\n=== Example 3: Suspend and Resume Graph ===\n";
        WorkGraph graph(&group);
        
        std::atomic<int> counter{0};
        
        // Create several nodes that increment counter
        auto node1 = graph.addNode([&counter]() {
            std::cout << "Node 1: Working...\n";
            std::this_thread::sleep_for(200ms);
            counter++;
            std::cout << "Node 1: Done (counter=" << counter.load() << ")\n";
        }, "node1");
        
        auto node2 = graph.addNode([&counter]() {
            std::cout << "Node 2: Working...\n";
            std::this_thread::sleep_for(200ms);
            counter++;
            std::cout << "Node 2: Done (counter=" << counter.load() << ")\n";
        }, "node2");
        
        // Yieldable node that increments counter multiple times
        auto yieldNode = graph.addYieldableNode([&counter]() -> WorkResult {
            static int iterations = 0;
            iterations++;
            std::cout << "Yield Node: Iteration " << iterations << "\n";
            counter++;
            std::this_thread::sleep_for(100ms);
            
            if (iterations < 5) {
                return WorkResult::Yield;
            }
            std::cout << "Yield Node: Complete (counter=" << counter.load() << ")\n";
            return WorkResult::Complete;
        }, "yield-node");
        
        auto node3 = graph.addNode([&counter]() {
            std::cout << "Node 3: Working...\n";
            std::this_thread::sleep_for(200ms);
            counter++;
            std::cout << "Node 3: Done (counter=" << counter.load() << ")\n";
        }, "node3");
        
        // Set up dependencies: node1 -> node2 -> yieldNode -> node3
        graph.addDependency(node1, node2);
        graph.addDependency(node2, yieldNode);
        graph.addDependency(yieldNode, node3);
        
        // Start execution
        graph.execute();
        std::cout << "Graph started\n";
        
        // Let it run for a bit
        std::this_thread::sleep_for(300ms);
        
        // Suspend the graph
        std::cout << "\n>>> SUSPENDING GRAPH <<<\n";
        graph.suspend();
        std::cout << "Graph suspended (counter=" << counter.load() << ")\n";
        
        // Wait while suspended - nothing new should schedule
        std::cout << "Waiting 1 second while suspended...\n";
        std::this_thread::sleep_for(1000ms);
        std::cout << "Counter after suspension wait: " << counter.load() << "\n";
        
        // Resume the graph
        std::cout << "\n>>> RESUMING GRAPH <<<\n";
        graph.resume();
        std::cout << "Graph resumed\n";
        
        // Wait for completion
        auto result = graph.wait();
        std::cout << "Graph 3 complete (final counter=" << counter.load() << ")\n";
    }
    
    service.stop();
    return 0;
}