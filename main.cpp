#include "pages/table.hpp"
#include <liburing.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <csignal>
#include <execinfo.h>

std::atomic<bool> running(true);

void signal_handler(int) {
    running = false;
    std::cout << "\nInterrupt received, shutting down..." << std::endl;
}

// A wrapper to run the 1000 inserts
VoidTask run_batch_inserts(Table& db, int count, std::atomic<int>& progress) {
    for (int i = 0; i < count; ++i) {
        if (!running) break;
        try {
            co_await db.insert_async(i, "data");
            progress++;
            std::cout << "Completed insert " << i << std::endl;  // Debug
        } catch (...){
            std::cout << "CRASH DETECTED at key: " << i << std::endl;
            throw;
        }
    }
    
    std::cout << "INSERT LOOP COMPLETE - returning from coroutine" << std::endl;
    co_return;
}

void segfault_handler(int sig) {
    void* array[10];
    size_t size = backtrace(array, 10);
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

int main() {
    std::signal(SIGINT, signal_handler);
    signal(SIGSEGV, segfault_handler);
    Table db{"my_database"};
    
    std::atomic<int> rows_inserted{0};
    VoidTask task = run_batch_inserts(db, 4000, rows_inserted);
    
    // Initial start
    task.handle.resume();
    
    auto last_update = std::chrono::steady_clock::now();
    
    // The True Blocking Loop
    // In main.cpp
    while (running && !task.is_done()) {
        // 1. Process anything already finished (non-blocking)
        db.get_pager()->process_completions(false); 

        // 2. If the task is STILL not done, and we have no CPU work,
        // we can afford a tiny sleep or a conditional wait.
        if (!task.is_done()) {
            // Option A: Small sleep to prevent 100% CPU usage
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            
        }
    }
    
    std::cout << "\nFinal count: " << rows_inserted.load() << " rows inserted." << std::endl;
    db.get_pager()->shutdown_gracefully();
    
    return 0;
}