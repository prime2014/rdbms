#include "../pages/pager.hpp"
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <coroutine>
#include <liburing.h>
#include <stdexcept>

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

void Pager::process_completions() {
    if (memory_only_) {
        std::vector<std::coroutine_handle<>> batch;
        batch.swap(memory_pending_resumes_);
        for (auto h : batch) {
            if (h && !h.done()) h.resume();
        }
        return;
    }

    struct io_uring_cqe* cqe;
    int completions_found = 0;

    // Non-blocking peek at the Completion Queue
    while (io_uring_peek_cqe(&ring, &cqe) == 0) {
        completions_found++;
        
        // Retrieve the coroutine handle from user_data
        auto h = std::coroutine_handle<>::from_address(io_uring_cqe_get_data(cqe));

        if (cqe->res < 0) {
            std::cerr << "I/O Error: " << std::strerror(-cqe->res) << std::endl;
        }

        io_uring_cqe_seen(&ring, cqe);

        // Resume the suspended B+ Tree task
        if (h && !h.done()) {
            h.resume();
        }
    }

    if (completions_found > 0) {
        std::cout << "Pager: Processed " << completions_found << " I/O completions." << std::endl;
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
    if (memory_only_) {
        if (page_cache.find(page_id) == page_cache.end()) {
            page_cache[page_id] = std::make_shared<Page>();
        }
        memory_pending_resumes_.push_back(h);
        return;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        io_uring_submit(&ring);
        sqe = io_uring_get_sqe(&ring);
    }

    auto page = std::make_shared<Page>();
    page_cache[page_id] = page;

    io_uring_prep_read(sqe, fd, page->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    io_uring_sqe_set_data(sqe, h.address());
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


// 1. Modified submit_write (now just queues)
void Pager::submit_write(uint32_t page_id, std::coroutine_handle<> h) {
    if (memory_only_) { /* ... */ return; }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        // If SQ is full, we are forced to submit to make room
        io_uring_submit(&ring);
        sqe = io_uring_get_sqe(&ring);
    }

    void* buffer = page_cache[page_id]->data;
    io_uring_prep_write(sqe, fd, buffer, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    io_uring_sqe_set_data(sqe, h.address());
    clear_dirty(page_id);
    // REMOVED io_uring_submit here
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