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
    
    public:
        Pager(const std::string& filename);
        ~Pager();

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

