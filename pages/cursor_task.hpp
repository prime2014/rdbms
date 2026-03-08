#ifndef CURSORTASK_HPP
#define CURSORTASK_HPP

#include "cursor.hpp"
#include <optional>

struct CursorTask {
    struct promise_type {
        std::optional<Cursor> result_cursor; 

        CursorTask get_return_object() {
            return { std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_never initial_suspend() { return {}; }

        std::suspend_always final_suspend() noexcept { return {}; }

        // This handles "co_return some_page"
        void return_value(Cursor c);
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    ~CursorTask() { if (handle) handle.destroy(); }

    CursorTask(std::coroutine_handle<promise_type> h) : handle(h) {}

    bool await_ready() { return handle.done(); }

    // 2. What to do if it's not done (suspend the caller)
    void await_suspend(std::coroutine_handle<> caller_handle) {
        // In a simple system, we just let the sub-task run.
        // For now, we assume initial_suspend was "never", 
        // so the task is already moving.
    };

    Cursor await_resume();
};


#endif