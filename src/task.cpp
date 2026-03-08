#include "../pages/task.hpp"
#include "../pages/pager.hpp"
#include <exception>

bool PageAwaiter::await_ready() {
    return pager->is_in_memory(page_id);
}

void PageAwaiter::await_suspend(std::coroutine_handle<> h) {
    pager->schedule_async_load(page_id, h);
}

std::shared_ptr<Page> PageAwaiter::await_resume() {
    return pager->get_page_from_cache(page_id);
}

bool VoidTask::await_ready() {
    if (handle) return handle.done();
    return pager && pager->is_in_memory(page_id);
}

void VoidTask::await_suspend(std::coroutine_handle<> h) {
    if (handle) {
        handle.promise().continuation = h;
    } else if (pager) {
        pager->schedule_async_load(page_id, h);
    }
}

void VoidTask::VoidTaskFinalAwaiter::await_suspend(std::coroutine_handle<VoidTask::promise_type> completing) noexcept {
    std::coroutine_handle<> cont = completing.promise().continuation;
    if (cont) cont.resume();
}

VoidTask::VoidTaskFinalAwaiter VoidTask::promise_type::final_suspend() noexcept {
    return {};
}

std::shared_ptr<Page> VoidTask::await_resume() {
    return pager ? pager->get_page_from_cache(page_id) : nullptr;
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