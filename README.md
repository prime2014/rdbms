# Async Database Engine (C++20 / io_uring / Coroutines)

A high-performance, multi-threaded asynchronous B+ Tree database storage engine built from scratch in C++20. This engine leverages Linux `io_uring` for true proactor I/O, C++20 coroutines for zero-overhead async control flow, and a zero-allocation 2MB Hugepage memory pool for physical page isolation.

---

## 🏛️ Architectural Overview
                  +------------------------------------------+
                  |         Application Layer / B+ Tree      |
                  +--------------------+---------------------+
                                       |
                             (Async Coroutines)
                                       |
                  +--------------------+---------------------+
                  |           Pager Engine Subsystem         |
                  +--------------------+---------------------+
                                       |
         +-----------------------------+-----------------------------+
         |                                                           |
         v                                                           v
+--------------------------+                               +--------------------------+
|    Read I/O Consumer     |                               |    Write I/O Consumer    |
| (Dedicated read_ring)    |                               | (Dedicated write_ring)   |
| High Priority / Low Tail |                               | Throughput / Batching    |
+------------+-------------+                               +------------+-------------+
        |                                                           |
        |       +-------------------------------------------+       |
        +-----> | 512MB Buffer Pool (HugepageAllocator)     | <-----+
                | 256x 2MB HugePages -> 131,072 DB Pages    |
                +---------------------+---------------------+
                                      |
                                (Linux io_uring)
                                      |
                +---------------------+---------------------+
                |       Physical Storage / Block NVMe       |
                +-------------------------------------------+


## Key Architectural Features

### 1. Decoupled Read/Write I/O Rings (`io_uring`)
Instead of a monolithic event loop, I/O handling is split across two dedicated `io_uring` ring instances:
* **Read Ring (`read_ring`):** Tuned for sub-millisecond tail latency. Serves point queries and range scans without waiting on background write pipeline contention.
* **Write Ring (`write_ring`):** Tuned for massive pipeline throughput. Handles background dirty-page flushes, node split writes, and multi-page batch completions.

### 2. Physical Backpressure & Ring Safety
To prevent query threads from overflowing submission queues under heavy write pressure, the engine employs strict slot accounting via `std::counting_semaphore`:
* In-flight operations are constrained directly by queue capacity (`MAX_IN_FLIGHT_IOS`).
* Producers block safely before SQE submission if queues are saturated—guaranteeing that `io_uring_get_sqe()` never returns `nullptr`.

### 3. Zero-Allocation Anonymous Hugepage Pool (`HugepageAllocator`)
* Memory is pre-allocated directly from the kernel using 2MB Linux Hugepages (`MAP_HUGETLB | MAP_ANONYMOUS`) to minimize Translation Lookaside Buffer (TLB) thrashing.
* The buffer pool is pinned using `mlock()` to prevent swap evicted latencies.
* Registered explicitly with the kernel via `io_uring_register_buffers` for direct DMA transfers between storage hardware and user-space memory pools.

### 4. Memory-Only Engine Mode
Supports ultra-fast in-memory unit execution or transient simulation testing bypass modes (`memory_only_ = true`). The system reserves internal resume tracks to eliminate standard heap allocations (`malloc`/`new`) during active runtime phases.

---

## 📁 System Requirements

* **OS:** Linux Kernel `5.19+` (Requires support for multi-shot io_uring operations and buffer ring registration)
* **Compiler:** `GCC 11+` or `Clang 13+` with standard C++20 coroutine support (`-std=c++20`)
* **Libraries:** `liburing-dev`
* **OS Setup:** Hugepages configured on host system:
  ```bash
  # Pre-allocate 256 HugePages (512MB total)
  echo 256 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages