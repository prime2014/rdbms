#include <cstdint>
#include <iostream>
#include <memory>
#include "../pages/table.hpp"
#include "../pages/cursor.hpp"
#include "../pages/internal_node.hpp"
#include "../pages/cursor_task.hpp"


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



// Change return type to a Task that can be awaited
VoidTask Table::scan_records_async(uint32_t start_page_id) {
    uint32_t current_id = start_page_id;

    while (current_id != 0) {
        // 1. Await the page fetch. This yields the coroutine if not in cache.
        auto page_handle = co_await pager->get_page_async(current_id);
        LeafNode node(page_handle.get(), current_id);

        // 2. Lookahead: Trigger async load for the NEXT page
        uint32_t next_id = node.get_next_page();
        if (next_id != 0 && !pager->is_in_memory(next_id)) {
            // Non-awaiting call to start the disk read in the background
            pager->async_load_from_disk(next_id); 
        }

        // 3. Process the current node
        co_await process_records_async(current_id); 
        
        current_id = next_id;
    }
}

Pager* Table::get_pager() {
    return this->pager.get();
}

PageTask Table::insert(uint32_t key, const char* value) {
            // 1. Find leaf asynchronously
    uint32_t leaf_id;
    std::shared_ptr<Page> leaf_page = co_await find_leaf_async(root_page_id, key, leaf_id);

    // 2. Handle pinning (Note: if you are using async, pinning becomes less relevant
    //    because the cache handles page residency)
    LeafNode leaf(leaf_page.get(), leaf_id);
    uint32_t parent_id = leaf.get_parent();

    // 3. Insert or Split
    if (leaf.get_key_count() >= LEAF_NODE_MAX_CELLS) {
        SplitResult result = co_await leaf.split_and_insert_async(key, value, *pager);
        if (parent_id == 0) { 
            co_await create_new_root_async(leaf_id, result.split_key, result.new_page_id);
        } else {
            co_await update_parent_async(parent_id, result);
        }
    } else {
        leaf.insert(key, value, *pager);
        // Ensure the change is flushed to disk
        co_await pager->flush_page_async(leaf_id);
    }

            // 4. Update the global count
    co_await increment_total_count_async();
            
    co_return leaf_page;
};


VoidTask Table::increment_total_count_async() {
   std::shared_ptr<Page> page_header =  co_await this->pager->get_page_async(0);

   uint32_t current_count = 0;

   std::memcpy(&current_count, &page_header->data[TABLE_TOTAL_COUNT_OFFSET], sizeof(uint32_t));

   current_count++;

   std::memcpy(&page_header->data[TABLE_TOTAL_COUNT_OFFSET], &current_count, sizeof(uint32_t));

   co_await this->pager->flush_page_async(0);

   co_return;
    
}


VoidTask Table::process_records_async(uint32_t start_leaf_id) {
    uint32_t current_id = start_leaf_id;

    while (current_id != 0) {
        // 1. Asynchronously fetch the page. 
        // The event loop is free to handle other inserts/flushes while we wait.
        std::shared_ptr<Page> page_handle = co_await pager->get_page_async(current_id);
        LeafNode node(page_handle.get(), current_id);

        uint32_t num_cells = node.get_num_cells();
        
        // 2. Process records (Synchronous CPU work)
        for (uint32_t i = 0; i < num_cells; i++) {
            uint32_t key = node.get_key(i);
            char* value = node.get_value(i);
            std::cout << "ID: " << key << " | Data: " << value << std::endl;
        }

        // 3. Move to the next page
        current_id = node.get_next_page(); 
    }
    
    co_return;
};

