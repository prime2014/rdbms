#include <cstdint>
#include <iostream>
#include <memory>
#include "pages/table.hpp"
#include "../pages/cursor.hpp"


// ────────────────────────────────────────────────
//   Portable prefetch helper
// ────────────────────────────────────────────────
#if defined(__GNUC__) || defined(__clang__)
  #define PREFETCH_READ(ptr)   __builtin_prefetch((const void*)(ptr), 0, 3)
  #define PREFETCH_WRITE(ptr)  __builtin_prefetch((const void*)(ptr), 1, 3)
#elif defined(_MSC_VER)
  #include <xmmintrin.h>           // _mm_prefetch
  #define PREFETCH_READ(ptr)   _mm_prefetch((const char*)(ptr), _MM_HINT_T0)
  #define PREFETCH_WRITE(ptr)  _mm_prefetch((const char*)(ptr), _MM_HINT_T0)  // no real write hint
#else
  #define PREFETCH_READ(ptr)   ((void)0)
  #define PREFETCH_WRITE(ptr)  ((void)0)
#endif


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
    uint32_t left_child_id = pager->get_unused_page_number();
    auto left_child_handle = pager->read_page(left_child_id);
    
    std::memcpy(left_child_handle->data, old_root.get_page()->data, PAGE_SIZE);

    InternalNode left_child(left_child_handle.get(), left_child_id);
    left_child.set_is_root(false);
    left_child.set_parent(0); // Page 0 is the root

    // Update parent pointers of ALL children now owned by left_child
    for (uint32_t i = 0; i < left_child.get_key_count(); i++) {
        uint32_t child_page_id = left_child.get_child(i);
        auto child_handle = pager->get_page(child_page_id);
        serialize_uint32(left_child_id, child_handle->data + PARENT_POINTER_OFFSET);
        pager->write_page(child_page_id, *child_handle);
    }
    
    // Fix: Use the Anchor accessor instead of get_right_child
    uint32_t anchor_child = left_child.get_leftmost_child();
    auto anchor_handle = pager->get_page(anchor_child);
    serialize_uint32(left_child_id, anchor_handle->data + PARENT_POINTER_OFFSET);
    pager->write_page(anchor_child, *anchor_handle);

    // 2. Re-initialize Page 0 as the NEW Root
    auto root_handle = old_root.get_page(); 
    InternalNode new_root(root_handle, 0);
    
    // We use our clean initialization logic here
    new_root.set_node_type(NODE_INTERNAL);
    new_root.set_is_root(true);
    new_root.set_key_count(1);
    
    // 3. Link the new children using cell 0
    new_root.set_leftmost_child(left_child_id);      // Left side of split_key
    new_root.set_key(0, internal_split.split_key);
    new_root.set_child(0, internal_split.new_page_id); // Right side of split_key

    // 4. Update Sibling's parent to Page 0
    auto sibling_handle = pager->read_page(internal_split.new_page_id);
    Node sibling(sibling_handle.get(), internal_split.new_page_id);
    sibling.set_parent(0);

    // 5. Persist
    pager->write_page(left_child_id, *left_child_handle);
    pager->write_page(internal_split.new_page_id, *sibling_handle);
    pager->write_page(0, *root_handle);
}


void Table::scan_records(uint32_t start_page_id) {
    uint32_t current_id = start_page_id;

    while (current_id != 0) {
        auto handle = pager->get_page(current_id);

        LeafNode node(handle.get(), current_id);

        // --- THE CPU HINT STEP ---
        uint32_t next_id = node.get_next_page();
        if (next_id != 0) {
            if (pager->is_in_memory(next_id)) {
                // HINT 1: It's in RAM. Tell CPU to pull it into L1 Cache.
                PREFETCH_READ(pager->get_page_ptr(next_id));
            } else {
                // HINT 2: It's on DISK. Tell the OS to start an Asynchronous I/O read.
                // This is the "Software version" of a CPU prefetch for Disk.
                pager->async_load_from_disk(next_id); 
            }
        }

        // Now process the current page. While the CPU does this math,
        // the NEXT page is already flying through the wires toward the CPU.
        process_records(node); 
        
        current_id = next_id;
    }
}


VoidTask Table::increment_total_count_async() {
   std::shared_ptr<Page> page_header =  co_await this->pager->get_page_async(0);

   uint32_t current_count = 0;

   std::memcpy(&current_count, &page_header->data[TABLE_TOTAL_COUNT_OFFSET], sizeof(uint32_t));

   current_count++;

   std::memcpy(&page_header->data[TABLE_TOTAL_COUNT_OFFSET], &current_count, sizeof(uint32_t));

   co_await this->pager->flush_page_async(0);

   co_return;
    
}


void Table::process_records(uint32_t start_leaf_id) {
    uint32_t current_id = start_leaf_id;

    while (current_id != 0) {
        // 1. Get the page from the pager
        auto page_handle = pager->get_page(current_id);
        LeafNode node(page_handle.get(), current_id);

        uint32_t num_cells = node.get_num_cells();
        
        // 2. Process all records in the CURRENT leaf
        for (uint32_t i = 0; i < num_cells; i++) {
            uint32_t key = node.get_key(i);
            char* value = node.get_value(i);
            
            // For testing: output the key and the 32-byte string
            std::cout << "ID: " << key << " | Data: " << value << std::endl;
        }

        // 3. Follow the "Trail" to the next leaf
        current_id = node.get_next_page(); 
    }
}

Cursor Table::find(uint32_t key) {
    return Cursor(this, key); 
}


PageTask Table::update_parent_async(uint32_t parent_id, SplitResult result) {
    // 1. Get the parent page
    std::shared_ptr<Page> parent_handle = co_await pager->get_page_async(parent_id);
    InternalNode parent(parent_handle.get(), parent_id);


    if (parent.get_key_count() < INTERNAL_NODE_MAX_CELLS) {
        // Simple case: Just insert the split_key and new_page_id into the existing parent
        parent.insert_child(result.split_key, result.new_page_id);
        co_await pager->flush_page_async(parent_id);
    } else {
        SplitResult parent_split = co_await parent.split_and_insert_internal_async(
            result.split_key,
            result.new_page_id,
            *pager
        );

        // Recursive call: move up the tree
        uint32_t grandparent_id = parent.get_parent();
        if (grandparent_id == 0) {
            // if the parent was the root, we need a new root
            co_await create_new_root_async(parent_id, parent_split.split_key, parent_split.new_page_id);
        } else {
            co_await update_parent_async(grandparent_id, parent_split);
        }

    }
}


PageTask Table::create_new_root_async(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id) {
    uint32_t new_root_id = pager->get_unused_page_number();
    std::shared_ptr<Page> root_page_handle = co_await pager->get_page_async(new_root_id);

    InternalNode new_root(root_page_handle.get(), new_root_id);

    std::shared_ptr<Page> left_handle = co_await pager->get_page_async(left_child_id);
    std::shared_ptr<Page> right_handle = co_await pager->get_page_async(right_child_id);

    Node left_node(left_handle.get(), left_child_id);
    Node right_node(right_handle.get(), right_child_id);
    
    left_node.set_parent(new_root_id);
    right_node.set_parent(new_root_id);
    left_node.set_is_root(0); // Old root is now just a child

    // This internal method handles setting the anchor, the first key, 
    // and the first child pointer (the right side) correctly.
    new_root.initialize_as_root(left_child_id, split_key, right_child_id);

    this->root_page_id = new_root_id;

    co_await pager->flush_page_async(new_root_id);
    co_await pager->flush_page_async(left_child_id);
    co_await pager->flush_page_async(right_child_id);

    co_return root_page_handle;
}
