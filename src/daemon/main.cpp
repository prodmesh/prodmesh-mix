// mixd: the headless engine daemon — the Tier 0 process. It owns the
// engine and the audio device; UIs are clients over a local socket and can
// crash, hang or restart without touching program audio.
//
//   mixd [--driver coreaudio|sim] [--rate HZ] [--block N] [--workers N]
//        [--socket NAME] [--session FILE] [--fresh] [--audible]
//        [--record DIR] [--ring-seconds S]
//
// The show is saved to --session (default: the app data dir) within half a
// second of any change, atomically, and reloaded on start — restarting the
// engine never loses the mix.
//
// With --record, every source (raw) and the program bus are recorded from
// the moment the engine starts, one take folder per run. Files a crash
// left unfinished are repaired on the next start.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSocketNotifier>
#include <QTimer>
#include <csignal>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

#include "daemon/shows.h"
#include "driver/driver.h"
#include "engine/engine.h"
#include "proto/protocol.h"
#include "record/recorder.h"
#include "record/wav_writer.h"
#include <QDateTime>
#include <QProcess>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QStorageInfo>

using namespace pm;

namespace {

struct RenderCtx {
    Engine* engine = nullptr;
    bool silence = true;
};

void render(void* ctx, const float* const* in, int numIn, float* const* out,
            int numOut, int frames) {
    auto* rc = static_cast<RenderCtx*>(ctx);
    rc->engine->process(in, numIn, out, numOut, frames);
    if (rc->silence)
        for (int c = 0; c < numOut; ++c)
            std::memset(out[c], 0, sizeof(float) * std::size_t(frames));
}

// SIGINT/SIGTERM -> Qt event loop, so shutdown stops the driver before the
// engine is destroyed.
int gSignalPipe[2] = {-1, -1};
void onSignal(int) {
    const char c = 1;
    (void)!::write(gSignalPipe[1], &c, 1);
}

class Daemon : public QObject {
public:
    Daemon(std::unique_ptr<Driver> driver, int workers, bool audible)
        : driver_(std::move(driver)) {
        const auto& dc = driver_->config();
        EngineConfig ec;
        ec.sampleRate = dc.sampleRate;
        ec.maxBlock = dc.blockSize;
        ec.numInputs = dc.numInputs;
        ec.numOutputs = dc.numOutputs;
        ec.workerThreads = workers;
        ec.workgroup = driver_->workgroup();
        engine_ = std::make_unique<Engine>(ec);
        ctx_.engine = engine_.get();
        ctx_.silence = !audible;
        periodUs_ = 1e6 * dc.blockSize / dc.sampleRate;
    }

    ~Daemon() override {
        if (driver_) driver_->stop();  // before the engine goes away
        if (recorder_) recorder_->stop();  // drains what the taps hold
        if (saveTimer_.isActive()) saveSession();
    }

    void setRecording(const QString& dir, double ringSeconds) {
        recordDir_ = dir;
        ringSeconds_ = ringSeconds;
    }

    bool start(const QString& socketName, const QString& session, bool fresh,
               QString* error) {
        sessionPath_ = session;
        std::string err;
        GraphDesc show = shows::livestream();
        if (!fresh && loadSession(show))
            qInfo("mixd: restored session %s", qPrintable(sessionPath_));
        if (!engine_->setGraph(show, &err)) {
            *error = QString::fromStdString(err);
            return false;
        }
        startCatalog();
        if (!recordDir_.isEmpty() && !startTake(error)) return false;
        if (!driver_->start(&render, &ctx_, &err)) {
            *error = QString::fromStdString(err);
            return false;
        }
        QLocalServer::removeServer(socketName);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(socketName)) {
            *error = server_.errorString();
            return false;
        }
        connect(&server_, &QLocalServer::newConnection, this,
                &Daemon::onConnection);

        connect(&pumpTimer_, &QTimer::timeout, this, &Daemon::pump);
        pumpTimer_.start(5);
        connect(&meterTimer_, &QTimer::timeout, this, &Daemon::sendMeters);
        meterTimer_.start(33);
        connect(&healthTimer_, &QTimer::timeout, this, &Daemon::sendHealth);
        healthTimer_.start(500);
        saveTimer_.setSingleShot(true);
        saveTimer_.setInterval(500);
        connect(&saveTimer_, &QTimer::timeout, this, &Daemon::saveSession);

