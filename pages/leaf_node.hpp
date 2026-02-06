#pragma once

#include <cstdint>
#include "page.hpp"
#include "node.hpp"

const uint32_t LEAF_NODE_CELL_SIZE = 36;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - HEADER_SIZE;
const uint32_t LEAF_NODE_MAX_CELLS = LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;



class LeafNode: public Node {
    public:
        LeafNode(Page* p): Node(p) {};

        // Helper to find the exact memory address of the specific slot (cell)
        char* cell_address(uint32_t cell_num) {
            return page->data + HEADER_SIZE + (cell_num * LEAF_NODE_CELL_SIZE);
        }

        // Accessors for the key of a specific cell
        uint32_t get_key(uint32_t cell_num) {
            return deserialize_uint32(cell_address(cell_num));
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

        void insert(uint32_t key, const char* value) {
            uint32_t num_cells = get_key_count();

            if (num_cells >= LEAF_NODE_MAX_CELLS) {
                return;
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
        }
};