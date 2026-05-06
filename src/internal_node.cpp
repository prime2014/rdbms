#include "../pages/internal_node.hpp"
#include "../pages/pager.hpp"
#include "../pages/task.hpp"
#include "../pages/page_header.hpp"
#include "./internalcell.cpp"
#include "../pages/internal-page-layout.hpp"


SplitResult InternalNode::perform_internal_split_logic(uint32_t incoming_key, uint32_t incoming_child_id, InternalNode& sibling, Pager& pager) {
    auto* left_layout = reinterpret_cast<InternalPageLayout*>(this->page->data);
    auto* right_layout = reinterpret_cast<InternalPageLayout*>(sibling.get_page()->data);
    
    uint32_t key_count = left_layout->header.key_count;
    uint32_t total_keys = key_count + 1;
    uint32_t midpoint = total_keys / 2; 

    // 1. Virtual buffer of STRUCTS
    // This allows us to sort/arrange all cells before splitting them back to pages
    std::vector<InnerCell> virtual_cells;
    virtual_cells.reserve(total_keys);

    // 2. Find insertion point and load the buffer
    uint32_t insertion_idx = 0;
    while (insertion_idx < key_count && left_layout->cells[insertion_idx].key < incoming_key) {
        insertion_idx++;
    }

    for (uint32_t i = 0; i < key_count; ++i) {
        if (i == insertion_idx) {
            virtual_cells.push_back({incoming_key, incoming_child_id});
        }
        virtual_cells.push_back(left_layout->cells[i]);
    }
    if (insertion_idx == key_count) {
        virtual_cells.push_back({incoming_key, incoming_child_id});
    }

    // 3. Identify the "Promoted" Key
    // Internal node splits move the midpoint key UP, while leaf splits COPY it
    InnerCell promoted_item = virtual_cells[midpoint];
    
    SplitResult promotion;
    promotion.split_key = promoted_item.key;
    promotion.new_page_id = sibling.get_page_id();

    // 4. Update Left Node (Current)
    // It keeps keys [0 ... midpoint-1]
    left_layout->header.key_count = midpoint;
    std::memcpy(left_layout->cells, 
                virtual_cells.data(), 
                midpoint * sizeof(InnerCell));

    // 5. Update Right Node (Sibling)
    // Its "Leftmost Child" is the child pointer that was associated with the promoted key
    right_layout->header.pointer.left_child_id = promoted_item.child_id;

    // It gets keys [midpoint + 1 ... end]
    uint32_t right_count = total_keys - midpoint - 1;
    right_layout->header.key_count = right_count;
    if (right_count > 0) {
        std::memcpy(right_layout->cells, 
                    &virtual_cells[midpoint + 1], 
                    right_count * sizeof(InnerCell));
    }

    return promotion;
}


uint32_t InternalNode::get_leftmost_child() { 
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    return header->pointer.left_child_id;
}
    
void InternalNode::set_leftmost_child(uint32_t id) { 
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    header->pointer.left_child_id = id; 
}


