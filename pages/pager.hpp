#ifndef PAGER_HPP
#define PAGER_HPP

#include <fstream>
#include <string>
#include <memory>
#include "page.hpp"
#include <cstdint>
#include <cstring>



class Pager {
    private:
        std::fstream file_stream;
        uint32_t file_length;
        uint32_t num_pages;

        // The "Registry"
        std::shared_ptr<Page> root_cache;
        std::shared_ptr<Page> last_page_pin;
        uint32_t last_page_id = 0xFFFFFFFF;
    
    public:
        Pager(const std::string& filename);
        ~Pager();

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

