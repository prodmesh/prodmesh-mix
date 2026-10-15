// mix-pluginhost: runs one plugin outside the engine (see
// src/plugin/sandbox_shm.h for the exchange). If the plugin crashes, only
// this process dies; the engine bypasses the insert and starts a new host.
//
//   mix-pluginhost --shm NAME --channels N --max-block N --rate HZ
//                  --plugin FORMAT BUNDLE ID [--plugin ...]
//                  [--state-file F]   lines "N base64": state per plugin
// Several --plugin run as one chain, in order.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(PM_HOST_GUI)
#include <QApplication>
#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#include <QCloseEvent>
#include <QMap>
#include <QSocketNotifier>
#include <QTimer>
#include <QWidget>
#endif

#include "core/base64.h"
#include "core/rt_guard.h"
#include "core/rt_thread.h"
#if defined(__APPLE__)
#include <AudioToolbox/AudioWorkInterval.h>
#include <os/workgroup.h>
#endif
#include "plugin/plugin.h"
#include "plugin/sandbox_shm.h"

using namespace pm;

namespace {

std::atomic<bool> gQuit{false};
void onTerm(int) { gQuit.store(true); }

#if defined(PM_HOST_GUI)
// An accessory process (no Dock icon) is not brought forward by showing a
// window; a plugin window the operator asked for must come to the front.
void bringToFront() {
#if defined(__APPLE__)
    auto* cls = reinterpret_cast<id>(objc_getClass("NSApplication"));
    using Shared = id (*)(id, SEL);
    using Activate = void (*)(id, SEL, BOOL);
    id nsApp = reinterpret_cast<Shared>(objc_msgSend)(cls, sel_registerName("sharedApplication"));
    reinterpret_cast<Activate>(objc_msgSend)(nsApp, sel_registerName("activateIgnoringOtherApps:"), YES);
#endif
}

// A plugin's editor in its own window; closing the window destroys the
// editor (the plugin keeps running).
class EditorWindow : public QWidget {
public:
    explicit EditorWindow(Processor& p) : plugin_(p) {
        setAttribute(Qt::WA_NativeWindow);
        setWindowTitle(QString::fromStdString(p.name()));
        plugin_.onEditorResize = [this](int w, int h) { setFixedSize(w, h); };
    }
    bool open() {
        // The native view must exist before the plugin attaches to it.
        resize(600, 400);
        show();
        int w = 0, h = 0;
        if (!plugin_.createEditor(reinterpret_cast<void*>(winId()), &w, &h))
            return false;
        setFixedSize(w, h);
        bringToFront();
        raise();
        activateWindow();
        return true;
    }

protected:
    void closeEvent(QCloseEvent* e) override {
        plugin_.onEditorResize = nullptr;
        plugin_.destroyEditor();
        e->accept();
        deleteLater();
    }

private:
    Processor& plugin_;
};
#endif

}  // namespace

int main(int argc, char** argv) {
    std::string shm, stateFile;
    std::vector<plugin::PluginRef> chain;
    int channels = 1, maxBlock = 128;
    double rate = 48000;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--plugin" && i + 3 < argc) {
            chain.push_back({argv[i + 1], argv[i + 2], argv[i + 3]});
            i += 3;
            continue;
        }
        if (i + 1 >= argc) break;
        const std::string v = argv[++i];
        if (k == "--shm") shm = v;
        else if (k == "--channels") channels = std::stoi(v);
        else if (k == "--max-block") maxBlock = std::stoi(v);
        else if (k == "--rate") rate = std::stod(v);
        else if (k == "--state-file") stateFile = v;
    }
    std::signal(SIGTERM, onTerm);
    std::signal(SIGINT, onTerm);
#if defined(PM_HOST_GUI)
    // Before any plugin loads: editors need the application object. Qt
    // gets only argv[0] — it would read our --plugin as its own -plugin.
    int qtArgc = 1;
    QApplication app(qtArgc, argv);
#if defined(__APPLE__)
    // An accessory process: plugin windows, but no Dock icon or menu bar
    // per host (a show can run dozens). NSApplicationActivationPolicyAccessory.
    {
        auto* cls = reinterpret_cast<id>(objc_getClass("NSApplication"));
        using Shared = id (*)(id, SEL);
        using SetPolicy = BOOL (*)(id, SEL, long);
        id nsApp = reinterpret_cast<Shared>(objc_msgSend)(cls, sel_registerName("sharedApplication"));
        reinterpret_cast<SetPolicy>(objc_msgSend)(nsApp, sel_registerName("setActivationPolicy:"), 1);
    }
#endif
    app.setQuitOnLastWindowClosed(false);
