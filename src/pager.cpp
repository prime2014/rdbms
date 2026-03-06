#include "../pages/pager.hpp"
#include <iostream>
#include <cstring>
#include <set>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <coroutine>
#include <map>

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

    // Initialize  page count
    this->num_pages = file_length / PAGE_SIZE;

    // check if the file is "corrupt" (not a multiple of 4KB)
    if (file_length % PAGE_SIZE != 0) {
        std::cerr << "Warning: DB file is not a multiple pf PAGE_SIZE!" << std::endl;
    };

    std::cout << "Opened " << filename << " with " << (file_length / PAGE_SIZE) << " pages " << std::endl;
}

uint32_t Pager::get_unused_page_number() {
    return num_pages;
}

Pager::~Pager() {
    if (file_stream.is_open()) {
        file_stream.close();
    }
}


std::unique_ptr<Page> Pager::read_page(uint32_t page_id) {
    auto page = std::make_unique<Page>();
    uint32_t offset = page_id * PAGE_SIZE;

    // Always clear flags (EOF/Fail) before a new operation
    file_stream.clear();

    if (offset < file_length) {
        file_stream.seekg(offset, std::ios::beg);
        file_stream.read(page->data, PAGE_SIZE);

        std::streamsize bytes_read = file_stream.gcount();
        if (bytes_read < PAGE_SIZE) {
            // Fill the rest of the buffer with zeros to prevent garbage data
            std::memset(page->data + bytes_read, 0, PAGE_SIZE - bytes_read);
        }
    } else {
        // Scenario 2: New page initialization
        std::memset(page->data, 0, PAGE_SIZE);

        if (page_id >= num_pages) {
            num_pages = page_id + 1;
        }
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

        num_pages = file_length / PAGE_SIZE;
    }
}



PageAwaiter Pager::flush_page_async(uint32_t page_id) {
    mark_as_dirty(page_id);

    return PageAwaiter{this, page_id};
}


void Pager::mark_as_dirty(uint32_t page_id) {
    
    // Find which byte holds this page's bit
    uint32_t byte_index = page_id / 8;

    // Find which bit inside that byte
    uint8_t bit_index = page_id % 8;

    // Ensure the bitmap is large enough
    if (byte_index >= dirty_bitmap.size()) {
        dirty_bitmap.resize(byte_index + 1, 0);   
    }

    // Set the bit to 1 using bitwise OR
    dirty_bitmap[byte_index] |= (1 << bit_index);
}


bool Pager::is_dirty(uint32_t page_id) const {
    uint32_t byte_index = page_id / 8;
    uint8_t bit_index = page_id % 8;
    if (byte_index >= dirty_bitmap.size()) return false;
    
    return (dirty_bitmap[byte_index] & (1 << bit_index)) != 0;
}


void Pager::clear_dirty(uint32_t page_id) {
    uint32_t byte_index = page_id / 8;
    uint8_t bit_index = page_id % 8;
    
    // Set the bit back to 0 using a bitwise AND with a NOT mask
    dirty_bitmap[byte_index] &= ~(1 << bit_index);
}


void Pager::schedule_write(uint32_t page_id, std::coroutine_handle<> h) {
    // 1. Ensure the page is actually in cache before we try to write it
    if (!is_in_memory(page_id)) {
        throw std::runtime_error("Cannot schedule write for page not in memory");
    }

    
    pending_io[page_id] = h;

    // 3. Mark it as dirty if not already (safety check)
    mark_as_dirty(page_id);

    // Note: We do NOT resume 'h' here. 
    // 'h' will stay frozen until the disk (or TCL) finishes the write.
}


void Pager::process_pending_writes() {
    if (pending_io.empty()) return;

    // Use a map to ensure we process pages in file-offset order
    std::map<uint32_t, std::coroutine_handle<>> sorted_io(pending_io.begin(), pending_io.end());
    pending_io.clear();

    auto it = sorted_io.begin();
    while (it != sorted_io.end()) {
        std::vector<struct iovec> clump_iov;
        std::vector<std::coroutine_handle<>> clump_handles;
        
        uint32_t start_id = it->first;
        uint32_t next_expected = start_id;

        // GATHER: Build a clump of contiguous pages
        while (it != sorted_io.end() && it->first == next_expected) {
            clump_iov.push_back({ page_cache[it->first]->data, (size_t)PAGE_SIZE });
            clump_handles.push_back(it->second);
            
            clear_dirty(it->first);
            next_expected++;
            it++;
        }

        // GATHER: Write this specific contiguous clump
        if (!clump_iov.empty()) {
            off_t offset = (off_t)start_id * PAGE_SIZE;
            pwritev(this->fd, clump_iov.data(), clump_iov.size(), offset);
        }

        // RESUME: Wake up the coroutines for this clump
        for (auto h : clump_handles) {
            if (h && !h.done()) h.resume();
        }
    }
}