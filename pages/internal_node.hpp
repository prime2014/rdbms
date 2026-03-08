#ifndef INTERNAL_NODE_HPP
#define INTERNAL_NODE_HPP

#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include "node.hpp"

class Pager;
struct PageAwaiter;
struct SplitTask;

// Header: 0-15 (Standard Node Headers), 16-19 (Leftmost Child), 20+ (Cells)

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
    SplitResult perform_internal_split_logic(uint32_t incoming_key, uint32_t incoming_child_id, InternalNode& sibling, Pager& pager);

public:
    InternalNode(Page *p, uint32_t id) : Node(p, id) {};

    uint32_t get_leftmost_child();

    void set_leftmost_child(uint32_t id);

    uint32_t get_child_for_key(uint32_t key);

    PageAwaiter initialize_as_root(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id);
    
    SplitResult split_and_insert(SplitResult result, Pager& pager);

    SplitTask split_and_insert_internal_async(uint32_t split_key, uint32_t new_child_id, Pager& pager);

    uint32_t get_child(uint32_t child_idx);

    void set_child(uint32_t child_idx, uint32_t child_id);

    uint32_t get_key(uint32_t key_idx);

    void set_key(uint32_t key_idx, uint32_t key);

    void insert_child(uint32_t split_key, uint32_t new_child_page_id);

    void set_node_type(NodeType type);

    void set_parent(uint32_t id);

    uint32_t get_parent();

    bool is_root() const;

    Page* get_page();
    
};


#endif