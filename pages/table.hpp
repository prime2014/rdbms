#ifndef TABLE_HPP
#define TABLE_HPP

#include <string>
#include <memory>
#include "pager.hpp"
#include "leaf_node.hpp"

class Cursor;
struct CursorTask;

class Table {
    private:
        std::string table_name;
        std::unique_ptr<Pager> pager;
        uint32_t root_page_id;

    public:
        Table(const std::string& name, bool memory_only = false): table_name(name), root_page_id(0) {
            pager = std::make_unique<Pager>(name + ".db", memory_only);

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
        Pager* get_pager();

        PageTask insert(uint32_t key, const char* value);
        
        PageTask insert_async(uint32_t key, const char* value);

        void update_parent(uint32_t parent_id, SplitResult result);

        CursorTask find_async(uint32_t key);

        void increment_total_count_sync();

        uint32_t get_total_count();

        VoidTask scan_records_async(uint32_t start_page_id);

        void create_new_root(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id);
        
    
        PageTask update_parent_async(uint32_t parent_id, SplitResult result);

        PageTask create_new_root_async(uint32_t page_id, uint32_t split_key, uint32_t new_page_id);

        VoidTask process_records_async(uint32_t start_leaf_id);

        friend class Cursor;


    private:
        uint32_t find_leaf(uint32_t page_id, uint32_t key);
        
        PageTask find_leaf_async(uint32_t root_id, uint32_t key, uint32_t& out_leaf_id);
        
        void increment_total_count();
};


#endif