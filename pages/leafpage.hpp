#pragma pack(push, 1)
#include "page_header.hpp"
#include "leafcell.hpp"
#include "node.hpp"

struct LeafPageLayout {
    PageHeader header;
    LeafCell cells[LEAF_NODE_MAX_CELLS];
};
#pragma pack(pop)