#include "page.hpp"
#include "pager.hpp"
#include <cstdint>


// Constants for offsets
const uint32_t NODE_TYPE_OFFSET = 0;
const uint32_t IS_ROOT_OFFSET = 1;
const uint32_t PARENT_POINTER_OFFSET = 4;
const uint32_t KEY_COUNT_OFFSET = 8;
const uint32_t HEADER_SIZE = 12;


class Node {
    protected:
        Page *page;
    
        public:
            Node(Page* p): page(p) {};

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
