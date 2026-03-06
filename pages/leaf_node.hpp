#pragma once

#include <cstdint>
#include "page.hpp"
#include "node.hpp"

const uint32_t LEAF_NODE_CELL_SIZE = 36;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_CELLS_START;
const uint32_t LEAF_NODE_MAX_CELLS = LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;



class LeafNode: public Node {
    public:
        LeafNode(Page* p, uint32_t id): Node(p, id) {};

        // Helper to find the exact memory address of the specific slot (cell)
        char* cell_address(uint32_t cell_num) {
            return page->data + LEAF_NODE_CELLS_START + (cell_num * LEAF_NODE_CELL_SIZE);
        }

        

        uint32_t get_num_cells();
        uint32_t get_parent();

        uint32_t find_insertion_index(uint32_t key);

        uint32_t get_key_at_index(uint32_t mid);

        uint32_t perform_memory_split(uint32_t key, const char* value, LeafNode& new_leaf);

        // Accessors for the key of a specific cell
        uint32_t get_key(uint32_t cell_num) {
            return deserialize_uint32(cell_address(cell_num));
        }

        uint32_t get_next_page() {
            return deserialize_uint32(page->data + NEXT_PAGE_OFFSET);
        }


        void set_next_page(uint32_t page_id) {
            serialize_uint32(page_id, page->data + NEXT_PAGE_OFFSET);
        }

        void set_key(uint32_t cell_num, uint32_t key) {
            serialize_uint32(key, cell_address(cell_num));
        }

        char* get_value(uint32_t cell_num) {
            // The value starts right after the 4-byte key
            return cell_address(cell_num) + sizeof(uint32_t);
        }

        void set_value(uint32_t cell_num, const char* value_ptr) {
            // We assume the value size is (LEAF_NODE-CELL_SIZE -4)
            uint32_t value_size = LEAF_NODE_CELL_SIZE - sizeof(uint32_t);
            std::memcpy(get_value(cell_num), value_ptr, value_size);
        }

        
        uint32_t perform_memory_split(uint32_t key, const char* value, LeafNode& new_leaf);

       
        SplitResult split_and_insert(uint32_t key, const char* value, Pager& pager) {
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

        SplitTask split_and_insert_async(uint32_t key, const char* valur, Pager& pager);

        
        SplitResult insert(uint32_t key, const char* value, Pager& pager) {
            
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

            return { 0, 0 };
        }

    
};