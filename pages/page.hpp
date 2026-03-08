#ifndef PAGE_HPP
#define PAGE_HPP

#include <vector>
#include <cstdint>


const uint32_t PAGE_SIZE = 4096;


struct alignas(PAGE_SIZE) Page {
    char data[PAGE_SIZE];
};


#endif