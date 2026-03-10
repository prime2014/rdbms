#include "../pages/pager.hpp"
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <coroutine>
#include <liburing.h>
#include <stdexcept>


struct IOContext {
    std::coroutine_handle<> handle;
    uint32_t page_id;
    std::shared_ptr<std::atomic<size_t>> batch_counter;
    std::unique_ptr<char[]> write_buffer;
};


Pager::Pager(const std::string& filename, bool memory_only) : memory_only_(memory_only) {
    if (memory_only_) {
        this->fd = -1;
        this->file_length = 0;
        this->num_pages = 0;
        return;
    }

    // Initialize io_uring
    if (io_uring_queue_init(256, &ring, 0) < 0) {
        throw std::runtime_error("Failed to initialize io_uring");
    }

    // Open file using low-level O_RDWR for io_uring compatibility
    this->fd = open(filename.c_str(), O_RDWR | O_CREAT, 0644);
    if (this->fd < 0) {
        throw std::runtime_error("Could not open file: " + filename);
    }

    // Determine file length and page count
    struct stat st;
    if (fstat(this->fd, &st) == 0) {
        this->file_length = st.st_size;
        this->num_pages = this->file_length / PAGE_SIZE;
    }

    if (this->file_length % PAGE_SIZE != 0) {
        std::cerr << "Warning: DB file size is not a multiple of PAGE_SIZE!" << std::endl;
    }

    std::cout << "Opened " << filename << " [FD: " << fd << "] with " << num_pages << " pages." << std::endl;
}

Pager::~Pager() {
    if (memory_only_) return;
    io_uring_queue_exit(&ring);
    if (this->fd >= 0) close(this->fd);
}

// --- Async I/O Core ---

void Pager::process_completions(bool wait) {
    struct io_uring_cqe* cqe;
    int res;

    // 1. Handle the initial wait attempt with signal resiliency
    if (wait) {
        while ((res = io_uring_wait_cqe(&ring, &cqe)) == -EINTR) {
            // Signal interrupted the wait; retry immediately
            continue; 
        }
    } else {
        res = io_uring_peek_cqe(&ring, &cqe);
    }

    // 2. Drain the queue
    while (res == 0 && cqe) {
        // Retrieve the pointer we gave to the kernel
        IOContext* ctx = static_cast<IOContext*>(io_uring_cqe_get_data(cqe));

        std::cout << "DEBUG: [3] Event found! Resuming coroutine for page: " << ctx->page_id << std::endl;

        
        if (ctx) {
            this->clear_dirty(ctx->page_id);
            
            // Handle batch logic if needed
            if (ctx->batch_counter && ctx->batch_counter->fetch_sub(1) > 1) {
                // Not the last one yet, don't resume
            } else {
                ctx->handle.resume();
            }
            
            // CRITICAL: Delete the heap memory to prevent leaks and dangling pointers
            delete ctx; 
        }

        io_uring_cqe_seen(&ring, cqe);
        
        // Peek for more items. No need to retry EINTR on peek_cqe 
        // because it doesn't block the thread.
        res = io_uring_peek_cqe(&ring, &cqe);
    }
}


void Pager::schedule_write(uint32_t page_id, std::coroutine_handle<> h) {
    if (memory_only_) {
        clear_dirty(page_id);
        memory_pending_resumes_.push_back(h);
        return;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        io_uring_submit(&ring);
        sqe = io_uring_get_sqe(&ring);
    }

    void* buffer = page_cache[page_id]->data;
    off_t offset = (off_t)page_id * PAGE_SIZE;

    io_uring_prep_write(sqe, fd, buffer, PAGE_SIZE, offset);
    io_uring_sqe_set_data(sqe, h.address());
    
    clear_dirty(page_id);
}

void Pager::schedule_async_load(uint32_t page_id, std::coroutine_handle<> h) {
    std::cout << "DEBUG: Submitting read for page " << page_id << std::endl;
    if (memory_only_) {
        if (page_cache.find(page_id) == page_cache.end()) {
            page_cache[page_id] = std::make_shared<Page>();
        }
        memory_pending_resumes_.push_back(h);
        return;
    }

    // Create the context on the HEAP so it survives after this function ends
    IOContext* ctx = new IOContext{h, page_id, nullptr}; 

    auto page = std::make_shared<Page>();
    page_cache[page_id] = page;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);

    io_uring_prep_read(sqe, fd, page->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    
    // Pass the HEAP POINTER to the kernel
    io_uring_sqe_set_data(sqe, ctx);

    std::cout << "DEBUG: [1] I/O Request submitted to SQ for page " << page_id << std::endl;

    int ret = io_uring_submit(&ring);

    if (ret < 0) {
        std::cerr << "io_uring_submit failed: " << ret << std::endl;
    }
}

// --- Synchronous Fallbacks & Metadata ---

void Pager::write_page(uint32_t page_id, const Page& page) {
    off_t offset = (off_t)page_id * PAGE_SIZE;
    if (pwrite(this->fd, page.data, PAGE_SIZE, offset) == -1) {
        throw std::runtime_error("Synchronous write failed");
    }
    
    if (offset + PAGE_SIZE > file_length) {
        file_length = offset + PAGE_SIZE;
        num_pages = file_length / PAGE_SIZE;
    }
}

