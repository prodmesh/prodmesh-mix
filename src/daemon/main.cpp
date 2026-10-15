// mixd: the headless engine daemon — the Tier 0 process. It owns the
// engine and the audio device; UIs are clients over a local socket and can
// crash, hang or restart without touching program audio.
//
//   mixd [--driver coreaudio|sim] [--rate HZ] [--block N] [--workers N]
//        [--socket NAME] [--session FILE] [--fresh] [--audible]
//        [--record DIR] [--ring-seconds S]
//        [--input DEVICE|none] [--output DEVICE] [--show demo|inputs]
//        [--list-devices]
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
#include <optional>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

#include "daemon/shows.h"
#include "dsp/loudness.h"
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
    // Scenes: stored mixes, recalled with an optional fade.
    struct Scene {
        int id = 0;
        QString name;
        double fadeMs = 0;
        GraphDesc graph;
    };

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
        recordFixed_ = !dir.isEmpty();  // --record wins over project settings
        ringSeconds_ = ringSeconds;
    }
    void setRingSeconds(double s) { ringSeconds_ = s; }
    static QPair<QString, bool> defaultProject() { return startupProject(); }
    // Project mode: the session and the takes live in this folder.
    void setProject(const QString& dir, bool fresh) {
        projectDir_ = QDir(dir).absolutePath();
        projectFresh_ = fresh;
    }

    bool start(const QString& socketName, const QString& session, bool fresh,
               const QString& showName, QString* error) {
        sessionPath_ = projectDir_.isEmpty() ? session : projectDir_ + "/session.json";
        if (!projectDir_.isEmpty()) QDir().mkpath(projectDir_);
        std::string err;
        // Real inputs when the device has them (or when asked), the demo
        // show of generators otherwise.
        const int ins = driver_->config().numInputs;
        GraphDesc show = showName == "demo" || (showName.isEmpty() && ins == 0)
                             ? shows::livestream()
                             : shows::inputs(std::min(ins, 64));
        if (!fresh && loadSession(show))
            qInfo("mixd: restored session %s", qPrintable(sessionPath_));
        shows::normalize(show, ins);
        if (!engine_->setGraph(show, &err)) {
            *error = QString::fromStdString(err);
            return false;
        }
        startCatalog();
        setUpLoudness();
        if (!projectDir_.isEmpty()) {
            if (!recordFixed_) recordDir_ = projectRecord_ ? projectDir_ + "/Takes" : QString();
            pushRecent(projectDir_);
            saveTimer_.start();  // a new project gets its file at once
        }
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
        // "Modified since recall": checked at 4 Hz, sent only on change.
        connect(&sceneTimer_, &QTimer::timeout, this, [this] {
            const bool mod = sceneDiffers();
            if (mod != sceneModified_) {
                sceneModified_ = mod;
                broadcast(scenesMessage(), nullptr);
            }
        });
        sceneTimer_.start(250);
        connect(&loudTimer_, &QTimer::timeout, this, &Daemon::meterLoudness);
        loudTimer_.start(100);

        qInfo("mixd: running on %s: %d in / %d out, %.0f Hz, block %d, socket %s",
              driver_->deviceName().c_str(), driver_->config().numInputs,
              driver_->config().numOutputs, driver_->config().sampleRate,
              driver_->config().blockSize, qPrintable(server_.fullServerName()));
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
            send(s, projectMessage(projectFresh_ && m["client"].toString() == "mix-ui"));
            if (m["client"].toString() == "mix-ui") projectFresh_ = false;  // offered once
            send(s, snapshot());
            send(s, catalogMessage());
            send(s, scenesMessage());
            send(s, markersMessage());
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
        } else if (op == "projects") {
            send(s, projectsMessage());
        } else if (op == "newProject" || op == "openProject") {
            QString err;
            if (!(op == "newProject" ? newProject(m, &err) : openProject(m["path"].toString(), &err)))
                send(s, {{"type", "error"}, {"op", op}, {"message", err}});
        } else if (op == "resetLoudness") {
            if (loud_) loud_->reset();
        } else if (op == "setLoudnessTarget") {
            loudTarget_ = std::clamp(m["lufs"].toDouble(loudTarget_), -40.0, -5.0);
            loudTpLimit_ = std::clamp(m["truePeak"].toDouble(loudTpLimit_), -10.0, 0.0);
            saveTimer_.start();
        } else if (op == "addMarker") {
            addMarker(m["label"].toString());
        } else if (op.endsWith("Scene") || op == "undoRecall") {
            handleScene(s, op, m);
        } else {
            send(s, {{"type", "error"}, {"op", op}, {"message", "unknown op"}});
        }
    }

    // --- projects -------------------------------------------------------------
    // A project is a folder: session.json (mix, scenes, loudness target,
    // record setting) and Takes/ (one folder per take, with markers.csv).

    // PM_MIX_HOME (tests): projects and the recent list live there instead
    // of ~/Music/ProdMesh Mix and the app data folder.
    static QString home() { return qEnvironmentVariable("PM_MIX_HOME"); }
    static QString projectsRoot() {
        if (!home().isEmpty()) return home() + "/Projects";
        return QStandardPaths::writableLocation(QStandardPaths::MusicLocation) +
               "/ProdMesh Mix";
    }
    static QString recentPath() {
        if (!home().isEmpty()) return home() + "/recent.json";
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
               "/recent.json";
    }
    static QStringList recentProjects() {
        QFile f(recentPath());
        QStringList out;
        if (f.open(QIODevice::ReadOnly))
            for (const auto& v : QJsonDocument::fromJson(f.readAll()).array())
                out << v.toString();
        return out;
    }
    static void pushRecent(const QString& dir) {
        QStringList list = recentProjects();
        list.removeAll(dir);
        list.prepend(dir);
        while (list.size() > 12) list.removeLast();
        QDir().mkpath(QFileInfo(recentPath()).absolutePath());
        QSaveFile f(recentPath());
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write(QJsonDocument(QJsonArray::fromStringList(list)).toJson());
        f.commit();
    }
    // Where mixd starts when not told: the last project, else a new
    // "Untitled" (and the first UI to connect offers the Start window).
    static QPair<QString, bool> startupProject() {
        for (const auto& d : recentProjects())
            if (QFileInfo::exists(d + "/session.json")) return {d, false};
        return {projectsRoot() + "/Untitled", true};
    }

    QJsonObject projectMessage(bool offerStart) const {
        return {{"type", "project"},
                {"name", projectDir_.isEmpty() ? QString() : QFileInfo(projectDir_).fileName()},
                {"path", projectDir_},
                {"record", !recordDir_.isEmpty()},
                {"offerStart", offerStart}};
    }
    QJsonObject projectsMessage() const {
        QJsonArray recent;
        for (const auto& d : recentProjects()) {
            const QFileInfo fi(d + "/session.json");
            if (!fi.exists()) continue;
            recent.append(QJsonObject{{"name", QFileInfo(d).fileName()},
                                      {"path", d},
                                      {"modified", fi.lastModified().toString(Qt::ISODate)}});
        }
        QJsonArray templates{
            QJsonObject{{"id", "blank"}, {"name", "Blank"},
                        {"description", "A Program bus on outputs 1-2. Add channels as you go."}},
            QJsonObject{{"id", "inputs"}, {"name", "All inputs"},
                        {"description", QString("One channel per device input (%1), into Program.")
                                            .arg(driver_->config().numInputs)}},
            QJsonObject{{"id", "livestream"}, {"name", "Livestream (demo)"},
                        {"description", "Talent and remote guests with groups and a "
                                        "mix-minus, on test tones."}}};
        return {{"type", "projects"},
                {"recent", recent},
                {"templates", templates},
                {"location", projectsRoot()},
                {"current", projectDir_}};
    }

    bool newProject(const QJsonObject& m, QString* err) {
        const QString name = m["name"].toString().trimmed();
        if (name.isEmpty() || name.contains('/') || name.startsWith('.')) {
            *err = "Pick a name without slashes.";
            return false;
        }
        const QString root = m["location"].toString().isEmpty() ? projectsRoot()
                                                                 : m["location"].toString();
        const QString dir = QDir(root).absoluteFilePath(name);
        if (QFileInfo::exists(dir + "/session.json")) {
            *err = "A project called \"" + name + "\" already exists there.";
            return false;
        }
        const QString t = m["template"].toString("blank");
        GraphDesc g = t == "livestream" ? shows::livestream()
                      : t == "inputs"   ? shows::inputs(std::min(driver_->config().numInputs, 64))
                                        : shows::blank();
        return switchTo(dir, &g, m["record"].toBool(true), err);
    }
    bool openProject(const QString& path, QString* err) {
        const QString dir = QDir(path).absolutePath();
        if (!QFileInfo::exists(dir + "/session.json")) {
            *err = "That folder is not a ProdMesh Mix project (no session.json).";
            return false;
        }
        return switchTo(dir, nullptr, true, err);
    }

    // Close this show (save, finish the take) and open another while audio
    // keeps running: the engine crossfades from one mix to the next.
    bool switchTo(const QString& dir, const GraphDesc* fromTemplate, bool record,
                  QString* err) {
        // Save the current show exactly as it is, before anything of the
        // next one is loaded into memory (scenes, targets live here too).
        if (saveTimer_.isActive()) saveTimer_.stop();
        saveSession();
        const QString oldSession = sessionPath_;
        auto restore = [&] {  // the next show failed: back to this one's state
            sessionPath_ = oldSession;
            scenes_.clear();
            GraphDesc ignored;
            loadSession(ignored);
        };
        GraphDesc g;
        if (fromTemplate) {
            g = *fromTemplate;
        } else {
            sessionPath_ = dir + "/session.json";
            scenes_.clear();
            if (!loadSession(g)) {
                restore();
                *err = "Could not read that project's session.";
                return false;
            }
        }
        shows::normalize(g, driver_->config().numInputs);
        std::string e;
        if (!engine_->validate(g, &e)) {  // nothing of the running show touched
            if (!fromTemplate) restore();
            *err = QString::fromStdString(e);
            return false;
        }
        stopTake();  // finish the old show's take

        projectDir_ = dir;
        projectFresh_ = false;
        sessionPath_ = dir + "/session.json";
        QDir().mkpath(dir);
        if (fromTemplate) {
            scenes_.clear();
            currentScene_ = 0;
            nextSceneId_ = 1;
            projectRecord_ = record;
        }
        beforeRecall_.reset();
        markers_.clear();
        engine_->setGraph(g, &e);
        loudNode_ = 0;  // a new show is a new measurement
        setUpLoudness();
        if (!recordFixed_) recordDir_ = projectRecord_ ? dir + "/Takes" : QString();
        QString takeErr;
        if (!recordDir_.isEmpty() && !startTake(&takeErr))
            qWarning("mixd: recording did not start: %s", qPrintable(takeErr));
        pushRecent(dir);
        saveSession();
        snapshotPending_ = true;
        broadcast(projectMessage(false), nullptr);
        broadcast(scenesMessage(), nullptr);
        broadcast(markersMessage(), nullptr);
        qInfo("mixd: project %s", qPrintable(dir));
        return true;
    }

    void stopTake() {
        if (!recorder_) return;
        recordTaps_.clear();
        applyTaps();          // the audio thread stops feeding the old take
        recorder_->stop();    // then drain and close its files
        recorder_.reset();
    }

    // --- program loudness (BS.1770) and markers ----------------------------

    // The program bus: the bus feeding device outputs 1-2.
    NodeId programNode() const {
        const auto& g = engine_->graph();
        for (const auto& e : g.edges) {
            const auto* to = g.find(e.to);
            const auto* from = g.find(e.from);
            if (to && from && to->kind == NodeKind::Output && to->ioChannel == 0 &&
                from->kind == NodeKind::Bus)
                return from->id;
        }
        return 0;
    }
    void applyTaps() {
        auto all = recordTaps_;
        if (loudTap_) all.push_back({loudNode_, loudTap_});
        engine_->setRecordTaps(std::move(all));
    }
    // The meter follows the program bus; a different bus starts a new
    // measurement. The audio thread only copies into the tap's ring.
    void setUpLoudness() {
        const NodeId id = programNode();
        const auto* n = engine_->graph().find(id);
        const int ch = n ? n->channels : 0;
        if (id == loudNode_ && ch == loudChannels_) return;
        loudNode_ = id;
        loudChannels_ = ch;
        loud_.reset();
        loudTap_.reset();
        if (id && ch) {
            const double fs = driver_->config().sampleRate;
            loudTap_ = std::make_shared<RecordTap>(ch, std::size_t(5 * fs));
            loud_ = std::make_unique<dsp::LoudnessMeter>(fs, ch);
        }
        applyTaps();
    }
    void meterLoudness() {
        if (!loud_ || !loudTap_) return;
        RecordTap::Gap gap;
        while (loudTap_->nextGap(gap)) {}  // a stalled daemon: just skip
        const std::size_t chunk = 4096;
        loudBuf_.resize(chunk * std::size_t(loudChannels_));
        while (const std::size_t n = loudTap_->pop(loudBuf_.data(), chunk, nullptr))
            loud_->process(loudBuf_.data(), n);
        broadcast({{"type", "loudness"},
                   {"m", loud_->momentary()},
                   {"s", loud_->shortTerm()},
                   {"i", loud_->integrated()},
                   {"mMax", loud_->maxMomentary()},
                   {"tp", loud_->takeRecentPeak()},
                   {"tpMax", loud_->truePeakMax()},
                   {"seconds", loud_->seconds()},
                   {"target", loudTarget_},
                   {"tpLimit", loudTpLimit_}},
                  nullptr);
    }

    // A marker: when it happened in the take (and on the wall clock), for
    // post. Written next to the take's files as it is dropped.
    void addMarker(QString label) {
        const double t = recorder_ ? takeClock_.elapsed() / 1000.0 : -1;
        const QString wall = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
        if (label.trimmed().isEmpty()) label = QString("Marker %1").arg(markers_.size() + 1);
        markers_.append({t, wall, label});
        if (recorder_) {
            QFile f(recordDir_ + "/" + takeName_ + "/markers.csv");
            const bool fresh = !f.exists();
            if (f.open(QIODevice::Append | QIODevice::Text)) {
                if (fresh) f.write("index,take_time,take_seconds,sample,wall_clock,label\n");
                const int s = int(t);
                const QString tc = QString("%1:%2:%3.%4")
                                       .arg(s / 3600, 2, 10, QChar('0'))
                                       .arg(s / 60 % 60, 2, 10, QChar('0'))
                                       .arg(s % 60, 2, 10, QChar('0'))
                                       .arg(int((t - s) * 1000), 3, 10, QChar('0'));
                QString safe = label;
                safe.replace('"', "'");
                f.write(QString("%1,%2,%3,%4,%5,\"%6\"\n")
                            .arg(markers_.size())
                            .arg(tc)
                            .arg(t, 0, 'f', 3)
                            .arg(qint64(t * driver_->config().sampleRate))
                            .arg(wall, safe)
                            .toUtf8());
            }
        }
        broadcast(markersMessage(), nullptr);
    }
    QJsonObject markersMessage() const {
        QJsonArray list;
        for (const auto& mk : markers_)
            list.append(QJsonObject{{"t", mk.takeSeconds}, {"wall", mk.wall}, {"label", mk.label}});
        return {{"type", "markers"}, {"markers", list}};
    }

    // --- scenes ------------------------------------------------------------

    Scene* scene(int id) {
        for (auto& sc : scenes_)
            if (sc.id == id) return &sc;
        return nullptr;
    }
    bool sceneDiffers() {
        const Scene* sc = scene(currentScene_);
        return sc && !sceneMatches(engine_->graph(), sc->graph);
    }
    QJsonObject scenesMessage() {
        QJsonArray list;
        for (const auto& sc : scenes_)
            list.append(QJsonObject{{"id", sc.id}, {"name", sc.name}, {"fadeMs", sc.fadeMs}});
        return {{"type", "scenes"},
                {"scenes", list},
                {"current", currentScene_},
                {"modified", sceneModified_},
                {"canUndo", beforeRecall_.has_value()}};
    }
    bool recall(const GraphDesc& target, double fadeMs, QString* err) {
        std::string e;
        if (!engine_->setGraph(applyScene(engine_->graph(), target), &e, fadeMs)) {
            *err = QString::fromStdString(e);
            return false;
        }
        snapshotPending_ = true;
        snapshotGlideMs_ = std::max(snapshotGlideMs_, fadeMs);
        return true;
    }

    void handleScene(QLocalSocket* s, const QString& op, const QJsonObject& m) {
        auto fail = [&](const QString& why) {
            send(s, {{"type", "error"}, {"op", op}, {"message", why}});
        };
        const int id = m["id"].toInt();
        if (op == "storeScene") {
            Scene sc;
            sc.id = nextSceneId_++;
            sc.name = m["name"].toString().trimmed();
            if (sc.name.isEmpty()) sc.name = QString("Scene %1").arg(sc.id);
            sc.fadeMs = m["fadeMs"].toDouble(0);
            sc.graph = engine_->graph();
            scenes_.append(sc);
            currentScene_ = sc.id;
        } else if (op == "updateScene") {
            Scene* sc = scene(id);
            if (!sc) return fail("no such scene");
            sc->graph = engine_->graph();
            currentScene_ = id;
        } else if (op == "recallScene") {
            Scene* sc = scene(id);
            if (!sc) return fail("no such scene");
            const GraphDesc before = engine_->graph();
            QString err;
            if (!recall(sc->graph, m["fadeMs"].toDouble(sc->fadeMs), &err))
                return fail(err);
            beforeRecall_ = before;
            beforeRecallScene_ = currentScene_;
            currentScene_ = id;
        } else if (op == "undoRecall") {
            if (!beforeRecall_) return fail("nothing to undo");
            QString err;
            if (!recall(*beforeRecall_, 0, &err)) return fail(err);
            beforeRecall_.reset();
            currentScene_ = scene(beforeRecallScene_) ? beforeRecallScene_ : 0;
        } else if (op == "editScene") {
            Scene* sc = scene(id);
            if (!sc) return fail("no such scene");
            if (m.contains("name") && !m["name"].toString().trimmed().isEmpty())
                sc->name = m["name"].toString().trimmed();
            if (m.contains("fadeMs")) sc->fadeMs = std::clamp(m["fadeMs"].toDouble(), 0.0, 60000.0);
        } else if (op == "deleteScene") {
            if (!scene(id)) return fail("no such scene");
            scenes_.removeIf([&](const Scene& sc) { return sc.id == id; });
            if (currentScene_ == id) currentScene_ = 0;
        } else if (op == "moveScene") {
            const int to = m["to"].toInt();
            int from = -1;
            for (int i = 0; i < scenes_.size(); ++i)
                if (scenes_[i].id == id) from = i;
            if (from < 0 || to < 0 || to >= scenes_.size()) return fail("no such scene");
            scenes_.move(from, to);
        } else {
            return fail("unknown op");
        }
        sceneModified_ = sceneDiffers();
        saveTimer_.start();
        broadcast(scenesMessage(), nullptr);
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
        recordTaps_ = std::move(taps);
        applyTaps();
        takeClock_.start();
        markers_.clear();
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
        loadScenes(doc);
        projectRecord_ = doc["record"].toBool(true);
        return true;
    }

    QJsonArray scenesJson() const {
        QJsonArray a;
        for (const auto& sc : scenes_)
            a.append(QJsonObject{{"id", sc.id},
                                 {"name", sc.name},
                                 {"fadeMs", sc.fadeMs},
                                 {"graph", proto::graphToJson(sc.graph)}});
        return a;
    }
    void loadScenes(const QJsonDocument& doc) {
        for (const auto& v : doc["scenes"].toArray()) {
            const auto o = v.toObject();
            Scene sc;
            sc.id = o["id"].toInt();
            sc.name = o["name"].toString();
            sc.fadeMs = o["fadeMs"].toDouble();
            if (sc.id <= 0 || !proto::graphFromJson(o["graph"].toObject(), sc.graph, nullptr))
                continue;
            nextSceneId_ = std::max(nextSceneId_, sc.id + 1);
            scenes_.append(sc);
        }
        currentScene_ = doc["currentScene"].toInt(0);
        const auto ld = doc["loudness"].toObject();
        loudTarget_ = ld["target"].toDouble(loudTarget_);
        loudTpLimit_ = ld["truePeak"].toDouble(loudTpLimit_);
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
                                  {"graph", proto::graphToJson(engine_->graph())},
                                  {"scenes", scenesJson()},
                                  {"record", projectRecord_},
                                  {"loudness", QJsonObject{{"target", loudTarget_},
                                                           {"truePeak", loudTpLimit_}}},
                                  {"currentScene", currentScene_}})
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
                {"engine", QJsonObject{{"device", QString::fromStdString(driver_->deviceName())},
                                       {"inputs", dc.numInputs},
                                       {"sampleRate", dc.sampleRate},
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
            setUpLoudness();  // no-op unless the program bus changed
            auto snap = snapshot();
            // Clients animate to the new values over the engine's glide.
            if (snapshotGlideMs_ > 0) snap["glideMs"] = snapshotGlideMs_;
            snapshotGlideMs_ = 0;
            broadcast(snap, nullptr);
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
    bool recordFixed_ = false;
    QString projectDir_;           // empty: legacy --session mode
    bool projectRecord_ = true;    // record every source (project setting)
    bool projectFresh_ = false;    // auto-created on a first run: offer Start
    double ringSeconds_ = 30;
    QElapsedTimer takeClock_;
    std::unique_ptr<rec::Recorder> recorder_;
    QJsonArray catalog_;
    bool snapshotPending_ = false;
    double snapshotGlideMs_ = 0;
    // Taps: the recorder's (per take) and the program loudness meter's.
    std::vector<std::pair<NodeId, std::shared_ptr<RecordTap>>> recordTaps_;
    std::shared_ptr<RecordTap> loudTap_;
    NodeId loudNode_ = 0;
    int loudChannels_ = 0;
    std::unique_ptr<dsp::LoudnessMeter> loud_;
    std::vector<float> loudBuf_;
    QTimer loudTimer_;
    double loudTarget_ = -24, loudTpLimit_ = -2;  // ATSC A/85 by default
    // Markers dropped during the show: take time (s), wall clock, label.
    struct Marker {
        double takeSeconds;
        QString wall, label;
    };
    QList<Marker> markers_;
    QProcess* scanner_ = nullptr;
    QList<Scene> scenes_;
    int currentScene_ = 0, nextSceneId_ = 1;
    bool sceneModified_ = false;
    std::optional<GraphDesc> beforeRecall_;  // for "undo recall"
    int beforeRecallScene_ = 0;
    QTimer sceneTimer_;
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
        {"project", "Project folder (session + takes); created if missing", "dir"},
        {"session", "Session file (single-file mode, no project)", "path"},
        {"fresh", "Start from the template, ignoring the saved session"},
        {"audible", "Send program to the device (default: silence)"},
        {"record", "Record every source and the program bus here", "dir"},
        {"sim-inputs", "With --driver sim: this many (silent) device inputs",
         "n", "0"},
        {"input", "Input device (name, prefix or UID; \"default\"; or none)",
         "device", "none"},
        {"output", "Output device (name, prefix or UID; default: system)",
         "device"},
        {"show", "Starting show when there is no session: demo or inputs",
         "name"},
        {"list-devices", "List audio devices and exit"},
        {"ring-seconds", "Per-track disk buffer (T0-8 headroom)", "s", "30"},
    });
    p.process(app);

