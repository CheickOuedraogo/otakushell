#include "otaku/supervisor.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <errno.h>

namespace otaku {

// ---------------------------------------------------------------------------
// Region naming
// ---------------------------------------------------------------------------

namespace {

std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += '-';
        }
    }
    return out;
}

std::string module_region_name(const std::string& name) {
    return "mod-" + sanitize(name);
}

}  // namespace

// ---------------------------------------------------------------------------
// ModuleSource
// ---------------------------------------------------------------------------

ModuleSource::ModuleSource(std::string name, pid_t pid)
    : name_(std::move(name)), reader_pid_(pid) {}

std::shared_ptr<ModuleSource> ModuleSource::open(const std::string& name,
                                                 pid_t pid) {
    auto src = std::shared_ptr<ModuleSource>(new ModuleSource(name, pid));
    // The producer creates the region when it starts; give it a moment and
    // retry so a slow startup doesn't lose the module.
    constexpr int kMaxAttempts = 50;  // 50 * 10 ms = 500 ms
    for (int i = 0; i < kMaxAttempts; ++i) {
        if (src->reopen()) return src;
        usleep(10'000);
    }
    return nullptr;
}

bool ModuleSource::reopen() {
    if (region_) {
        shm_close_region(region_, kRingRegionSize,
                         module_region_name(name_).c_str(), /*unlink=*/false);
        region_ = nullptr;
    }
    region_ = shm_open_region(module_region_name(name_).c_str(), kRingRegionSize,
                              /*create=*/false);
    if (!region_) return false;
    reader_.init(region_, kRingRegionSize, reader_pid_);
    return reader_.ready();
}

void ModuleSource::poll(uint64_t now_ns) {
    if (!reader_.ready()) reopen();
    if (!reader_.ready()) {
        alive_ = false;
        return;
    }
    ModuleSample s;
    while (reader_.pop(reinterpret_cast<uint8_t*>(&s), sizeof(s))) {
        latest_ = s;
        latest_seq_ = s.seq;
    }
    alive_ = reader_.producer_alive(now_ns);
}

// ---------------------------------------------------------------------------
// ModuleSupervisor
// ---------------------------------------------------------------------------

ModuleSupervisor::ModuleSupervisor(bool simulate) : simulate_(simulate) {}

ModuleSupervisor::~ModuleSupervisor() { shutdown(); }

std::string ModuleSupervisor::producer_path() {
    const char* env = getenv("OTAKU_MOD_PATH");
    return (env && *env) ? std::string(env) : std::string("otakud-mod");
}

bool ModuleSupervisor::spawn(Producer& p) {
    const std::string exe = producer_path();

    pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "otakud-mod: fork failed: %s\n", std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Child: become the module producer.
        setpgid(0, 0);
        std::vector<std::string> arg_s;
        arg_s.push_back(exe);
        arg_s.push_back(p.name);
        arg_s.push_back(p.opts);
        if (simulate_) arg_s.push_back("--simulate");
        arg_s.push_back("--daemonize");

        std::vector<char*> argv;
        argv.reserve(arg_s.size() + 1);
        for (auto& a : arg_s) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        execvp(exe.c_str(), argv.data());
        std::fprintf(stderr, "otakud-mod: could not exec '%s': %s\n", exe.c_str(),
                     std::strerror(errno));
        _exit(127);
    }

    p.pid = pid;
    return true;
}

void ModuleSupervisor::kill_and_wait(pid_t pid) {
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {  // up to 500 ms for a graceful exit
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return;
        if (r < 0 && errno != EINTR) return;
        usleep(10'000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

std::shared_ptr<ModuleSource> ModuleSupervisor::acquire(const std::string& name,
                                                        const std::string& opts) {
    auto src = sources_.find(name);
    auto prod = prods_.find(name);

    if (src != sources_.end() && prod != prods_.end())
        return src->second;  // already running and shared

    Producer p{-1, name, opts};
    if (prod == prods_.end()) {
        if (!spawn(p)) return nullptr;
        prods_[name] = p;
    } else {
        p = prod->second;  // producer missing? respawn below
        prods_.erase(prod);
        if (!spawn(p)) return nullptr;
        prods_[name] = p;
    }

    std::shared_ptr<ModuleSource> s;
    if (src != sources_.end()) {
        s = src->second;  // re-bind the reader to the new producer pid
        s->set_producer_pid(p.pid);
    } else {
        s = ModuleSource::open(name, p.pid);
        if (!s) {
            kill_and_wait(p.pid);
            prods_.erase(name);
            return nullptr;
        }
        sources_[name] = s;
    }
    return s;
}

void ModuleSupervisor::poll_all(uint64_t now_ns) {
    for (auto& [name, src] : sources_) src->poll(now_ns);
}

void ModuleSupervisor::reap() {
    std::vector<Producer> to_respawn;
    for (auto it = prods_.begin(); it != prods_.end();) {
        int status = 0;
        pid_t r = waitpid(it->second.pid, &status, WNOHANG);
        if (r == it->second.pid) {
            std::fprintf(stderr, "otakud-mod: producer '%s' exited, respawning\n",
                         it->second.name.c_str());
            to_respawn.push_back(it->second);
            it = prods_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& p : to_respawn) {
        if (spawn(p)) {
            prods_[p.name] = p;
            if (auto it = sources_.find(p.name); it != sources_.end())
                it->second->set_producer_pid(p.pid);
        }
    }
}

void ModuleSupervisor::shutdown() {
    for (auto& [name, p] : prods_) {
        (void)name;
        kill_and_wait(p.pid);
    }
    prods_.clear();
    // Regions are unlinked by the producers themselves on exit.
    sources_.clear();
}

std::vector<std::pair<std::string, pid_t>> ModuleSupervisor::entries() const {
    std::vector<std::pair<std::string, pid_t>> out;
    out.reserve(prods_.size());
    for (const auto& [name, p] : prods_)
        out.emplace_back(name, p.pid);
    return out;
}

}  // namespace otaku