        qInfo("mixd: %s %.0f Hz, block %d, socket %s",
              driver_->config().numOutputs ? "running" : "running (no outputs)",
              driver_->config().sampleRate, driver_->config().blockSize,
              qPrintable(server_.fullServerName()));
        return true;
    }

private:
    void onConnection() {
        while (auto* s = server_.nextPendingConnection()) {
            clients_.append(s);
            connect(s, &QLocalSocket::readyRead, this, [this, s] { read(s); });
            connect(s, &QLocalSocket::disconnected, this, [this, s] {
                clients_.removeAll(s);
                s->deleteLater();
            });
        }
    }

    void read(QLocalSocket* s) {
        while (s->canReadLine()) {
            const auto line = s->readLine();
            QJsonParseError pe;
            const auto doc = QJsonDocument::fromJson(line, &pe);
            if (!doc.isObject()) {
                send(s, {{"type", "error"}, {"message", pe.errorString()}});
                continue;
            }
            handle(s, doc.object());
        }
    }

    void handle(QLocalSocket* s, const QJsonObject& m) {
        const auto op = m["op"].toString();
        if (op == "hello") {
            send(s, snapshot());
            send(s, catalogMessage());
        } else if (op == "ping") {
            send(s, {{"type", "pong"}, {"t", m["t"]}});
        } else if (op == "setLevel") {
            const auto id = NodeId(m["id"].toDouble());
            const float db = float(m["faderDb"].toDouble());
            const bool mute = m["mute"].toBool();
            engine_->setLevel(id, db, mute);
            saveTimer_.start();
            broadcast({{"type", "level"},
                       {"id", double(id)},
                       {"faderDb", db},
                       {"mute", mute}},
                      s);
        } else if (op == "openEditor") {
            std::string err;
            if (!engine_->openEditor(NodeId(m["id"].toDouble()),
                                     m["insert"].toInt(), &err))
                send(s, {{"type", "error"},
                         {"op", op},
                         {"message", QString::fromStdString(err)}});
        } else if (op == "setGraph") {
            GraphDesc g;
            QString err;
            std::string engineErr;
            if (!proto::graphFromJson(m["graph"].toObject(), g, &err)) {
                send(s, {{"type", "error"}, {"op", op}, {"message", err}});
            } else if (!engine_->setGraph(g, &engineErr)) {
                send(s, {{"type", "error"},
                         {"op", op},
                         {"message", QString::fromStdString(engineErr)}});
            } else {
                // Latency and plugin warnings are known once the edit has
                // been compiled; pump() sends the snapshot then.
                snapshotPending_ = true;
                saveTimer_.start();
            }
        } else {
            send(s, {{"type", "error"}, {"op", op}, {"message", "unknown op"}});
        }
    }

    // Repairs WAVs whose header lags their data (the engine died mid-take).
    void recoverUnfinished() {
        QDirIterator it(recordDir_, {"*.wav"}, QDir::Files,
                        QDirIterator::Subdirectories);
        int fixed = 0;
        while (it.hasNext()) {
            const auto path = it.next().toStdString();
            rec::WavInfo info;
            if (!rec::WavWriter::readInfo(path, info) || !info.channels)
                continue;
            const auto bytes = std::uint64_t(QFileInfo(it.filePath()).size()) -
                               info.dataOffset;
            if (bytes / (std::uint64_t(info.channels) * 4) > info.frames &&
                rec::WavWriter::recover(path, nullptr))
                ++fixed;
        }
        if (fixed)
            qInfo("mixd: repaired %d recording(s) left unfinished by a crash",
                  fixed);
    }