#endif

    const int fd = shm_open(shm.c_str(), O_RDWR, 0);
    if (fd < 0) {
        std::fprintf(stderr, "mix-pluginhost: no shared memory %s\n", shm.c_str());
        return 2;
    }
    const std::size_t size = sandbox::regionSize(channels, maxBlock);
    void* region = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (region == MAP_FAILED) return 2;
    auto view = sandbox::View::of(region, channels, maxBlock);
    auto& h = *view.header;
    if (h.magic != sandbox::kMagic || h.version != sandbox::kVersion ||
        h.channels != channels || h.maxBlock != maxBlock)
        return 2;

    std::vector<std::unique_ptr<Processor>> plugins;
    std::string names;
    int latency = 0;
    for (const auto& r : chain) {
        std::string err;
        auto p = plugin::load(r.format, r.path, r.id, rate, maxBlock, channels, &err);
        if (!p) {
            std::fprintf(stderr, "mix-pluginhost: %s\n", err.c_str());
            h.state.store(std::uint32_t(sandbox::HostState::Failed));
            return 3;
        }
        names += (names.empty() ? "" : " + ") + p->name();
        latency += p->latencySamples();
        plugins.push_back(std::move(p));
    }
    if (!stateFile.empty())
        if (FILE* f = std::fopen(stateFile.c_str(), "r")) {
            std::string line;
            char buf[65536];
            while (std::fgets(buf, sizeof buf, f)) {
                line += buf;
                if (line.empty() || line.back() != '\n') continue;
                line.pop_back();
                const auto sp = line.find(' ');
                const int i = std::atoi(line.c_str());
                std::string raw;
                if (sp != std::string::npos && i >= 0 && i < int(plugins.size()) &&
                    base64Decode(line.substr(sp + 1), raw))
                    plugins[std::size_t(i)]->loadState(raw);
                line.clear();
            }
            std::fclose(f);
        }
    std::strncpy(h.name, names.c_str(), sizeof h.name - 1);
    h.latency.store(latency);
#if defined(__APPLE__)
    // Like the engine's own render threads: an audio work interval with a
    // deadline per block, so the scheduler knows the render thread is on
    // the clock (P-cores, priority) rather than inferring it. Without it, a
    // busy machine (a VM, say) delayed some wake-ups by ~2 ms. Created
    // before Ready: the first one in a process costs ~100 ms, which must
    // not be spent passing audio through dry (e.g. after a restart).
    // PM_HOST_NO_INTERVAL=1: without it, for A/B measurements only.
    os_workgroup_interval_t interval =
        std::getenv("PM_HOST_NO_INTERVAL")
            ? nullptr
            : AudioWorkIntervalCreate("ProdMesh Mix plugin host",
                                      OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);
