#ifndef PAGETASK_HPP
#define PAGETASK_HPP

#include <coroutine>
#include <memory>
#include "page.hpp"


// FORWARD DECLARATION: 
class Pager;
struct SplitResult result;

struct PageTask {
    struct promise_type {
        std::shared_ptr<Page> result_page;

        PageTask get_return_object() {
            return { std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_never initial_suspend() { return {}; }

        std::suspend_always final_suspend() noexcept { return {}; }

        // This handles "co_return some_page"
        void return_value(std::shared_ptr<Page> p) { result_page = p; }
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    bool await_ready() { return handle.done(); }

    // 2. What to do if it's not done (suspend the caller)
    void await_suspend(std::coroutine_handle<> caller_handle) {
        // In a simple system, we just let the sub-task run.
        // For now, we assume initial_suspend was "never", 
        // so the task is already moving.
    }

    // 3. What to return when the co_await finishes
    std::shared_ptr<Page> await_resume() {
        return handle.promise().result_page;
    }
};



struct PageAwaiter {
    Pager* pager;
    uint32_t page_id;

    struct promise_type {
        PageAwaiter get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {} // This allows "co_return;" to work
        void unhandled_exception() { std::terminate(); }
    };
    bool await_ready();
    void await_suspend(std::coroutine_handle<> h);
    std::shared_ptr<Page> await_resume() {
        return pager->get_page_from_cache(page_id); 
    }
};


struct VoidTask {
    Pager* pager;
    uint32_t page_id;

    struct promise_type {
        VoidTask get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void unhandled_exception() {}
        void return_void() {} // ONLY return_void
    };

    bool await_ready();
    void await_suspend(std::coroutine_handle<> h);
    std::shared_ptr<Page> await_resume() {
        return pager->get_page_from_cache(page_id); 
    }
};


struct FlushAwaiter {
    Pager* pager;
    uint32_t page_id;

    bool await_ready() { return false; } // Always suspend to simulate I/O
    void await_suspend(std::coroutine_handle<> h) {
        pager->schedule_write(page_id, h);
    }
    void await_resume() { 
        // Return nothing! The write is done.
    }
};


struct SplitResult {
    uint32_t split_key;
    uint32_t new_page_id;
};


struct SplitTask {
    struct promise_type {
        SplitResult result; // Holds our key and ID

        SplitTask get_return_object() {
            return { std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        
        // This now handles "co_return SplitResult{...}"
        void return_value(SplitResult r) { result = r; }
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    // Awaiter interface so we can co_await this task
    bool await_ready() { return handle.done(); }
    void await_suspend(std::coroutine_handle<>) {}
    SplitResult await_resume() { return handle.promise().result; }
    
    // Cleanup
    ~SplitTask() { if (handle) handle.destroy(); }
};


#endif