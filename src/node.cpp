#include "../pages/node.hpp"
#include "../pages/pager.hpp"
#include "../pages/page_header.hpp"

uint32_t Node::get_page_id() const {
    return page_id;
};

void Node::set_parent(uint32_t parent_id) {
    validate_node();
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    header->parent_id = parent_id;
}

uint32_t Node::get_parent() {
    validate_node();
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    return header->parent_id;
}

// Every node needs to know if it's the root
void Node::set_is_root(bool is_root) {
    validate_node();
    uint8_t value = is_root ? 1 : 0;
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    header->is_root = value;
}

void Node::init_new_node(uint8_t type, bool is_root) {
    // Access the header DIRECTLY. No validate_node() calls here.
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    
    header->node_type = type;
    header->is_root = is_root ? 1 : 0;
    header->parent_id = 0;
    header->key_count = 0;

    // Set the specific magic number
    switch (type) {
        case NODE_LEAF:     header->magic = MAGIC_LEAF;     break;
        case NODE_INTERNAL: header->magic = MAGIC_INTERNAL; break;
        case NODE_META:     header->magic = MAGIC_META;     break;
        default:            header->magic = 0;              break;
    }
}

void Node::set_node_type(uint8_t type) { 
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    header->node_type = type;

    // Map logic type to verification logic
    switch (type) {
        case NODE_LEAF: header->magic = MAGIC_LEAF;         break;
        case NODE_INTERNAL: header->magic = MAGIC_INTERNAL; break;
        case NODE_META: header->magic = MAGIC_META;         break;
        default: header->magic = 0;                         break;
    }
}


uint8_t Node::get_node_type() { 
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    return header->node_type;
}

void Node::validate_node() {
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    
    uint32_t expected_magic;
    uint8_t current_type = header->node_type;

    if (current_type == NODE_LEAF) expected_magic = MAGIC_LEAF;
    else if (current_type == NODE_INTERNAL) expected_magic = MAGIC_INTERNAL;
    else if (current_type == NODE_META) expected_magic = MAGIC_META;
    else {
        std::cerr << "CRITICAL: Node on page " << page_id << " has unknown type: " 
                  << (int)current_type << std::endl;
        std::abort();
    }

    if (header->magic != expected_magic) {
        std::cerr << "CRITICAL: Magic Number Mismatch on Page " << page_id << "!" << std::endl;
        std::cerr << "Expected: " << std::hex << expected_magic 
                  << " Found: " << std::hex << header->magic << std::endl;
        std::cerr << "This usually means a pointer was invalidated or memory was reused incorrectly." << std::endl;
        std::abort();
    }
}

            
void Node::set_key_count(uint32_t count) {
    validate_node();
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    header->key_count = count;

}

uint32_t Node::get_key_count() {
    validate_node();
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    return header->key_count;
}

void Node::set_sibling(uint32_t sibling_id) {
    validate_node();
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    header->pointer.sibling_id = sibling_id;
}


void Node::set_left_child(uint32_t left_child_id) {
    validate_node();
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    header->pointer.left_child_id = left_child_id;
}