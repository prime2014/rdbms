#ifndef CONTEXT_HPP
#define CONTEXT_HPP

#include <coroutine>
#include <atomic>
#include <vector>
#include <memory>

struct Page;

// Tracks compound batch flushes across multiple SQEs
struct BatchContext {
    std::coroutine_handle<> awaiting_coroutine;
    std::atomic<size_t> counter;
    std::vector<Page*> allocated_pages;

    BatchContext(std::coroutine_handle<> h, size_t count) 
        : awaiting_coroutine(h), counter(count) {
        allocated_pages.reserve(count);
    }
};

// Tracks single IO descriptors (Reads or non-batched Writes)
struct IOContext {
    uint32_t page_id;
    std::coroutine_handle<> waiter;
    BatchContext* batch;                       // Point to parent batch if part of a group
    std::unique_ptr<char[]> write_buffer;      // Retain snapshot buffers for standard writes

    // Constructor A: Standard individual operation (e.g., Single Page Read)
    IOContext(uint32_t id, std::coroutine_handle<> h)
        : page_id(id), waiter(h), batch(nullptr), write_buffer(nullptr) {}

    // Constructor B: Batched operation tracking with isolated snapshot buffer
    IOContext(BatchContext* b, uint32_t id, std::unique_ptr<char[]> buffer)
        : page_id(id), waiter(b ? b->awaiting_coroutine : nullptr), batch(b), write_buffer(std::move(buffer)) {}
};

#endif