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
    // 1. Move the OLD data out of Page 0 into a brand new page
    uint32_t left_child_id = pager->get_unused_page_number();
    auto left_child_handle = pager->read_page(left_child_id);
    
    // Copy the contents of Page 0 to the new page
    std::memcpy(left_child_handle->data, old_root.get_page()->data, PAGE_SIZE);
    
    InternalNode left_child(left_child_handle.get(), left_child_id);
    left_child.set_is_root(false);
    left_child.set_parent(0); // It now points back to Page 0

    // 2. Re-initialize Page 0 as the NEW Root
    // We don't change root_page_id; it stays 0.
    auto root_handle = old_root.get_page(); // This IS Page 0
    
    // Clear node-specific header bits (but be careful of byte 4-7 if you store row count there!)
    InternalNode new_root(root_handle, 0);
    new_root.set_node_type(NODE_INTERNAL);
    new_root.set_is_root(true);
    new_root.set_key_count(1);

    // 3. Link the new children
    new_root.set_child(0, left_child_id);       // The old data we just moved
    new_root.set_key(0, internal_split.split_key);
    new_root.set_right_child(internal_split.new_page_id);

    // 4. Update Sibling's parent to Page 0
    auto sibling_handle = pager->read_page(internal_split.new_page_id);
    InternalNode sibling(sibling_handle.get(), internal_split.new_page_id);
    sibling.set_parent(0);

    // 5. Persist
    pager->write_page(left_child_id, *left_child_handle);
    pager->write_page(internal_split.new_page_id, *sibling_handle);
    pager->write_page(0, *root_handle);

}