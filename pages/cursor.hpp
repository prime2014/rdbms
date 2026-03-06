#include "table.hpp"
#include <cstdint>
#include <cstring>

class Table;

class Cursor {
private:
    Table* table;
    uint32_t current_leaf_id;
    uint32_t current_cell_index;
    bool end_of_table;

public:
    Cursor(Table* t, uint32_t start_key) : table(t), end_of_table(false) {
        current_leaf_id = table->find_leaf(table->root_page_id, start_key);
        
        // Use the pager directly from the table (now accessible via friend or getter)
        auto page = table->pager->get_page(current_leaf_id);
        LeafNode leaf(page.get(), current_leaf_id);
        current_cell_index = leaf.find_insertion_index(start_key);
    }

    // Returns true if more data exists
    bool next(uint32_t& out_key, char* out_value) {
        if (end_of_table) return false;

        auto page = table->pager->get_page(current_leaf_id);
        LeafNode leaf(page.get(), current_leaf_id);

        // If we ran out of cells in this leaf, move to next page
        if (current_cell_index >= leaf.get_num_cells()) {
            current_leaf_id = leaf.get_next_page();
            if (current_leaf_id == 0) {
                end_of_table = true;
                return false;
            }
            current_cell_index = 0;
            return next(out_key, out_value); // Recursive step to load next page
        }

        // Extract current data
        out_key = leaf.get_key(current_cell_index);
        std::memcpy(out_value, leaf.get_value(current_cell_index), 32);

        current_cell_index++;
        return true;
    }
};