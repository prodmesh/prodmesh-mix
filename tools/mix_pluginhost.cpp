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
#include <QCloseEvent>
#include <QMap>
#include <QSocketNotifier>
#include <QTimer>
#include <QWidget>
#endif

#include "core/base64.h"
#include "core/rt_guard.h"
#include "core/rt_thread.h"
#include "plugin/plugin.h"
#include "plugin/sandbox_shm.h"

using namespace pm;

namespace {

std::atomic<bool> gQuit{false};
void onTerm(int) { gQuit.store(true); }

#if defined(PM_HOST_GUI)
// A plugin's editor in its own window; closing the window destroys the
// editor (the plugin keeps running).
class EditorWindow : public QWidget {
public:
    explicit EditorWindow(Processor& p) : plugin_(p) {
        setAttribute(Qt::WA_NativeWindow);
        setWindowTitle(QString::fromStdString(p.name()));
    }
    bool open() {
        // The native view must exist before the plugin attaches to it.
        resize(600, 400);
        show();
        int w = 0, h = 0;
        if (!plugin_.createEditor(reinterpret_cast<void*>(winId()), &w, &h))
            return false;
        setFixedSize(w, h);
        raise();
        activateWindow();
        return true;
    }

protected:
    void closeEvent(QCloseEvent* e) override {
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
    h.state.store(std::uint32_t(sandbox::HostState::Ready), std::memory_order_release);

    // The render loop gets its own real-time thread; the main thread is
    // free for plugin editors later.
    std::thread render([&] {
        const auto period = std::uint64_t(1e9 * maxBlock / rate);
        rt::promoteCurrentThread(period, period / 2);
        // Allocate before entering the guarded scope: only the plugins'
        // own allocations should be counted against them.
        std::vector<float*> ptr(static_cast<std::size_t>(channels));
        rt::ScopedRealtime realtime;
        rt::ScopedThirdParty attribute(&h.hostAllocs);
        std::uint64_t last = h.done.load();
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
            for (int c = 0; c < channels; ++c) {
                std::memcpy(view.out(slot, c), view.in(slot, c),
                            sizeof(float) * std::size_t(frames));
                ptr[std::size_t(c)] = view.out(slot, c);
            }
            for (auto& p : plugins) p->process(ptr.data(), channels, frames);
            h.done.store(seq, std::memory_order_release);
            last = seq;
        }
        for (auto& p : plugins) p->stopProcessing();
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
    app.exec();
    gQuit.store(true);
    for (auto* w : editors) w->close();
#endif
    render.join();
    plugins.clear();
    munmap(region, size);
    return 0;
}
