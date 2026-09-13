#include "otaku/shm.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>

namespace otaku {

namespace {
std::string region_name(const char* name) {
    std::string user = getenv("USER") ? getenv("USER") : "user";
    return "/otakuShell-" + user + "-" + name;
}
}  // namespace

void* shm_open_region(const char* name, size_t size, bool create) {
    const std::string full = region_name(name);
    int flags = O_RDWR;
    if (create) flags |= O_CREAT;
    int fd = ::shm_open(full.c_str(), flags, 0600);
    if (fd < 0) return nullptr;
    if (create && ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return nullptr;
    }
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (ptr == MAP_FAILED) return nullptr;
    return ptr;
}

void shm_close_region(void* ptr, size_t size, const char* name, bool unlink) {
    if (ptr && ptr != MAP_FAILED) munmap(ptr, size);
    if (unlink) {
        const std::string full = region_name(name);
        ::shm_unlink(full.c_str());
    }
}

// ---------------------------------------------------------------------------
// Lock-free SPSC ring buffer (cursor logic adapted from Frasch/cppcon2023
// Fifo4b, Unlicense / public domain). Both cursors count *elements* in
// [0 .. capacity], wrapped to 0; one extra trailing slot acts as sentinel so
// "full" and "empty" are distinguishable. All stale state (old producer's
// ring contents) is wiped by the producer's init before data flows again.
// ---------------------------------------------------------------------------

namespace {

bool ring_full(uint64_t push, uint64_t pop, uint64_t cap) {
    if (push < pop) return push == pop - 1;
    if (pop < push) return pop == push - cap;
    return false;
}

}  // namespace

void RingWriter::init(void* base, size_t size, pid_t self_pid) {
    hdr_ = nullptr;
    const size_t slot = sizeof(ModuleSample);
    if (!base || size < sizeof(RingHeader) + 2 * slot) return;
    const uint64_t usable = size / slot - 2;  // reserve one sentinel slot
    cap_ = usable;
    data_ = static_cast<uint8_t*>(base) + sizeof(RingHeader);

    // Wipe any leftover ring from a previous producer, then rebuild the
    // header; reading processes re-sync on our pid/magic.
    std::memset(base, 0, size);
    hdr_ = ::new (base) RingHeader{};
    hdr_->magic.store(kShmMagic, std::memory_order_relaxed);
    hdr_->slot_size.store(static_cast<uint32_t>(slot), std::memory_order_relaxed);
    hdr_->capacity.store(static_cast<uint32_t>(cap_), std::memory_order_relaxed);
    hdr_->pid.store(self_pid, std::memory_order_release);
    cached_pop_ = 0;
}

void RingWriter::shutdown() {
    if (!hdr_) return;
    // Unpublish the region so readers stop consuming stale data.
    hdr_->pid.store(0, std::memory_order_relaxed);
    hdr_->magic.store(0, std::memory_order_relaxed);
    hdr_ = nullptr;
}

bool RingWriter::push(const void* data, size_t len) {
    if (!ready() || len > sizeof(ModuleSample)) return false;
    auto push_cursor = hdr_->push.load(std::memory_order_relaxed);
    if (ring_full(push_cursor, cached_pop_, cap_)) {
        cached_pop_ = hdr_->pop.load(std::memory_order_acquire);
        if (ring_full(push_cursor, cached_pop_, cap_)) return false;
    }
    std::memcpy(data_ + push_cursor * sizeof(ModuleSample), data, len);
    hdr_->push.store((push_cursor == cap_) ? 0 : push_cursor + 1,
                     std::memory_order_release);
    return true;
}

void RingWriter::set_heartbeat(uint64_t now_ns) {
    if (hdr_) hdr_->last_write_ns.store(now_ns, std::memory_order_relaxed);
}

void RingReader::init(void* base, size_t size, pid_t expected_pid) {
    hdr_ = nullptr;
    if (!base || size < sizeof(RingHeader)) return;
    hdr_ = static_cast<RingHeader*>(base);
    slot_size_ = hdr_->slot_size.load(std::memory_order_relaxed);
    cap_ = hdr_->capacity.load(std::memory_order_relaxed);
    expected_pid_ = expected_pid;
    data_ = static_cast<uint8_t*>(base) + sizeof(RingHeader);
    cached_push_ = 0;
    synced_ = false;
}

bool RingReader::pop(uint8_t* out, size_t len) {
    if (!hdr_) return false;

    const auto magic = hdr_->magic.load(std::memory_order_relaxed);
    const auto pid = hdr_->pid.load(std::memory_order_relaxed);
    if (!synced_ || magic != kShmMagic || pid != expected_pid_) {
        // A (new) producer initialized the ring: cursors start at zero.
        if (synced_ && magic == kShmMagic && pid == 0) return false;  // stopped
        cached_push_ = 0;
        synced_ = magic == kShmMagic;
        if (!synced_ || pid != expected_pid_) return false;
    }
    if (slot_size_ == 0 || cap_ == 0) return false;

    auto pop_cursor = hdr_->pop.load(std::memory_order_relaxed);
    if (cached_push_ == pop_cursor) {
        cached_push_ = hdr_->push.load(std::memory_order_acquire);
        if (cached_push_ == pop_cursor) return false;
    }
    std::memcpy(out, data_ + pop_cursor * slot_size_, len);
    hdr_->pop.store((pop_cursor == cap_) ? 0 : pop_cursor + 1,
                    std::memory_order_release);
    return true;
}

bool RingReader::online() const {
    return hdr_ &&
           hdr_->pid.load(std::memory_order_relaxed) == expected_pid_ &&
           hdr_->magic.load(std::memory_order_relaxed) == kShmMagic;
}

bool RingReader::producer_alive(uint64_t now_ns) const {
    if (!online()) return false;
    const uint64_t last = hdr_->last_write_ns.load(std::memory_order_relaxed);
    return last != 0 && now_ns > last &&
           (now_ns - last) < kModuleStaleTimeoutNs;
}

}  // namespace otaku