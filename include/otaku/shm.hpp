#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace otaku {

// Lock-free SPSC ring buffer over shared memory, used as the IPC bus between
// module producers (`otakud-mod`) and the shell (otakud / preview).
//
// The cursor handling (full/empty semantics, acquire/release ordering,
// cache-line separated cursors, cached remote cursors) is adapted from
// Charles Frasch's CppCon2023 SPSC queue (Fifo4b), released into the public
// domain (Unlicense). See third_party/LICENSE-unlicense and the README.
//
// Unlike the original, the control block lives at a *fixed offset* in the
// mapped region (no absolute pointers), so the same memory works when mapped
// at different virtual addresses in each process.

constexpr uint32_t kShmMagic = 0x4F54414Bu;        // "OTAK"
constexpr uint32_t kModuleTextMax = 224;           // max module text payload
constexpr size_t kRingRegionSize = 64 * 1024;      // per-module region size
constexpr uint64_t kModuleStaleTimeoutNs = 5'000'000'000ull;  // 5 s heartbeat

// A frame of data published by a module producer. Trivially copyable, stored
// verbatim in the ring buffer.
struct ModuleSample {
    uint64_t seq{0};              // producer-side increment counter
    char text[kModuleTextMax]{0}; // display text (already formatted)
};

// Header at offset 0 of every module shared-memory region.
struct alignas(64) RingHeader {
    std::atomic<uint32_t> magic{0};        // kShmMagic once initialized
    std::atomic<uint32_t> slot_size{0};    // sizeof(ModuleSample)
    std::atomic<uint32_t> capacity{0};     // usable element slots
    std::atomic<int32_t> pid{0};           // producer pid (0 = none yet)
    std::atomic<uint64_t> last_write_ns{0};// heartbeat from the producer

    // Write and read cursors, cache-line separated (false sharing).
    alignas(64) std::atomic<uint64_t> push{0};
    alignas(64) std::atomic<uint64_t> pop{0};
};

// Writer side of the ring. Exactly one process (the producer).
class RingWriter {
public:
    void init(void* base, size_t size, pid_t self_pid);
    void shutdown();  // mark "no producer" (pid=0) before unlinking
    bool push(const void* data, size_t len);
    void set_heartbeat(uint64_t now_ns);
    bool ready() const { return hdr_ != nullptr; }

private:
    RingHeader* hdr_{nullptr};
    uint8_t* data_{nullptr};
    uint64_t cap_{0};
    uint64_t cached_pop_{0};
};

// Reader side of the ring. Exactly one process (the shell).
class RingReader {
public:
    void init(void* base, size_t size, pid_t expected_pid);
    // Pop one sample (false = empty). Detects producer restarts and re-syncs.
    bool pop(uint8_t* out, size_t len);
    // True when the mapped region is a valid, live ring (matches producer pid).
    bool online() const;
    // True when the producer stopped publishing within the heartbeat window.
    bool producer_alive(uint64_t now_ns) const;
    bool ready() const { return hdr_ != nullptr; }

private:
    RingHeader* hdr_{nullptr};
    uint8_t* data_{nullptr};
    uint64_t slot_size_{0};
    uint64_t cap_{0};
    pid_t expected_pid_{-1};
    uint64_t cached_push_{0};
    bool synced_{false};
};

// Status region (written by otakud, read by `otakushell status`).
constexpr size_t kStatusRegionSize = 4096;
constexpr uint32_t kStatusMaxModules = 16;
constexpr uint32_t kStatusMagic = 0x4F545354u;  // "OTST"

struct StatusEntry {
    char name[48]{0};
    pid_t pid{-1};
};

struct alignas(64) StatusRegion {
    std::atomic<uint32_t> magic{0};
    char version[24]{0};
    uint64_t start_ns{0};
    uint32_t frame_count{0};
    std::atomic<uint32_t> module_count{0};
    StatusEntry modules[kStatusMaxModules];
};

// Open (or create) a named shared-memory region `/otakuShell-<user>-<name>`.
// Returns a mapped pointer of `size` bytes, or nullptr on failure.
void* shm_open_region(const char* name, size_t size, bool create);

// Close and unmap a region returned by shm_open_region.
void shm_close_region(void* ptr, size_t size, const char* name, bool unlink);

inline constexpr size_t kShmDefaultSize = 1 << 20;  // 1 MiB per plain region

}  // namespace otaku