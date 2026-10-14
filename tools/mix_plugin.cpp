// mix-plugin: plugin scanning and checks, out of the engine's process.
//
//   mix-plugin scan [--cache F] [DIR...]
//                                     catalog every bundle (JSON lines);
//                                     with --cache, unchanged bundles are
//                                     not loaded again (see scan());
//                                     each bundle is described in a child
//                                     process with a timeout, so a plugin
//                                     that crashes or hangs on load is
//                                     reported as quarantined, nothing more
//   mix-plugin describe BUNDLE        (the child) list one bundle's plugins
//   mix-plugin bench BUNDLE ID        load in-process and time it as an
//                                     insert on a mono strip (.clap/.vst3)
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/rt_guard.h"
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
                    "\"name\":\"%s\",\"vendor\":\"%s\",\"version\":\"%s\","
                    "\"instrument\":%s}\n",
                    p.format.c_str(), jsonEscape(p.path).c_str(),
                    jsonEscape(p.id).c_str(), jsonEscape(p.name).c_str(),
                    jsonEscape(p.vendor).c_str(), jsonEscape(p.version).c_str(),
                    p.instrument ? "true" : "false");
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

// Identifies a bundle version without loading it: the Info.plist's (or
// the bundle's) modification time and size.
std::string stampOf(const fs::path& bundle) {
    std::error_code ec;
    fs::path probe = bundle / "Contents" / "Info.plist";
    if (!fs::exists(probe, ec)) probe = bundle;
    const auto t = fs::last_write_time(probe, ec).time_since_epoch().count();
    const auto size = fs::is_regular_file(probe, ec) ? fs::file_size(probe, ec) : 0;
    return std::to_string(static_cast<long long>(t)) + "-" +
           std::to_string(static_cast<unsigned long long>(size));
}

// Value of a string field in one of our own JSON lines (we write them, so
// only \" and \\ need unescaping).
std::string field(const std::string& line, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    auto at = line.find(k);
    if (at == std::string::npos) return {};
    std::string out;
    for (at += k.size(); at < line.size() && line[at] != '"'; ++at) {
        if (line[at] == '\\' && at + 1 < line.size()) ++at;
        out += line[at];
    }
    return out;
}

std::string withStamp(std::string line, const std::string& stamp) {
    const auto close = line.rfind('}');
    if (close != std::string::npos)
        line.insert(close, ",\"stamp\":\"" + stamp + "\"");
    return line;
}

// Unchanged bundles come from the cache without loading any plugin code —
// some plugins pop up licence dialogs (iLok) or hang when loaded, so each
// version is loaded at most once. Quarantine is remembered the same way.
int scan(const char* self, std::vector<std::string> args) {
    std::string cachePath;
    std::vector<std::string> dirs;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--cache" && i + 1 < args.size()) cachePath = args[++i];
        else dirs.push_back(args[i]);
    }
    if (dirs.empty()) {
        const char* home = getenv("HOME");
        const std::string h = home ? home : "";
        dirs = {"/Library/Audio/Plug-Ins/CLAP", h + "/Library/Audio/Plug-Ins/CLAP",
                "/Library/Audio/Plug-Ins/VST3", h + "/Library/Audio/Plug-Ins/VST3"};
    }
    std::map<std::string, std::vector<std::string>> cached;  // path -> lines
    std::map<std::string, std::string> cachedStamp;
    if (!cachePath.empty())
        if (FILE* f = std::fopen(cachePath.c_str(), "r")) {
            char buf[8192];
            while (std::fgets(buf, sizeof buf, f)) {
                std::string line(buf);
                while (!line.empty() && line.back() == '\n') line.pop_back();
                const auto path = field(line, "path");
                const auto stamp = field(line, "stamp");
                if (path.empty() || stamp.empty()) continue;
                cached[path].push_back(line);
                cachedStamp[path] = stamp;
            }
            std::fclose(f);
        }

    int reused = 0, loaded = 0, bad = 0;
    for (const auto& d : dirs) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(d, ec)) {
            const auto path = e.path().string();
            const auto ext = e.path().extension();
            if (ext != ".clap" && ext != ".vst3") continue;
            const auto stamp = stampOf(e.path());
            if (auto it = cachedStamp.find(path);
                it != cachedStamp.end() && it->second == stamp) {
                for (const auto& line : cached[path]) {
                    std::puts(line.c_str());
                    bad += field(line, "quarantined").empty() ? 0 : 1;
                }
                ++reused;
                continue;
            }
            std::string out, why;
            ++loaded;
            if (describeIsolated(self, path, out, why)) {
                std::size_t start = 0, nl;
                while ((nl = out.find('\n', start)) != std::string::npos) {
                    std::puts(withStamp(out.substr(start, nl - start), stamp).c_str());
                    start = nl + 1;
                }
                if (out.empty())  // no effects in it: remember that too
                    std::printf("{\"path\":\"%s\",\"empty\":true,\"stamp\":\"%s\"}\n",
                                jsonEscape(path).c_str(), stamp.c_str());
            } else {
                std::printf("{\"path\":\"%s\",\"quarantined\":\"%s\",\"stamp\":\"%s\"}\n",
                            jsonEscape(path).c_str(), why.c_str(), stamp.c_str());
                ++bad;
            }
        }
    }
    std::fprintf(stderr, "%d bundle(s): %d from cache, %d loaded, %d quarantined\n",
                 reused + loaded, reused, loaded, bad);
    return 0;
}