    // Sources are named after the strip they feed ("Host", not "Host
    // source"); the program bus records post-limiter.
    bool startTake(QString* error) {
        recoverUnfinished();
        const auto& g = engine_->graph();
        std::vector<rec::TrackSpec> tracks;
        std::vector<std::pair<NodeId, std::shared_ptr<RecordTap>>> taps;
        const auto ring = std::size_t(ringSeconds_ * driver_->config().sampleRate);
        auto add = [&](const NodeDesc& n, std::string name) {
            auto tap = std::make_shared<RecordTap>(n.channels, ring);
            tracks.push_back({n.id, std::move(name), tap});
            taps.push_back({n.id, tap});
        };
        for (const auto& n : g.nodes) {
            if (n.kind != NodeKind::Input && n.kind != NodeKind::Generator)
                continue;
            std::string name = n.name;
            for (const auto& e : g.edges)
                if (e.from == n.id)
                    if (const auto* to = g.find(e.to);
                        to && to->kind == NodeKind::Strip) {
                        name = to->name;
                        break;
                    }
            add(n, name);
        }
        for (const auto& e : g.edges) {
            const auto* to = g.find(e.to);
            const auto* from = g.find(e.from);
            if (to && from && to->kind == NodeKind::Output &&
                to->ioChannel == 0 && from->kind == NodeKind::Bus)
                add(*from, from->name.empty() ? "Program" : from->name);
        }
        takeName_ = QDateTime::currentDateTime().toString("yyyy-MM-dd HHmmss");
        rec::RecorderOptions ro;
        ro.sampleRate = std::uint32_t(driver_->config().sampleRate);
        recorder_ = std::make_unique<rec::Recorder>(ro);
        std::string err;
        if (!recorder_->start((recordDir_ + "/" + takeName_).toStdString(),
                              std::move(tracks), &err)) {
            *error = QString::fromStdString(err);
            return false;
        }
        engine_->setRecordTaps(std::move(taps));
        takeClock_.start();
        qInfo("mixd: recording take \"%s\" to %s", qPrintable(takeName_),
              qPrintable(recordDir_));
        return true;
    }

    // Plugins that allocate on the audio thread: a dropout risk the
    // operator should know about (and the vendor should fix).
    QJsonArray pluginRt() const {
        QJsonArray out;
        for (const auto& i : engine_->pluginRtIssues())
            out.append(QJsonObject{{"node", double(i.node)},
                                   {"plugin", QString::fromStdString(i.plugin)},
                                   {"allocations", double(i.allocations)},
                                   {"missed", double(i.missed)}});
        return out;
    }

    QJsonObject recordingHealth() const {
        if (!recorder_) return {{"on", false}};
        std::uint64_t dropped = 0;
        double fill = 0;
        const auto stats = recorder_->stats();
        for (const auto& t : stats) {
            dropped += t.droppedFrames;
            fill = std::max(fill, double(t.ringFill));
        }
        const QStorageInfo disk(recordDir_);
        const double bytesPerSec =
            double(stats.size()) * 2 * 4 * driver_->config().sampleRate;
        return {{"on", true},
                {"take", takeName_},
                {"seconds", takeClock_.elapsed() / 1000.0},
                {"tracks", int(stats.size())},
                {"droppedFrames", double(dropped)},
                {"droppedSeconds",
                 stats.empty() ? 0.0
                               : double(dropped) / double(stats.size()) /
                                     driver_->config().sampleRate},
                {"ringFill", fill},
                {"diskFreeBytes", double(disk.bytesAvailable())},
                // Upper bound (stereo for every track) — errs on the safe side.
                {"diskSecondsLeft",
                 bytesPerSec > 0 ? double(disk.bytesAvailable()) / bytesPerSec
                                 : 0.0},
                {"error", QString::fromStdString(recorder_->lastError())}};
    }

    bool loadSession(GraphDesc& out) {
        QFile f(sessionPath_);
        if (!f.open(QIODevice::ReadOnly)) return false;
        const auto doc = QJsonDocument::fromJson(f.readAll());
        GraphDesc g;
        QString err;
        std::string engineErr;
        if (doc["format"].toString() != "prodmesh-mix-session" ||
            !proto::graphFromJson(doc["graph"].toObject(), g, &err)) {
            qWarning("mixd: ignoring unreadable session %s",
                     qPrintable(sessionPath_));
            return false;
        }
        out = std::move(g);
        return true;
    }

    // QSaveFile writes a temp file and renames it over the old one, so a
    // crash mid-save leaves the previous session intact.
    void saveSession() {
        if (sessionPath_.isEmpty()) return;
        // Plugins' own settings (what was dialled into their windows).
        engine_->collectPluginStates(200);
        QDir().mkpath(QFileInfo(sessionPath_).absolutePath());
        QSaveFile f(sessionPath_);
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write(QJsonDocument(QJsonObject{
                                  {"format", "prodmesh-mix-session"},
                                  {"version", 1},
                                  {"graph", proto::graphToJson(engine_->graph())}})
                    .toJson(QJsonDocument::Indented));
        if (!f.commit())
            qWarning("mixd: could not save session %s", qPrintable(sessionPath_));
    }

