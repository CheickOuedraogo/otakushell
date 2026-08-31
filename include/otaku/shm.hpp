#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace otaku {

// Lock-free single-producer single-consumer ring buffer over shared memory.
// Used as the IPC bus between the daemon and modules.
struct RingBuffer {
    // Header is stored at the front of the shm region.
    std::atomic<uint64_t> head{0};  // write index (producer)
    std::atomic<uint64_t> tail{0};  // read index (consumer)

    // A message: len-prefixed bytes stored in `data[]` (split allowed).
    struct Message {
        uint8_t* ptr;
        size_t len;
    };
};

// Open (or create) a named shared-memory region `/otakuShell-<user>-<name>`.
// Returns a mapped pointer of `size` bytes, or nullptr on failure.
void* shm_open_region(const char* name, size_t size, bool create);

// Close and unmap a region returned by shm_open_region.
void shm_close_region(void* ptr, size_t size, const char* name, bool unlink);

inline constexpr size_t kShmDefaultSize = 1 << 20;  // 1 MiB per region

}  // namespace otaku
