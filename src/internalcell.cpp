#pragma pack(push, 1)
#include <cstdint>

struct InternalCell {
    uint32_t key;
    uint32_t right_child_id;
};
#pragma pack(pop)