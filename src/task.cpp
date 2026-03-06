#include "../pages/task.hpp"
#include "../pages/pager.hpp"

bool PageAwaiter::await_ready() {
    return pager->is_in_memory(page_id);
}

void PageAwaiter::await_suspend(std::coroutine_handle<> h) {
    pager->schedule_async_load(page_id, h);
}

std::shared_ptr<Page> PageAwaiter::await_resume() {
    // Note: ensure this method exists in your Pager class
    return pager->get_page(page_id); 
}