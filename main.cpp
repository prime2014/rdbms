#include "pages/table.hpp"
#include <liburing.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <csignal>


std::atomic<bool> running(true);

void signal_handler(int) {
    running = false;
}

// A wrapper to run the 1000 inserts
VoidTask run_batch_inserts(Table& db, int count) {
    try {
        for (int i = 0; i < count; ++i) {
            if (!running) break; // Check for interrupt signal
            
            co_await db.insert_async(i, "data");
        }
    } catch (const std::exception& e) {
        std::cerr << "Task failed: " << e.what() << std::endl;
    }
    co_return;
}


int main() {
    // Register signal handler
    std::signal(SIGINT, signal_handler);
    Table db("my_database");

    // Hold the task handle so it doesn't get destroyed
    VoidTask task = run_batch_inserts(db, 1000);

    uint32_t last_count = 0;
   
    while (db.get_total_count() < 1000) {
        // 1. Trigger the actual hardware I/O for any queued writes
        db.get_pager()->submit_all();
        
        // 2. Wait for completion (blocking) so we don't burn CPU
        // This suspends the thread until an I/O operation finishes.
        db.get_pager()->process_completions(true); 
    
        // Optional: Log progress
        std::cout << "Rows: " << db.get_total_count() << std::endl;
    }

    // After the loop breaks:
    std::cout << "Interrupt received. Draining system..." << std::endl;
    db.get_pager()->shutdown_gracefully();

    return 0;
}