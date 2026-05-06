#ifndef NODE_HPP
#define NODE_HPP

#include "page.hpp"
#include <cstdint>
#include "page_header.hpp"
#include <memory>


class Pager;

// --- TABLE LEVEL METADATA (Special 4-byte slot only used in Page 0) ---
const uint32_t TABLE_TOTAL_COUNT_OFFSET = 4;  // Bytes 4, 5, 6, 7

// --- SHARED HEADER (Starts after the Table Count) ---
const uint32_t NODE_TYPE_OFFSET = 0;          // Byte 0
const uint32_t IS_ROOT_OFFSET = 1;            // Byte 1
// (Bytes 2-3 are padding/unused)

const uint32_t PARENT_POINTER_OFFSET = 8;     // Bytes 8, 9, 10, 11
const uint32_t KEY_COUNT_OFFSET = 12;         // Bytes 12, 13, 14, 15

// --- ROLE-SPECIFIC "EXIT" POINTER (The Switch) ---
const uint32_t RIGHT_CHILD_OFFSET = 16;       // Bytes 16, 17, 18, 19 (Internal)
const uint32_t NEXT_PAGE_OFFSET = 16;         // Bytes 16, 17, 18, 19 (Leaf)

// --- DATA START ---
const uint32_t COMMON_HEADER_SIZE = 20;       // Total header length
const uint32_t INTERNAL_NODE_CELLS_START = 20;
const uint32_t LEAF_NODE_CELLS_START = 20;

const uint32_t LEAF_NODE_CELL_SIZE = 36;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - sizeof(PageHeader);
const uint32_t LEAF_NODE_MAX_CELLS = LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;


const uint32_t INTERNAL_NODE_MAX_CELLS = (PAGE_SIZE - sizeof(PageHeader)) / 8;

struct SplitResult {
    uint32_t split_key;
    uint32_t new_page_id;
};

enum NodeType {
    NODE_INVALID = 0,      // important: 0 = invalid/uninitialized
    NODE_INTERNAL = 1,
    NODE_LEAF = 2,
    NODE_META = 3
};

class Node {
    protected:
        std::shared_ptr<Page> page; // Ownership!
        Pager* pager;
        uint32_t page_id;
    
        public:
            Node(std::shared_ptr<Page> p, uint32_t id, Pager* pg = nullptr): page(p), pager(pg), page_id(id) {};

            std::shared_ptr<Page> get_page() { return page; }

            uint32_t get_page_id() const;

            void set_parent(uint32_t parent_id);

            uint32_t get_parent();

            void set_is_root(bool is_root);

            void set_node_type(uint8_t type);

            void set_key_count(uint32_t count);

            uint32_t get_key_count();

            uint8_t get_node_type();

            void set_sibling(uint32_t sibling_id);

            void set_left_child(uint32_t left_child_id);

            void validate_node();

            void init_new_node(uint8_t type, bool is_root);

};


#endif