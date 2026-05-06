// pages/task.hpp
#ifndef CONTEXT_HPP
#define CONTEXT_HPP

#include <coroutine>
#include <atomic>
#include <vector>
#include <memory>

struct BatchContext {
    std::coroutine_handle<> handle;
    // Direct atomic: No more shared_ptr member errors
    std::atomic<size_t> counter; 
    std::atomic<bool> resumed{false};
    // Keep pages here if needed for tracking, but usually 
    // the Pager just needs the handle and counter.
    std::vector<uint32_t> pages;

    BatchContext(std::coroutine_handle<> h, size_t count) 
        : handle(h), counter(count) {}
};



struct IOContext {
    // CHANGE: Use shared_ptr so the BatchContext stays alive 
    // as long as this specific IO operation is in flight.
    std::shared_ptr<BatchContext> batch; 
    uint32_t page_id;
    std::unique_ptr<char[]> write_buffer;

    IOContext(std::shared_ptr<BatchContext> b, uint32_t id, 
              std::unique_ptr<char[]> buffer)
        : batch(std::move(b)), 
          page_id(id), 
          write_buffer(std::move(buffer)) {}

    // Update the second constructor similarly
    IOContext(std::coroutine_handle<> h, uint32_t id, 
              std::unique_ptr<char[]> buffer)
        : page_id(id), 
          write_buffer(std::move(buffer)) {
        batch = std::make_shared<BatchContext>(h, 1);
    }
};


#endif