// Adds current stamps to an unstamped catalog without loading anything:
// adopts a previous scan's results as the cache for unchanged bundles.
int stampCatalog(const char* in) {
    FILE* f = std::fopen(in, "r");
    if (!f) return 1;
    char buf[8192];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string line(buf);
        while (!line.empty() && line.back() == '\n') line.pop_back();
        const auto path = field(line, "path");
        if (path.empty() || !field(line, "stamp").empty()) {
            if (!path.empty()) std::puts(line.c_str());
            continue;
        }
        std::puts(withStamp(line, stampOf(path)).c_str());
    }
    std::fclose(f);
    return 0;
}

int bench(const char* bundle, const char* id) {
    // PM_RT_ABORT=1: abort at the first audio-thread allocation (run it
    // under lldb to see who allocated).
    if (std::getenv("PM_RT_ABORT")) rt::setPolicy(rt::Policy::Abort);
    constexpr int kBlock = 128;
    EngineConfig c;
    c.maxBlock = kBlock;
    c.numOutputs = 2;
    Engine e(c);
    GraphDesc g;
    NodeDesc gen;
    gen.id = 1;
    gen.kind = NodeKind::Generator;
    NodeDesc strip;
    strip.id = 2;
    // PM_BENCH_CHANNELS=2 benches on a stereo strip (default mono).
    if (const char* ch = std::getenv("PM_BENCH_CHANNELS"))
        strip.channels = std::atoi(ch);
    const std::string format =
        fs::path(bundle).extension() == ".vst3" ? "vst3" : "clap";
    strip.processing.inserts.push_back(InsertDesc::plugin(format, bundle, id));
    NodeDesc out;
    out.id = 3;
    out.kind = NodeKind::Output;
    out.channels = strip.channels;
    g.nodes = {gen, strip, out};
    g.edges = {{1, 2, 0, 0}, {2, 3, 0, 0}};

    const auto t0 = std::chrono::steady_clock::now();
    if (!e.setGraph(g)) return 1;
    e.pump();
    const double loadMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
    for (const auto& w : e.warnings()) std::printf("warning: %s\n", w.c_str());
    std::string lerr;
    if (auto probe = plugin::load(format, bundle, id, 48000, kBlock,
                                  strip.channels, &lerr))
        std::printf("latency %d samples\n", probe->latencySamples());

    std::vector<float> buf(kBlock), buf2(kBlock);
    float* outp[2] = {buf.data(), buf2.data()};
    double worst = 0, total = 0;
    float peak = 0;
    bool finite = true;
    const int blocks = 48000 * 10 / kBlock;  // 10 s of audio
    for (int b = 0; b < blocks; ++b) {
        const auto s = std::chrono::steady_clock::now();
        {
            rt::ScopedRealtime realtime;  // count allocations, as mixd does
            e.process(nullptr, 0, outp, 2, kBlock);
        }
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
    std::printf("%s\n  rt violations %llu (allocations on the audio thread)\n",
                id, (unsigned long long)rt::violations());
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
    if (argc >= 3 && !std::strcmp(argv[1], "stamp-catalog")) return stampCatalog(argv[2]);
    std::puts("mix-plugin scan [DIR...] | describe BUNDLE | bench BUNDLE ID");
    return 2;
}
