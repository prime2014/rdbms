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
    
    for (int i = 0; i < count; ++i) {
        if (!running) break; // Check for interrupt signal
            
        co_await db.insert_async(i, "data");
    }
    
    co_return;
}


int main() {
    std::signal(SIGINT, signal_handler);
    Table db("my_database");

    VoidTask task = run_batch_inserts(db, 1000);

    while (running && !task.is_done()) {
        db.get_pager()->process_completions(true);

        int rows_inserted = 0;
        // ... inside your insertion loop ...
        // Increment the counter manually
        rows_inserted++; 
        std::cout << "Progress: " << rows_inserted << " rows inserted.\r" << std::flush;
    }

    std::cout << "\nOperation finished or interrupted. Cleaning up..." << std::endl;
    db.get_pager()->shutdown_gracefully();
    return 0;
}