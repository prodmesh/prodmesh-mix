// mix-plugin: plugin scanning and checks, out of the engine's process.
//
//   mix-plugin scan [DIR...]          catalog every bundle (JSON lines);
//                                     each bundle is described in a child
//                                     process with a timeout, so a plugin
//                                     that crashes or hangs on load is
//                                     reported as quarantined, nothing more
//   mix-plugin describe BUNDLE        (the child) list one bundle's plugins
//   mix-plugin bench BUNDLE ID        load in-process and time it as an
//                                     insert on a mono strip
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "engine/engine.h"
#include "plugin/plugin.h"

extern char** environ;
using namespace pm;
namespace fs = std::filesystem;

namespace {

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        o += c;
    }
    return o;
}

int describe(const char* path) {
    std::vector<plugin::PluginInfo> list;
    std::string err;
    if (!plugin::describeBundle(path, list, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    for (const auto& p : list)
        std::printf("{\"format\":\"%s\",\"path\":\"%s\",\"id\":\"%s\","
                    "\"name\":\"%s\",\"vendor\":\"%s\",\"version\":\"%s\"}\n",
                    p.format.c_str(), jsonEscape(p.path).c_str(),
                    jsonEscape(p.id).c_str(), jsonEscape(p.name).c_str(),
                    jsonEscape(p.vendor).c_str(), jsonEscape(p.version).c_str());
    return 0;
}

// Runs `self describe bundle` with a deadline; returns the child's stdout.
bool describeIsolated(const char* self, const std::string& bundle,
                      std::string& out, std::string& why) {
    int pipeFd[2];
    if (pipe(pipeFd)) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipeFd[1], 1);
    posix_spawn_file_actions_addclose(&fa, pipeFd[0]);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    const char* argv[] = {self, "describe", bundle.c_str(), nullptr};
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, self, &fa, nullptr,
                               const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipeFd[1]);
    if (rc) {
        close(pipeFd[0]);
        why = "spawn failed";
        return false;
    }
    fcntl(pipeFd[0], F_SETFL, O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int status = 0;
    bool done = false;
    char buf[4096];
    while (!done) {
        ssize_t n;
        while ((n = read(pipeFd[0], buf, sizeof buf)) > 0) out.append(buf, std::size_t(n));
        if (waitpid(pid, &status, WNOHANG) == pid) {
            done = true;
            while ((n = read(pipeFd[0], buf, sizeof buf)) > 0) out.append(buf, std::size_t(n));
        } else if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            close(pipeFd[0]);
            why = "timed out";
            return false;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    close(pipeFd[0]);
    if (WIFSIGNALED(status)) {
        why = std::string("crashed (") + strsignal(WTERMSIG(status)) + ")";
        return false;
    }
    if (WEXITSTATUS(status)) {
        why = "failed to load";
        return false;
    }
    return true;
}

int scan(const char* self, std::vector<std::string> dirs) {
    if (dirs.empty()) {
        const char* home = getenv("HOME");
        dirs = {"/Library/Audio/Plug-Ins/CLAP",
                std::string(home ? home : "") + "/Library/Audio/Plug-Ins/CLAP"};
    }
    int ok = 0, bad = 0;
    for (const auto& d : dirs) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(d, ec)) {
            const auto path = e.path().string();
            if (e.path().extension() != ".clap") continue;
            std::string out, why;
            if (describeIsolated(self, path, out, why)) {
                std::fputs(out.c_str(), stdout);
                ++ok;
            } else {
                std::printf("{\"path\":\"%s\",\"quarantined\":\"%s\"}\n",
                            jsonEscape(path).c_str(), why.c_str());
                ++bad;
            }
        }
    }
    std::fprintf(stderr, "scanned %d bundle(s), %d quarantined\n", ok + bad, bad);
    return 0;
}

int bench(const char* bundle, const char* id) {
    constexpr int kBlock = 128;
    EngineConfig c;
    c.maxBlock = kBlock;
    c.numOutputs = 1;
    Engine e(c);
    GraphDesc g;
    NodeDesc gen;
    gen.id = 1;
    gen.kind = NodeKind::Generator;
    NodeDesc strip;
    strip.id = 2;
    strip.processing.inserts.push_back(InsertDesc::plugin("clap", bundle, id));
    NodeDesc out;
    out.id = 3;
    out.kind = NodeKind::Output;
    g.nodes = {gen, strip, out};
    g.edges = {{1, 2, 0, 0}, {2, 3, 0, 0}};

    const auto t0 = std::chrono::steady_clock::now();
    if (!e.setGraph(g)) return 1;
    e.pump();
    const double loadMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
    for (const auto& w : e.warnings()) std::printf("warning: %s\n", w.c_str());
    std::string lerr;
    if (auto probe = plugin::load("clap", bundle, id, 48000, kBlock, 1, &lerr))
        std::printf("latency %d samples\n", probe->latencySamples());

    std::vector<float> buf(kBlock);
    float* outp[1] = {buf.data()};
    double worst = 0, total = 0;
    float peak = 0;
    bool finite = true;
    const int blocks = 48000 * 10 / kBlock;  // 10 s of audio
    for (int b = 0; b < blocks; ++b) {
        const auto s = std::chrono::steady_clock::now();
        e.process(nullptr, 0, outp, 1, kBlock);
        const double us = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - s).count();
        worst = std::max(worst, us);
        total += us;
        for (float v : buf) {
            finite = finite && std::isfinite(v);
            peak = std::max(peak, std::fabs(v));
        }
    }
    const double period = 1e6 * kBlock / 48000.0;
    std::printf("%s\n  load+activate %.0f ms  mean %.1f us/block (%.2f%% of a "
                "core)  worst %.0f us  peak %.3f  %s\n",
                id, loadMs, total / blocks, 100 * total / blocks / period, worst,
                peak, finite ? "finite" : "NON-FINITE OUTPUT");
    return finite && e.warnings().empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "describe")) return describe(argv[2]);
    if (argc >= 2 && !std::strcmp(argv[1], "scan"))
        return scan(argv[0], std::vector<std::string>(argv + 2, argv + argc));
    if (argc >= 4 && !std::strcmp(argv[1], "bench")) return bench(argv[2], argv[3]);
    std::puts("mix-plugin scan [DIR...] | describe BUNDLE | bench BUNDLE ID");
    return 2;
}
