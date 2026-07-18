#include <cstdint>
#include <iostream>
#include <memory>
#include "../pages/table.hpp"
#include "../pages/cursor.hpp"
#include "../pages/internal_node.hpp"
#include "../pages/cursor_task.hpp"
#include "../pages/wal_debug.hpp"
#include "./leafcell.cpp"


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
        LeafNode node(page_handle, current_id);

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
    
    FoundLeaf leaf_info = co_await find_leaf_async(root_page_id, key);

    uint32_t leaf_id = leaf_info.id;
    std::shared_ptr<Page> leaf_page = leaf_info.page;

    // 2. Handle pinning (Note: if you are using async, pinning becomes less relevant
    //    because the cache handles page residency)
    LeafNode leaf(leaf_page, leaf_id);
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
    increment_total_count_sync();
            
    co_return leaf_page;
};


void Table::increment_total_count_sync() {
    // Get Page 0 (Header)
    Page* page_header = pager->get_page(0); 

    PageHeader* header = reinterpret_cast<PageHeader*>(page_header->data);

    header->total_count++;

    // Just mark it dirty; let the final flush handle the disk I/O
    pager->mark_dirty(0);
}


VoidTask Table::process_records_async(uint32_t start_leaf_id) {
    uint32_t current_id = start_leaf_id;

    while (current_id != 0) {
        // 1. Asynchronously fetch the page. 
        // The event loop is free to handle other inserts/flushes while we wait.
        std::shared_ptr<Page> page_handle = co_await pager->get_page_async(current_id);
        LeafNode node(page_handle, current_id);

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
    InternalNode internal(page_handle, page_id);
    uint32_t child_id = internal.get_child_for_key(key);
    return find_leaf(child_id, key);
};


// LeafSearchTask Table::find_leaf_async(uint32_t root_id, uint32_t key) {
//     uint32_t current_id = root_id;
//     auto page_handle = pager->get_page_shared(current_id);

//     while (true) {
//         Node node(page_handle, current_id);
//         if (node.get_node_type() == NODE_LEAF) {
//             // Pack both into the struct
//             co_return FoundLeaf{ page_handle, current_id }; 
//         }

//         InternalNode internal(page_handle, current_id);
//         current_id = internal.get_child_for_key(key);
        
//         // This still returns shared_ptr<Page>, which is fine!
//         page_handle = co_await pager->get_page_async(current_id);
//     }
// }


// LeafSearchTask Table::find_leaf_async(uint32_t root_id, uint32_t key) {
//     uint32_t current_id = root_id;
//     auto page_handle = pager->get_page_shared(current_id);

//     while (true) {
//         Node node(page_handle, current_id);
//         if (node.get_node_type() == NODE_LEAF) {
//             co_return FoundLeaf{ page_handle, current_id }; 
//         }

//         InternalNode internal(page_handle, current_id);
//         current_id = internal.get_child_for_key(key);
        
//         // Clean, public API check!
//         if (pager->is_page_cached(current_id)) {
//             page_handle = pager->get_page_shared(current_id);
//         } else {
//             page_handle = co_await pager->get_page_async(current_id);
//         }
//     }
// }


LeafSearchTask Table::find_leaf_async(uint32_t root_id, uint32_t key) {
    uint32_t current_id = root_id;
    std::shared_ptr<Page> page_handle;

    while (true) {
        page_handle = co_await pager->get_page_async(current_id);
        
        Node node(page_handle, current_id);
        if (node.get_node_type() == NODE_LEAF) {
            co_return FoundLeaf{ page_handle, current_id }; 
        }

        InternalNode internal(page_handle, current_id);
        current_id = internal.get_child_for_key(key);
    }
}


void Table::increment_total_count() {
    auto root_page = pager->read_page(0);
    Page* meta_page = root_page.get();

    PageHeader* meta_header = reinterpret_cast<PageHeader*>(meta_page->data);
    meta_header->total_count++;
    pager->mark_dirty(0);
};


CursorTask Table::find_async(uint32_t key) {
    // 2. Perform the async search for the leaf
    FoundLeaf page_info = co_await find_leaf_async(root_page_id, key);

    uint32_t leaf_id = page_info.id;
    std::shared_ptr<Page> page = page_info.page;
    
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
    InternalNode parent(parent_handle, parent_id);


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


// void Table::update_parent(uint32_t parent_id, SplitResult result) {
//     // 1. Get the parent synchronously from the Buffer Pool
//     wal.log(WALOp::PAGE_READ_REQ, result.split_key, parent_id);
//     Page* parent_ptr = pager->get_page(parent_id);
//     InternalNode parent(parent_ptr, parent_id);

//     if (parent.get_key_count() < INTERNAL_NODE_MAX_CELLS) {
//         // Simple case: Memory-only modification
//         parent.insert_child(result.split_key, result.new_page_id);
//         pager->mark_dirty(parent_id);
//     } else {
//         // Recursive case: Still synchronous memory operations
//         SplitResult parent_split = parent.split_and_insert_internal(result.split_key, result.new_page_id, *pager);

//         uint32_t grandparent_id = parent.get_parent();
//         if (grandparent_id == 0) {
//             // Root split: synchronously handle root creation
//             create_new_root(parent_id, parent_split.split_key, parent_split.new_page_id);
//         } else {
//             // Recursive call: propagate up the tree synchronously
//             update_parent(grandparent_id, parent_split);
//         }
//     }
// }


void Table::update_parent(uint32_t parent_id, SplitResult result) {
    if (parent_id == 0) {
        std::cout << "WARNING: update_parent called with parent_id == 0" << std::endl;
        return;
    }

    std::cout << "DEBUG: Updating parent " << parent_id 
              << " with split key " << result.split_key << std::endl;

    // FIX 1: Use shared handle instead of raw pointer
    auto parent_handle = pager->get_page_shared(parent_id);
    InternalNode parent(parent_handle, parent_id, pager.get()); // Pass pager pointer for internal lookups

    if (parent.get_key_count() < INTERNAL_NODE_MAX_CELLS) {
        parent.insert_child(result.split_key, result.new_page_id);
        pager->mark_dirty(parent_id);
        std::cout << "Inserted into existing internal node" << std::endl;
    } else {
        std::cout << "DEBUG: Internal node " << parent_id << " is full → splitting it" << std::endl;

        SplitResult parent_split = parent.split_and_insert_internal(result.split_key, result.new_page_id, *pager);

        uint32_t grandparent_id = parent.get_parent();

        if (grandparent_id == 0) {
            std::cout << "DEBUG: Root internal node split detected. Creating new root." << std::endl;
            uint32_t new_root_id = create_new_root(parent_id, parent_split.split_key, parent_split.new_page_id);
            
            // FIX 2: Use shared handle for Meta Page (Page 0)
            auto meta_handle = pager->get_page_shared(0);
            PageHeader* meta_header = reinterpret_cast<PageHeader*>(meta_handle->data); 
            meta_header->pointer.root_page_id = new_root_id;
            
            this->root_page_id = new_root_id; // Sync in-memory state
            pager->mark_dirty(0);
        } else {
            update_parent(grandparent_id, parent_split);
        }
    }
}

PageTask Table::create_new_root_async(uint32_t left_child_id, uint32_t split_key, uint32_t right_child_id) {
    uint32_t new_root_id = pager->get_unused_page_number();
    
    // Use shared_ptr throughout to keep memory pinned during suspensions
    std::shared_ptr<Page> root_page_handle = co_await pager->get_page_async(new_root_id);
    std::shared_ptr<Page> left_handle = co_await pager->get_page_async(left_child_id);
    std::shared_ptr<Page> right_handle = co_await pager->get_page_async(right_child_id);

    // 1. Initialize the new root (Verification + Logic)
    // Pass the shared_ptr, not the raw pointer!
    InternalNode new_root(root_page_handle, new_root_id); 
    new_root.set_node_type(NODE_INTERNAL); // This sets MAGIC_INTERNAL
    new_root.set_is_root(true);
    new_root.initialize_as_root(left_child_id, split_key, right_child_id);

    // 2. Update Children
    Node left_node(left_handle, left_child_id);
    Node right_node(right_handle, right_child_id);
    
    left_node.set_parent(new_root_id);
    left_node.set_is_root(false); // No longer root
    
    right_node.set_parent(new_root_id);
    right_node.set_is_root(false);

    // 3. Persist change to Metadata (Page 0)
    this->root_page_id = new_root_id;
    std::shared_ptr<Page> meta_handle = co_await pager->get_page_async(0);
    PageHeader* meta_h = reinterpret_cast<PageHeader*>(meta_handle->data);
    meta_h->pointer.root_page_id = new_root_id;

    // 4. Mark everything dirty
    pager->mark_dirty(new_root_id);
    pager->mark_dirty(left_child_id);
    pager->mark_dirty(right_child_id);
    pager->mark_dirty(0);

    // Optional: Pin the new root in the Pager
    pager->pin_root(new_root_id, root_page_handle);

    co_return root_page_handle;
}



// uint32_t Table::create_new_root(uint32_t old_root_id, uint32_t split_key, uint32_t new_leaf_id) {
//     // 1. Copy old root data to a fresh left child
//     uint32_t left_child_id = pager->get_unused_page_number();
//     Page* left_ptr = pager->get_page(left_child_id);
//     Page* old_root_ptr = pager->get_page(old_root_id);
//     std::memcpy(left_ptr->data, old_root_ptr->data, PAGE_SIZE);

//     // Update Left Child Header (no longer a root)
//     PageHeader* left_h = reinterpret_cast<PageHeader*>(left_ptr->data);
//     left_h->is_root = 0;
//     left_h->parent_id = old_root_id; 
//     pager->mark_dirty(left_child_id);

//     // 2. Clear and transform the old root page into an INTERNAL node
//     std::memset(old_root_ptr->data, 0, PAGE_SIZE);
//     PageHeader* root_h = reinterpret_cast<PageHeader*>(old_root_ptr->data);

//     root_h->node_type = NODE_INTERNAL;
//     root_h->is_root = 1;
//     root_h->key_count = 1;
//     root_h->pointer.left_child_id = left_child_id;

//     // 3. Serialize the pivot key and right child
//     char* cell = old_root_ptr->data + sizeof(PageHeader);
//     serialize_uint32(split_key, cell);
//     serialize_uint32(new_leaf_id, cell + 4);

//     // 4. Fix Right Child Header
//     PageHeader* right_h = reinterpret_cast<PageHeader*>(pager->get_page(new_leaf_id)->data);
//     right_h->parent_id = old_root_id;
//     right_h->is_root = 0;
//     pager->mark_dirty(new_leaf_id);

//     pager->mark_dirty(old_root_id);
//     return old_root_id; // Still the same page ID, but new content
// }


uint32_t Table::create_new_root(uint32_t old_root_id, uint32_t split_key, uint32_t new_leaf_id) {
    // 1. Get a fresh page for the left child
    uint32_t left_child_id = pager->get_unused_page_number();
    auto left_handle = pager->get_page_shared(left_child_id);
    auto old_root_handle = pager->get_page_shared(old_root_id);

    // Copy old root data to left child
    std::memcpy(left_handle->data, old_root_handle->data, PAGE_SIZE);

    // Update Left Child via Node wrapper to ensure Magic Number/Header consistency
    Node left_node(left_handle, left_child_id, pager.get());
    left_node.set_is_root(false);
    left_node.set_parent(old_root_id);
    pager->mark_dirty(left_child_id);

    // 2. Transform the old root page into an INTERNAL node
    // Instead of memset, use init_new_node which handles the Magic Number
    InternalNode root_node(old_root_handle, old_root_id, pager.get());
    root_node.init_new_node(NODE_INTERNAL, true); // Sets type, root=true, and MAGIC_NUMBER

    // 3. Set up the internal structure
    root_node.set_leftmost_child(left_child_id);
    root_node.insert_child(split_key, new_leaf_id); // This increments key_count automatically

    // 4. Update the Right Child's parent pointer
    auto right_handle = pager->get_page_shared(new_leaf_id);
    Node right_node(right_handle, new_leaf_id, pager.get());
    right_node.set_parent(old_root_id);
    right_node.set_is_root(false);
    
    pager->mark_dirty(new_leaf_id);
    pager->mark_dirty(old_root_id);

    return old_root_id; 
}


void Table::validate_tree(uint32_t page_id, int depth) {
    if (page_id == 0) return; // Safety check for null pointers in the tree

    // FIX 1: Retrieve the shared handle to keep the page alive during recursion
    auto page_handle = pager->get_page_shared(page_id);
    Node node(page_handle, page_id, pager.get());
    
    std::string indent(depth * 2, ' ');
    uint8_t type = node.get_node_type();

    std::cout << indent << (type == NODE_LEAF ? "[Leaf] " : "[Internal] ")
              << "Page: " << page_id 
              << " | Keys: " << node.get_key_count() 
              << " | Parent: " << node.get_parent() << std::endl;

    if (type == NODE_INTERNAL) {
        // FIX 2: Use the same shared handle to create the InternalNode wrapper
        InternalNode internal(page_handle, page_id, pager.get());
        
        // Recursively visit children
        validate_tree(internal.get_leftmost_child(), depth + 1);
        
        uint32_t key_count = internal.get_key_count();
        for (uint32_t i = 0; i < key_count; ++i) {
            validate_tree(internal.get_child_at(i), depth + 1);
        }
    }
}

bool Table::is_page_locked(uint32_t page_id) {
    return split_latches.find(page_id) != split_latches.end();
}

// Register a coroutine to be woken up when the split finishes
void Table::register_waiting_coroutine(uint32_t page_id, std::coroutine_handle<> h) {
    split_latches[page_id].push_back(h);
}


void Table::release_latch(uint32_t page_id) {
    auto it = split_latches.find(page_id);
    if (it != split_latches.end()) {
        std::vector<std::coroutine_handle<>> waiters = std::move(it->second);
        split_latches.erase(it);
        for (auto h : waiters) {
            if (h) h.resume();
        }
    }
}


// Table.cpp
PageTask Table::handle_split_node(uint32_t leaf_id, LeafNode& leaf, uint32_t key, std::string_view value) {
    std::cout << "DEBUG: Node [Split]: Starting split on page " << leaf_id << std::endl;

    // 1. Perform the physical split (Memory Mutation Node)
    // Pass value.data() safely here if split_and_insert_async expects a const char*
    SplitResult result = co_await leaf.split_and_insert_async(key, value.data(), *pager);

    // 2. Handle Parent/Root Adjustment (Structural Node)
    bool was_root_split = (leaf_id == this->root_page_id);
    if (was_root_split) {
        uint32_t new_root_id = create_new_root(leaf_id, result.split_key, result.new_page_id);
        
        Page* meta_page = pager->get_page(0);
        PageHeader* root_ptr = reinterpret_cast<PageHeader*>(meta_page->data); 
        root_ptr->pointer.root_page_id = new_root_id;
        
        this->root_page_id = new_root_id; 
        pager->mark_dirty(0);
    } else {
        uint32_t parent_id = leaf.get_parent();
        if (parent_id != 0) {
            update_parent(parent_id, result);
        }
    }

    // 3. Finalize Memory (Re-discovery Node)
    FoundLeaf re_find = co_await find_leaf_async(this->root_page_id, key);
    
    co_return re_find.page;
}

PageTask Table::insert_async(uint32_t key, const std::string_view value) {
    std::cout << "[INSERT START] Key: " << key << " | Root: " << this->root_page_id << std::endl;

    // NODE 1: DISCOVERY
    std::cout << "[DEBUG] Finding leaf for key: " << key << "..." << std::endl;
    FoundLeaf leaf_info = co_await find_leaf_async(this->root_page_id, key);
    std::cout << "[DEBUG] Found leaf ID: " << leaf_info.id << " for key: " << key << std::endl;

    LeafNode leaf(leaf_info.page, leaf_info.id);
    std::shared_ptr<Page> final_page;

    // NODE 2: CONDITIONAL MUTATION
    if (leaf.get_key_count() < LEAF_NODE_MAX_CELLS) {
        std::cout << "[DEBUG] Normal Insert: Page " << leaf_info.id << " has " 
                  << (int)leaf.get_key_count() << " cells. Adding key: " << key << std::endl;
        
        leaf.insert(key, value.data(), *pager);
        pager->mark_dirty(leaf_info.id);
        final_page = leaf_info.page;
    } else {
        std::cout << "[DEBUG] SPLIT REQUIRED: Page " << leaf_info.id << " is full. Key: " << key << std::endl;
        final_page = co_await handle_split_node(leaf_info.id, leaf, key, value);
        std::cout << "[DEBUG] Split complete for key: " << key << std::endl;
    }

    // NODE 3: PERSISTENCE BARRIER
    std::cout << "[BARRIER] Initiating flush for key: " << key << "..." << std::endl;
    co_await pager->flush_all_dirty_async();
    std::cout << "[BARRIER] Flush confirmed. Key " << key << " is now persistent." << std::endl;

    std::cout << "[INSERT COMPLETE] Key: " << key << std::endl;
    co_return final_page;
}




// Add to Table.cpp
void Table::render_tree_mermaid() {
    std::cout << "\n--- COPY INTO MERMAID LIVE EDITOR ---" << std::endl;
    std::cout << "graph TD" << std::endl;
    print_mermaid_recursive(root_page_id);
    std::cout << "--------------------------------------" << std::endl;
}


void Table::print_mermaid_recursive(uint32_t page_id) {
    if (page_id == 0) return; // Basic safety check

    // FIX 1: Use get_page_shared to keep the page memory safe during recursion
    auto page_handle = pager->get_page_shared(page_id);
    Node node(page_handle, page_id, pager.get());
    
    uint8_t type = node.get_node_type();

    std::cout << "   P" << page_id << "[\"Page " << page_id 
              << (type == NODE_LEAF ? " (Leaf)" : " (Int)")
              << " <br/> Keys: ";
    
    uint32_t key_count = node.get_key_count();
    for (uint32_t i = 0; i < key_count; ++i) {
        uint32_t key;
        if (type == NODE_INTERNAL) {
            // FIX 2: Use the shared_ptr handle for specialized node wrappers
            InternalNode internal(page_handle, page_id, pager.get());
            key = internal.get_key(i);
        } else {
            // FIX 3: Ensure LeafNode constructor also accepts shared_ptr<Page>
            LeafNode leaf(page_handle, page_id, pager.get());
            key = leaf.get_key(i);
        }
        std::cout << key << (i == key_count - 1 ? "" : ", ");
    }

    std::cout << "\"]" << std::endl;

    if (type == NODE_INTERNAL) {
        InternalNode internal(page_handle, page_id, pager.get());
        
        // Link Leftmost child
        uint32_t left = internal.get_leftmost_child();
        if (left != 0) {
            std::cout << "   P" << page_id << " -- Left --> P" << left << std::endl;
            print_mermaid_recursive(left);
        }

        // Link subsequent children
        for (uint32_t i = 0; i < internal.get_key_count(); ++i) {
            uint32_t child = internal.get_child(i);
            if (child != 0) {
                std::cout << "   P" << page_id << " -- " << internal.get_key(i) << " --> P" << child << std::endl;
                print_mermaid_recursive(child);
            }
        }
    }
}