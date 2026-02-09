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