uint32_t Table::find_leaf(uint32_t page_id, uint32_t key) {
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

PageTask Table::find_leaf_async(uint32_t root_id, uint32_t key, uint32_t& out_leaf_id) {
    uint32_t current_id = root_id;
    while (true) {
        std::shared_ptr<Page> page = co_await pager->get_page_async(current_id);
                
        if (page->data[NODE_TYPE_OFFSET] == 1) { // Leaf
            out_leaf_id = current_id;
            co_return page;
        }

        InternalNode internal(page.get(), current_id);
        current_id = internal.get_child_for_key(key);
    }
}


void Table::increment_total_count() {
    uint32_t count = get_total_count();
    auto root_page = pager->read_page(0);
    serialize_uint32(count + 1, root_page->data + TABLE_TOTAL_COUNT_OFFSET);
    pager->write_page(0, *root_page);
};


CursorTask Table::find_async(uint32_t key) {
    // 2. Perform the async search for the leaf
    uint32_t leaf_id = 0;
    std::shared_ptr<Page> page = co_await find_leaf_async(root_page_id, key, leaf_id);
    
    // 3. Initialize the cursor now that we have the starting page
    Cursor cursor(this);
    
    // 4. Use a private helper to set the internal cursor state
    // This avoids performing I/O in the constructor
    cursor.initialize_at_key(leaf_id, key, page);
    
    co_return cursor;
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


void Table::update_parent(uint32_t parent_id, SplitResult result) {
    // 1. Get the parent synchronously from the Buffer Pool
    Page* parent_ptr = pager->get_page(parent_id);
    InternalNode parent(parent_ptr, parent_id);

    if (parent.get_key_count() < INTERNAL_NODE_MAX_CELLS) {
        // Simple case: Memory-only modification
        parent.insert_child(result.split_key, result.new_page_id);
        pager->mark_dirty(parent_id);
    } else {
        // Recursive case: Still synchronous memory operations
        SplitResult parent_split = parent.split_and_insert_internal(result.split_key, result.new_page_id, *pager);

        uint32_t grandparent_id = parent.get_parent();
        if (grandparent_id == 0) {
            // Root split: synchronously handle root creation
            create_new_root(parent_id, parent_split.split_key, parent_split.new_page_id);
        } else {
            // Recursive call: propagate up the tree synchronously
            update_parent(grandparent_id, parent_split);
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


void Table::create_new_root(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id) {
    uint32_t new_root_id = pager->get_unused_page_number();
    
    // These now return a pointer immediately from your internal cache map
    Page* root_ptr = pager->get_page(new_root_id); 
    Page* left_ptr = pager->get_page(left_child_id);
    Page* right_ptr = pager->get_page(right_child_id);

    InternalNode new_root(root_ptr, new_root_id);
    Node left_node(left_ptr, left_child_id);
    Node right_node(right_ptr, right_child_id);
    
    left_node.set_parent(new_root_id);
    right_node.set_parent(new_root_id);
    left_node.set_is_root(0); 

    new_root.initialize_as_root(left_child_id, split_key, right_child_id);
    this->root_page_id = new_root_id;

    // Mark as dirty instead of flushing synchronously
    pager->mark_dirty(new_root_id);
    pager->mark_dirty(left_child_id);
    pager->mark_dirty(right_child_id);
}


uint32_t Table::get_total_count() {
    auto root_page = pager->read_page(0);
    return deserialize_uint32(root_page->data + 4);
}



PageTask Table::insert_async(uint32_t key, const char* value) {
    uint32_t leaf_id;


    std::shared_ptr<Page> leaf_page = co_await find_leaf_async(root_page_id, key, leaf_id);

    LeafNode leaf(leaf_page.get(), leaf_id);
    uint32_t parent_id = leaf.get_parent();

    // 3. Handle the insert/split logic
    if (leaf.get_key_count() >= LEAF_NODE_MAX_CELLS) {
        SplitResult result = co_await leaf.split_and_insert_async(key, value, *pager);

        if (parent_id == 0) { // leaf was root
            create_new_root(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id);
        } else {
            update_parent_async(parent_id, result);
        }

        co_await FlushAwaiter{pager.get(), leaf_id};
    } else {
        leaf.insert(key, value, *pager);

        co_await FlushAwaiter{pager.get(), leaf_id};
    }

    // 4. Update the global count in the header of Page 0
    co_await increment_total_count_async();

    co_return leaf_page;
};