#ifndef FOUND_LEAF_HPP
#define FOUND_LEAF_HPP
#include <memory>
#include <cstdint>


class Page;


struct FoundLeaf {
    std::shared_ptr<Page> page;
    uint32_t id;
};

#endif