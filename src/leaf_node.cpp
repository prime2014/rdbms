#include "../pages/leaf_node.hpp"
#include "../pages/task.hpp"


const uint8_t NODE_INTERNAL = 0;
const uint8_t NODE_LEAF = 1;

// Change return type to Task<SplitResult>
SplitTask LeafNode::split_and_insert_async(uint32_t key, const char* value, Pager& pager) {

    uint32_t new_page_id = pager.get_unused_page_number();

    // 1. Get the new page (potential suspension)
    std::shared_ptr<Page> new_page_handle = co_await pager.get_page_async(new_page_id);
    LeafNode new_leaf(new_page_handle.get(), new_page_id);

    // 2. Memory Shuffle (No suspension)
    uint32_t split_key = perform_memory_split(key, value, new_leaf);


    // 4. Persistence (Potential suspension - "Hitching a Ride")
    co_await pager.flush_page_async(this->get_page_id());
    co_await pager.flush_page_async(new_page_id);

    // 5. Return the metadata required by the parent node
    co_return SplitResult{ split_key, new_page_id };
}

uint32_t LeafNode::get_parent() {
    return deserialize_uint32(this->page->data + PARENT_POINTER_OFFSET);
}



uint32_t LeafNode::perform_memory_split(uint32_t key, const char* value, LeafNode& new_leaf) {
    uint32_t total_keys = get_key_count();
    uint32_t insertion_idx = find_insertion_index(key);
    
    uint32_t total_after = total_keys + 1;
    uint32_t left_count = total_after / 2;
    uint32_t right_count = total_after - left_count;

    // Build virtual buffer
    std::vector<uint8_t> buffer(total_after * LEAF_NODE_CELL_SIZE);
    char* src_data = page->data + LEAF_NODE_CELLS_START;

    std::memcpy(buffer.data(), src_data, insertion_idx * LEAF_NODE_CELL_SIZE);
    
    // Insert new record into buffer
    serialize_uint32(key, (char*)buffer.data() + (insertion_idx * LEAF_NODE_CELL_SIZE));
    std::memcpy((char*)buffer.data() + (insertion_idx * LEAF_NODE_CELL_SIZE) + 4, value, 32);

    // Copy remainder
    std::memcpy((char*)buffer.data() + ((insertion_idx + 1) * LEAF_NODE_CELL_SIZE), 
                src_data + (insertion_idx * LEAF_NODE_CELL_SIZE), 
                (total_keys - insertion_idx) * LEAF_NODE_CELL_SIZE);

    // Important: Update Sibling Pointers BEFORE clearing data
    new_leaf.set_next_page(this->get_next_page());
    this->set_next_page(new_leaf.get_page_id());

    // Distribute data
    std::memcpy(page->data + LEAF_NODE_CELLS_START, buffer.data(), left_count * LEAF_NODE_CELL_SIZE);
    this->set_key_count(left_count);

    std::memcpy(new_leaf.get_page()->data + LEAF_NODE_CELLS_START, 
                buffer.data() + (left_count * LEAF_NODE_CELL_SIZE), 
                right_count * LEAF_NODE_CELL_SIZE);
    new_leaf.set_key_count(right_count);

    return new_leaf.get_key(0); // The divider key for the parent
}


uint32_t LeafNode::find_insertion_index(uint32_t key) {
    uint32_t count = get_num_cells();
    if (count == 0) return 0;

    uint32_t low = 0;
    uint32_t high = count; // Range is [low, high)

    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        uint32_t mid_key = get_key_at_index(mid);

        if (mid_key == key) {
            return mid;
        } else if (mid_key < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low; // This is the index where 'key' should be inserted
}


uint32_t LeafNode::get_key_at_index(uint32_t index) {
    // 1. Calculate the byte offset from the start of the page
    uint32_t offset = COMMON_HEADER_SIZE + (index * 36);

    // 2. Point to that specific spot in memory
    // We use char* to ensure the math is done in 1-byte increments
    char* key_ptr = this->page->data + offset;

    // 3. Cast that address to a uint32_t pointer and read it
    return *(reinterpret_cast<uint32_t*>(key_ptr));
}

uint32_t LeafNode::get_num_cells() {
    return deserialize_uint32(page->data + KEY_COUNT_OFFSET);
}