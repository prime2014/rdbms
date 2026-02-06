#include <iostream>
#include <vector>
#include <string>
#include "pages/pager.hpp"
#include "pages/leaf_node.hpp"

// A helper to make 32-byte data blocks easily
std::vector<char> format_data(const std::string& text) {
    std::vector<char> buffer(32, 0); // Initialize with 32 zeros
    size_t length = std::min(text.length(), (size_t)31); // Leave room for \0
    for (size_t i = 0; i < length; ++i) {
        buffer[i] = text[i];
    }
    return buffer;
}

int main() {
    std::string filename = "test_db.bin";
    Pager pager(filename);

    // 1. Get Page 0 and set it up as a Leaf Node
    auto page0 = pager.read_page(0);
    LeafNode leaf(page0.get());

    // Initialize Header (If it's a new file)
    leaf.set_node_type(1);  // 1 = Leaf
    leaf.set_is_root(1);   
    leaf.set_key_count(0);

    // 2. Insert out of order to test the "Shifting" and "Carving"
    std::cout << "Inserting ID 20...\n";
    leaf.insert(20, format_data("Bob").data());

    std::cout << "Inserting ID 10 (Should shift Bob to the right)...\n";
    leaf.insert(10, format_data("Alice").data());

    std::cout << "Inserting ID 30...\n";
    leaf.insert(30, format_data("Charlie").data());

    // 3. Write back to disk
    pager.write_page(0, *page0);
    std::cout << "Saved to disk.\n\n";

    // 4. Verify by reading from memory
    uint32_t count = leaf.get_key_count();
    std::cout << "Database contains " << count << " records:\n";

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key = leaf.get_key(i);
        char* value = leaf.get_value(i);
        std::cout << "Cell " << i << ": [ID: " << key << "] [Data: " << value << "]\n";
    }

    return 0;
}