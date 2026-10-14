// Engine side of a sandboxed plugin insert: owns the shared-memory region
// and the mix-pluginhost process, exchanges one block per call without ever
// waiting, and restarts the host if it dies.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/rt_thread.h"
#include "plugin/plugin.h"
#include "plugin/sandbox_shm.h"

extern char** environ;

namespace pm::plugin {
namespace {

std::string hostExecutable() {
    if (const char* p = std::getenv("PM_PLUGINHOST")) return p;
    char buf[4096];
    std::uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return "mix-pluginhost";
    std::string self(buf);
    return self.substr(0, self.rfind('/') + 1) + "mix-pluginhost";
}

class SandboxProcessor final : public Processor {
public:
    ~SandboxProcessor() override {
        stopHost();
        if (ctlFd_ >= 0) close(ctlFd_);
        if (region_) munmap(region_, size_);
        if (!shmName_.empty()) shm_unlink(shmName_.c_str());
    }

    bool init(const std::vector<PluginRef>& chain, double fs, int maxBlock,
              int channels, std::string* error) {
        chain_ = chain;
        id_.clear();
        for (const auto& r : chain) id_ += (id_.empty() ? "" : " + ") + r.id;
        const std::string& id = id_;
        rate_ = fs;
        maxBlock_ = maxBlock;
        channels_ = std::min(channels, sandbox::kMaxChannels);
        static std::atomic<int> counter{0};
        // PSHMNAMLEN is 31 on macOS.
        shmName_ = "/pmsb" + std::to_string(getpid()) + "_" +
                   std::to_string(counter++);
        size_ = sandbox::regionSize(channels_, maxBlock_);
        const int fd = shm_open(shmName_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0 || ftruncate(fd, off_t(size_)) != 0) {
            if (fd >= 0) close(fd);
            if (error) *error = id + ": cannot create shared memory";
            return false;
        }
        region_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (region_ == MAP_FAILED) {
            region_ = nullptr;
            if (error) *error = id + ": cannot map shared memory";
            return false;
        }
        std::memset(region_, 0, size_);
        view_ = sandbox::View::of(region_, channels_, maxBlock_);
        auto& h = *view_.header;
        h.magic = sandbox::kMagic;
        h.version = sandbox::kVersion;
        h.channels = channels_;
        h.maxBlock = maxBlock_;
        if (!startHost(error)) return false;
        name_ = h.name[0] ? h.name : id_;
        latency_ = h.latency.load() + maxBlock_;  // + the exchange block
        return true;
    }

    // Audio thread. Never waits: last block's result, or last block's
    // input (dry, same latency) if the host did not deliver.
    void process(float* const* ch, int nch, int n) override {
        auto& h = *view_.header;
        const std::uint64_t seq = ++seq_;
        const int slot = int(seq & 1), prev = slot ^ 1;
        const int frames = std::min(n, maxBlock_);
        for (int c = 0; c < channels_; ++c)
            std::memcpy(view_.in(slot, c), ch[std::min(c, nch - 1)],
                        sizeof(float) * std::size_t(frames));
        const bool ready = seq > 1 && h.done.load(std::memory_order_acquire) == seq - 1;
        if (seq > 1 && !ready) misses_.fetch_add(1, std::memory_order_relaxed);
        for (int c = 0; c < nch; ++c) {
            const int src = std::min(c, channels_ - 1);
            if (seq == 1)
                std::memset(ch[c], 0, sizeof(float) * std::size_t(frames));
            else
                std::memcpy(ch[c], ready ? view_.out(prev, src) : view_.in(prev, src),
                            sizeof(float) * std::size_t(frames));
        }
        h.frames[slot] = std::uint32_t(frames);
        h.request.store(seq, std::memory_order_release);
        sandbox::ring(h);
    }

    int latencySamples() const override { return latency_; }
    std::string name() const override { return name_; }
    std::uint64_t rtAllocations() const override {
        return view_.header ? view_.header->hostAllocs.load() : 0;
    }

