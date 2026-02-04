#ifndef PAGER_HPP
#define PAGER_HPP

#include <fstream>
#include <string>
#include <memory>
#include "page.hpp"



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


#endif