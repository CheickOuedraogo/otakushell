#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sys/types.h>

#include "otaku/shm.hpp"

namespace otaku {

// A live view of one module producer: the single reader of that module's
// ring region plus the last sample seen. Instances are owned by the
// ModuleSupervisor (one reader per region → SPSC holds) and shared with the
// IModule renderers, which only peek at the latest sample.
class ModuleSource : public std::enable_shared_from_this<ModuleSource> {
public:
    // Open the region for `name`; read-only (create=false), expects `pid`.
    static std::shared_ptr<ModuleSource> open(const std::string& name, pid_t pid);

    // True when the region is mapped and alive (producer writes within the
    // heartbeat window). Refreshed by poll_all().
    bool alive() const { return alive_; }

    // Latest sample (valid when latest_seq() > 0).
    const ModuleSample& sample() const { return latest_; }
    uint64_t latest_seq() const { return latest_seq_; }

    const std::string& name() const { return name_; }
    pid_t producer_pid() const { return reader_pid_; }

    // Called by the supervisor when a producer restarts.
    void set_producer_pid(pid_t pid) {
        reader_pid_ = pid;
        reader_.init(region_, kRingRegionSize, pid);
    }

    // Drain the ring into latest_. Invoked by the supervisor once per poll.
    void poll(uint64_t now_ns);

private:
    ModuleSource(std::string name, pid_t pid);
    bool reopen();

    std::string name_;
    pid_t reader_pid_{-1};
    RingReader reader_;
    void* region_{nullptr};
    ModuleSample latest_{};
    uint64_t latest_seq_{0};
    bool alive_{false};
};

// Owns the producer processes for enabled modules and hands out the shared
// ModuleSource (one per module name). Used by both the daemon (real data)
// and the preview (--simulate data).
class ModuleSupervisor {
public:
    explicit ModuleSupervisor(bool simulate);
    ~ModuleSupervisor();

    ModuleSupervisor(const ModuleSupervisor&) = delete;
    ModuleSupervisor& operator=(const ModuleSupervisor&) = delete;

    // Returns the shared source for `name`, spawning a producer first if
    // none is running yet. Nullptr if the producer could not be started.
    std::shared_ptr<ModuleSource> acquire(const std::string& name,
                                          const std::string& opts);

    // Poll all live sources (drain rings). Call once per event-loop tick.
    void poll_all(uint64_t now_ns);

    // Reap dead producers and respawn them.
    void reap();

    // Soft-stop every producer (SIGTERM), wait, and unmap regions.
    void shutdown();

    // name + pid of every running producer (for the status region).
    std::vector<std::pair<std::string, pid_t>> entries() const;

    size_t producer_count() const { return prods_.size(); }

    // Resolve the actual producer executable (`OTAKU_MOD_PATH` then `PATH`).
    static std::string producer_path();

private:
    struct Producer {
        pid_t pid;
        std::string name;
        std::string opts;
    };

    bool spawn(Producer& p);
    void kill_and_wait(pid_t pid);

    bool simulate_;
    std::map<std::string, Producer> prods_;
    std::map<std::string, std::shared_ptr<ModuleSource>> sources_;
    bool shutting_down_{false};
};

}  // namespace otaku