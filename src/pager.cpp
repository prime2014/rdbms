#include "../pages/pager.hpp"
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <coroutine>
#include <liburing.h>
#include <stdexcept>
#include "../pages/wal_debug.hpp"
#include "../pages/context.hpp"



struct InsertContext {
    uint32_t key;
    std::string value;
    std::vector<uint32_t> affected_pages;
    std::shared_ptr<BatchContext> batch_ctx; // The same latch we discussed
    bool is_complete = false;
};


Pager::Pager(const std::string& filename, bool memory_only) : memory_only_(memory_only) {
    if (memory_only_) {
        this->fd = -1;
        this->file_length = 0;
        this->num_pages = 0;
        return;
    }

    // Initialize a 512MB Buffer Pool backed by 256 HugePages (2MB each)
    // This yields 131,072 individual 4KB DB Pages
    mem_pool = std::make_unique<HugepageAllocator>(256);

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


std::shared_ptr<Page> Pager::get_root_shared() {
    // 1. Check the dedicated pin first
    if (root_cache) {
        return root_cache;
    }

    // 2. Fallback: If root_cache is null, check the general cache
    // Note: You'll need to know which ID is the root. 
    // In a real DB, the Table class usually passes this ID.
    auto it = page_cache.find(1); // Assuming 1 is initial root
    if (it != page_cache.end()) {
        return it->second;
    }

    // 3. Absolute Fallback: Error or forced load
    std::cerr << "CRITICAL: Root page not found in cache!" << std::endl;
    return nullptr; 
}





void Pager::pin_root(uint32_t root_id, std::shared_ptr<Page> existing_page) {
    if (existing_page) {
        // Use the page we just created in the constructor
        this->page_cache[root_id] = existing_page;
        this->root_cache = existing_page;
        std::cout << "DEBUG: Pinning: Using provided page " << root_id << std::endl;
    } else {
        // Fallback to disk read
        auto it = page_cache.find(root_id);
        if (it == page_cache.end()) {
            auto page = read_page(root_id);
            this->root_cache = page_cache[root_id];
        } else {
            this->root_cache = it->second;
        }
    }
}


// std::shared_ptr<Page> Pager::get_page_shared(uint32_t page_id) {
//     auto it = page_cache.find(page_id);

//     // 1. If it's already in the cache, just return the pointer
//     if (it != page_cache.end()) {
//         return it->second;
//     }

//     // 2. Cache Miss: Perform a synchronous read
//     // This is used during startup or specific sync operations
//     auto page = std::make_shared<Page>();
//     ssize_t bytes = pread(this->fd, page->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    
//     if (bytes < 0) throw std::runtime_error("Sync read failed for page " + std::to_string(page_id));
    
//     // Ensure the rest of the page is clean if it's a partial read
//     if (bytes < PAGE_SIZE) std::memset(page->data + bytes, 0, PAGE_SIZE - bytes);

//     // 3. Save to cache and return
//     page_cache[page_id] = page;
//     return page;
// }


std::shared_ptr<Page> Pager::get_page_shared(uint32_t page_id) {
    auto it = page_cache.find(page_id);

    // 1. Cache Hit: Just return the shared pointer
    if (it != page_cache.end()) {
        return it->second;
    }

    // 2. Cache Miss: Request memory from our member Hugepage pool
    if (!mem_pool) {
        throw std::runtime_error("Hugepage pool (mem_pool) was not initialized!");
    }

    Page* raw_page = mem_pool->allocate();

    // Wrap it in a std::shared_ptr with a custom deleter lambda that returns it to our allocator pool
    std::shared_ptr<Page> page(raw_page, [this](Page* p) {
        if (this->mem_pool) {
            this->mem_pool->deallocate(p);
        } else {
            p->~Page();
        }
    });

    // 3. Populate page data
    if (memory_only_) {
        std::memset(page->data, 0, PAGE_SIZE);
    } else {

        ssize_t bytes = pread(this->fd, page->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
        
        if (bytes < 0) throw std::runtime_error("Sync read failed for page " + std::to_string(page_id));
        
        // Ensure the rest of the page is clean if it's a partial read
        if (bytes < PAGE_SIZE) std::memset(page->data + bytes, 0, PAGE_SIZE - bytes);
    }

    page_cache[page_id] = page;
    return page;

}


void Pager::process_completions(bool wait) {
    struct io_uring_cqe* cqe;
    
    // 1. Wait for at least one completion if requested
    if (wait) {
        int ret;
        // Standard io_uring wait loop to handle signal interruptions (EINTR)
        while ((ret = io_uring_wait_cqe(&ring, &cqe)) == -EINTR);
        if (ret < 0) return;
    }

    std::vector<std::coroutine_handle<>> tasks_to_resume;

    // 2. Peek through all available CQEs
    while (io_uring_peek_cqe(&ring, &cqe) == 0) {
        IOContext* ctx = reinterpret_cast<IOContext*>(io_uring_cqe_get_data(cqe));
        
        if (ctx) {
            // 1. Handle READ Operations (Page Loading)
            if (pending_io.count(ctx->page_id)) {
                auto& entry = pending_io[ctx->page_id];
                
                // Move the loaded data into the cache
                page_cache[ctx->page_id] = entry.page_buffer;
                
                // Collect all handles waiting for this specific page
                for (auto h : entry.waiters) {
                    tasks_to_resume.push_back(h);
                }
                pending_io.erase(ctx->page_id);
            } 
            // 2. Handle WRITE Operations (Flushing/Barriers)
            else if (ctx->batch) {
                auto& batch = ctx->batch;
                if (batch->counter.fetch_sub(1) == 1) {
                    bool expected = false;
                    if (batch->resumed.compare_exchange_strong(expected, true)) {
                        tasks_to_resume.push_back(batch->handle);
                    }
                }
            }
            
            delete ctx; 
        }
        io_uring_cqe_seen(&ring, cqe);
    }

    // 3. Resume the collected tasks outside the CQE loop
    for (auto h : tasks_to_resume) {
        h.resume();
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

// void Pager::schedule_async_load(uint32_t page_id, std::coroutine_handle<> h) {
//     // 1. Double-Check Cache (Safety first)
//     if (page_cache.find(page_id) != page_cache.end()) {
//         std::cout << "DEBUG: Page " << page_id << " landed in cache during suspension." << std::endl;
//         h.resume(); 
//         return;
//     }

//     auto page = std::make_shared<Page>();
//     pending_io[page_id] = { OpType::READ, {h}, page };

//     // Create a context wrapper for the Read operation
//     // We use the single-handle constructor for IOContext
//     IOContext* ctx = new IOContext(h, page_id, nullptr);

//     struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
//     io_uring_prep_read(sqe, fd, page->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    
//     // Pass the pointer, not the ID
//     io_uring_sqe_set_data(sqe, ctx); 
//     io_uring_submit(&ring);
// }


void Pager::schedule_async_load(uint32_t page_id, std::coroutine_handle<> h) {
    if (page_cache.find(page_id) != page_cache.end()) {
        std::cout << "DEBUG: Page " << page_id << " landed in cache during suspension." << std::endl;
        h.resume();
        return;
    }

    // 2. Allocate the page frame from your Hugepage pool
    if (!mem_pool) {
        throw std::runtime_error("Hugepage pool (mem_pool) was not initialized!");
    }

    Page* raw_page = mem_pool->allocate();

    // Wrap in shared_ptr with your custom deleter to recycle memory automatically on release
    std::shared_ptr<Page> page(raw_page, [this](Page* p) {
        if (this->mem_pool) {
            this->mem_pool->deallocate(p);
        } else {
            p->~Page();
        }
    });

    // 3. Register the IORequest in pending_io map
    pending_io[page_id] = { OpType::READ, {h}, page };

    // 4. Handle Mmeory-Only Mode vs Disk I/O
    if (memory_only_) {
        // Clear buffer memory instantly
        std::memset(page->data, 0, PAGE_SIZE);

        // In memory-only mode, simulate immediate completion by pushing to the resume queue
        memory_pending_resumes_.push_back(h);
    } else {
        // Disk Mode: Prepare an IOContext wrapper for the async read completion
        IOContext* ctx = new IOContext(h, page_id, nullptr);

        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);

        if (!sqe) {
            throw std::runtime_error("Failed to acquire io_uring submission queue entry (SQE)!");
        }

        // Setup async pread from file descriptor directly int our fast Hugepage memory
        // Setup async pread from file descriptor directly into our fast Hugepage memory buffer
        io_uring_prep_read(sqe, fd, page->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
        
        // Pass the allocated context pointer as user data to matching completions
        io_uring_sqe_set_data(sqe, ctx); 
        io_uring_submit(&ring);
    }
}


// --- Synchronous Fallbacks & Metadata ---

void Pager::write_page(uint32_t page_id, const Page& page) {
    std::cout << "Writing page: " << page_id << std::endl;
    off_t offset = (off_t)page_id * PAGE_SIZE;
    if (pwrite(this->fd, page.data, PAGE_SIZE, offset) == -1) {
        throw std::runtime_error("Synchronous write failed");
    }
    
    if (offset + PAGE_SIZE > file_length) {
        file_length = offset + PAGE_SIZE;
        num_pages = file_length / PAGE_SIZE;
    }

}



// std::shared_ptr<Page> Pager::read_page(uint32_t page_id) {
//     // 1. Create the page
//     auto page = std::make_shared<Page>();
    
//     // 2. Read directly into the shared memory
//     ssize_t bytes = pread(this->fd, page->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    
//     if (bytes < 0) throw std::runtime_error("Sync read failed");
//     if (bytes < PAGE_SIZE) std::memset(page->data + bytes, 0, PAGE_SIZE - bytes);

//     // 3. Store in cache
//     page_cache[page_id] = page;

//     // 4. Return the SHARED pointer. No copying!
//     return page;
// }

std::shared_ptr<Page> Pager::read_page(uint32_t page_id) {
    if (!mem_pool) {
        throw std::runtime_error("The mem_pool was not Initialized!");
    }


    Page* raw_page = mem_pool->allocate();

    std::shared_ptr<Page> page(raw_page, [this](Page* p) {
        if (this->mem_pool) {
            this->mem_pool->deallocate(p);
        } else {
            p->~Page();
        }
    });
    
    // Read directly from disk
    ssize_t bytes = pread(this->fd, page->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);

    if (bytes < 0) throw std::runtime_error("Sync read failed");
    if (bytes < PAGE_SIZE) std::memset(page->data + bytes, 0, PAGE_SIZE - bytes);

    // Store in cache
    page_cache[page_id] = page;

    return page;

}

// --- Awaiter Factories ---

FlushAwaiter Pager::flush_page_async(uint32_t page_id) {
    return FlushAwaiter{this, page_id};
}

PageAwaiter Pager::get_page_async(uint32_t page_id) {
    std::cout << "DEBUG: Requesting page " << page_id << std::endl;
    return PageAwaiter{this, page_id};
}

// --- Bitmap & Cache Helpers ---

Page* Pager::get_page(uint32_t page_id) {
    auto it = page_cache.find(page_id);
    if (it != page_cache.end()) {
        return it->second.get();
    }

    // This now returns the shared_ptr, and we just grab the raw pointer for the Node
    return read_page(page_id).get(); 
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

// uint32_t Pager::get_unused_page_number() {
//     uint32_t new_id = num_pages;
//     num_pages++;

//     // 1. Physically stretch the file on disk
//     if (this->fd != -1) {
//         if (ftruncate(this->fd, (off_t)num_pages * PAGE_SIZE) == -1) {
//             throw std::runtime_error("CRITICAL: Failed to extend database file! Check disk space.");
//         }
//     }

//     // 2. SHORTCUT: Put a blank page in the cache immediately
//     auto blank_page = std::make_shared<Page>();
//     std::memset(blank_page->data, 0, PAGE_SIZE);
    
//     // By putting it in the cache now, get_page_async will find it 
//     // in memory and won't trigger an io_uring disk read.
//     page_cache[new_id] = blank_page;

//     std::cout << "DEBUG: Allocated New Page " << new_id << " (Skipping Disk Read)" << std::endl;
//     return new_id;
// }


uint32_t Pager::get_unused_page_number() {
    uint32_t new_id = num_pages;
    num_pages++;

    // 1. Physically stretch the file on disk
    if (this->fd != -1) {
        if (ftruncate(this->fd, (off_t)num_pages * PAGE_SIZE) == -1) {
            throw std::runtime_error("CRITICAL: Failed to extend database file! Check disk space.");
        }
    }

    if (!mem_pool) {
        throw std::runtime_error("The Hugepage pool (mem_pool) was not initialised!");
    }

    Page* raw_page = mem_pool->allocate();

    std::shared_ptr<Page> page(raw_page, [this](Page* p) {
        if (mem_pool) {
            this->mem_pool->deallocate(p);
        } else {
            p->~Page();
        }
    });

    std::memset(page->data, 0, PAGE_SIZE);

    page_cache[new_id] = page;

    std::cout << "DEBUG: Allocated New Page " << new_id << " (Skipping Disk Read)" << std::endl;

    return new_id;

}


uint32_t Pager::get_num_pages() { return num_pages; }
int Pager::get_fd() { return fd; }


void Pager::submit_to_kernel() {
    io_uring_submit(&ring);
}



void Pager::submit_write(uint32_t page_id, 
                         std::shared_ptr<BatchContext> batch) { 
    
    off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;

    // Snapshot the data to avoid race conditions if the page is modified during IO
    auto buffer_snapshot = std::make_unique<char[]>(PAGE_SIZE);
    std::memcpy(buffer_snapshot.get(), page_cache[page_id]->data, PAGE_SIZE);

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) return; // Always check for ring fullness

    io_uring_prep_write(sqe, fd, buffer_snapshot.get(), PAGE_SIZE, offset);
    
    // Now the arguments match: (shared_ptr<BatchContext>, uint32_t, unique_ptr<char[]>)
    IOContext* ctx = new IOContext(batch, page_id, std::move(buffer_snapshot));
    
    io_uring_sqe_set_data(sqe, ctx);
}


bool Pager::can_perform_op(uint32_t page_id, OpType requested_op) {
    auto it = pending_io.find(page_id);

    if (it == pending_io.end()) return true; // Nothing pending, proceed

    // If there is an existing operation
    if (requested_op == OpType::READ) {
        // Can read if the existing op is also a READ
        return it->second.operation == OpType::READ;
    }

    // Writes require total exclusivity
    return false;
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
// void Pager::async_load_from_disk(uint32_t page_id) {
//     if (page_cache.find(page_id) == page_cache.end()) {
//         page_cache[page_id] = std::make_shared<Page>();
//     }

//     struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
//     if (!sqe) return; 

//     io_uring_prep_read(sqe, fd, page_cache[page_id]->data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
//     io_uring_sqe_set_data(sqe, (void*)(uintptr_t)page_id);
//     io_uring_submit(&ring);
// }


// 2. Implementation for async_load_from_disk()
void Pager::async_load_from_disk(uint32_t page_id) {

    if (page_cache.find(page_id) == page_cache.end()) {
        if (!mem_pool) {
            throw std::runtime_error("The Hugepage pool (mem_pool) was not initialized!");
        }

        Page* raw_page = mem_pool->allocate();

        std::shared_ptr<Page> page(raw_page, [this](Page* p) {
            if (mem_pool) {
                this->mem_pool->deallocate(p);
            } else {
                p->~Page();
            }
        });

        page_cache[page_id] = page;
    }
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;

    io_uring_prep_read(sqe, fd, page_cache[page_id]->data, PAGE_SIZE, (off_t) page_id * PAGE_SIZE);
    io_uring_sqe_set_data(sqe, (void*)(uintptr_t)page_id);
    io_uring_submit(&ring);
    
}


MultiFlushAwaiter Pager::flush_all_dirty_async() {
    // If nothing to flush, the awaiter handles immediate resumption
    if (dirty_pages.empty()) {
        return MultiFlushAwaiter{this, {}, nullptr, true}; 
    }

    // Capture the current dirty set
    std::vector<uint32_t> ids(dirty_pages.begin(), dirty_pages.end());
    dirty_pages.clear();

    // We pass the RAW size (or a shared_ptr to it) to the awaiter.
    // The Awaiter will be responsible for putting this into the 
    // BatchContext tether once it has the coroutine handle 'h'.
    auto counter = std::make_shared<std::atomic<size_t>>(ids.size());
    
    return MultiFlushAwaiter{this, std::move(ids), counter};
}