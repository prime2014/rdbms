#ifndef WAL_DEBUG_HPP
#define WAL_DEBUG_HPP

#include <fcntl.h>
#include <unistd.h>
#include <string>


enum class WALOp { INSERT_START, SPLIT_START, PAGE_ALLOC, PAGE_READ_REQ };

struct WALEntry {
    WALOp op;
    uint32_t key;
    uint32_t page_id;
    uint64_t timestamp;
};

class DebugWAL {
private:
    int fd;
public:
    DebugWAL(const std::string& path) {
        // Open with O_SYNC to ensure every write is on disk before returning
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
    }

    void log(WALOp op, uint32_t key, uint32_t page_id) {
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "OP: %d | KEY: %u | PAGE: %u\n", 
                           (int)op, key, page_id);
        write(fd, buf, len); 
        // No fsync needed because of O_SYNC, but very safe.
    }

    ~DebugWAL() { close(fd); }
};


#endif