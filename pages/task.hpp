// #endif
#ifndef PAGETASK_HPP
#define PAGETASK_HPP

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <atomic>
#include <vector>
#include <cstdint>
#include <span>
#include <new>
#include "../pages/node.hpp"
#include <atomic>
#include <thread>


// Forward declarations to avoid direct implementation/inline pollution
class Cursor;
class Pager; 
class Page;
class Table;
struct SplitResult; 



// ============================================================================
// Zero-Allocation Thread-Local Arena for Coroutine Frames
// ============================================================================
class CoroutineFrameArena {
public:
    static constexpr size_t ArenaSize = 128 * 1024; // 128KB scratchpad per thread
    alignas(std::max_align_t) char storage[ArenaSize];
    size_t offset = 0;

    void* allocate(size_t size) noexcept {
        // Align up to max_align_t boundary
        size = (size + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);
        if (offset + size <= ArenaSize) {
            void* ptr = &storage[offset];
            offset += size;
            return ptr;
        }
        return nullptr; // Fallback to global heap if arena is exhausted
    }

    void deallocate(void* p, size_t size) noexcept {
        size = (size + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);
        // Strictly handles LIFO deallocations (perfect for nested coroutines)
        if (static_cast<char*>(p) + size == &storage[offset]) {
            offset -= size;
        }
    }
};

inline thread_local CoroutineFrameArena db_coro_arena;


struct FoundLeaf {
    std::shared_ptr<Page> page;
    uint32_t id;
};

// ==========================================
// 1. PageTask (RAII Compliant, Lazy Engine Pattern)
// ==========================================
// ==========================================
// 1. PageTask (Refactored for Memory Pools)
// ==========================================
struct PageTask {
    struct promise_type {
        std::shared_ptr<Page> result_page;
        std::coroutine_handle<> continuation{nullptr};
        std::exception_ptr exception{nullptr};

