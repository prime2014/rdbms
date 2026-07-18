#ifndef LEAF_NODE_HPP
#define LEAF_NODE_HPP

#include <cstdint>
#include "page.hpp"
#include "node.hpp"
#include <string_view>

class Pager;
struct SplitTask;

class LeafNode: public Node {
    public:
        LeafNode(std::shared_ptr<Page> p, uint32_t id, Pager* pg = nullptr) : Node(p, id, pg) {};

        char* cell_address(uint32_t cell_num);

        uint32_t get_num_cells();
        uint32_t get_parent();

        uint32_t find_insertion_index(uint32_t key);

        uint32_t get_key_at_index(uint32_t mid);

        uint32_t perform_memory_split(uint32_t key, std::string_view value, LeafNode& new_leaf);

        uint32_t get_key(uint32_t cell_num);

        uint32_t get_next_page();

        void set_next_page(uint32_t page_id);

        void set_key(uint32_t cell_num, uint32_t key);

        char* get_value(uint32_t cell_num);

        void set_value(uint32_t cell_num, const char* value_ptr);

        SplitResult split_and_insert(uint32_t key, const char* value, Pager& pager);

        SplitResult insert(uint32_t key, const char* value, Pager& pager);

        SplitTask split_and_insert_async(uint32_t key, std::string_view value, Pager& pager);

    
};


#endif