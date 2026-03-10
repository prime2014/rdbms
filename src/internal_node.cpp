#include "../pages/internal_node.hpp"
#include "../pages/pager.hpp"
#include "../pages/task.hpp"

SplitResult InternalNode::perform_internal_split_logic(uint32_t incoming_key, uint32_t incoming_child_id, InternalNode& sibling, Pager& pager) {
        uint32_t key_count = this->get_key_count();
        uint32_t total_keys = key_count + 1;
        uint32_t midpoint = total_keys / 2; 

        // 1. Virtual Buffer to handle the (N+1) overflow temporarily
        std::vector<uint8_t> virtual_buffer(total_keys * 8);
        uint8_t* cell_start = reinterpret_cast<uint8_t*>(page->data) + INTERNAL_NODE_CELLS_START;

        // 2. Find insertion point for the new key/child pair
        uint32_t insertion_index = 0;
        while (insertion_index < key_count && get_key(insertion_index) < incoming_key) {
            insertion_index++;
        }

        // 3. Load the virtual buffer with sorted data
        std::memcpy(virtual_buffer.data(), cell_start, insertion_index * 8);
        std::memcpy(virtual_buffer.data() + (insertion_index * 8), &incoming_child_id, 4);
        std::memcpy(virtual_buffer.data() + (insertion_index * 8) + 4, &incoming_key, 4);
        std::memcpy(virtual_buffer.data() + ((insertion_index + 1) * 8), 
                    cell_start + (insertion_index * 8), 
                    (key_count - insertion_index) * 8);

        // 4. Promotion: The key at midpoint is sent to the parent
        SplitResult promotion;
        promotion.split_key = *(uint32_t*)(virtual_buffer.data() + (midpoint * 8) + 4);
        promotion.new_page_id = sibling.get_page_id();

        // 5. Update Left Node (current node)
        this->set_key_count(midpoint);
        std::memcpy(cell_start, virtual_buffer.data(), midpoint * 8);

        // 6. Update Right Node (sibling)
        uint32_t right_key_count = total_keys - midpoint - 1;
        sibling.set_node_type(NODE_INTERNAL);
        sibling.set_key_count(right_key_count);
        sibling.set_parent(this->get_parent());
        
        // The pointer to the LEFT of the promoted key becomes the sibling's anchor
        uint32_t sibling_anchor = *(uint32_t*)(virtual_buffer.data() + (midpoint * 8));
        sibling.set_leftmost_child(sibling_anchor);
        
        // Copy remaining cells to sibling
        std::memcpy(reinterpret_cast<uint8_t*>(sibling.page->data) + INTERNAL_NODE_CELLS_START, 
                    virtual_buffer.data() + ((midpoint + 1) * 8), 
                    right_key_count * 8);

        return promotion;
}

uint32_t InternalNode::get_leftmost_child() { 
    return deserialize_uint32(page->data + 16); 
}
    
void InternalNode::set_leftmost_child(uint32_t id) { 
    serialize_uint32(id, page->data + 16); 
}

uint32_t InternalNode::get_child_for_key(uint32_t key) {
    uint32_t num_keys = get_key_count();

    if (num_keys == 0 || key < get_key(0)) {
        return get_leftmost_child(); 
    }

    for (uint32_t i = 0; i < num_keys; i++) {
        if (i == num_keys - 1 || key < get_key(i + 1)) {
            return get_child(i); 
        }
    }
    return get_child(num_keys - 1);
}

PageAwaiter InternalNode::initialize_as_root(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id) {
    this->set_node_type(NODE_INTERNAL);
    this->set_is_root(1);
    this->set_key_count(1);
    this->set_leftmost_child(left_child_id);
    this->set_child(0, right_child_id);
    this->set_key(0, split_key);
    co_return;
}

SplitResult InternalNode::split_and_insert(SplitResult result, Pager& pager) {
    uint32_t sibling_id = pager.get_unused_page_number();
    auto sibling_page = pager.read_page(sibling_id);
    InternalNode sibling(sibling_page.get(), sibling_id);

    SplitResult promotion = perform_internal_split_logic(result.split_key, result.new_page_id, sibling, pager);

    pager.write_page(this->page_id, *(this->page));
    pager.write_page(sibling_id, *sibling_page);
    return promotion;
}

SplitTask InternalNode::split_and_insert_internal_async(uint32_t split_key, uint32_t new_child_id, Pager& pager) {
    uint32_t sibling_id = pager.get_unused_page_number();
    std::shared_ptr<Page> sibling_page = co_await pager.get_page_async(sibling_id);
    InternalNode sibling(sibling_page.get(), sibling_id);

    SplitResult result = perform_internal_split_logic(split_key, new_child_id, sibling, pager);
        
    uint32_t right_key_count = sibling.get_key_count();
    for (uint32_t i = 0; i <= right_key_count; ++i) {
        uint32_t child_id = (i == 0) ? sibling.get_leftmost_child() : sibling.get_child(i - 1);
            
        std::shared_ptr<Page> child_page = co_await pager.get_page_async(child_id);
        Node child(child_page.get(), child_id);
        child.set_parent(sibling_id);
            
        pager.mark_as_dirty(child_id);
        co_await pager.flush_page_async(child_id);
    }

    co_await pager.flush_page_async(this->get_page_id());
    co_await pager.flush_page_async(sibling_id);
    co_return result;
}

