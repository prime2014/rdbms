#include "pages/pager.hpp"
#include <iostream>
#include <cstring>


int main() {
    Pager pager("data/test.db");

    // 2. Prepare a page with data
    auto page_to_write = std::make_unique<Page>();

    std::memset(page_to_write->data, 0, PAGE_SIZE);
    std::strcpy(page_to_write->data, "Hello RDBMS!");

    //3. Write to Page 1
    std::cout << "Writing to Page 1..." << std::endl;
    pager.write_page(1, *page_to_write);

    //4. Read back from page1
    std::cout << "Reading from page 1..." << std::endl;
    std::unique_ptr<Page> read_back = pager.read_page(1);

    std::cout << "Data recovered: " << read_back->data << std::endl;

}