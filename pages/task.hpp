#ifndef PAGETASK_HPP
#define PAGETASK_HPP

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include "node.hpp"
#include <atomic>

// FORWARD DECLARATION:

class Cursor;
class Pager; // Forward declare
class Page;

struct PageTask {
    struct promise_type;

    struct PageTaskFinalAwaiter : std::suspend_always {
        void await_suspend(std::coroutine_handle<promise_type> completing) noexcept;
    };

    struct promise_type {
        std::shared_ptr<Page> result_page;
        std::coroutine_handle<> continuation;
        std::exception_ptr exception;

        PageTask get_return_object() {
            return { std::coroutine_handle<promise_type>::from_promise(*this) };
        };

        std::suspend_never initial_suspend();
        PageTaskFinalAwaiter final_suspend() noexcept;

        void return_value(std::shared_ptr<Page> p);

        void unhandled_exception();

        
    };

    std::coroutine_handle<promise_type> handle;

    ~PageTask() { if (handle) handle.destroy(); }

    PageTask(std::coroutine_handle<promise_type> h) : handle(h) {}

    bool await_ready() { return handle.done(); }

    // 2. What to do if it's not done (suspend the caller)
    void await_suspend(std::coroutine_handle<> h) {
        handle.promise().continuation = h;
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
    std::shared_ptr<Page> await_resume();
};




// Revised VoidTask (Pure Task handle)
struct VoidTask {
    struct promise_type {
        std::atomic<bool> is_done{false};
        std::coroutine_handle<> continuation;
        VoidTask get_return_object() { return { std::coroutine_handle<promise_type>::from_promise(*this) }; }
        std::suspend_never initial_suspend() { return {}; }
        auto final_suspend() noexcept { return FinalAwaiter{continuation}; }
        void unhandled_exception() { std::terminate(); }
        void return_void() {}
    };

    struct FinalAwaiter {
        std::coroutine_handle<> continuation;
        bool await_ready() noexcept { return !continuation; }
        void await_suspend(std::coroutine_handle<promise_type> h) noexcept { continuation.resume(); }
        void await_resume() noexcept {}
    };

    std::coroutine_handle<promise_type> handle;
    // Remove pager/page_id from here!

    bool await_ready() { 
        return handle && handle.done(); 
    }
    
    void await_suspend(std::coroutine_handle<> h) {
        // When this task is awaited, attach the caller (h) as a continuation
        handle.promise().continuation = h;
    }
    
    void await_resume() {}
    
    ~VoidTask() { if (handle) handle.destroy(); }

    bool is_done() const {
        return handle ? handle.promise().is_done.load() : true;
    }
};


struct FlushAwaiter {
    Pager* pager;
    uint32_t page_id;

    bool await_ready() { return false; } // Always suspend to simulate I/O
    void await_suspend(std::coroutine_handle<> h);
    void await_resume();
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




struct MultiFlushAwaiter {
    Pager* pager;
    std::vector<uint32_t> page_ids;
    std::shared_ptr<std::atomic<size_t>> remaining;
    bool ready;
    // Constructor
    MultiFlushAwaiter(Pager* p, std::vector<uint32_t> ids, std::shared_ptr<std::atomic<size_t>> rem, bool r = false)
        : pager(p), page_ids(std::move(ids)), remaining(rem), ready(r) {}

    bool await_ready() { return page_ids.empty(); }

    void await_suspend(std::coroutine_handle<> h);

    void await_resume() {}
};



#endif