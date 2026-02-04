#include "../pages/pager.hpp"
#include <iostream>
#include <cstring>


Pager::Pager(const std::string& filename) {
    file_stream.open(filename, std::ios::in | std::ios::out | std::ios::binary);

    if (!file_stream.is_open()) {
        // if file does not exist, create it by opening in "out" mode only first
        file_stream.open(filename, std::ios::out | std::ios::binary);
        file_stream.close();

        // Re-open with full permissions
        file_stream.open(filename, std::ios::in | std::ios::out | std::ios::binary);
    }


    // Determine file length
    file_stream.seekg(0, std::ios::end);
    file_length = file_stream.tellg();
    file_stream.clear();
    file_stream.seekg(0, std::ios::beg);


    // check if the file is "corrupt" (not a multiple of 4KB)
    if (file_length % PAGE_SIZE != 0) {
        std::cerr << "Warning: DB file is not a multiple pf PAGE_SIZE!" << std::endl;
    };

    std::cout << "Opened " << filename << " with " << (file_length / PAGE_SIZE) << " pages " << std::endl;
}

Pager::~Pager() {
    if (file_stream.is_open()) {
        file_stream.close();
    }
}


std::unique_ptr<Page> Pager::read_page(uint32_t page_id) {
    auto page = std::make_unique<Page>();
    uint32_t offset = page_id * PAGE_SIZE;

    if (offset < file_length) {
        // Scenario 1: Page is on disk
        file_stream.seekg(offset, std::ios::beg);
        file_stream.read(page->data, PAGE_SIZE);

        if (file_stream.gcount() != PAGE_SIZE) {
            std::cerr << "Error: Short read from file at page " << page_id << std::endl;
        }
    } else {
        // Scenario 2: Page Fault (Requetsed a page we haven't written yet)
        // We initialize the page with zeros (empty page)
        std::memset(page->data, 0, PAGE_SIZE);
        std::cout << "Page Fault: Initialized new page " << page_id << " in memory." << std::endl;
    }

    return page;

}


void Pager::write_page(uint32_t page_id, const Page& page) {
    uint32_t offset = page_id * PAGE_SIZE;

    file_stream.seekp(offset, std::ios::beg);
    file_stream.write(page.data, PAGE_SIZE);
    file_stream.flush();

    uint32_t current_end = offset + PAGE_SIZE;

    if (current_end > file_length) {
        file_length = current_end;
    }
}