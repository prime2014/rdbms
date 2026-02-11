#include <cstdint>
#include <memory>
#include "pages/table.hpp"


void Table::update_parent(uint32_t parent_id, SplitResult result) {
    auto parent_handle = pager->read_page(parent_id);
    InternalNode parent(parent_handle.get(), parent_id);

    // Check if the internal node has room for one or more [ChildID + Key]
    // Max cells for internal; (4096-20-4 for right child) / 8 = 509 cells
    if(parent.get_key_count() < 500) {
        parent.insert_child(result.split_key, result.new_page_id);
        pager->write_page(parent_id, *parent_handle);
    } else {
        SplitResult internal_split = parent.split_and_insert(result, *pager);

        // 2. If this  was the root, we need a new root
        if (parent.is_root()) {
            process_internal_root_split(parent, internal_split);
        } else {
            update_parent(parent.get_parent(), internal_split);
        }
    }

}


void Table::process_internal_root_split(InternalNode old_root, SplitResult internal_split) {
    uint32_t new_root_id = pager->get_unused_page_number();
    auto new_root_handle =  pager->read_page(new_root_id);

    InternalNode new_root(new_root_handle.get(), new_root_id);
    
    // initialize the root
    new_root.set_node_type(NODE_INTERNAL);
    new_root.set_is_root(true);
    new_root.set_key_count(1);


    // link the children
    new_root.set_child(0, old_root.get_page_id());
    new_root.set_key(0, internal_split.split_key);
    new_root.set_right_child(internal_split.new_page_id);

    // 4. Update the old root's metadata
    old_root.set_is_root(false);
    old_root.set_parent(new_root_id);

    // 5. Update the Sibling's metadata (It needs to know who its new father is)
    auto sibling_handle = pager->read_page(internal_split.new_page_id);
    InternalNode sibling(sibling_handle.get(), internal_split.new_page_id);
    sibling.set_parent(new_root_id);

    // 6. Persist everything
    pager->write_page(new_root_id, *new_root_handle);
    pager->write_page(old_root.get_page_id(), *(old_root.get_page()));
    pager->write_page(internal_split.new_page_id, *sibling_handle);

    // 7. Update the table pointer
    this->root_page_id = new_root_id;

}