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
#include "task.hpp"


class Pager {
    private:
        int fd;
        std::fstream file_stream;
        uint32_t file_length;
        uint32_t num_pages;

        // The "Registry"
        std::shared_ptr<Page> root_cache;
        std::shared_ptr<Page> last_page_pin;
        uint32_t last_page_id = 0xFFFFFFFF;
        std::unordered_map<uint32_t, std::shared_ptr<Page>> page_cache;
        std::unordered_map<uint32_t, std::coroutine_handle<>> pending_io;    
        const size_t BATCH_THRESHOLD = 16;

    public:
        std::vector<uint8_t> dirty_bitmap;
        Pager(const std::string& filename);
        ~Pager();

        // Inside Pager class



        void schedule_write(uint32_t page_id, std::coroutine_handle<> h);

        void process_pending_writes();

        std::shared_ptr<Page> get_page_from_cache(uint32_t page_id) {
            auto it = page_cache.find(page_id);

            if (it != page_cache.end()) {
                return it->second;
            };

            throw std::runtime_error("Page not in cache! Did you co_await get_page_async?");
        }

        void mark_as_dirty(uint32_t page_id);

        PageAwaiter flush_page_async(uint32_t page_id);

        bool is_dirty(uint32_t page_id) const;

        void clear_dirty(uint32_t page_id);

        PageAwaiter get_page_async(uint32_t page_id) {
            return PageAwaiter{this, page_id};
        }

        void schedule_async_load(uint32_t page_id, std::coroutine_handle<> h) {
            pending_io[page_id] = h;
        }

        // This is called by your Event Loop when the disk finishes
        void on_page_ready(uint32_t page_id) {
            if(pending_io.contains(page_id)) {
                auto h = pending_io[page_id];
                pending_io.erase(page_id);
                h.resume(); // This jumps back into the B+ Tree logic
            }
        }
        
        bool is_in_memory(uint32_t page_id) const {
            return page_cache.find(page_id) != page_cache.end();
        }


        void* get_page_ptr(uint32_t page_id) {
            if (is_in_memory(page_id)) {
                return page_cache[page_id]->data;
            }
            return nullptr;
        }

        std::shared_ptr<Page> get_page(uint32_t page_id) {
            // 1. Check Root Cache
            if (page_id == 0 && root_cache) return root_cache;

            // 2. Check Pinned Last page
            if (page_id == last_page_id && last_page_pin) return last_page_pin;

            // 3. Physical read
            auto page = std::make_shared<Page>();
            uint32_t offset = page_id * PAGE_SIZE;

            if (offset < file_length) {
                file_stream.seekg(offset, std::ios::beg);
                file_stream.read(page->data, PAGE_SIZE);
            } else {
                std::memset(page->data, 0, PAGE_SIZE);
            }

            // Logic for auto-caching
            if (page_id == 0) root_cache = page;

            return page;
        }
        
        void async_load_from_disk(uint32_t start_page_id);

        // Explicitly pin the last page
        void pin_last_page(uint32_t page_id, std::shared_ptr<Page> page) {
            last_page_pin = page;
            last_page_id = page_id;
        }

        uint32_t get_num_pages() const { return num_pages; }

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