#endif
    h.state.store(std::uint32_t(sandbox::HostState::Ready), std::memory_order_release);

    // The render loop gets its own real-time thread; the main thread is
    // free for plugin editors later.
    std::thread render([&] {
        const auto period = std::uint64_t(1e9 * maxBlock / rate);
        rt::promoteCurrentThread(period, period / 2);
#if defined(__APPLE__)
        rt::WorkgroupMembership member(interval);
        bool intervalOk = member.joined();
#endif
        // Allocate before entering the guarded scope: only the plugins'
        // own allocations should be counted against them.
        std::vector<float*> ptr(static_cast<std::size_t>(channels));
        rt::ScopedRealtime realtime;
        rt::ScopedThirdParty attribute(&h.hostAllocs);
        std::uint64_t last = h.done.load();
        // PM_HOST_STATS=1: per-block timing, printed to stderr at exit.
        // wake = request posted -> this thread running; work = plugins.
        const bool stats = std::getenv("PM_HOST_STATS") != nullptr;
        std::uint64_t blocks = 0, late = 0, maxWake = 0, maxWork = 0;
        std::uint64_t skipped = 0, firstSkipAt = 0;  // requests never seen
        std::uint64_t wakeOver[4] = {}, workOver[4] = {};  // >0.25,0.5,0.75,1 period
        while (!gQuit.load(std::memory_order_relaxed)) {
            const std::uint32_t bell = h.doorbell.load(std::memory_order_acquire);
            const std::uint64_t seq = h.request.load(std::memory_order_acquire);
            if (seq == last) {
                // 100 ms timeout: notice an engine that went away.
                sandbox::waitDoorbell(h, bell, 100'000'000);
                if (getppid() == 1) gQuit.store(true);
                continue;
            }
            const int slot = int(seq & 1);
            const int frames = int(h.frames[slot]);
            const std::uint64_t t0 = stats ? rt::nowNs() : 0;
            const std::uint64_t posted = h.requestNs[slot].load(std::memory_order_relaxed);
            for (int c = 0; c < channels; ++c) {
                std::memcpy(view.out(slot, c), view.in(slot, c),
                            sizeof(float) * std::size_t(frames));
                ptr[std::size_t(c)] = view.out(slot, c);
            }
#if defined(__APPLE__)
            if (intervalOk && posted &&
                os_workgroup_interval_start(interval, rt::nsToHostTicks(posted),
                                            rt::nsToHostTicks(posted + period),
                                            nullptr) != 0)
                intervalOk = false;  // a scheduling hint only
#endif
            for (auto& p : plugins) p->process(ptr.data(), channels, frames);
            h.done.store(seq, std::memory_order_release);
#if defined(__APPLE__)
            if (intervalOk && posted &&
                os_workgroup_interval_finish(interval, nullptr) != 0)
                intervalOk = false;
#endif
            if (stats && last && seq > last + 1) {
                if (!skipped) firstSkipAt = seq;
                skipped += seq - last - 1;
            }
            if (stats && seq == last + 1) {
                const std::uint64_t t1 = rt::nowNs();
                const std::uint64_t wake = t0 > posted ? t0 - posted : 0, work = t1 - t0;
                ++blocks;
                late += wake + work > period;
                maxWake = std::max(maxWake, wake);
                maxWork = std::max(maxWork, work);
                for (int q = 0; q < 4; ++q) {
                    wakeOver[q] += wake > period * std::uint64_t(q + 1) / 4;
                    workOver[q] += work > period * std::uint64_t(q + 1) / 4;
                }
            }
            last = seq;
        }
        if (stats)
            std::fprintf(stderr,
                         "pluginhost %d: blocks %llu late %llu skipped %llu (first at %llu) | wake max %.0f us, "
                         ">25/50/75/100%% %llu/%llu/%llu/%llu | work max %.0f us, "
                         ">25/50/75/100%% %llu/%llu/%llu/%llu\n",
                         int(getpid()), (unsigned long long)blocks,
                         (unsigned long long)late, (unsigned long long)skipped,
                         (unsigned long long)firstSkipAt, maxWake / 1e3,
                         (unsigned long long)wakeOver[0], (unsigned long long)wakeOver[1],
                         (unsigned long long)wakeOver[2], (unsigned long long)wakeOver[3],
                         maxWork / 1e3, (unsigned long long)workOver[0],
                         (unsigned long long)workOver[1], (unsigned long long)workOver[2],
                         (unsigned long long)workOver[3]);
        for (auto& p : plugins) p->stopProcessing();
#if defined(__APPLE__)
        if (interval) os_release(interval);
#endif
    });
#if defined(PM_HOST_GUI)
    // Main thread: plugin editors, and line commands from the engine on
    // stdin ("editor N"). EOF means the engine went away.
    QMap<int, QWidget*> editors;
    QSocketNotifier commands(0, QSocketNotifier::Read);
    std::string pending;
    QObject::connect(&commands, &QSocketNotifier::activated, [&] {
        char buf[256];
        const ssize_t n = read(0, buf, sizeof buf);
        if (n <= 0) {
            gQuit.store(true);
            commands.setEnabled(false);
            return;
        }
        pending.append(buf, std::size_t(n));
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (line == "state-save") {
                // Main thread, as CLAP requires; one line per plugin.
                for (std::size_t i = 0; i < plugins.size(); ++i) {
                    std::string raw;
                    if (plugins[i]->saveState(raw))
                        std::printf("state %zu %s\n", i, base64Encode(raw).c_str());
                }
                std::fflush(stdout);
                continue;
            }
            if (line.rfind("editor ", 0) == 0) {
                const int i = std::atoi(line.c_str() + 7);
                if (i < 0 || i >= int(plugins.size())) continue;
                if (auto* w = editors.value(i); w && w->isVisible()) {
                    bringToFront();
                    w->raise();
                    w->activateWindow();
                    continue;
                }
                auto* w = new EditorWindow(*plugins[std::size_t(i)]);
                QObject::connect(w, &QObject::destroyed, [&editors, i] {
                    editors.remove(i);
                });
                if (w->open()) {
                    editors[i] = w;
                } else {
                    std::fprintf(stderr, "mix-pluginhost: %s has no editor\n",
                                 plugins[std::size_t(i)]->name().c_str());
                    w->deleteLater();
                }
            }
        }
    });
    QTimer quitCheck;
    QObject::connect(&quitCheck, &QTimer::timeout, [&] {
        if (gQuit.load()) app.quit();
    });
    quitCheck.start(100);
    // ~30 Hz main-thread housekeeping (VST3: meters to the controller).
    QTimer idle;
    QObject::connect(&idle, &QTimer::timeout, [&] {
        for (auto& p : plugins) p->idle();
    });
    idle.start(33);
    app.exec();
    gQuit.store(true);
    for (auto* w : editors) w->close();
#endif
    render.join();
    plugins.clear();
    munmap(region, size);
    return 0;
}
