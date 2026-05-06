#include "../pages/cursor_task.hpp"
#include "../pages/cursor.hpp"
#include "../pages/pager.hpp"
#include "../pages/leaf_node.hpp"
#include <cstring>

// Now you can define the bodies
void CursorTask::promise_type::return_value(Cursor c) {
    result_cursor = c;
}

Cursor CursorTask::await_resume() {
    return handle.promise().result_cursor.value();
}

// A simple non-blocking state setter
void Cursor::initialize_at_key(uint32_t leaf_id, uint32_t key, std::shared_ptr<Page> initial_page) {
    current_leaf_id = leaf_id;
    LeafNode leaf(initial_page, leaf_id);
    current_cell_index = leaf.find_insertion_index(key);
}

    // Async version of next()
PageTask Cursor::next_async(uint32_t& out_key, char* out_value) {
    while (!end_of_table) {
        std::shared_ptr<Page> page = co_await table->pager->get_page_async(current_leaf_id);
        LeafNode leaf(page, current_leaf_id);
        
        if (current_cell_index < leaf.get_key_count()) {
            out_key = leaf.get_key(current_cell_index);
            std::memcpy(out_value, leaf.get_value(current_cell_index), 32);
            current_cell_index++;
            co_return page;
        }
         // Move to next page
        current_leaf_id = leaf.get_next_page();
        if (current_leaf_id == 0) {
            end_of_table = true;
            co_return nullptr;
        }
        current_cell_index = 0;
    }
    co_return nullptr;
}

PageTask Cursor::seek_async(uint32_t start_key) {
    FoundLeaf result = co_await table->find_leaf_async(table->root_page_id, start_key);
    uint32_t leaf_id = result.id;
    std::shared_ptr<Page> page = result.page;
    
    
    // Now you can safely use page
    LeafNode leaf(page, leaf_id);
    current_cell_index = leaf.find_insertion_index(start_key);
    end_of_table = false;
    
    co_return page;
}