    // Control thread: notice a dead host and bring it back.
    std::string maintain() override {
        if (pid_ <= 0) return {};
        int status = 0;
        if (waitpid(pid_, &status, WNOHANG) != pid_) return {};
        pid_ = -1;
        const std::string why =
            WIFSIGNALED(status) ? std::string("crashed (") + strsignal(WTERMSIG(status)) + ")"
                                : "exited";
        ++crashes_;
        if (crashes_ > 3)
            return name_ + " " + why + " again; left bypassed";
        std::string err;
        const bool back = startHost(&err);
        return name_ + " " + why + (back ? "; restarted (fresh state)" : "; restart failed: " + err);
    }

    std::uint64_t missedBlocks() const override { return misses_.load(); }

    bool requestEditor(int index, std::string* error) override {
        if (pid_ <= 0 || ctlFd_ < 0) {
            if (error) *error = name_ + " is not running";
            return false;
        }
        const std::string cmd = "editor " + std::to_string(index) + "\n";
        if (write(ctlFd_, cmd.data(), cmd.size()) != ssize_t(cmd.size())) {
            if (error) *error = "plugin host did not take the command";
            return false;
        }
        return true;
    }
    bool sandboxed() const override { return true; }

private:
    bool startHost(std::string* error) {
        auto& h = *view_.header;
        h.state.store(std::uint32_t(sandbox::HostState::Starting));
        const std::string exe = hostExecutable();
        const std::string ch = std::to_string(channels_), mb = std::to_string(maxBlock_),
                          rate = std::to_string(rate_);
        std::vector<const char*> argv = {exe.c_str(), "--shm", shmName_.c_str(),
                                         "--channels", ch.c_str(), "--max-block",
                                         mb.c_str(), "--rate", rate.c_str()};
        for (const auto& r : chain_) {
            argv.push_back("--plugin");
            argv.push_back(r.format.c_str());
            argv.push_back(r.path.c_str());
            argv.push_back(r.id.c_str());
        }
        argv.push_back(nullptr);
        // A control pipe on the host's stdin: line commands such as
        // "editor 1". Closing it (or our death) tells the host to quit.
        int pipeFd[2];
        if (pipe(pipeFd) != 0) {
            if (error) *error = "cannot create control pipe";
            return false;
        }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, pipeFd[0], 0);
        posix_spawn_file_actions_addclose(&fa, pipeFd[1]);
        const int rc = posix_spawn(&pid_, exe.c_str(), &fa, nullptr,
                                   const_cast<char* const*>(argv.data()), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(pipeFd[0]);
        if (ctlFd_ >= 0) close(ctlFd_);
        ctlFd_ = pipeFd[1];
        fcntl(ctlFd_, F_SETFL, O_NONBLOCK);
        fcntl(ctlFd_, F_SETFD, FD_CLOEXEC);
        if (rc != 0) {
            pid_ = -1;
            if (error) *error = exe + ": cannot start plugin host";
            return false;
        }
        // Plugins take tens to hundreds of ms to load; this runs on the
        // control thread, never the audio thread.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto st = sandbox::HostState(h.state.load());
            if (st == sandbox::HostState::Ready) return true;
            int status = 0;
            if (st == sandbox::HostState::Failed || waitpid(pid_, &status, WNOHANG) == pid_) {
                if (error) *error = id_ + ": plugin host could not load it";
                stopHost();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (error) *error = id_ + ": plugin host timed out loading it";
        stopHost();
        return false;
    }

    void stopHost() {
        if (pid_ <= 0) return;
        kill(pid_, SIGTERM);
        for (int i = 0; i < 100; ++i) {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }

    std::vector<PluginRef> chain_;
    std::string id_, name_, shmName_;
    double rate_ = 48000;
    int maxBlock_ = 0, channels_ = 1, latency_ = 0, crashes_ = 0;
    void* region_ = nullptr;
    std::size_t size_ = 0;
    sandbox::View view_;
    pid_t pid_ = -1;
    int ctlFd_ = -1;
    std::uint64_t seq_ = 0;  // audio thread
    std::atomic<std::uint64_t> misses_{0};
};

}  // namespace

std::unique_ptr<Processor> loadSandboxedChain(const std::vector<PluginRef>& chain,
                                              double fs, int maxBlock,
                                              int channels, std::string* error) {
    auto p = std::make_unique<SandboxProcessor>();
    if (!p->init(chain, fs, maxBlock, channels, error)) return nullptr;
    return p;
}

}  // namespace pm::plugin
