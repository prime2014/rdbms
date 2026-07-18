#ifndef PAGE_HPP
#define PAGE_HPP

#include <vector>
#include <cstdint>
#include <sys/mman.h>
#include <stdexcept>
#include <iostream>


const uint32_t PAGE_SIZE = 4096;
const size_t HUGEPAGE_SIZE = 2 * 1024 * 1024;

struct alignas(PAGE_SIZE) Page {
    char data[PAGE_SIZE];
};


// A simple, high-performance Hugepage-backed Arena Allocator
class HugepageAllocator {
    private:
        void* pool_start_;
        size_t pool_size_;
        std::vector<Page*> free_pages_;

    public:
        explicit HugepageAllocator(size_t num_2mb_pages) {
            // Must be a multiple of 2MB
            pool_size_ = num_2mb_pages * HUGEPAGE_SIZE; 

            // Allocate contiguous memory using 2MB Hugepages
            pool_start_ = mmap(NULL, pool_size_,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                               -1, 0);

            if (pool_start_ == MAP_FAILED) {
                std::cerr << "Warning: Hugepage allocation failed (is nr_hugepages set?). Falling back to standard mmap." << std::endl;

                pool_start_ = mmap(NULL, pool_size_, 
                               PROT_READ | PROT_WRITE, 
                               MAP_PRIVATE | MAP_ANONYMOUS, 
                               -1, 0);
                
                if (pool_start_ == MAP_FAILED) {
                    throw std::runtime_error("Failed to allocate memory pool");
                }
            }

            // Slice the pool into 4 KB Page chunks
            size_t total_4kb_pages = pool_size_ / PAGE_SIZE;

            free_pages_.reserve(total_4kb_pages);

            char* current = static_cast<char*>(pool_start_);
            for (size_t i = 0; i < total_4kb_pages; ++i) {
                free_pages_.push_back(reinterpret_cast<Page*>(current));
                current += PAGE_SIZE;
            }
        }


        ~HugepageAllocator() {
            if (pool_start_ && pool_start_ != MAP_FAILED) {
                munmap(pool_start_, pool_size_);
            }
        }

        void* get_pool_start() const { return pool_start_; }
        size_t get_pool_size() const { return pool_size_; }

        // Allocate a 4KB Page from the Hugepage pool
        Page* allocate() {
            if (free_pages_.empty()) {
                throw std::runtime_error("Hugepage pool exhausted! Increase buffer pool size");
            }
            Page* page = free_pages_.back();
            free_pages_.pop_back();
            // Placement new to initialize the page (if it has constructors)
            return new (page) Page();
        }


        // Return the 4KB Page to the pool
        void deallocate(Page* page) {
            page->~Page();
            free_pages_.push_back(page);
        }

        int get_buffer_index(Page* page) const {
            if (page < pool_start_ || reinterpret_cast<char*>(page) >= (static_cast<char*>(pool_start_) + pool_size_)) {
                throw std::runtime_error("Pointer does not belong to this HugepageAllocator pool!");
            }
            
            // Pointer arithmetic: distance in bytes divided by page size
            char* page_ptr = reinterpret_cast<char*>(page);
            char* base_ptr = static_cast<char*>(pool_start_);
            
            return static_cast<int>((page_ptr - base_ptr) / PAGE_SIZE);
        }
};



#endif