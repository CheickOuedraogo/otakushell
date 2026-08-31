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

}  // namespace otaku
