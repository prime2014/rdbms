#include <cstdint>
#include "node.hpp"

// We define this to ensure we skip the Right Child pointer and Table Count
const uint32_t INTERNAL_NODE_CELLS_START = 20; 


enum NodeType {
    NODE_INTERNAL = 0,
    NODE_LEAF = 1
};

class InternalNode : public Node {
public:
    InternalNode(Page *p, uint32_t id) : Node(p, id) {};

    // Navigation: Which child should we follow?
    uint32_t get_child_for_key(uint32_t key) {
        uint32_t num_keys = get_key_count();

        // Sequential scan through divider keys
        for (uint32_t i = 0; i < num_keys; i++) {
            uint32_t key_at_index = get_key(i);
            if (key <= key_at_index) {
                return get_child(i);
            }
        }
        return get_right_child();
    }

    Page* get_page() {
        return this->page;
    }


    uint32_t get_parent() {
        return *(uint32_t*)(page->data + PARENT_POINTER_OFFSET);
    };

    bool is_root() const {
        return (uint8_t) *(page->data + IS_ROOT_OFFSET) == 1;
    };

    // --- Memory Accessors using 8-byte steps ---

    uint32_t get_child(uint32_t child_idx) {
        // Starts at 20. Each cell is [ChildID(4b)][Key(4b)]
        char* addr = page->data + INTERNAL_NODE_CELLS_START + (child_idx * 8);
        return deserialize_uint32(addr);
    }

    void set_node_type(NodeType type) {
        uint8_t value = static_cast<uint8_t>(type);
        *(page->data + 0) = value; 
    }

    SplitResult split_and_insert(SplitResult result, Pager& pager) {
        char* cell_start = this->page->data + INTERNAL_NODE_CELLS_START;
        uint32_t key_count = this->get_key_count();

        char virtual_buffer[PAGE_SIZE] = {0};
        std::memcpy(virtual_buffer, cell_start, key_count * 8);

        uint32_t insertion_index = 0;
        while(insertion_index < key_count) {
            // Use your existing get_key logic but on the buffer
            uint32_t existing_key = *(uint32_t*) (virtual_buffer + (insertion_index * 8) + 4);
            if (existing_key > result.split_key) break;
            insertion_index++;

        }

        uint32_t cells_to_move = key_count - insertion_index;
        std::memmove(
            virtual_buffer + ((insertion_index + 1) * 8),
            virtual_buffer + (insertion_index * 8),
            cells_to_move * 8
        );

        *(uint32_t*)(virtual_buffer + (insertion_index * 8)) = result.new_page_id;
        *(uint32_t*)(virtual_buffer + (insertion_index * 8) + 4) = result.split_key;

        uint32_t total_keys = key_count + 1;
        uint32_t midpoint = total_keys / 2; 

        uint32_t sibling_page_id = pager.get_unused_page_number();
        
        auto sibling_page_ptr = pager.read_page(sibling_page_id);
        InternalNode sibling_node(sibling_page_ptr.get(), sibling_page_id);
        
        sibling_node.set_node_type(NODE_INTERNAL);
        sibling_node.set_key_count(0);

        SplitResult promotion;
        promotion.split_key = *(uint32_t*)(virtual_buffer + (midpoint * 8) + 4);
        promotion.new_page_id = sibling_page_id;

        // FIX 3: Save the OLD right child before overwriting it
        uint32_t old_right_child = this->get_right_child();

        uint32_t new_left_right_child = *(uint32_t*)(virtual_buffer + (midpoint * 8));
        
        this->set_key_count(midpoint);
        this->set_right_child(new_left_right_child);
        std::memcpy(cell_start, virtual_buffer, midpoint * 8);

        uint32_t right_key_count = total_keys - midpoint - 1;
        sibling_node.set_key_count(right_key_count);
        
        // The sibling inherits the original node's old Right Child
        sibling_node.set_right_child(old_right_child);

        char* sibling_cell_start = sibling_node.page->data + INTERNAL_NODE_CELLS_START;
        std::memcpy(sibling_cell_start, virtual_buffer + ((midpoint + 1) * 8), right_key_count * 8);

        // FIX 4: Write the new sibling page back to the pager
        pager.write_page(sibling_page_id, *sibling_page_ptr);
        pager.write_page(this->page_id, *(this->page));
        return promotion;
    }



    void set_child(uint32_t child_idx, uint32_t child_id) {
        char* addr = page->data + INTERNAL_NODE_CELLS_START + (child_idx * 8);
        serialize_uint32(child_id, addr);
    }

    uint32_t get_key(uint32_t key_idx) {
        // The key is the second half of the 8-byte cell (+4 offset)
        char* addr = page->data + INTERNAL_NODE_CELLS_START + (key_idx * 8) + 4;
        return deserialize_uint32(addr);
    }

    void set_key(uint32_t key_idx, uint32_t key) {
        char* addr = page->data + INTERNAL_NODE_CELLS_START + (key_idx * 8) + 4;
        serialize_uint32(key, addr);
    }

    // --- Special Right Child (The "Else" Pointer) ---

    uint32_t get_right_child() {
        // RIGHT_CHILD_OFFSET is 16
        return deserialize_uint32(page->data + RIGHT_CHILD_OFFSET);
    }

    void set_right_child(uint32_t id) {
        serialize_uint32(id, page->data + RIGHT_CHILD_OFFSET);
    }


    void set_parent(uint32_t new_root_id) {
        uint32_t* addr = (uint32_t*)(page->data + PARENT_POINTER_OFFSET);
        *addr = new_root_id;
    }


    void insert_child(uint32_t split_key, uint32_t new_child_page_id) {
        uint32_t num_keys = get_key_count();
        uint32_t target_idx = num_keys;

        // 1. Find the correct slot for the new divider key
        // We want to keep keys in ascending order
        for(uint32_t i = 0; i < num_keys; i++) {
            if(split_key < get_key(i)) {
                target_idx = i;
                break;
            }
        }

        // 2. Shift existing entires to the right by 8 bytes
        // Each entry is: [4b Child ID][4b Key]
        char* src = page->data + INTERNAL_NODE_CELLS_START + (target_idx * 8);
        char* dest = src + 8;
        uint32_t bytes_to_move = (num_keys - target_idx) * 8;

        if (num_keys > target_idx) {
            std::memmove(dest, src, bytes_to_move);
        }

        // 3. Insert the new data
        // The key from the SplitResult becomes the divider
        // The new_page_id becomes the child pointer for that divider
        set_child(target_idx, new_child_page_id);
        set_key(target_idx, split_key);

        // 4. Increment the count
        set_key_count(num_keys + 1);
    }
};