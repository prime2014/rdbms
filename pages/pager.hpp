#ifndef PAGER_HPP
#define PAGER_HPP

#include <fstream>
#include <string>
#include <memory>
#include "page.hpp"
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include "task.hpp"
#include <vector>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <liburing.h>

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
        std::unordered_map<uint32_t, std::shared_ptr<Page>> page_cache;
        std::unordered_map<uint32_t, std::coroutine_handle<>> pending_io;    
        const size_t BATCH_THRESHOLD = 16;

        // Memory-only test mode: no file, no io_uring; completions are simulated
        bool memory_only_ = false;
        std::vector<std::coroutine_handle<>> memory_pending_resumes_;

    public:
        std::vector<uint8_t> dirty_bitmap;
        Pager(const std::string& filename, bool memory_only = false);
        ~Pager();

        int get_fd();

        void submit_all();
        // Inside Pager class
        bool is_ring_idle();

        std::shared_ptr<Page> get_page_from_cache(uint32_t page_id);


        void schedule_write(uint32_t page_id, std::coroutine_handle<> h);


        void submit_write(uint32_t page_id, std::coroutine_handle<> h);

        void shutdown_gracefully();

        void process_completions();


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
        std::unique_ptr<Page> read_page(uint32_t page_id);

        // Writes a page from RAM back to the specific slot on disk
        void write_page(uint32_t page_id, const Page& page);
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

#endif

