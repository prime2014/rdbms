#ifndef PAGE_HEADER_HPP
#define PAGE_HEADER_HPP

#include <cstdint>

struct __attribute__((packed)) PageHeader {
    uint32_t magic;        // Offset 0: The sanity check
    uint8_t node_type;    // 1 byte
    uint8_t is_root;      // 1 byte
    uint16_t reserved;     // 2 bytes
    uint32_t total_count;  // 4 bytes
    uint32_t parent_id;    // 4 bytes
    uint32_t key_count;    // 4 bytes
    union {
        uint32_t sibling_id;
        uint32_t left_child_id;
        uint32_t root_page_id;
    } pointer;             // 4 bytes
};


const uint32_t MAGIC_LEAF = 0x4C454146; // LEAF
const uint32_t MAGIC_INTERNAL = 0x494E4458; // INDX
const uint32_t MAGIC_META = 0x4D455441; // META
const uint32_t MAGIC_FREE = 0x46524545; // "FREE" (for unused pages)




#endif