        // Custom Allocator Overrides to completely bypass global malloc/free
        void* operator new(std::size_t size) noexcept {
            if (void* p = db_coro_arena.allocate(size)) return p;
            return ::operator new(size);
        }
        void operator delete(void* ptr, std::size_t size) noexcept {
            if (ptr >= &db_coro_arena.storage[0] && ptr < &db_coro_arena.storage[CoroutineFrameArena::ArenaSize]) {
                db_coro_arena.deallocate(ptr, size);
            } else {
                ::operator delete(ptr);
            }
        }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation) return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        PageTask get_return_object() { return PageTask{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() { return {}; } 
        FinalAwaiter final_suspend() noexcept { return {}; } 
        void return_value(std::shared_ptr<Page> p) { result_page = std::move(p); }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle{nullptr};
    explicit PageTask(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~PageTask() { if (handle) handle.destroy(); }
    
    PageTask(const PageTask&) = delete;
    PageTask& operator=(const PageTask&) = delete;
    PageTask(PageTask&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    PageTask& operator=(PageTask&& other) noexcept {
        if (this != &other) {
            if (handle) handle.destroy();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    bool await_ready() { return handle.done(); }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) { 
        handle.promise().continuation = h; 
        return handle; 
    }
    std::shared_ptr<Page> await_resume() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result_page);
    }
};

// Apply this exact same operator new / operator delete snippet inside 
// LeafSearchTask::promise_type and SplitTask::promise_type.


// ==========================================
// 2. LeafSearchTask (RAII Compliant)
// ==========================================
struct LeafSearchTask {
    struct promise_type {
        FoundLeaf result;
        std::coroutine_handle<> continuation{nullptr};
        std::exception_ptr exception{nullptr};

        void* operator new(std::size_t size) noexcept {
            if (void* p = db_coro_arena.allocate(size)) return p;
            return ::operator new(size);
        }
        void operator delete(void* ptr, std::size_t size) noexcept {
            if (ptr >= &db_coro_arena.storage[0] && ptr < &db_coro_arena.storage[CoroutineFrameArena::ArenaSize]) {
                db_coro_arena.deallocate(ptr, size);
            } else {
                ::operator delete(ptr);
            }
        }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation) return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        LeafSearchTask get_return_object() { 
            return LeafSearchTask{ std::coroutine_handle<promise_type>::from_promise(*this) }; 
        }
        std::suspend_always initial_suspend() { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_value(FoundLeaf val) { result = std::move(val); }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle{nullptr};

    explicit LeafSearchTask(std::coroutine_handle<promise_type> h) : handle(h) {}
    
    ~LeafSearchTask() { if (handle) handle.destroy(); }
    LeafSearchTask(const LeafSearchTask&) = delete;
    LeafSearchTask& operator=(const LeafSearchTask&) = delete;
    
    LeafSearchTask(LeafSearchTask&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    LeafSearchTask& operator=(LeafSearchTask&& other) noexcept {
        if (this != &other) {
            if (handle) handle.destroy();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    bool await_ready() { return handle.done(); }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) { 
        handle.promise().continuation = h; 
        return handle;
    }
    FoundLeaf await_resume() { 
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result); 
    }
};

// ==========================================
// 3. SplitTask (RAII Compliant)
// ==========================================
struct SplitTask {
    struct promise_type {
        SplitResult result; 
        std::coroutine_handle<> continuation{nullptr};
        std::exception_ptr exception{nullptr};

        void* operator new(std::size_t size) noexcept {
            if (void* p = db_coro_arena.allocate(size)) return p;
            return ::operator new(size);
        }
        void operator delete(void* ptr, std::size_t size) noexcept {
            if (ptr >= &db_coro_arena.storage[0] && ptr < &db_coro_arena.storage[CoroutineFrameArena::ArenaSize]) {
                db_coro_arena.deallocate(ptr, size);
            } else {
                ::operator delete(ptr);
            }
        }
    
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation) return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }
        SplitTask get_return_object() {
            return SplitTask{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        std::suspend_always initial_suspend() { return {}; }
        void return_value(SplitResult r) { result = r; }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle{nullptr};

    explicit SplitTask(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~SplitTask() { if (handle) handle.destroy(); }

    SplitTask(const SplitTask&) = delete;
    SplitTask& operator=(const SplitTask&) = delete;

    SplitTask(SplitTask&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    SplitTask& operator=(SplitTask&& other) noexcept {
        if (this != &other) {
            if (handle) handle.destroy();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    bool await_ready() { return handle.done(); }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) { 
        handle.promise().continuation = h; 
        return handle;
    }
    SplitResult await_resume() {
        if (handle.promise().exception) {
            std::rethrow_exception(handle.promise().exception);
        }
        return handle.promise().result;
    }
};

// ==========================================
// 4. VoidTask (Fire-and-Forget Guarded RAII)
// ==========================================
struct VoidTask {
    struct promise_type {
        std::coroutine_handle<> continuation = nullptr;

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation) return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        VoidTask get_return_object() { 
            return VoidTask{ std::coroutine_handle<promise_type>::from_promise(*this) }; 
        }
        std::suspend_always initial_suspend() { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle{nullptr};

    explicit VoidTask(std::coroutine_handle<promise_type> h) : handle(h) {}
    
    ~VoidTask() { if (handle) handle.destroy(); }

    VoidTask(const VoidTask&) = delete;
    VoidTask& operator=(const VoidTask&) = delete;

    VoidTask(VoidTask&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    VoidTask& operator=(VoidTask&& other) noexcept {
        if (this != &other) {
            if (handle) handle.destroy();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    bool await_ready() const noexcept { return handle ? handle.done() : true; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        handle.promise().continuation = caller;
        return handle; 
    }
    void await_resume() const noexcept {}

    bool is_done() const noexcept {
        return !handle || handle.done();
    }
};

// ==========================================
// 5. Zero-Allocation Transient Awaiters
// ==========================================
struct PageAwaiter {
    Pager* pager;
    uint32_t page_id;

    bool await_ready();
    void await_suspend(std::coroutine_handle<> h);
    std::shared_ptr<Page> await_resume();
};

struct FlushAwaiter {
    Pager* pager;
    uint32_t page_id;

    bool await_ready() { return false; } 
    void await_suspend(std::coroutine_handle<> h);
    void await_resume();
};

// Optimized to prevent heap allocation per-flush sequence via std::span

struct MultiFlushAwaiter {
    Pager* pager;
    const std::vector<uint32_t>& page_ids;
    std::coroutine_handle<> awaiting_coroutine;
    
    std::atomic<size_t> counter{0};
    std::vector<Page*> allocated_pages; 

    MultiFlushAwaiter(Pager* p, const std::vector<uint32_t>& ids);

    ~MultiFlushAwaiter();

    // Ensure we don't accidentally copy or move destructively
    MultiFlushAwaiter(const MultiFlushAwaiter&) = delete;
    MultiFlushAwaiter& operator=(const MultiFlushAwaiter&) = delete;

    bool await_ready() noexcept { return page_ids.empty(); }
    void await_suspend(std::coroutine_handle<> h);
    void await_resume() noexcept {
        // Double check safeguard: Ensure everything actually hit home before releasing
        while (counter.load(std::memory_order_acquire) > 0) {
            // Tight spin or low latency yield fallback 
            // This guarantees the object cannot be destroyed if a CQE is lagging
            std::this_thread::yield(); 
        }
    }
};

struct PageLatch {
    Table* table;
    uint32_t page_id;

    bool await_ready();        
    void await_suspend(std::coroutine_handle<> h); 
    void await_resume();       
};

#endif