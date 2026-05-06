#include "../pages/leaf_node.hpp"
#include "../pages/task.hpp"
#include "../pages/pager.hpp"
#include "../pages/page_header.hpp"
#include "../pages/leafcell.hpp"
#include "../pages/leafpage.hpp"

#include <vector>
#include <cstring>


char* LeafNode::cell_address(uint32_t cell_num) {
    return page->data + sizeof(PageHeader) + (cell_num * LEAF_NODE_CELL_SIZE);
}


SplitTask LeafNode::split_and_insert_async(uint32_t key, const char* value, Pager& pager) {
    uint32_t new_page_id = pager.get_unused_page_number();
    std::shared_ptr<Page> new_page_handle = co_await pager.get_page_async(new_page_id);
    LeafNode new_leaf(new_page_handle, new_page_id);

    // 1. Perform the memory shuffle
    uint32_t split_key = perform_memory_split(key, value, new_leaf);

    // 2. Mark everything dirty (No co_await here!)
    pager.mark_dirty(this->get_page_id());
    pager.mark_dirty(new_page_id);

    co_return SplitResult{ split_key, new_page_id };
}


uint32_t LeafNode::get_parent() {
    PageHeader* header = reinterpret_cast<PageHeader*>(this->page->data);
    
    return header->parent_id;
}


uint32_t LeafNode::perform_memory_split(uint32_t key, const char* value, LeafNode& new_leaf) {
    uint32_t total_keys = get_key_count();
    uint32_t insertion_idx = find_insertion_index(key);
    
    uint32_t total_after = total_keys + 1;
    uint32_t left_count = total_after / 2;
    uint32_t right_count = total_after - left_count;

    // 1. Build virtual buffer using the STRUCT type
    // This handles all the "byte jumping" logic automatically
    std::vector<LeafCell> buffer(total_after);
    LeafCell* src_data = reinterpret_cast<LeafCell*>(page->data + sizeof(PageHeader));

    // 2. Copy data before the insertion point
    if (insertion_idx > 0) {
        std::memcpy(buffer.data(), src_data, insertion_idx * sizeof(LeafCell));
    }
    
    // 3. Insert new record with precision
    buffer[insertion_idx].key = key;
    // Use strncpy to prevent buffer overflow if 'value' is too long
    std::memset(buffer[insertion_idx].value, 0, 32); // Clear first
    if (value) {
        std::strncpy(buffer[insertion_idx].value, value, 31);
    }

    // 4. Copy remainder
    if (insertion_idx < total_keys) {
        std::memcpy(&buffer[insertion_idx + 1], 
                    &src_data[insertion_idx], 
                    (total_keys - insertion_idx) * sizeof(LeafCell));
    }

    // 5. Initialize new leaf metadata
    new_leaf.set_node_type(NODE_LEAF);
    new_leaf.set_is_root(false);
    new_leaf.set_key_count(right_count);

    // 6. Sibling pointer and Parent handling
    new_leaf.set_next_page(this->get_next_page());
    this->set_next_page(new_leaf.get_page_id());
    
    uint32_t current_parent = this->get_parent();
    this->set_parent(current_parent);
    new_leaf.set_parent(current_parent);

    // 7. Distribute the data back to the actual pages
    auto* left_layout = reinterpret_cast<LeafPageLayout*>(this->page->data);
    auto* right_layout = reinterpret_cast<LeafPageLayout*>(new_leaf.get_page()->data);

    // Current Page (Left)
    std::memcpy(left_layout->cells, 
                buffer.data(), 
                left_count * sizeof(LeafCell));
    left_layout->header.key_count = left_count;

    // New Sibling Page (Right)
    std::memcpy(right_layout->cells, 
                buffer.data() + left_count, 
                right_count * sizeof(LeafCell));
    right_layout->header.key_count = right_count;

    return buffer[left_count].key; // Smallest key in the right leaf
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
    uint32_t offset = sizeof(PageHeader) + (index * LEAF_NODE_CELL_SIZE);

    // 2. Point to that specific spot in memory
    // We use char* to ensure the math is done in 1-byte increments
    char* key_ptr = this->page->data + offset;

    // 3. Cast that address to a uint32_t pointer and read it
    return *(reinterpret_cast<uint32_t*>(key_ptr));
}

uint32_t LeafNode::get_num_cells() {
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    return header->key_count;
}


// Accessors for the key of a specific cell
uint32_t LeafNode::get_key(uint32_t cell_num) {
    return deserialize_uint32(cell_address(cell_num));
}

uint32_t LeafNode::get_next_page() {
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    return header->pointer.sibling_id;
}


void LeafNode::set_next_page(uint32_t page_id) {
    PageHeader* header = reinterpret_cast<PageHeader*>(page->data);
    header->pointer.sibling_id = page_id;
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
    
    // FIX 1: Get the shared_ptr handle to satisfy the new constructor
    auto new_page_handle = pager.get_page_shared(new_page_id); 
    
    // Pass the handle and the pager pointer (if your constructor allows)
    LeafNode new_leaf(new_page_handle, new_page_id, &pager);
    
    // FIX 2: Use the robust initialization method to avoid Magic Number Mismatches
    // This sets type, root status, and magic number in one safe go.
    new_leaf.init_new_node(NODE_LEAF, false);

    // Perform the actual data movement logic
    uint32_t promoted_key = perform_memory_split(key, value, new_leaf);

    // FIX 3: Ensure your pager method names are consistent (mark_dirty vs mark_as_dirty)
    pager.mark_dirty(this->page_id);
    pager.mark_dirty(new_page_id);

    return { promoted_key, new_page_id };
}



SplitResult LeafNode::insert(uint32_t key, const char* value, Pager& pager) {
    // 1. Map the whole page to our layout struct
    auto* layout = reinterpret_cast<LeafPageLayout*>(this->page->data);
    
    uint32_t num_cells = layout->header.key_count; // Access header directly!

    if (num_cells >= LEAF_NODE_MAX_CELLS) {
        return split_and_insert(key, value, pager);
    }

    uint32_t target_idx = find_insertion_index(key);

    // 2. Shift existing cells
    if (target_idx < num_cells) {
        // Use the array member 'cells'
        std::memmove(&layout->cells[target_idx + 1], 
                     &layout->cells[target_idx], 
                     (num_cells - target_idx) * sizeof(LeafCell));
    }

    // 3. Insert the new data into the specific array slot
    layout->cells[target_idx].key = key;
    
    // Clear and copy string safely
    std::memset(layout->cells[target_idx].value, 0, sizeof(LeafCell::value));
    std::strncpy(layout->cells[target_idx].value, value, sizeof(LeafCell::value) - 1);

    // 4. Update the header through the struct
    layout->header.key_count = num_cells + 1;
    
    pager.mark_dirty(this->page_id);

    return { 0, 0 };
}