uint32_t InternalNode::get_child(uint32_t child_idx) {
    char* addr = page->data + INTERNAL_NODE_CELLS_START + (child_idx * 8) + 4;
    return deserialize_uint32(addr);
}

void InternalNode::set_child(uint32_t child_idx, uint32_t child_id) {
    char* addr = page->data + INTERNAL_NODE_CELLS_START + (child_idx * 8) + 4;
    serialize_uint32(child_id, addr);
}

uint32_t InternalNode::get_key(uint32_t key_idx) {
    char* addr = page->data + INTERNAL_NODE_CELLS_START + (key_idx * 8); 
    return deserialize_uint32(addr);
}

void InternalNode::set_key(uint32_t key_idx, uint32_t key) {
    char* addr = page->data + INTERNAL_NODE_CELLS_START + (key_idx * 8);
    serialize_uint32(key, addr);
}

void InternalNode::insert_child(uint32_t split_key, uint32_t new_child_page_id) {
    uint32_t num_keys = get_key_count();
    uint32_t target_idx = num_keys;

    for(uint32_t i = 0; i < num_keys; i++) {
        if(split_key < get_key(i)) {
            target_idx = i;
            break;
        }
    }

    char* src = page->data + INTERNAL_NODE_CELLS_START + (target_idx * 8);
    char* dest = src + 8;
    uint32_t bytes_to_move = (num_keys - target_idx) * 8;

    if (num_keys > target_idx) {
        std::memmove(dest, src, bytes_to_move);
    }

    set_child(target_idx, new_child_page_id);
    set_key(target_idx, split_key);
    set_key_count(num_keys + 1);
}

void InternalNode::set_node_type(NodeType type) {
    *(page->data + 0) = static_cast<uint8_t>(type); 
}

void InternalNode::set_parent(uint32_t id) {
    serialize_uint32(id, page->data + PARENT_POINTER_OFFSET);
}

uint32_t InternalNode::get_parent() {
    return deserialize_uint32(page->data + PARENT_POINTER_OFFSET);
}

bool InternalNode::is_root() const {
    return (uint8_t) *(page->data + IS_ROOT_OFFSET) == 1;
}

Page* InternalNode::get_page() { return this->page; }

SplitResult InternalNode::split_and_insert_internal(uint32_t split_key, uint32_t child_id, Pager& pager) {

    uint32_t new_page_id = pager.get_unused_page_number();
    Page* new_page_ptr = pager.get_page(new_page_id);
    InternalNode new_sibling(new_page_ptr, new_page_id);

    // Initialize the new sibling with metadata
    new_sibling.set_is_root(false);
    new_sibling.set_node_type(NODE_INTERNAL);

    // get key count 
    uint32_t key_count = this->get_key_count();
    
    // 1. Index of the key to promote
    uint32_t middle_idx = key_count / 2;

    // 2. Start copying from the key immediately AFTER the promoted key
    // If middle_idx is 5, we copy starting at index 6.
    uint32_t start_copy_idx = middle_idx + 1; 

    // 3. Bytes to copy = (Total remaining keys) * 8
    uint32_t keys_to_move = key_count - start_copy_idx;
    uint32_t bytes_to_move = keys_to_move * 8;

    // 4. Perform the copy
    std::memcpy(
        new_sibling.page->data + INTERNAL_NODE_CELLS_START, 
        this->page->data + INTERNAL_NODE_CELLS_START + (start_copy_idx * 8), 
        bytes_to_move
    );

    for (uint32_t i = 0; i < keys_to_move; ++i) {
        // Each cell is 8 bytes: 4 bytes key, 4 bytes child_id
        uint32_t child_id = *reinterpret_cast<uint32_t*>(new_sibling.page->data + 
                            INTERNAL_NODE_CELLS_START + (i * 8) + 4);
        
        // Get the child page and update its parent
        Page* child_ptr = pager.get_page(child_id);
        Node child_node(child_ptr, child_id);
        child_node.set_parent(new_sibling.page_id);
        
        // Mark the child as dirty because we changed its parent pointer
        pager.mark_dirty(child_id);
    }


    uint32_t promoted_key = *reinterpret_cast<uint32_t*>(this->page->data + INTERNAL_NODE_CELLS_START + (middle_idx * 8));
    uint32_t right_child_id = *reinterpret_cast<uint32_t*>(this->page->data + INTERNAL_NODE_CELLS_START + (middle_idx * 8) + 4);

    new_sibling.set_parent(this->get_parent());
    this->set_key_count(middle_idx);
    new_sibling.set_key_count(keys_to_move);

    new_sibling.set_leftmost_child(right_child_id);

    pager.mark_dirty(this->page_id);
    pager.mark_dirty(new_page_id);

    return { promoted_key, new_page_id };
}


