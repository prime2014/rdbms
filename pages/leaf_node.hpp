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


        SplitResult split_and_insert(uint32_t key, const char* value, Pager& pager) {
            // 1. Create a new page for the right side
            uint32_t new_page_num = pager.get_unused_page_number();
            auto new_page = pager.read_page(new_page_num);
            LeafNode right_node(new_page.get(), new_page_num);

            // Initialize the new right node
            right_node.set_node_type(1); 
            right_node.set_is_root(0);   // The new sibling is never the root
            right_node.set_key_count(0);
            right_node.set_next_page(this->get_next_page());
            this->set_next_page(new_page_num);

            // 2. Move half the cells
            uint32_t total_cells = get_key_count();
            uint32_t left_count = total_cells / 2;
            uint32_t right_count = total_cells - left_count;

            for (uint32_t i = 0; i < right_count; i++) {
                uint32_t old_index = left_count + i;
                right_node.set_key(i, this->get_key(old_index));
                right_node.set_value(i, this->get_value(old_index));
            }

            this->set_key_count(left_count);
            right_node.set_key_count(right_count);

            // 3. Insert the new record into the correct side
            if (key <= this->get_key(left_count - 1)) {
                this->insert(key, value, pager);
            } else {
                right_node.insert(key, value, pager);
            }

            // 4. Commit to disk
            pager.write_page(new_page_num, *new_page);
            pager.write_page(this->get_page_id(), *(this->page));

            // 5. RETURN the info needed for promotion
            // We use the first key of the right node as the divider
            return { right_node.get_key(0), new_page_num };
        };

        
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