uint32_t InternalNode::get_child_for_key(uint32_t key) {
    uint32_t num_keys = get_key_count();
    
    // Binary search for the first key > search_key
    uint32_t low = 0;
    uint32_t high = num_keys;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (get_key(mid) <= key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    // low is now the index of the first key strictly greater than 'key'
    if (low == 0) {
        return get_leftmost_child();
    } else {
        // Return the child to the right of the largest key <= search_key
        return get_child(low - 1);
    }
}


void InternalNode::initialize_as_root(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id) {
    this->set_node_type(NODE_INTERNAL);
    this->set_is_root(1);
    this->set_key_count(1);
    this->set_leftmost_child(left_child_id);
    this->set_child(0, right_child_id);
    this->set_key(0, split_key);
    return;
}

SplitResult InternalNode::split_and_insert(SplitResult result, Pager& pager) {
    uint32_t sibling_id = pager.get_unused_page_number();
    auto sibling_page = pager.read_page(sibling_id);
    InternalNode sibling(sibling_page, sibling_id);

    SplitResult promotion = perform_internal_split_logic(result.split_key, result.new_page_id, sibling, pager);

    pager.write_page(this->page_id, *(this->page));
    pager.write_page(sibling_id, *sibling_page);
    return promotion;
}

SplitTask InternalNode::split_and_insert_internal_async(uint32_t split_key, uint32_t new_child_id, Pager& pager) {
    uint32_t sibling_id = pager.get_unused_page_number();
    std::shared_ptr<Page> sibling_page = co_await pager.get_page_async(sibling_id);
    InternalNode sibling(sibling_page, sibling_id);
    sibling.set_node_type(NODE_INTERNAL);

    SplitResult result = perform_internal_split_logic(split_key, new_child_id, sibling, pager);
    std::vector<uint32_t> modified_pages;    

    uint32_t leftmost = sibling.get_leftmost_child();
    if (leftmost != 0) {
        auto child_page = co_await pager.get_page_async(leftmost);
        Node child(child_page, leftmost);
        child.set_parent(sibling_id);
        pager.mark_dirty(leftmost);
    }

    // Update the rest of the children stored in cells
    for (uint32_t i = 0; i < sibling.get_key_count(); ++i) {
        uint32_t child_id = sibling.get_child(i);
        auto child_page = co_await pager.get_page_async(child_id);
        Node child(child_page, child_id);
        child.set_parent(sibling_id);
        pager.mark_dirty(child_id);
    }

    // 2. TRIGGER ASYNC FLUSHES FOR ALL MODIFIED CHILDREN AT ONCE
    for (uint32_t id : modified_pages) {
        // This submits the SQE to io_uring without waiting yet
        // (Assuming your flush_page_async handles the submission)
        co_await pager.flush_page_async(id); 
    }

    co_await pager.flush_page_async(this->get_page_id());
    co_await pager.flush_page_async(sibling_id);
    co_return result;
}



uint32_t InternalNode::get_child(uint32_t child_idx) {
    char* addr = page->data + sizeof(PageHeader) + (child_idx * 8) + 4;
    return deserialize_uint32(addr);
}

void InternalNode::set_child(uint32_t child_idx, uint32_t child_id) {
    // Offset within InternalCell: Key is first 4 bytes, Child is second 4 bytes
    char* addr = page->data + sizeof(PageHeader) + (child_idx * sizeof(InternalCell)) + offsetof(InternalCell, right_child_id);
    serialize_uint32(child_id, addr);
}

uint32_t InternalNode::get_key(uint32_t key_idx) {
    char* addr = page->data + sizeof(PageHeader) + (key_idx * 8); 
    return deserialize_uint32(addr);
}

void InternalNode::set_key(uint32_t key_idx, uint32_t key) {
    char* addr = page->data + sizeof(PageHeader) + (key_idx * 8);
    serialize_uint32(key, addr);
}

void InternalNode::insert_child(uint32_t split_key, uint32_t new_child_page_id) {
    auto* layout = reinterpret_cast<InternalPageLayout*>(this->page->data);
    uint32_t num_keys = layout->header.key_count;

    // 1. Find insertion index
    uint32_t target_idx = num_keys;
    for (uint32_t i = 0; i < num_keys; i++) {
        if (split_key < layout->cells[i].key) {
            target_idx = i;
            break;
        }
    }

    // 2. Shift cells (InternalCell handles the 8-byte logic automatically)
    if (target_idx < num_keys) {
        std::memmove(&layout->cells[target_idx + 1], 
                     &layout->cells[target_idx], 
                     (num_keys - target_idx) * sizeof(InnerCell));
    }

    // 3. Insert the new key-child pair
    layout->cells[target_idx].key = split_key;
    layout->cells[target_idx].child_id = new_child_page_id;

    // 4. Update header
    layout->header.key_count = num_keys + 1;
}


void InternalNode::set_node_type(NodeType type) {
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    header->node_type = static_cast<uint8_t>(type);
    
    // Automatically set magic based on the type
    if (type == NODE_INTERNAL) {
        header->magic = MAGIC_INTERNAL;
    } else if (type == NODE_LEAF) {
        header->magic = MAGIC_LEAF;
    }
}

void InternalNode::set_parent(uint32_t id) {
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    header->parent_id = id;
}

uint32_t InternalNode::get_parent() {
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    return header->parent_id;
}

bool InternalNode::is_root() const {
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    return header->is_root == 1;
}

// Page* InternalNode::get_page() { return this->page; }


SplitResult InternalNode::split_and_insert_internal(uint32_t incoming_key, uint32_t incoming_child_id, Pager& pager) {
    uint32_t new_sibling_id = pager.get_unused_page_number();
    
    // FIX 1: Get the shared_ptr instead of the raw pointer
    auto new_sibling_handle = pager.get_page_shared(new_sibling_id); 
    InternalNode sibling(new_sibling_handle, new_sibling_id);

    // 1. Initialize Sibling Metadata
    sibling.init_new_node(NODE_INTERNAL, false); // Use the init method we created
    sibling.set_parent(this->get_parent());

    // 2. Delegate logic
    SplitResult promotion = perform_internal_split_logic(incoming_key, incoming_child_id, sibling, pager);

    // 3. CRITICAL: Update Parent Pointers
    uint32_t sibling_anchor = sibling.get_leftmost_child();
    if (sibling_anchor != 0) {
        // FIX 2: Use shared_ptr here too
        auto anchor_handle = pager.get_page_shared(sibling_anchor);
        Node anchor_node(anchor_handle, sibling_anchor);
        anchor_node.set_parent(new_sibling_id);
        pager.mark_dirty(sibling_anchor);
    }

    uint32_t sibling_keys = sibling.get_key_count();
    for (uint32_t i = 0; i < sibling_keys; ++i) {
        uint32_t child_id = sibling.get_child(i);
        if (child_id != 0) {
            // FIX 3: And here
            auto child_handle = pager.get_page_shared(child_id);
            Node child_node(child_handle, child_id);
            child_node.set_parent(new_sibling_id);
            pager.mark_dirty(child_id);
        }
    }

    pager.mark_dirty(this->page_id);
    pager.mark_dirty(new_sibling_id);

    return promotion;
}




uint32_t InternalNode::get_child_at(uint32_t index) {
    if (index >= get_key_count()) {
        // Logic error: index out of bounds
        return 0; 
    }
    
    // Each cell is 8 bytes. 
    // Data layout: [4 bytes Key] [4 bytes ChildID]
    // The child ID is at offset 4 within the cell.
    uint32_t* cell_ptr = reinterpret_cast<uint32_t*>(
        this->page->data + sizeof(PageHeader) + (index * 8) + 4
    );
    
    return *cell_ptr;
}

