#include "../pages/task.hpp"
#include "../pages/context.hpp"
#include "../pages/pager.hpp"
#include "../pages/table.hpp"
#include <exception>
#include <iostream>


bool PageAwaiter::await_ready() {
    return pager->is_in_memory(page_id);
}

void PageAwaiter::await_suspend(std::coroutine_handle<> h) {
    
    pager->schedule_async_load(page_id, h);
}

std::shared_ptr<Page> PageAwaiter::await_resume() {
    return pager->get_page_from_cache(page_id);
}



void FlushAwaiter::await_suspend(std::coroutine_handle<> h) {
    // 1. Create a BatchContext for this single operation
    // The count is 1 because this awaiter is only waiting for one page.
    auto batch = std::make_shared<BatchContext>(h, 1);

    // 2. Pass the shared_ptr to the pager
    // This matches the new signature: void submit_write(uint32_t, shared_ptr<BatchContext>)
    pager->submit_write(page_id, batch);
}

void FlushAwaiter::await_resume() { }

// Example of defining the nested FinalAwaiter logic in the .cpp
bool PageTask::promise_type::FinalAwaiter::await_ready() noexcept { 
    return false; 
}

std::coroutine_handle<> PageTask::promise_type::FinalAwaiter::await_suspend(std::coroutine_handle<promise_type> h) noexcept {
    if (h.promise().continuation) return h.promise().continuation;
    return std::noop_coroutine();
}

void PageTask::promise_type::FinalAwaiter::await_resume() noexcept {}

// Your existing definitions
PageTask PageTask::promise_type::get_return_object() { 
    return { std::coroutine_handle<promise_type>::from_promise(*this) }; 
}

std::suspend_never PageTask::promise_type::initial_suspend() { return {}; }

PageTask::promise_type::FinalAwaiter PageTask::promise_type::final_suspend() noexcept {
    return {};
}

void PageTask::promise_type::return_value(std::shared_ptr<Page> p) { result_page = std::move(p); }

void PageTask::promise_type::unhandled_exception() {
    exception = std::current_exception();
}


void MultiFlushAwaiter::await_suspend(std::coroutine_handle<> h) {
    if (page_ids->empty()) {
        h.resume();
        return;
    }

    // 1. Prepare the BatchContext
    auto batch_ctx = std::make_shared<BatchContext>(h, 0); 
    
    // We move the counter value into the tethered context here
    batch_ctx->counter.store(remaining->load()); 
    batch_ctx->pages = *page_ids;
    batch_ctx->resumed.store(false);

    // 2. Prepare all SQEs
    for (uint32_t page_id : *page_ids) {
        off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;
        auto buffer_snapshot = std::make_unique<char[]>(PAGE_SIZE);
        std::memcpy(buffer_snapshot.get(), pager->page_cache[page_id]->data, PAGE_SIZE);

        // FIX: Remove 'remaining'. The constructor signature is:
        // IOContext(shared_ptr<BatchContext>, uint32_t, unique_ptr<char[]>)
        IOContext* ctx = new IOContext(batch_ctx, page_id, std::move(buffer_snapshot));

        struct io_uring_sqe* sqe = io_uring_get_sqe(&pager->ring);
        if (!sqe) {
            io_uring_submit(&pager->ring);
            sqe = io_uring_get_sqe(&pager->ring);
        }
        
        // Use the buffer inside the ctx to ensure memory stays valid for the kernel
        io_uring_prep_write(sqe, pager->fd, ctx->write_buffer.get(), PAGE_SIZE, offset);
        io_uring_sqe_set_data(sqe, ctx);
    }
    
    io_uring_submit(&pager->ring);
}




bool PageLatch::await_ready() {
    // This now works because the compiler knows what 'table' can do
    return !table->is_page_locked(page_id);
}

void PageLatch::await_suspend(std::coroutine_handle<> h) {
    table->register_waiting_coroutine(page_id, h);
    std::cout << "DEBUG: Coroutine suspended waiting for Page " << page_id << std::endl;
}

void PageLatch::await_resume() {
    std::cout << "DEBUG: Page " << page_id << " is now free. Resuming..." << std::endl;
}