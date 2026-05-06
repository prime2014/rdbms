#ifndef PAGER_HPP
#define PAGER_HPP

#include <fstream>
#include <string>
#include <memory>
#include "page.hpp"
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include "task.hpp"
#include <vector>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <liburing.h>
#include <iostream>
#include <map>
#include <set>


enum class OpType { READ, WRITE };

struct IORequest {
    OpType operation;
    std::vector<std::coroutine_handle<>> waiters;
    std::shared_ptr<Page> page_buffer;
    std::shared_ptr<std::atomic<size_t>> batch_counter = nullptr;
};


struct BatchContext;


class Pager {
    private:
        int fd;
        std::fstream file_stream;
        uint32_t file_length;
        uint32_t num_pages;
        struct io_uring ring;

        // The "Registry"
        std::shared_ptr<Page> root_cache;
        std::shared_ptr<Page> last_page_pin;
        uint32_t last_page_id = 0xFFFFFFFF; 

        std::map<uint32_t, std::shared_ptr<Page>> page_cache;
        std::map<uint32_t, IORequest> pending_io;
        std::set<uint32_t> dirty_pages;
        const size_t BATCH_THRESHOLD = 16;

        // Memory-only test mode: no file, no io_uring; completions are simulated
        bool memory_only_ = false;
        std::vector<std::coroutine_handle<>> memory_pending_resumes_;

    public:
        std::vector<uint8_t> dirty_bitmap;
        Pager(const std::string& filename, bool memory_only = false);
        ~Pager();

        void submit_to_kernel();

        void pin_root(uint32_t root_id, std::shared_ptr<Page> existing_page);
        Page* get_root() { 
            if (root_cache) return root_cache.get();

            // 2. Fallback: If not, try to look up the root_id in the map
            // You need to know what the current root_id is!
            return nullptr;
        }
        

        int get_fd();

        void submit_all();
        // Inside Pager class
        bool is_ring_idle();

        std::shared_ptr<Page> get_page_from_cache(uint32_t page_id);

        Page* get_page(uint32_t page_id);
        void mark_dirty(uint32_t page_id);

        void schedule_write(uint32_t page_id, std::coroutine_handle<> h);

        void submit_write(uint32_t page_id, std::shared_ptr<BatchContext> batch);

        void shutdown_gracefully();

        std::shared_ptr<Page> get_root_shared();

        std::shared_ptr<Page> get_page_shared(uint32_t page_id);

        bool can_perform_op(uint32_t page_id, OpType requested_op);

        void process_completions(bool wait);

        MultiFlushAwaiter flush_all_dirty_async();

        void mark_as_dirty(uint32_t page_id);

        FlushAwaiter flush_page_async(uint32_t page_id);

        bool is_dirty(uint32_t page_id) const;

        void clear_dirty(uint32_t page_id);

        PageAwaiter get_page_async(uint32_t page_id);

        void schedule_async_load(uint32_t page_id, std::coroutine_handle<> h);

        bool is_in_memory(uint32_t page_id) const;

        void* get_page_ptr(uint32_t page_id);
        
        
        void async_load_from_disk(uint32_t start_page_id);

        void pin_last_page(uint32_t page_id, std::shared_ptr<Page> page);

        uint32_t get_num_pages();

        uint32_t get_unused_page_number();
        // Returns a pointer to a page loaded from disk
        std::shared_ptr<Page> read_page(uint32_t page_id);

        // Writes a page from RAM back to the specific slot on disk
        void write_page(uint32_t page_id, const Page& page);

        friend struct MultiFlushAwaiter;
        friend struct PageAwaiter;
};


// Writes a 32-bit integer into a specific spot in the page
inline void serialize_uint32(uint32_t value, char* destination) {
    std::memcpy(destination, &value, sizeof(uint32_t));
}

// Reads a 32-bit integer from a specific spot in the page
inline uint32_t deserialize_uint32(char* source) {
    uint32_t value;
    std::memcpy(&value, source, sizeof(uint32_t));
    return value;
}

inline void serialize_string_32(const char* source, char* destination) {
    std::memset(destination, 0, 32);
    std::strncpy(destination, source, 31);

    // ADD THIS:
    if (std::strstr(destination, "Progress") != nullptr) {
        std::cerr << "CRITICAL: Corrupted data detected before write!" << std::endl;
        std::exit(1); 
    }
}

#endif