    // --- plugin catalog: cached, rescanned out of process ----------------
    // Machine-wide, not per show: every session shares one catalog, so a
    // new session never triggers a rescan (plugins can pop up licence
    // dialogs when loaded).
    QString catalogPath() const {
        const auto dir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        return dir + "/plugins.jsonl";
    }

    void loadCatalog(const QByteArray& lines) {
        QJsonArray list;
        for (const auto& l : lines.split('\n')) {
            const auto o = QJsonDocument::fromJson(l).object();
            if (o.isEmpty() || o.contains("quarantined") || o["empty"].toBool() ||
                o["instrument"].toBool())
                continue;
            list.append(o);
        }
        catalog_ = list;
    }

    void startCatalog() {
        QFile cache(catalogPath());
        if (cache.open(QIODevice::ReadOnly)) loadCatalog(cache.readAll());
        const QString tool =
            QCoreApplication::applicationDirPath() + "/mix-plugin";
        if (!QFileInfo::exists(tool)) return;
        scanner_ = new QProcess(this);
        connect(scanner_, &QProcess::finished, this, [this](int code) {
            const auto out = scanner_->readAllStandardOutput();
            scanner_->deleteLater();
            scanner_ = nullptr;
            if (code == 0) {
                QSaveFile f(catalogPath());
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(out);
                    f.commit();
                }
                loadCatalog(out);
            }
            broadcast(catalogMessage(), nullptr);
        });
        // Incremental: only new or updated bundles get loaded.
        scanner_->start(tool, {"scan", "--cache", catalogPath()});
    }

    QJsonObject catalogMessage() const {
        return {{"type", "catalog"},
                {"plugins", catalog_},
                {"scanning", scanner_ != nullptr}};
    }

    QJsonObject snapshot() const {
        const auto& dc = driver_->config();
        QJsonObject latency, delay;
        for (const auto& n : engine_->graph().nodes) {
            if (const int l = engine_->latencyOf(n.id))
                latency[QString::number(n.id)] = l;
            const auto d = engine_->delayInfo(n.id);
            QJsonArray comp;
            for (const auto& [to, samples] : d.compensation)
                comp.append(QJsonArray{double(to), samples});
            delay[QString::number(n.id)] = QJsonObject{
                {"chain", d.chain}, {"arrival", d.arrival}, {"comp", comp}};
        }
        QJsonArray warnings;
        for (const auto& w : engine_->warnings())
            warnings.append(QString::fromStdString(w));
        return {{"type", "snapshot"},
                {"proto", proto::kVersion},
                {"graph", proto::graphToJson(engine_->graph())},
                {"latency", latency},
                {"delay", delay},
                {"warnings", warnings},
                {"engine", QJsonObject{{"sampleRate", dc.sampleRate},
                                       {"block", dc.blockSize},
                                       {"outputs", dc.numOutputs},
                                       {"periodUs", periodUs_}}}};
    }

    void pump() {
        engine_->pump();
        for (const auto& e : engine_->drainEvents()) {
            qWarning("mixd: %s", e.c_str());
            broadcast({{"type", "event"},
                       {"level", "warning"},
                       {"message", QString::fromStdString(e)}},
                      nullptr);
        }
        if (snapshotPending_ && engine_->settled()) {
            snapshotPending_ = false;
            broadcast(snapshot(), nullptr);
        }
        while (auto smp = driver_->stats().samples.pop()) {
            const double us = smp->durationNs / 1e3;
            windowMax_ = std::max(windowMax_, us);
            windowSum_ += us;
            ++windowCount_;
        }
    }

    void sendMeters() {
        if (clients_.isEmpty()) return;
        QJsonArray m;
        for (const auto& n : engine_->graph().nodes) {
            MeterReading r;
            if (!engine_->takeMeter(n.id, r)) continue;
            const int c1 = r.channels > 1 ? 1 : 0;
            m.append(QJsonArray{double(n.id), proto::toDbfs(r.peak[0]),
                                proto::toDbfs(r.peak[c1]),
                                proto::toDbfs(r.rms[0]),
                                proto::toDbfs(r.rms[c1])});
        }
        broadcast({{"type", "meters"}, {"m", m}}, nullptr);
    }

    void sendHealth() {
        const auto& st = driver_->stats();
        const double mean = windowCount_ ? windowSum_ / windowCount_ : 0;
        broadcast({{"type", "health"},
                   {"xruns", double(st.xruns.load())},
                   {"callbacks", double(st.callbacks.load())},
                   {"meanLoad", mean / periodUs_},
                   {"worstLoad", windowMax_ / periodUs_},
                   {"rtViolations", double(rt::violations())},
                   {"settled", engine_->settled()},
                   {"lastError", QString::fromStdString(engine_->lastError())},
                   {"recording", recordingHealth()},
                   {"pluginRt", pluginRt()}},
                  nullptr);
        windowSum_ = windowMax_ = 0;
        windowCount_ = 0;
    }

    void send(QLocalSocket* s, const QJsonObject& o) {
        // A client that stops reading must not grow our memory forever.
        if (s->bytesToWrite() > (8 << 20)) {
            s->abort();
            return;
        }
        s->write(proto::frame(o));
    }

    void broadcast(const QJsonObject& o, QLocalSocket* except) {
        const auto bytes = proto::frame(o);
        for (auto* c : clients_)
            if (c != except) {
                if (c->bytesToWrite() > (8 << 20)) c->abort();
                else c->write(bytes);
            }
    }

    std::unique_ptr<Driver> driver_;
    std::unique_ptr<Engine> engine_;
    RenderCtx ctx_;
    QLocalServer server_;
    QList<QLocalSocket*> clients_;
    QTimer pumpTimer_, meterTimer_, healthTimer_, saveTimer_;
    QString sessionPath_;
    QString recordDir_, takeName_;
    double ringSeconds_ = 30;
    QElapsedTimer takeClock_;
    std::unique_ptr<rec::Recorder> recorder_;
    QJsonArray catalog_;
    bool snapshotPending_ = false;
    QProcess* scanner_ = nullptr;
    double periodUs_ = 0;
    double windowSum_ = 0, windowMax_ = 0;
    std::uint64_t windowCount_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("ProdMesh");
    app.setApplicationName("Mix");

    QCommandLineParser p;
    p.addHelpOption();
    p.addOptions({
        {"driver", "coreaudio or sim", "name", "coreaudio"},
        {"rate", "Sample rate", "hz", "48000"},
        {"block", "Block size", "frames", "128"},
        {"workers", "Extra render threads", "n", "0"},
        {"socket", "Local socket name", "name", proto::kDefaultSocket},
        {"session", "Session file", "path"},
        {"fresh", "Start from the template, ignoring the saved session"},
        {"audible", "Send program to the device (default: silence)"},
        {"record", "Record every source and the program bus here", "dir"},
        {"ring-seconds", "Per-track disk buffer (T0-8 headroom)", "s", "30"},
    });
    p.process(app);

    DriverConfig dc;
    dc.sampleRate = p.value("rate").toDouble();
    dc.blockSize = p.value("block").toInt();
    dc.numOutputs = 4;
    std::unique_ptr<Driver> driver;
    std::string err;
    if (p.value("driver") == "sim") {
        driver = makeSimDriver(dc, false);
#if defined(__APPLE__)
    } else if (p.value("driver") == "coreaudio") {
        driver = makeCoreAudioDriver(dc, &err);
#endif
    }
    if (!driver) {
        qCritical("mixd: no driver: %s", err.c_str());
        return 1;
    }

    ::socketpair(AF_UNIX, SOCK_STREAM, 0, gSignalPipe);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    QSocketNotifier sn(gSignalPipe[0], QSocketNotifier::Read);
    QObject::connect(&sn, &QSocketNotifier::activated, &app,
                     &QCoreApplication::quit);

    auto daemon = std::make_unique<Daemon>(std::move(driver),
                                           p.value("workers").toInt(),
                                           p.isSet("audible"));
    QString why;
    if (p.isSet("record"))
        daemon->setRecording(p.value("record"),
                             p.value("ring-seconds").toDouble());
    const QString session =
        p.isSet("session")
            ? p.value("session")
            : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                  "/session.json";
    if (!daemon->start(p.value("socket"), session, p.isSet("fresh"), &why)) {
        qCritical("mixd: %s", qPrintable(why));
        return 1;
    }
    const int rc = app.exec();
    daemon.reset();
    return rc;
}
