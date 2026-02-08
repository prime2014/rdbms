#include "page.hpp"
#include "pager.hpp"
#include <cstdint>


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


struct SplitResult {
    uint32_t split_key;
    uint32_t new_page_id;
};


class Node {
    protected:
        Page *page;
        uint32_t page_id;
    
        public:
            Node(Page* p, uint32_t id): page(p), page_id(id) {};

            uint32_t get_page_id() const {
                return page_id;
            };

            void set_node_type(uint8_t type) { page->data[NODE_TYPE_OFFSET] = type; }
            uint8_t get_node_type() { return page->data[NODE_TYPE_OFFSET]; }

            void set_is_root(uint8_t is_root) {
                page->data[IS_ROOT_OFFSET] = is_root;
            }

            void set_key_count(uint32_t count) {
                serialize_uint32(count, page->data + KEY_COUNT_OFFSET);
            }

            uint32_t get_key_count() {
                return deserialize_uint32(page->data + KEY_COUNT_OFFSET);
            }

};
