#ifndef TABLE_HPP
#define TABLE_HPP

#include <string_view>
#include <string>
#include <memory>
#include "pager.hpp"
#include "leaf_node.hpp"
#include "wal_debug.hpp"
#include "basenode.hpp"
#include "node.hpp"
#include <atomic>


class Cursor;
struct CursorTask;



class Table {
    private:
        std::string_view table_name;
        std::unique_ptr<Pager> pager;
        uint32_t root_page_id;
        std::atomic<bool> is_splitting{false};
        DebugWAL wal;
        std::unordered_map<uint32_t, std::vector<std::coroutine_handle<>>> split_latches;
    public:

        Table(const std::string_view name, bool memory_only = false) : table_name(name), wal("engine_debug.log") {
            std::string db_filename;
            db_filename.reserve(name.size() + 3);
            db_filename.append(name).append(".db");

            pager = std::make_unique<Pager>(db_filename, memory_only);

            if (pager->get_num_pages() == 0) {
                // 1. Allocate IDs
                uint32_t meta_id = pager->get_unused_page_number(); // Page 0
                uint32_t root_id = pager->get_unused_page_number(); // Page 1
                this->root_page_id = root_id;

                // 2. Initialize Metadata Page (Page 0)
                auto meta_handle = pager->get_page_shared(meta_id); 
                std::memset(meta_handle->data, 0, PAGE_SIZE);
                PageHeader* meta_header = reinterpret_cast<PageHeader*>(meta_handle->data);
                
                meta_header->magic = MAGIC_META;      // Verification
                meta_header->node_type = NODE_META;   // Logic
                meta_header->pointer.root_page_id = root_id;
                meta_header->total_count = 0;
                pager->mark_dirty(meta_id);

                // 3. Initialize Root Leaf Page (Page 1)
                auto root_handle = pager->get_page_shared(root_id); 
                std::memset(root_handle->data, 0, PAGE_SIZE); 

                // Use the Node class to handle formatting
                Node root_node(root_handle, root_id);

                // Replace the 3 setter calls with this one:
                root_node.init_new_node(NODE_LEAF, true); 

                // Now that it's initialized, you can safely call dirty/pin
                pager->pin_root(root_id, root_handle);
                pager->mark_dirty(root_id);

            } else {
                // 1. Synchronously load Meta Page and VALIDATE
                Page* meta_page = pager->get_page(0);
                PageHeader* meta_header = reinterpret_cast<PageHeader*>(meta_page->data);
                
                // CHECK MAGIC FIRST (Verification)
                if (meta_header->magic != MAGIC_META) {
                    throw std::runtime_error("File is not a valid database: Magic Number Mismatch on Page 0");
                }

                // THEN CHECK TYPE (Logic)
                if (meta_header->node_type != NODE_META) {
                    throw std::runtime_error("Page 0 is not a Metadata page!");
                }

                this->root_page_id = meta_header->pointer.root_page_id;
                pager->pin_root(this->root_page_id, pager->get_page_shared(this->root_page_id));
            };
        }


        Pager* get_pager();

        PageTask insert(uint32_t key, const char* value);

        void render_tree_mermaid();

        void print_mermaid_recursive(uint32_t page_id);

        bool is_page_locked(uint32_t page_id);

        void register_waiting_coroutine(uint32_t page_id, std::coroutine_handle<> h);

        void release_latch(uint32_t page_id);

        void validate_tree(uint32_t page_id, int depth);
        
        PageTask insert_async(uint32_t key, const std::string_view value);

        void update_parent(uint32_t parent_id, SplitResult result);

        PageTask handle_split_node(uint32_t leaf_id, LeafNode& leaf, uint32_t key, std::string_view value);

        CursorTask find_async(uint32_t key);

        void increment_total_count_sync();

        uint32_t get_total_count();

        VoidTask scan_records_async(uint32_t start_page_id);

        uint32_t create_new_root(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id);
        
    
        PageTask update_parent_async(uint32_t parent_id, SplitResult result);

        PageTask create_new_root_async(uint32_t page_id, uint32_t split_key, uint32_t new_page_id);

        VoidTask process_records_async(uint32_t start_leaf_id);

        uint32_t get_root_id() const { return root_page_id; }

        friend class Cursor;
        friend class BaseNode;


    private:
        uint32_t find_leaf(uint32_t page_id, uint32_t key);
        
        LeafSearchTask find_leaf_async(uint32_t root_id, uint32_t key);
        
        void increment_total_count();
};


#endif