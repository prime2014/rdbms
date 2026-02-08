#include <cstdint>
#include "node.hpp"

// We define this to ensure we skip the Right Child pointer and Table Count
const uint32_t INTERNAL_NODE_CELLS_START = 20; 

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

    // --- Memory Accessors using 8-byte steps ---

    uint32_t get_child(uint32_t child_idx) {
        // Starts at 20. Each cell is [ChildID(4b)][Key(4b)]
        char* addr = page->data + INTERNAL_NODE_CELLS_START + (child_idx * 8);
        return deserialize_uint32(addr);
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
};