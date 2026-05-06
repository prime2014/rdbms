#ifndef LEAFCELL_HPP
#define LEAFCELL_HPP

#include <cstdint>
#include <memory>

class Page;

#pragma pack(push, 1)
struct LeafCell {
    uint32_t key;
    char value[32];
};
#pragma pack(pop)

#endif // LEAFCELL_HPP