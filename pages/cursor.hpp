#ifndef CURSOR_HPP
#define CURSOR_HPP

#include "table.hpp"

class Cursor {
private:
    Table* table;
    uint32_t current_leaf_id;
    uint32_t current_cell_index;
    bool end_of_table;

public:
    // Constructor remains simple, but note that find_leaf must be async
    Cursor(Table* t) : table(t), current_leaf_id(0), current_cell_index(0), end_of_table(false) {}

    void initialize_at_key(uint32_t leaf_id, uint32_t key, std::shared_ptr<Page> initial_page);

    PageTask next_async(uint32_t& out_key, char* out_value);

    PageTask seek_async(uint32_t start_key);
};

#endif