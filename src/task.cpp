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

MultiFlushAwaiter::MultiFlushAwaiter(Pager* p, const std::vector<uint32_t>& ids)
    : pager(p), page_ids(ids) {

    // Take the pre-allocated capacity from the pager's scratchpad
    std::swap(allocated_pages, pager->flush_scratch_vector);
    allocated_pages.clear(); // Resets size to 0 but keeps the underlying capacity!
    allocated_pages.reserve(page_ids.size());
}

MultiFlushAwaiter::~MultiFlushAwaiter() {
    std::swap(pager->flush_scratch_vector, allocated_pages);
}


void FlushAwaiter::await_suspend(std::coroutine_handle<> h) {
    // 1. Create a BatchContext for this single operation
    auto batch = std::make_shared<BatchContext>(h, 1);

    // 2. Pass the raw pointer using .get()
    pager->submit_write(page_id, batch.get());
}

void FlushAwaiter::await_resume() { }

// Example of defining the nested FinalAwaiter logic in the .cpp
// bool PageTask::promise_type::FinalAwaiter::await_ready() noexcept { 
//     return false; 
// }

// std::coroutine_handle<> PageTask::promise_type::FinalAwaiter::await_suspend(std::coroutine_handle<promise_type> h) noexcept {
//     if (h.promise().continuation) return h.promise().continuation;
//     return std::noop_coroutine();
// }

// void PageTask::promise_type::FinalAwaiter::await_resume() noexcept {}

// Your existing definitions
// PageTask PageTask::promise_type::get_return_object() { 
//     return { std::coroutine_handle<promise_type>::from_promise(*this) }; 
// }

// PageTask PageTask::promise_type::get_return_object() { 
//     return PageTask{ std::coroutine_handle<promise_type>::from_promise(*this) }; 
// }

// std::suspend_never PageTask::promise_type::initial_suspend() { return {}; }

// std::suspend_always PageTask::promise_type::initial_suspend() { return {}; }

// PageTask::promise_type::FinalAwaiter PageTask::promise_type::final_suspend() noexcept {
//     return {};
// }

// void PageTask::promise_type::return_value(std::shared_ptr<Page> p) { result_page = std::move(p); }

// void PageTask::promise_type::unhandled_exception() {
//     exception = std::current_exception();
// }


// void MultiFlushAwaiter::await_suspend(std::coroutine_handle<> h) {
//     awaiting_coroutine = h;
//     allocated_pages.reserve(page_ids.size());

//     for (size_t i = 0; i < page_ids.size(); ++i) {
//         uint32_t page_id = page_ids[i];
//         off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;

//         // 1. Allocate a zero-TLB 4KB Page from your HugePage arena
//         Page* huge_page = pager->mem_pool->allocate();
//         allocated_pages.push_back(huge_page);

//         //2. Snapshot dirty page cache directly into the HugeOage slot
//         std::memcpy(huge_page->data, pager->page_cache[page_id]->data, PAGE_SIZE);

//         struct io_uring_sqe* sqe = io_uring_get_sqe(&pager->ring);
//         if (!sqe) {
//             io_uring_submit(&pager->ring);
//             sqe = io_uring_get_sqe(&pager->ring);
//         }

//         io_uring_prep_write_fixed(sqe, pager->fd, huge_page->data, PAGE_SIZE, offset, 0);
//         io_uring_sqe_set_data(sqe, this);
//     }

//     io_uring_submit(&pager->ring);

// }


void MultiFlushAwaiter::await_suspend(std::coroutine_handle<> h) {
    this->awaiting_coroutine = h;
    this->counter.store(page_ids.size(), std::memory_order_relaxed);

    // FIX: This guarantees the push_back below won't allocate on the heap!
    this->allocated_pages.reserve(page_ids.size()); 

    uintptr_t tagged_ptr = reinterpret_cast<uintptr_t>(this) | 1;
    void* user_data_tag = reinterpret_cast<void*>(tagged_ptr);

    size_t pending_in_ring = 0;
    size_t totally_submitted = 0;

    for (size_t i = 0; i < page_ids.size(); ++i) {
        uint32_t page_id = page_ids[i];
        off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;

        Page* huge_page = pager->mem_pool->allocate();
        this->allocated_pages.push_back(huge_page); // Safe from allocation due to reserve()
        std::memcpy(huge_page->data, pager->page_cache[page_id]->data, PAGE_SIZE);

        struct io_uring_sqe* sqe = io_uring_get_sqe(&pager->ring);
        if (!sqe) {
            int ret = io_uring_submit(&pager->ring);
            if (ret < 0) {
                std::cerr << "[FATAL] Mid-batch submission failed: " << std::strerror(-ret) << std::endl;
                
                // Adjust counter to only expect what we managed to push to the ring
                size_t failed_to_submit = page_ids.size() - totally_submitted;
                size_t remaining = this->counter.fetch_sub(failed_to_submit, std::memory_order_acq_rel) - failed_to_submit;
                
                if (remaining == 0) {
                    for (Page* page : this->allocated_pages) pager->mem_pool->deallocate(page);
                    h.resume();
                }
                return;
            }
            totally_submitted += pending_in_ring;
            pending_in_ring = 0;
            sqe = io_uring_get_sqe(&pager->ring);
        }

        io_uring_prep_write_fixed(sqe, pager->fd, huge_page->data, PAGE_SIZE, offset, 0);
        io_uring_sqe_set_data(sqe, user_data_tag);
        pending_in_ring++;
    }

    int submitted = io_uring_submit(&pager->ring);
    if (submitted < 0) {
        std::cerr << "[FATAL] io_uring_submit failed: " << std::strerror(-submitted) << std::endl;
        
        size_t failed_to_submit = page_ids.size() - totally_submitted;
        size_t remaining = this->counter.fetch_sub(failed_to_submit, std::memory_order_acq_rel) - failed_to_submit;
        
        if (remaining == 0) {
            for (Page* page : this->allocated_pages) pager->mem_pool->deallocate(page);
            h.resume();
        }
    } else {
        totally_submitted += pending_in_ring;
    }
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