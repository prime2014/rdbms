#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include "node.hpp"

// Header: 0-15 (Standard Node Headers), 16-19 (Leftmost Child), 20+ (Cells)
const uint32_t INTERNAL_NODE_CELLS_START = 20; 

enum NodeType {
    NODE_INTERNAL = 0,
    NODE_LEAF = 1
};

class InternalNode : public Node {
private:
    /**
     * Handles the logic of splitting an overfull node.
     * Internal nodes "promote" the middle key to the parent and do NOT keep it.
     */
    SplitResult perform_internal_split_logic(uint32_t incoming_key, uint32_t incoming_child_id, InternalNode& sibling) {
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

public:
    InternalNode(Page *p, uint32_t id) : Node(p, id) {};

    // --- Header Accessors ---
    
    // Byte 16 is the "Anchor". It points to the subtree where all keys are LESS than key(0).
    uint32_t get_leftmost_child() { 
        return deserialize_uint32(page->data + 16); 
    }
    
    void set_leftmost_child(uint32_t id) { 
        serialize_uint32(id, page->data + 16); 
    }

    // --- Navigation ---

    uint32_t get_child_for_key(uint32_t key) {
        uint32_t num_keys = get_key_count();

        // If key < first separator, go to the leftmost anchor
        if (num_keys == 0 || key < get_key(0)) {
            return get_leftmost_child(); 
        }

        // Search through the separators
        for (uint32_t i = 0; i < num_keys; i++) {
            // If we're at the end or key is less than the next separator, 
            // the target is the child pointer associated with current key (to its right).
            if (i == num_keys - 1 || key < get_key(i + 1)) {
                return get_child(i); 
            }
        }
        return get_child(num_keys - 1);
    }

    // --- Lifecycle and Persistence ---

    PageAwaiter initialize_as_root(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id) {
        this->set_node_type(NODE_INTERNAL);
        this->set_is_root(1);
        this->set_key_count(1);
        this->set_leftmost_child(left_child_id);
        this->set_child(0, right_child_id);
        this->set_key(0, split_key);
        co_return;
    }

    SplitResult split_and_insert(SplitResult result, Pager& pager) {
        uint32_t sibling_id = pager.get_unused_page_number();
        auto sibling_page = pager.read_page(sibling_id);
        InternalNode sibling(sibling_page.get(), sibling_id);

        SplitResult promotion = perform_internal_split_logic(result.split_key, result.new_page_id, sibling);

        pager.write_page(this->page_id, *(this->page));
        pager.write_page(sibling_id, *sibling_page);
        return promotion;
    }

    SplitTask split_and_insert_internal_async(uint32_t split_key, uint32_t new_child_id, Pager& pager) {
        uint32_t sibling_id = pager.get_unused_page_number();
        std::shared_ptr<Page> sibling_page = co_await pager.get_page_async(sibling_id);
        InternalNode sibling(sibling_page.get(), sibling_id);

        SplitResult result = perform_internal_split_logic(split_key, new_child_id, sibling);

        co_await pager.flush_page_async(this->get_page_id());
        co_await pager.flush_page_async(sibling_id);
        co_return result;
    }

    // --- Memory Accessors (8-byte cells: [ChildID 4b][Key 4b]) ---

    uint32_t get_child(uint32_t child_idx) {
        char* addr = page->data + INTERNAL_NODE_CELLS_START + (child_idx * 8);
        return deserialize_uint32(addr);
    }

    void set_child(uint32_t child_idx, uint32_t child_id) {
        char* addr = page->data + INTERNAL_NODE_CELLS_START + (child_idx * 8);
        serialize_uint32(child_id, addr);
    }

    uint32_t get_key(uint32_t key_idx) {
        char* addr = page->data + INTERNAL_NODE_CELLS_START + (key_idx * 8) + 4;
        return deserialize_uint32(addr);
    }

    void set_key(uint32_t key_idx, uint32_t key) {
        char* addr = page->data + INTERNAL_NODE_CELLS_START + (key_idx * 8) + 4;
        serialize_uint32(key, addr);
    }

    // --- Helpers ---

    void insert_child(uint32_t split_key, uint32_t new_child_page_id) {
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

    void set_node_type(NodeType type) {
        *(page->data + 0) = static_cast<uint8_t>(type); 
    }

    void set_parent(uint32_t id) {
        serialize_uint32(id, page->data + PARENT_POINTER_OFFSET);
    }

    uint32_t get_parent() {
        return deserialize_uint32(page->data + PARENT_POINTER_OFFSET);
    }

    bool is_root() const {
        return (uint8_t) *(page->data + IS_ROOT_OFFSET) == 1;
    }

    Page* get_page() { return this->page; }
};