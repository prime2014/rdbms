#include "../pages/leaf_node.hpp"
#include "../pages/task.hpp"
#include "../pages/pager.hpp"

#include <vector>
#include <cstring>

const uint8_t NODE_INTERNAL = 0;
const uint8_t NODE_LEAF = 1;

char* LeafNode::cell_address(uint32_t cell_num) {
    return page->data + LEAF_NODE_CELLS_START + (cell_num * LEAF_NODE_CELL_SIZE);
}


SplitTask LeafNode::split_and_insert_async(uint32_t key, const char* value, Pager& pager) {
    uint32_t new_page_id = pager.get_unused_page_number();
    std::shared_ptr<Page> new_page_handle = co_await pager.get_page_async(new_page_id);
    LeafNode new_leaf(new_page_handle.get(), new_page_id);

    // 1. Perform the memory shuffle
    uint32_t split_key = perform_memory_split(key, value, new_leaf);

    // 2. Mark everything dirty (No co_await here!)
    pager.mark_dirty(this->get_page_id());
    pager.mark_dirty(new_page_id);

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

    serialize_string_32(value, (char*)buffer.data() + (insertion_idx * LEAF_NODE_CELL_SIZE) + 4);

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


// Accessors for the key of a specific cell
uint32_t LeafNode::get_key(uint32_t cell_num) {
    return deserialize_uint32(cell_address(cell_num));
}

uint32_t LeafNode::get_next_page() {
    return deserialize_uint32(page->data + NEXT_PAGE_OFFSET);
}


void LeafNode::set_next_page(uint32_t page_id) {
    serialize_uint32(page_id, page->data + NEXT_PAGE_OFFSET);
}

void LeafNode::set_key(uint32_t cell_num, uint32_t key) {
    serialize_uint32(key, cell_address(cell_num));
}

char* LeafNode::get_value(uint32_t cell_num) {
    // The value starts right after the 4-byte key
    return cell_address(cell_num) + sizeof(uint32_t);
}

void LeafNode::set_value(uint32_t cell_num, const char* value_ptr) {
    uint32_t value_size = LEAF_NODE_CELL_SIZE - sizeof(uint32_t);
    char* dest = get_value(cell_num);

    // 1. Clear the destination first to remove any "ghost" data
    std::memset(dest, 0, value_size);

    // 2. Copy only the string contents, not the extra memory after it
    // Use strncpy to safely copy up to the buffer size
    std::strncpy(dest, value_ptr, value_size - 1);
}


SplitResult LeafNode::split_and_insert(uint32_t key, const char* value, Pager& pager) {
    uint32_t new_page_id = pager.get_unused_page_number();
    auto new_page_handle = pager.read_page(new_page_id);
    LeafNode new_leaf(new_page_handle.get(), new_page_id);
    // Use the optimized memory split logic
    uint32_t promoted_key = perform_memory_split(key, value, new_leaf);
    // Persist both
    pager.write_page(this->get_page_id(), *(this->page));
    pager.write_page(new_page_id, *new_page_handle);
    return { promoted_key, new_page_id };
}


SplitResult LeafNode::insert(uint32_t key, const char* value, Pager& pager) {
            
    uint32_t num_cells = get_key_count();

    if (num_cells >= LEAF_NODE_MAX_CELLS) {
        return split_and_insert(key, value, pager);
    }

            // 1. Find the spot where this ID belongs
    uint32_t target_cell = 0;
    while (target_cell < num_cells && get_key(target_cell) < key) {
        target_cell++;
    }

            // 2. Shift existing records to the right to make a hole
    if (target_cell < num_cells) {
        char* src = cell_address(target_cell);
        char*dest = cell_address(target_cell + 1);
        uint32_t bytes_to_move = (num_cells - target_cell) * LEAF_NODE_CELL_SIZE;
        std::memmove(dest, src, bytes_to_move);
    }

            // 3. Write the carved data into the hole
    set_key(target_cell, key);
    set_value(target_cell, value);

            // 4. Update the header so we know there is a new record
    set_key_count(num_cells + 1);

    pager.mark_as_dirty(this->page_id);

    return { 0, 0 };
}
