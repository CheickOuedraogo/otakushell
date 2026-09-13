// Round-trip test for the lock-free SPSC shared-memory ring (src/shm.cpp).
// Runs entirely in-process: the writer "process" and the reader "process" are
// the same thread, which is enough to exercise cursor/full/empty semantics.

#include <cstdio>
#include <cstring>

#include <sys/types.h>
#include <unistd.h>

#include "otaku/shm.hpp"

using namespace otaku;

namespace {
int fail(const char* what) {
    std::fprintf(stderr, "ring-test: FAIL: %s\n", what);
    return 1;
}
}  // namespace

int main() {
    const char* region_name = "ringtest";
    void* region = shm_open_region(region_name, kRingRegionSize, /*create=*/true);
    if (!region) return fail("shm_open_region");

    RingWriter writer;
    writer.init(region, kRingRegionSize, getpid());
    if (!writer.ready()) return fail("writer init");

    RingReader reader;
    reader.init(region, kRingRegionSize, getpid());

    ModuleSample s;
    // Region must start empty.
    if (reader.pop(reinterpret_cast<uint8_t*>(&s), sizeof(s)))
        return fail("not empty at start");

    // Fill past capacity: pushed/succeeded then drained in order.
    static constexpr int kAttempts = 400;
    int pushed = 0;
    for (int i = 0; i < kAttempts; ++i) {
        s.seq = static_cast<uint64_t>(i) + 1;
        std::snprintf(s.text, sizeof(s.text), "item-%d", i);
        if (writer.push(&s, sizeof(s))) ++pushed;
    }
    if (pushed < 100 || pushed >= kAttempts)
        return fail("pushed count unexpected (full not detected)");

    uint64_t expected = 1;
    int drained = 0;
    while (reader.pop(reinterpret_cast<uint8_t*>(&s), sizeof(s))) {
        if (s.seq != expected) {
            std::fprintf(stderr, "ring-test: out of order (got %llu, want %llu)\n",
                         static_cast<unsigned long long>(s.seq),
                         static_cast<unsigned long long>(expected));
            return 1;
        }
        ++expected;
        ++drained;
    }
    if (drained != pushed) return fail("drained != pushed");

    if (!reader.pop(reinterpret_cast<uint8_t*>(&s), sizeof(s)))
        ;  // empty again, as expected
    else
        return fail("not empty after drain");

    // Producer liveness: fresh heartbeat -> alive; shutdown -> dead.
    // (alive() is pid-scoped; within this process the pid matches.)
    if (!reader.online()) return fail("reader offline while producer running");

    writer.shutdown();
    if (reader.online()) return fail("reader online after shutdown");

    shm_close_region(region, kRingRegionSize, region_name, /*unlink=*/true);
    std::printf("ring-test: OK (%d pushed, %d drained)\n", pushed, drained);
    return 0;
}