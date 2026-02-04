#pragma once
#include <vector>
#include <cstdint>


const uint32_t PAGE_SIZE = 4096;


struct Page {
    char data[PAGE_SIZE];
};