std::unique_ptr<Page> Pager::read_page(uint32_t page_id) {
    auto page = std::make_shared<Page>();
    ssize_t bytes = pread(this->fd, page->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    
    if (bytes < 0) throw std::runtime_error("Sync read failed");
    if (bytes < PAGE_SIZE) std::memset(page->data + bytes, 0, PAGE_SIZE - bytes);

    page_cache[page_id] = page;
    return std::make_unique<Page>(*page);
}

// --- Awaiter Factories ---

FlushAwaiter Pager::flush_page_async(uint32_t page_id) {
    return FlushAwaiter{this, page_id};
}

PageAwaiter Pager::get_page_async(uint32_t page_id) {
    return PageAwaiter{this, page_id};
}

// --- Bitmap & Cache Helpers ---

Page* Pager::get_page(uint32_t page_id) {
    auto it = page_cache.find(page_id);

    if (it != page_cache.end()) {
        return it->second.get();
    }

    // 2. Cache Miss: Load from disk (Slow Path)
    // Assuming 'read_page' is your existing synchronous method
    auto handle = read_page(page_id);
    page_cache[page_id] = std::shared_ptr<Page>(std::move(handle));

    return page_cache[page_id].get();
}


void Pager::mark_dirty(uint32_t page_id) {
    dirty_pages.insert(page_id);
}

void Pager::mark_as_dirty(uint32_t page_id) {
    uint32_t idx = page_id / 8;
    if (idx >= dirty_bitmap.size()) dirty_bitmap.resize(idx + 1, 0);
    dirty_bitmap[idx] |= (1 << (page_id % 8));
}

void Pager::clear_dirty(uint32_t page_id) {
    uint32_t idx = page_id / 8;
    if (idx < dirty_bitmap.size()) dirty_bitmap[idx] &= ~(1 << (page_id % 8));
}

bool Pager::is_dirty(uint32_t page_id) const {
    uint32_t idx = page_id / 8;
    if (idx >= dirty_bitmap.size()) return false;
    return (dirty_bitmap[idx] & (1 << (page_id % 8))) != 0;
}

bool Pager::is_in_memory(uint32_t page_id) const {
    return page_cache.find(page_id) != page_cache.end();
}

std::shared_ptr<Page> Pager::get_page_from_cache(uint32_t page_id) {
    return page_cache.at(page_id);
}

bool Pager::is_ring_idle() {
    return memory_only_ ? true : (io_uring_sq_ready(&ring) == 0);
}

uint32_t Pager::get_unused_page_number() { return num_pages; }
uint32_t Pager::get_num_pages() { return num_pages; }
int Pager::get_fd() { return fd; }


void Pager::submit_to_kernel() {
    io_uring_submit(&ring);
}



void Pager::submit_write(uint32_t page_id, 
                         std::coroutine_handle<> h, 
                         std::shared_ptr<std::atomic<size_t>> counter) { 
    
    // 1. Calculate the offset based on the page_id
    off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;

    // 2. Snapshot the data
    auto buffer_snapshot = std::make_unique<char[]>(PAGE_SIZE);
    std::memcpy(buffer_snapshot.get(), page_cache[page_id]->data, PAGE_SIZE);

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    
    // 3. Point the kernel at the snapshot
    io_uring_prep_write(sqe, fd, buffer_snapshot.get(), PAGE_SIZE, offset);
    
    // 4. Important: Store the buffer snapshot in your IOContext
    // Ensure your IOContext struct has a member like: std::unique_ptr<char[]> write_buffer;
    IOContext* ctx = new IOContext{h, page_id, counter, std::move(buffer_snapshot)};
    io_uring_sqe_set_data(sqe, ctx);
}

// 2. New submit_all method
void Pager::submit_all() {
    io_uring_submit(&ring);
}


// 1. Implementation for shutdown_gracefully()
void Pager::shutdown_gracefully() {
    if (memory_only_) return;
    // Flush all dirty pages before closing
    for (uint32_t i = 0; i < num_pages; ++i) {
        if (is_dirty(i)) {
            write_page(i, *page_cache[i]);
        }
    }
    io_uring_submit(&ring);
    std::cout << "Pager: All dirty pages flushed." << std::endl;
}

// 2. Implementation for async_load_from_disk()
void Pager::async_load_from_disk(uint32_t page_id) {
    if (page_cache.find(page_id) == page_cache.end()) {
        page_cache[page_id] = std::make_shared<Page>();
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) return; 

    io_uring_prep_read(sqe, fd, page_cache[page_id]->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    io_uring_sqe_set_data(sqe, (void*)(uintptr_t)page_id);
    io_uring_submit(&ring);
}


MultiFlushAwaiter Pager::flush_all_dirty_async() {
    if (dirty_pages.empty()) {
        // Now it uses the new constructor with the 4th argument as 'true'
        return MultiFlushAwaiter{this, {}, nullptr, true}; 
    }

    std::vector<uint32_t> ids(dirty_pages.begin(), dirty_pages.end());
    dirty_pages.clear();

    auto counter = std::make_shared<std::atomic<size_t>>(ids.size());
    
    // Uses constructor, 'ready' defaults to false
    return MultiFlushAwaiter{this, std::move(ids), counter};
}