#if defined(__APPLE__)
    if (p.isSet("list-devices")) {
        for (const auto& d : listAudioDevices())
            std::printf("%-34s in %3d  out %3d  %6.0f Hz%s%s\n", d.name.c_str(),
                        d.inputs, d.outputs, d.sampleRate,
                        d.defaultInput ? "  [default input]" : "",
                        d.defaultOutput ? "  [default output]" : "");
        return 0;
    }
#endif
    DriverConfig dc;
    dc.sampleRate = p.value("rate").toDouble();
    dc.blockSize = p.value("block").toInt();
    dc.numOutputs = 4;
    dc.inputDevice = p.value("input") == "default" ? "" : p.value("input").toStdString();
    dc.outputDevice = p.value("output").toStdString();
    std::unique_ptr<Driver> driver;
    std::string err;
    if (p.value("driver") == "sim") {
        dc.numInputs = std::clamp(p.value("sim-inputs").toInt(), 0, 128);
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
    daemon->setRingSeconds(p.value("ring-seconds").toDouble());
    if (p.isSet("record"))
        daemon->setRecording(p.value("record"),
                             p.value("ring-seconds").toDouble());
    // --project DIR, or (without --session) the last project / a new
    // Untitled. --session keeps the old single-file mode (tests, gates).
    if (p.isSet("project")) {
        daemon->setProject(p.value("project"), false);
    } else if (!p.isSet("session")) {
        const auto [dir, fresh] = Daemon::defaultProject();
        daemon->setProject(dir, fresh);
    }
    const QString session =
        p.isSet("session")
            ? p.value("session")
            : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                  "/session.json";
    if (!daemon->start(p.value("socket"), session, p.isSet("fresh"),
                       p.value("show"), &why)) {
        qCritical("mixd: %s", qPrintable(why));
        return 1;
    }
    const int rc = app.exec();
    daemon.reset();
    return rc;
}
