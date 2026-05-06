#pragma pack(push, 1)
#include <cstdint>
#include "page_header.hpp"
#include "node.hpp"


struct InnerCell {
    uint32_t key;
    uint32_t child_id; // The right child of this key
};

struct InternalPageLayout {
    PageHeader header;
    // The header.pointer.left_child_id handles the very first child
    InnerCell cells[INTERNAL_NODE_MAX_CELLS];
};
#pragma pack(pop)

static_assert(sizeof(InnerCell) == 8, "InternalCell must be exactly 8 bytes");