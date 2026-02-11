#pragma once

#include <string>
#include <memory>
#include "pager.hpp"
#include "leaf_node.hpp"
#include "internal_node.hpp"


class Table {
    private:
        std::string table_name;
        std::unique_ptr<Pager> pager;
        uint32_t root_page_id;

    public:
        Table(const std::string& name): table_name(name), root_page_id(0) {
            pager = std::make_unique<Pager>(name + ".db");

            // If the database is brand new, initialize Page 0 as a Leaf Root
            if (pager->get_num_pages() == 0) {
                auto root_handle = pager->read_page(0);
                
                // Use LeafNode to format the blank page
                LeafNode root_node(root_handle.get(), 0);
                root_node.set_node_type(1); // 1 = Leaf
                root_node.set_is_root(1);
                root_node.set_key_count(0);
                root_node.set_next_page(0); // No sibling yet
                
                // Initialize the global row count to 0
                serialize_uint32(0, root_handle->data + TABLE_TOTAL_COUNT_OFFSET);

                // Commit the "empty" root to disk
                pager->write_page(0, *root_handle);
            }
        }

        void process_internal_root_split(InternalNode parent, SplitResult internal_split);

        // The high-level interface for the Database class
        void insert(uint32_t key, const char* value) {
            // 1. Find the correct leaf where this key belongs
            uint32_t leaf_id = find_leaf(root_page_id, key);

            // 2. Load that leaf
            auto page_handle = pager->read_page(leaf_id);
            LeafNode leaf(page_handle.get(), leaf_id);

            // 3. Handle the insert/split logic
            if (leaf.get_key_count() >= LEAF_NODE_MAX_CELLS) {
                leaf.split_and_insert(key, value, *pager);
            } else {
                leaf.insert(key, value, *pager);
            }

            // 4. Update the global count in the header of Page 0
            increment_total_count();
        };


        uint32_t get_total_count() {
            auto root_page = pager->read_page(0);
            return deserialize_uint32(root_page->data + 4);
        }

        void create_new_root(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id) {
            auto root_handle = pager->read_page(0);
            
            // Format Page 0 as an Internal Node
            InternalNode root(root_handle.get(), 0);
            root.set_node_type(NODE_INTERNAL); // Internal
            root.set_is_root(1);
            root.set_key_count(1);
            
            // Set the pointers: [Child 0] [Key 0] [Right Child]
            root.set_child(0, left_child_id);      // Points to original data
            root.set_key(0, split_key);            // The divider key
            root.set_right_child(right_child_id);  // Points to new sibling
            
            pager->write_page(0, *root_handle);
        }

        void update_parent(uint32_t parent_id, SplitResult result);
    
    private:
        // Navigation logic: Start at root, follow pointer down to the leaf
        uint32_t find_leaf(uint32_t page_id, uint32_t key) {
            auto page_handle = pager->read_page(page_id);

            // if it is a leaf, we found our target
            if (page_handle->data[NODE_TYPE_OFFSET] == 1) {
                return page_id;
            }

            // if it's internal, find which child to follow
            InternalNode internal(page_handle.get(), page_id);
            uint32_t child_id = internal.get_child_for_key(key);
            return find_leaf(child_id, key);
        };

        void increment_total_count() {
            uint32_t count = get_total_count();
            auto root_page = pager->read_page(0);
            serialize_uint32(count + 1, root_page->data + TABLE_TOTAL_COUNT_OFFSET);
            pager->write_page(0, *root_page);
        };
};