#ifndef PAGETASK_HPP
#define PAGETASK_HPP

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include "node.hpp"
#include <atomic>
#include "../src/leafcell.cpp"

// FORWARD DECLARATION:

class Cursor;
class Pager; // Forward declare
class Page;
class Table;

struct PageTask {
    struct promise_type {
        std::shared_ptr<Page> result_page;
        std::coroutine_handle<> continuation;
        std::exception_ptr exception;

        struct FinalAwaiter {
            bool await_ready() noexcept; // Logic removed
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept; // Logic removed
            void await_resume() noexcept; // Logic removed
        };

        PageTask get_return_object(); // Declaration only
        std::suspend_never initial_suspend(); // Declaration only
        FinalAwaiter final_suspend() noexcept; // Declaration only
        void return_value(std::shared_ptr<Page> p); // Declaration only
        void unhandled_exception(); // Declaration only
    };

    std::coroutine_handle<promise_type> handle;
    PageTask(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~PageTask() { if (handle) handle.destroy(); }

    PageTask(const PageTask&) = delete;
    PageTask(PageTask&& other) noexcept : handle(other.handle) { other.handle = nullptr; }

    bool await_ready() { return handle.done(); }
    void await_suspend(std::coroutine_handle<> h) { handle.promise().continuation = h; }
    std::shared_ptr<Page> await_resume() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result_page);
    }
};


struct FoundLeaf {
    std::shared_ptr<Page> page;
    uint32_t id;
};


struct LeafSearchTask {
    struct promise_type {
        FoundLeaf result;
        std::coroutine_handle<> continuation;

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation) return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        LeafSearchTask get_return_object() { return { std::coroutine_handle<promise_type>::from_promise(*this) }; }
        std::suspend_never initial_suspend() { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_value(FoundLeaf val) { result = std::move(val); }
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;
    LeafSearchTask(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~LeafSearchTask() { if (handle) handle.destroy(); }

    bool await_ready() { return handle.done(); }
    void await_suspend(std::coroutine_handle<> h) { handle.promise().continuation = h; }
    FoundLeaf await_resume() { return std::move(handle.promise().result); }
};


struct PageAwaiter {
    Pager* pager;
    uint32_t page_id;

    bool await_ready();
    void await_suspend(std::coroutine_handle<> h);
    std::shared_ptr<Page> await_resume();
};




struct VoidTask {
    struct promise_type {
        std::coroutine_handle<> continuation = nullptr;
        bool is_complete = false;

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                h.promise().is_complete = true;
                if (h.promise().continuation) return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        VoidTask get_return_object() { 
            return { std::coroutine_handle<promise_type>::from_promise(*this) }; 
        }
        std::suspend_always initial_suspend() { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    // 1. Check if the task is already finished
    bool await_ready() const noexcept { 
        return handle ? handle.done() : true; 
    }

    // 2. Suspend the caller and save its handle as our continuation
    void await_suspend(std::coroutine_handle<> caller) noexcept {
        handle.promise().continuation = caller;
        
        // If the task was created in a suspended state (initial_suspend is suspend_always),
        // we must resume it here to start the work.
        if (!handle.done()) {
            handle.resume();
        }
    }

    // 3. What to return when the co_await expression completes
    void await_resume() const noexcept {}
    
    ~VoidTask() { 
        if (handle) {
            // Only destroy if the coroutine is actually suspended at a suspend point
            // and hasn't been destroyed by its own promise logic.
            handle.destroy();
            handle = nullptr; 
        }
    }
    
    bool is_done() const { 
        return !handle || handle.done(); 
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
        SplitResult result; 
        std::coroutine_handle<> continuation;
    
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                // Symmetric Transfer: resume the parent immediately
                if (h.promise().continuation) return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        // ONLY THIS ONE:
        FinalAwaiter final_suspend() noexcept { return {}; }

        SplitTask get_return_object() {
            return { std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        
        std::suspend_never initial_suspend() { return {}; }
        
        void return_value(SplitResult r) { result = r; }
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    SplitTask(std::coroutine_handle<promise_type> h) : handle(h) {}

    bool await_ready() { return handle.done(); }
    
    // CRITICAL: You must save the caller's handle to 'continuation'
    void await_suspend(std::coroutine_handle<> h) { 
        handle.promise().continuation = h; 
    }
    
    SplitResult await_resume() { return handle.promise().result; }
    
    ~SplitTask() { if (handle) handle.destroy(); }
};




// struct MultiFlushAwaiter {
//     Pager* pager;
//     std::vector<uint32_t> page_ids;
//     std::shared_ptr<std::atomic<size_t>> remaining;
//     bool ready;
//     // Constructor
//     MultiFlushAwaiter(Pager* p, std::vector<uint32_t> ids, std::shared_ptr<std::atomic<size_t>> rem, bool r = false)
//         : pager(p), page_ids(std::move(ids)), remaining(rem), ready(r) {}

//     bool await_ready() { return page_ids.empty(); }

//     void await_suspend(std::coroutine_handle<> h);

//     void await_resume() {}
// };



struct MultiFlushAwaiter {
    Pager* pager;
    std::shared_ptr<std::vector<uint32_t>> page_ids;  // Shared pointer to vector
    std::shared_ptr<std::atomic<size_t>> remaining;   // Keep this for quick decrement
    bool ready;
    
    // Constructor
    MultiFlushAwaiter(Pager* p, std::vector<uint32_t> ids, 
                      std::shared_ptr<std::atomic<size_t>> rem, bool r = false)
        : pager(p), page_ids(std::make_shared<std::vector<uint32_t>>(std::move(ids))), 
          remaining(rem), ready(r) {}

    bool await_ready() { return page_ids->empty(); }

    void await_suspend(std::coroutine_handle<> h);
    void await_resume() {}
};

struct PageLatch {
    Table* table;
    uint32_t page_id;

    bool await_ready();                        // Declare only
    void await_suspend(std::coroutine_handle<> h); // Declare only
    void await_resume();                       // Declare only
};



#endif