#include <cstdint>


struct io_uring_cqe {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};