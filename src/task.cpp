#include "../pages/task.hpp"
#include "../pages/pager.hpp"
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
    pager->submit_write(page_id, h);
}

void FlushAwaiter::await_resume() { }

std::suspend_never PageTask::promise_type::initial_suspend() { return {}; }

void PageTask::PageTaskFinalAwaiter::await_suspend(std::coroutine_handle<PageTask::promise_type> completing) noexcept {
    std::coroutine_handle<> cont = completing.promise().continuation;
    if (cont) cont.resume();
}

PageTask::PageTaskFinalAwaiter PageTask::promise_type::final_suspend() noexcept {
    return {};
}

void PageTask::promise_type::return_value(std::shared_ptr<Page> p) { result_page = p; }
void PageTask::promise_type::unhandled_exception() {
    exception = std::current_exception();
}

void MultiFlushAwaiter::await_suspend(std::coroutine_handle<> h) {
    for (uint32_t id : page_ids) {
        // Pass the shared_ptr to the submit_write function
        pager->submit_write(id, h, remaining);
    }

    // Now kick the kernel to process all batched writes at once
    io_uring_submit(&pager->ring);
    
    std::cout << "DEBUG: Batch write submitted for " << page_ids.size() << " pages." << std::endl;
    
}