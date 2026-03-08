#include "../pages/node.hpp"
#include "../pages/pager.hpp"

uint32_t Node::get_page_id() const {
    return page_id;
};

void Node::set_parent(uint32_t parent_id) {
    serialize_uint32(parent_id, page->data + PARENT_POINTER_OFFSET);
}

uint32_t Node::get_parent() {
    return deserialize_uint32(page->data + PARENT_POINTER_OFFSET);
}

// Every node needs to know if it's the root
void Node::set_is_root(bool is_root) {
    uint8_t value = is_root ? 1 : 0;
    *(page->data + IS_ROOT_OFFSET) = value;
}

void Node::set_node_type(uint8_t type) { page->data[NODE_TYPE_OFFSET] = type; }
uint8_t Node::get_node_type() { return page->data[NODE_TYPE_OFFSET]; }

            

void Node::set_key_count(uint32_t count) {
    serialize_uint32(count, page->data + KEY_COUNT_OFFSET);
}

uint32_t Node::get_key_count() {
    return deserialize_uint32(page->data + KEY_COUNT_OFFSET);
}
