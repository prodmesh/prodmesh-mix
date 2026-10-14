// ProdMesh Mix: the mixer UI. A client of mixd — it owns no audio. If it
// crashes or is closed, program audio carries on; when it starts again it
// reconnects and rebuilds itself from the engine's snapshot.
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMap>
#include <QScrollArea>
#include <QTimer>
#include <functional>

#include "proto/protocol.h"
#include "ui/theme.h"
#include "ui/plugin_browser.h"
#include "ui/widgets.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>

namespace pm::ui {

// Connection to the engine daemon, with automatic reconnect.
class MixClient : public QObject {
public:
    std::function<void(const QJsonObject&)> onMessage;
    std::function<void(bool)> onConnected;

    explicit MixClient(QString name) : name_(std::move(name)) {
        connect(&sock_, &QLocalSocket::connected, this, [this] {
            send({{"op", "hello"}, {"client", "mix-ui"}, {"proto", proto::kVersion}});
            if (onConnected) onConnected(true);
        });
        connect(&sock_, &QLocalSocket::disconnected, this, [this] { lost(); });
        connect(&sock_, &QLocalSocket::errorOccurred, this,
                [this](QLocalSocket::LocalSocketError) { lost(); });
        connect(&sock_, &QLocalSocket::readyRead, this, [this] {
            while (sock_.canReadLine()) {
                const auto doc = QJsonDocument::fromJson(sock_.readLine());
                if (doc.isObject() && onMessage) onMessage(doc.object());
            }
        });
        retry_.setSingleShot(true);
        retry_.setInterval(250);
        connect(&retry_, &QTimer::timeout, this, [this] { open(); });
        flush_.setInterval(16);  // fader moves: at most one per frame
        connect(&flush_, &QTimer::timeout, this, [this] { flushLevels(); });
        flush_.start();
    }

    void open() { sock_.connectToServer(name_); }
    bool connected() const {
        return sock_.state() == QLocalSocket::ConnectedState;
    }

    void setLevel(quint32 id, float db, bool mute) {
        pending_[id] = {db, mute};
    }
    void openEditor(quint32 id, int insert) {
        send({{"op", "openEditor"}, {"id", double(id)}, {"insert", insert}});
    }
    void setGraph(const GraphDesc& g) {
        flushLevels();  // keep fader moves ordered before the edit
        send({{"op", "setGraph"}, {"graph", proto::graphToJson(g)}});
    }

private:
    void send(const QJsonObject& o) {
        if (connected()) sock_.write(proto::frame(o));
    }
    void flushLevels() {
        for (auto it = pending_.cbegin(); it != pending_.cend(); ++it)
            send({{"op", "setLevel"},
                  {"id", double(it.key())},
                  {"faderDb", it.value().first},
                  {"mute", it.value().second}});
        pending_.clear();
    }
    void lost() {
        if (onConnected) onConnected(false);
        if (!retry_.isActive()) retry_.start();
    }

    QString name_;
    QLocalSocket sock_;
    QTimer retry_, flush_;
    QMap<quint32, QPair<float, bool>> pending_;
};

// Top bar: identity, connection, and engine health — always visible.
class HealthBar : public QWidget {
public:
    HealthBar() { setFixedHeight(44); }

    void setConnected(bool c) {
        connected_ = c;
        update();
    }
    void setFormat(double rate, int block, const QString& device) {
        format_ = QString("%1 · %2 kHz · %3 samples")
                      .arg(device.isEmpty() ? "Engine" : device)
                      .arg(rate / 1000.0, 0, 'g', 3)
                      .arg(block);
        update();
    }
    void setHealth(const QJsonObject& h) {
        meanLoad_ = h["meanLoad"].toDouble();
        worstLoad_ = std::max(worstLoad_ * 0.9, h["worstLoad"].toDouble());
        xruns_ = qint64(h["xruns"].toDouble());
        rtViolations_ = qint64(h["rtViolations"].toDouble());
        const auto r = h["recording"].toObject();
        recOn_ = r["on"].toBool();
        recSeconds_ = r["seconds"].toDouble();
        recTracks_ = r["tracks"].toInt();
        recDropped_ = qint64(r["droppedFrames"].toDouble());
        recDroppedSeconds_ = r["droppedSeconds"].toDouble();
        recRingFill_ = r["ringFill"].toDouble();
        recDiskSeconds_ = r["diskSecondsLeft"].toDouble();
        recError_ = r["error"].toString();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), theme::bg);
        p.setPen(theme::grid);
        p.drawLine(0, height() - 1, width(), height() - 1);

        QFont f = font();
        f.setPixelSize(14);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(theme::textMax);
        p.drawText(QRect(16, 0, 200, height()), Qt::AlignVCenter,
                   "ProdMesh Mix");

        // Connection pill.
        f.setPixelSize(11);
        f.setWeight(QFont::Medium);
        p.setFont(f);
        const QString state =
            connected_ ? "Engine connected" : "Engine not reachable";
        const QColor col = connected_ ? theme::accent : theme::clip;
        const int pw = p.fontMetrics().horizontalAdvance(state) + 30;
        const QRect pill(130, 12, pw, 20);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(col.red(), col.green(), col.blue(), 36));
        p.drawRoundedRect(pill, 10, 10);
        p.setBrush(col);
        p.drawEllipse(QPoint(pill.left() + 11, pill.center().y() + 1), 4, 4);
        p.setPen(col);
        p.drawText(pill.adjusted(20, 0, 0, 0), Qt::AlignVCenter, state);
        int left = pill.right() + 12;
        if (connected_) {
            p.setPen(theme::text);
            const int fw = p.fontMetrics().horizontalAdvance(format_);
            p.drawText(QRect(left, 0, fw, height()), Qt::AlignVCenter, format_);
            left += fw + 18;
        }
        if (connected_ && recOn_) paintRecording(p, left);

        // Right side: DSP load bar, xruns — only while they are live.
        if (!connected_) return;
        int x = width() - 16;
        auto rightText = [&](const QString& s, const QColor& c) {
            const int w = p.fontMetrics().horizontalAdvance(s);
            x -= w;
            p.setPen(c);
            p.drawText(QRect(x, 0, w, height()), Qt::AlignVCenter, s);
            x -= 18;
        };
        if (rtViolations_ > 0)
            rightText(QString("%1 RT violations").arg(rtViolations_),
                      theme::clip);
        rightText(xruns_ == 1 ? "1 dropout" : QString("%1 dropouts").arg(xruns_),
                  xruns_ ? theme::clip : theme::text);

        const QString load = QString("DSP %1%  peak %2%")
                                 .arg(int(std::round(meanLoad_ * 100)))
                                 .arg(int(std::round(worstLoad_ * 100)));
        rightText(load, theme::textHi);
        const QRect bar(x - 120, height() / 2 - 3, 120, 6);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::control);
        p.drawRoundedRect(bar, 3, 3);
        const auto barColor = worstLoad_ > 0.7 ? theme::clip
                              : worstLoad_ > 0.5 ? theme::warn
                                                 : theme::accent;
        p.setBrush(barColor);
        p.drawRoundedRect(QRect(bar.left(), bar.top(),
                                int(std::min(1.0, meanLoad_) * bar.width()),
                                bar.height()),
                          3, 3);
        p.setBrush(theme::textMax);
        p.drawRect(QRect(bar.left() + int(std::min(1.0, worstLoad_) *
                                          bar.width()) - 1,
                         bar.top() - 2, 2, bar.height() + 4));
    }

private:
    // "● REC 01:23:45 · 9 tracks · 41 h disk" — red while healthy (red is
    // the universal record colour); amber text when the disk or the
    // writer needs attention.
    void paintRecording(QPainter& p, int x) {
        const int s = int(recSeconds_);
        const QString clock = QString("%1:%2:%3")
                                  .arg(s / 3600, 2, 10, QChar('0'))
                                  .arg(s / 60 % 60, 2, 10, QChar('0'))
                                  .arg(s % 60, 2, 10, QChar('0'));
        const QRect pill(x, 12, 98, 20);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(229, 72, 77, 40));
        p.drawRoundedRect(pill, 10, 10);
        p.setBrush(theme::clip);
        p.drawEllipse(QPoint(pill.left() + 11, pill.center().y() + 1), 4, 4);
        p.setPen(QColor("#ffd9db"));
        p.drawText(pill.adjusted(20, 0, 0, 0), Qt::AlignVCenter,
                   "REC " + clock);

        const double hours = recDiskSeconds_ / 3600.0;
        QString detail = QString("%1 tracks · %2 disk")
                             .arg(recTracks_)
                             .arg(hours >= 1 ? QString("%1 h").arg(int(hours))
                                             : QString("%1 min").arg(
                                                   int(recDiskSeconds_ / 60)));
        bool attention = hours < 2;
        if (recDropped_ > 0) {
            detail += QString(" · %1 s of gaps").arg(recDroppedSeconds_, 0, 'f', 1);
            attention = true;
        }
        if (recRingFill_ > 0.5) {
            detail += QString(" · disk slow (%1% buffered)")
                          .arg(int(recRingFill_ * 100));
            attention = true;
        }
        if (!recError_.isEmpty()) {
            detail += " · " + recError_;
            attention = true;
        }
        p.setPen(attention ? theme::warn : theme::text);
        p.drawText(QRect(pill.right() + 10, 0, 520, height()),
                   Qt::AlignVCenter, detail);
    }

    bool connected_ = false;
    bool recOn_ = false;
    double recSeconds_ = 0, recRingFill_ = 0, recDiskSeconds_ = 0;
    double recDroppedSeconds_ = 0;
    int recTracks_ = 0;
    qint64 recDropped_ = 0;
    QString recError_;
    QString format_;
    double meanLoad_ = 0, worstLoad_ = 0;
    qint64 xruns_ = 0, rtViolations_ = 0;
};

class SectionLabel : public QLabel {
public:
    explicit SectionLabel(const QString& t) : QLabel(t.toUpper()) {
        QFont f = font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        setFont(f);
        setStyleSheet("color: #8a92a6; padding-left: 2px;");
    }
};

class MainWindow : public QWidget {
public:
    explicit MainWindow(const QString& socket) : client_(socket) {
        setWindowTitle("ProdMesh Mix");
        setStyleSheet("QWidget#root { background: #14161c; }");
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        health_ = new HealthBar;
        outer->addWidget(health_);

        banner_ = new QLabel(
            "Audio engine not reachable. Program audio is down until the "
            "engine (mixd) is running again. Retrying automatically.");
        banner_->setStyleSheet(
            "background: #3a1c20; color: #ffd9db; font-size: 13px;"
            " font-weight: 600; padding: 9px 16px;"
            " border-bottom: 1px solid #e5484d;");
        banner_->hide();
        outer->addWidget(banner_);
        warnings_ = new QLabel;
        warnings_->setWordWrap(true);
        warnings_->setStyleSheet(
            "background: #362f1a; color: #f3e3b0; font-size: 12px;"
            " padding: 7px 16px; border-bottom: 1px solid #e0c05c;");
        warnings_->hide();
        outer->addWidget(warnings_);
        events_ = new QLabel;
        events_->setWordWrap(true);
        events_->setStyleSheet(
            "background: #3a1c20; color: #ffd9db; font-size: 12px;"
            " padding: 7px 16px; border-bottom: 1px solid #e5484d;");
        events_->hide();
        outer->addWidget(events_);
        eventsTimer_.setSingleShot(true);
        connect(&eventsTimer_, &QTimer::timeout, events_, &QLabel::hide);

        auto* body = new QWidget;
        body->setObjectName("root");
        bodyLayout_ = new QHBoxLayout(body);
        bodyLayout_->setContentsMargins(16, 14, 16, 16);
        bodyLayout_->setSpacing(16);
        outer->addWidget(body, 1);

        scroll_ = new QScrollArea;
        scroll_->setWidgetResizable(true);
        scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_->setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget"
                               " { background: #14161c; }");
        bodyLayout_->addWidget(scroll_, 1);

        masterHost_ = new QWidget;
        masterHost_->setObjectName("root");
        auto* mh = new QVBoxLayout(masterHost_);
        mh->setContentsMargins(0, 0, 0, 0);
        mh->setSpacing(8);
        mh->addWidget(new SectionLabel("Program"));
        bodyLayout_->addWidget(masterHost_);

        // Honest about what it means: no engine, no program audio.
        offline_ = new QLabel(
            "<div style='font-size:16px; color:#e8ecf4; font-weight:600;'>"
            "Audio engine not reachable</div>"
            "<div style='font-size:13px; color:#8a92a6; margin-top:8px;'>"
            "Program audio stops while the engine (mixd) is not running."
            "<br>Retrying automatically.</div>",
            body);
        offline_->setAlignment(Qt::AlignCenter);
        offline_->setTextFormat(Qt::RichText);

        client_.onConnected = [this](bool c) {
            health_->setConnected(c);
            if (!c) setOffline(true);
        };
        client_.onMessage = [this](const QJsonObject& m) { handle(m); };
        sendTimer_.setSingleShot(true);
        connect(&sendTimer_, &QTimer::timeout, this,
                [this] { client_.setGraph(graph_); });
        client_.open();
        setOffline(true);
    }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        offline_->setGeometry(offline_->parentWidget()->rect());
    }

private:
    void setOffline(bool off) {
        offline_->setVisible(off && strips_.isEmpty());
        // With a mixer already built, keep it on screen so the operator
        // keeps their bearings — but inert, with meters emptied (frozen
        // meters that look live are worse than none) and a clear banner.
        banner_->setVisible(off && !strips_.isEmpty());
        masterHost_->setVisible(!strips_.isEmpty());
        scroll_->setEnabled(!off);
        masterHost_->setEnabled(!off);
        if (off)
            for (auto* st : strips_) st->clearMeter();
    }

    void handle(const QJsonObject& m) {
        const auto type = m["type"].toString();
        if (type == "snapshot") {
            if (!loggedSnapshot_) {
                // Read by tools/t05_ui_isolation.py.
                qInfo("mix-ui: snapshot %lld ms after start",
                      qint64(uptime_.elapsed()));
                loggedSnapshot_ = true;
            }
            const auto eng = m["engine"].toObject();
            rate_ = eng["sampleRate"].toDouble();
            deviceOutputs_ = eng["outputs"].toInt(2);
            health_->setFormat(rate_, eng["block"].toInt(), eng["device"].toString());
            GraphDesc g;
            if (proto::graphFromJson(m["graph"].toObject(), g, nullptr)) {
                // Compare with what the strips were built from: a local edit
                // already changed graph_.
                if (!sameLayout(g, pendingGraph_) || strips_.isEmpty()) build(g);
                // A local edit not sent yet is newer than this snapshot.
                if (!sendTimer_.isActive()) graph_ = g;
                const auto delay = m["delay"].toObject();
                for (auto it = gainSource_.cbegin(); it != gainSource_.cend(); ++it)
                    if (const auto* src = graph_.find(it.value()))
                        if (auto* st = strips_.value(it.key())) st->gain()->setGain(src->faderDb);
                for (const auto& n : graph_.nodes)
                    if (auto* st = strips_.value(n.id)) {
                        st->setLevel(n.faderDb, n.mute);
                        st->inserts()->setInserts(insertNames(n), inEngineFlags(n));
                        const auto d = delay[QString::number(n.id)].toObject();
                        QList<QPair<QString, int>> comp;
                        for (const auto& c : d["comp"].toArray()) {
                            const auto pair = c.toArray();
                            const auto* to = graph_.find(NodeId(pair[0].toDouble()));
                            if (to && to->kind != NodeKind::Output)
                                comp.append({QString::fromStdString(to->name),
                                             pair[1].toInt()});
                        }
                        st->setDelay(d["chain"].toInt(), d["arrival"].toInt(), comp,
                                     rate_);
                    }
            }
            refreshRouting();
            QStringList w;
            for (const auto& v : m["warnings"].toArray()) w << v.toString();
            warnings_->setText(
                w.isEmpty() ? QString()
                            : QString("%1 couldn't load and %2 bypassed: %3")
                                  .arg(w.size() == 1 ? "A plugin" : QString("%1 plugins").arg(w.size()),
                                       w.size() == 1 ? "is" : "are", w.join(" · ")));
            warnings_->setVisible(!w.isEmpty());
            loadFailures_ = w;
            refreshNotes();
            setOffline(false);
        } else if (type == "error") {
            events_->setText(m["message"].toString());
            events_->show();
            eventsTimer_.start(8000);
        } else if (type == "event") {
            // Transient: a plugin host crashed and was restarted, etc.
            events_->setText(m["message"].toString() +
                             " — audio passed straight through meanwhile.");
            events_->show();
            eventsTimer_.start(15000);
        } else if (type == "catalog") {
            catalog_ = m["plugins"].toArray();
            scanning_ = m["scanning"].toBool();
            for (const auto& n : graph_.nodes)  // names may resolve now
                if (auto* st = strips_.value(n.id))
                    st->inserts()->setInserts(insertNames(n), inEngineFlags(n));
        } else if (type == "meters") {
            for (const auto& v : m["m"].toArray()) {
                const auto r = v.toArray();
                auto* s = strips_.value(quint32(r[0].toDouble()));
                if (!s) continue;
                const float pk[2] = {float(r[1].toDouble()), float(r[2].toDouble())};
                const float rms[2] = {float(r[3].toDouble()), float(r[4].toDouble())};
                s->setMeter(pk, rms);
            }
        } else if (type == "level") {
            const auto id = quint32(m["id"].toDouble());
            if (auto* s = strips_.value(id))
                s->setLevel(float(m["faderDb"].toDouble()), m["mute"].toBool());
            for (auto it = gainSource_.cbegin(); it != gainSource_.cend(); ++it)
                if (it.value() == id)
                    if (auto* s = strips_.value(it.key()))
                        s->gain()->setGain(float(m["faderDb"].toDouble()));
        } else if (type == "health") {
            health_->setHealth(m);
            rtIssues_.clear();
            for (const auto& v : m["pluginRt"].toArray()) {
                const auto o = v.toObject();
                rtIssues_[quint32(o["node"].toDouble())][o["plugin"].toString()] =
                    {qint64(o["allocations"].toDouble()), qint64(o["missed"].toDouble())};
            }
            refreshNotes();
        }
    }

    // Same nodes in the same order: update strips in place, no rebuild.
    static bool sameLayout(const GraphDesc& a, const GraphDesc& b) {
        if (a.nodes.size() != b.nodes.size()) return false;
        for (std::size_t i = 0; i < a.nodes.size(); ++i)
            if (a.nodes[i].id != b.nodes[i].id ||
                a.nodes[i].kind != b.nodes[i].kind ||
                a.nodes[i].name != b.nodes[i].name)
                return false;
        return true;
    }

    QString pluginName(const InsertDesc& ins) const {
        for (const auto& v : catalog_) {
            const auto o = v.toObject();
            if (o["id"].toString().toStdString() == ins.pluginId &&
                o["path"].toString().toStdString() == ins.path)
                return o["name"].toString();
        }
        return QFileInfo(QString::fromStdString(ins.path)).completeBaseName();
    }

    QList<bool> inEngineFlags(const NodeDesc& n) const {
        QList<bool> out;
        for (const auto& ins : n.processing.inserts)
            out << (ins.type == InsertDesc::Type::Plugin && !ins.sandboxed);
        return out;
    }

    QStringList insertNames(const NodeDesc& n) const {
        QStringList out;
        for (const auto& ins : n.processing.inserts) {
            switch (ins.type) {
            case InsertDesc::Type::Plugin: out << pluginName(ins); break;
            case InsertDesc::Type::Gain:
                out << QString("Gain %1 dB").arg(double(ins.value));
                break;
            case InsertDesc::Type::CpuLoad: out << "CPU load"; break;
            }
        }
        return out;
    }

public:
    void openBrowser(quint32 id, const QString& search, QPoint at = {}) {
        auto* b = new PluginBrowser(catalog_, scanning_, this);
        b->onPick = [this, id](const QJsonObject& e) { addInsert(id, e); };
        if (at.isNull())
            if (auto* st = strips_.value(id))
                at = st->inserts()->mapToGlobal(QPoint(0, 0));
        b->move(at - QPoint(20, 20));
        b->setSearch(search);
        b->show();
        browser_ = b;
    }
    QWidget* browser() const { return browser_; }

private:
    void refreshNotes() {
        for (const auto& n : graph_.nodes) {
            auto* st = strips_.value(n.id);
            if (!st) continue;
            const auto names = insertNames(n);
            QMap<int, QString> notes;
            const auto issues = rtIssues_.value(n.id);
            for (int i = 0; i < names.size(); ++i) {
                const auto& ins = n.processing.inserts[std::size_t(i)];
                const auto path = QString::fromStdString(ins.path);
                for (const auto& f : loadFailures_)
                    if (!path.isEmpty() && f.contains(path))
                        notes[i] = names[i] + " — couldn't load; audio passes "
                                              "straight through";
                for (auto it = issues.cbegin(); it != issues.cend(); ++it) {
                    // An isolated chain reports as "A + B": note each member.
                    if (!it.key().split(" + ").contains(names[i]) &&
                        !names[i].startsWith(it.key()))
                        continue;
                    QStringList why;
                    if (it.value().first)
                        why << QString("allocated memory on the audio thread %1× "
                                       "(not real-time safe; a dropout risk "
                                       "under load)").arg(it.value().first);
                    if (it.value().second)
                        why << QString("its isolated host was late %1× (those "
                                       "blocks passed through dry)")
                                   .arg(it.value().second);
                    notes[i] = names[i] + " — " + why.join("; ");
                }
            }
            st->inserts()->setNotes(notes);
        }
    }

    void addInsert(NodeId id, const QJsonObject& entry) {
        GraphDesc g = graph_;
        if (auto* n = g.find(id)) {
            // Isolated by default: a plugin crash must not stop the show.
            n->processing.inserts.push_back(InsertDesc::plugin(
                entry["format"].toString().toStdString(),
                entry["path"].toString().toStdString(),
                entry["id"].toString().toStdString(), true));
            client_.setGraph(g);
        }
    }

    void removeInsert(NodeId id, int index) {
        GraphDesc g = graph_;
        if (auto* n = g.find(id); n && index < int(n->processing.inserts.size())) {
            n->processing.inserts.erase(n->processing.inserts.begin() + index);
            client_.setGraph(g);
        }
    }

    // The program bus is the one that feeds device outputs 1-2.
    static NodeId programBus(const GraphDesc& g) {
        for (const auto& e : g.edges) {
            const auto* to = g.find(e.to);
            const auto* from = g.find(e.from);
            if (to && from && to->kind == NodeKind::Output &&
                to->ioChannel == 0 && from->kind == NodeKind::Bus)
                return from->id;
        }
        return 0;
    }

    void build(const GraphDesc& g) {
        for (auto* s : strips_) s->deleteLater();
        strips_.clear();
        gainSource_.clear();
        pendingGraph_ = g;

        auto* row = new QWidget;
        row->setObjectName("root");
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(24);

        auto section = [&](const QString& title, NodeKind kind, NodeId skip,
                           QWidget* tail = nullptr) {
            auto* col = new QWidget;
            col->setObjectName("root");
            auto* v = new QVBoxLayout(col);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(8);
            v->addWidget(new SectionLabel(title));
            auto* strips = new QHBoxLayout;
            strips->setSpacing(6);
            int count = 0;
            for (const auto& n : g.nodes) {
                if (n.kind != kind || n.id == skip) continue;
                strips->addWidget(makeStrip(n, false));
                ++count;
            }
            if (tail) {
                strips->addWidget(tail);
                ++count;
            }
            v->addLayout(strips, 1);
            if (count) h->addWidget(col);
            else delete col;
        };
        const NodeId program = programBus(g);
        section("Inputs", NodeKind::Strip, 0);
        auto* newBus = new QPushButton("+ Bus");
        newBus->setCursor(Qt::PointingHandCursor);
        newBus->setFixedWidth(64);
        newBus->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        newBus->setToolTip("New aux, group or mix-minus bus");
        newBus->setStyleSheet(
            "QPushButton { background: transparent; border: 1px dashed #323848;"
            " border-radius: 6px; color: #8a92a6; font-size: 11px; }"
            "QPushButton:hover { border-color: #46506a; color: #e8ecf4; }");
        connect(newBus, &QPushButton::clicked, this, [this, newBus] {
            newBusMenu(newBus->mapToGlobal(QPoint(0, 0)));
        });
        section("Buses", NodeKind::Bus, program, newBus);
        h->addStretch(1);
        scroll_->setWidget(row);

        if (const auto* p = g.find(program))
            masterHost_->layout()->addWidget(makeStrip(*p, true));
    }

    // --- Routing ---------------------------------------------------------
    // Assignments (strip -> Program/group bus, bus -> output) are edges;
    // sends are edges to aux buses: buses that do not reach Program, and
    // mix-minus buses, which the engine fills by itself (resolveMixMinus).

    static QMenu* styledMenu(QWidget* parent) {
        auto* m = new QMenu(parent);
        m->setAttribute(Qt::WA_DeleteOnClose);
        m->setStyleSheet(
            "QMenu { background: #232733; border: 1px solid #46506a;"
            " color: #e8ecf4; padding: 4px; }"
            "QMenu::item { padding: 5px 18px 5px 22px; border-radius: 4px; }"
            "QMenu::item:selected { background: #2a3a37; }"
            "QMenu::item:disabled { color: #5a6172; }"
            "QMenu::separator { height: 1px; background: #323848; margin: 4px 6px; }"
            "QMenu::indicator { width: 12px; left: 6px; }");
        return m;
    }

    bool isAux(const NodeDesc& n) const {
        const NodeId program = programBus(graph_);
        return n.kind == NodeKind::Bus && n.id != program &&
               (n.mixMinus.follows || !program || !graph_.reaches(n.id, program));
    }
    QList<NodeId> auxBuses() const {
        QList<NodeId> out;
        for (const auto& n : graph_.nodes)
            if (isAux(n)) out << n.id;
        return out;
    }
    static EdgeDesc* edgeOf(GraphDesc& g, NodeId from, NodeId to) {
        for (auto& e : g.edges)
            if (e.from == from && e.to == to && !e.derived) return &e;
        return nullptr;
    }
    static void removeEdge(GraphDesc& g, NodeId from, NodeId to) {
        std::erase_if(g.edges, [&](const EdgeDesc& e) {
            return e.from == from && e.to == to;
        });
    }
    // The pan a strip has in the program; sends keep the same image.
    float mainPan(NodeId id) const {
        for (const auto& e : graph_.edges)
            if (e.from == id && !e.derived)
                if (const auto* to = graph_.find(e.to); to && !isAux(*to))
                    return e.pan;
        return 0;
    }
    static QString outputName(int ioChannel, int channels) {
        return channels > 1 ? QString("Out %1-%2").arg(ioChannel + 1).arg(ioChannel + 2)
                            : QString("Out %1").arg(ioChannel + 1);
    }
    static bool excluded(const NodeDesc& bus, NodeId id) {
        const auto& x = bus.mixMinus.exclude;
        return std::find(x.begin(), x.end(), id) != x.end();
    }

    // Apply an edit: locally at once (so the next edit builds on it), and
    // to the engine — coalesced while dragging.
    void commit(GraphDesc g, bool soon = false) {
        resolveMixMinus(g);
        graph_ = std::move(g);
        refreshRouting();
        if (!soon) {
            sendTimer_.stop();
            client_.setGraph(graph_);
            return;
        }
        if (!sendTimer_.isActive()) sendTimer_.start(30);
    }

    QList<SendList::Row> sendRows(const NodeDesc& n) const {
        QList<SendList::Row> rows;
        const NodeId program = programBus(graph_);
        for (NodeId a : auxBuses()) {
            const auto* bus = graph_.find(a);
            SendList::Row r;
            r.bus = QString::fromStdString(bus->name);
            const QString name = QString::fromStdString(n.name);
            const bool mm = bus->mixMinus.follows != 0;
            // Buses do not feed a mix-minus (a group can hide the very
            // voice it must leave out); no send may close a loop.
            if (n.id == program || n.id == a || (n.kind == NodeKind::Bus && mm) ||
                graph_.reaches(a, n.id)) {
                rows << r;
                continue;
            }
            const EdgeDesc* explicitSend = nullptr;
            bool derived = false;
            for (const auto& e : graph_.edges)
                if (e.from == n.id && e.to == a) {
                    if (e.derived) derived = true;
                    else explicitSend = &e;
                }
            if (explicitSend) {
                r.state = SendList::State::On;
                r.db = explicitSend->gainDb;
                r.tip = QString("%1 → %2: %3 dB, after the fader.\nDrag to "
                                "change (Option = fine); double-click for %4.")
                            .arg(name, r.bus, fmtDb(r.db),
                                 mm ? "automatic" : "0 dB");
            } else if (derived) {
                r.state = SendList::State::Auto;
                r.tip = QString("%1 is in %2 automatically, like in %3.\n"
                                "Drag to set its own level; click the dot to "
                                "leave it out.")
                            .arg(name, r.bus,
                                 QString::fromStdString(
                                     graph_.find(bus->mixMinus.follows)->name));
            } else if (mm && excluded(*bus, n.id)) {
                r.state = SendList::State::Minus;
                r.tip = QString("%1 is left out of %2, so whoever listens to "
                                "it never hears themselves.\nClick to put it "
                                "back in.").arg(name, r.bus);
            } else {
                r.state = SendList::State::Off;
                r.tip = QString("Not sent to %1. Click the dot to send.").arg(r.bus);
            }
            rows << r;
        }
        return rows;
    }

    // "Program", "Talent +1", "Out 3-4"; empty = goes nowhere.
    QPair<QString, QString> assignText(const NodeDesc& n) const {
        QStringList to;
        for (const auto& e : graph_.edges) {
            if (e.from != n.id || e.derived) continue;
            const auto* d = graph_.find(e.to);
            if (!d) continue;
            if (d->kind == NodeKind::Output)
                to << outputName(d->ioChannel, d->channels);
            else if (d->kind == NodeKind::Bus && !isAux(*d))
                to << QString::fromStdString(d->name);
        }
        if (to.isEmpty())
            return {{}, "Not routed anywhere: this channel is not heard. "
                        "Click to assign it."};
        const QString text = to.size() == 1 ? to[0]
                                             : QString("%1 +%2").arg(to[0]).arg(to.size() - 1);
        return {text, "Routed to " + to.join(", ") + ". Click to change."};
    }

    void refreshRouting() {
        for (const auto& n : graph_.nodes) {
            auto* st = strips_.value(n.id);
            if (!st) continue;
            st->sends()->setRows(sendRows(n));
            const auto [text, tip] = assignText(n);
            st->assign()->setText(text, tip);
            if (n.kind != NodeKind::Bus) continue;
            if (n.mixMinus.follows) {
                QStringList out;
                for (NodeId x : n.mixMinus.exclude)
                    if (const auto* xn = graph_.find(x))
                        out << QString::fromStdString(xn->name);
                const auto* f = graph_.find(n.mixMinus.follows);
                const QString follows = f ? QString::fromStdString(f->name) : "Program";
                st->setCaption(out.isEmpty() ? "mix-minus" : "minus " + out.join(", "),
                               QString("Mix-minus: everything in %1%2.\nNew "
                                       "channels join by themselves.")
                                   .arg(follows, out.isEmpty() ? QString()
                                                               : " except " + out.join(", ")));
            } else if (n.id == programBus(graph_)) {
                st->setCaption("");
            } else {
                st->setCaption(isAux(n) ? "aux" : "group");
            }
        }
    }

    const NodeDesc* auxAt(int row) const {
        const auto a = auxBuses();
        return row >= 0 && row < a.size() ? graph_.find(a[row]) : nullptr;
    }

    void toggleSend(NodeId id, int row) {
        const auto* bus = auxAt(row);
        if (!bus) return;
        const NodeId a = bus->id;
        GraphDesc g = graph_;
        auto* mm = g.find(a);
        if (mm->mixMinus.follows && g.find(id)->kind == NodeKind::Strip) {
            const bool feeding = std::any_of(
                g.edges.begin(), g.edges.end(),
                [&](const EdgeDesc& e) { return e.from == id && e.to == a; });
            auto& x = mm->mixMinus.exclude;
            if (excluded(*mm, id)) {
                std::erase(x, id);
                // A channel that is not in the program has no automatic
                // send; including it means an explicit one.
                if (!g.reaches(id, mm->mixMinus.follows))
                    g.edges.push_back({id, a, 0, mainPan(id)});
            } else if (feeding) {
                x.push_back(id);
                removeEdge(g, id, a);
            } else {
                g.edges.push_back({id, a, 0, mainPan(id)});
            }
        } else if (edgeOf(g, id, a)) {
            removeEdge(g, id, a);
        } else {
            g.edges.push_back({id, a, 0, mainPan(id)});
        }
        commit(std::move(g));
    }

    void setSendLevel(NodeId id, int row, float db) {
        const auto* bus = auxAt(row);
        if (!bus) return;
        GraphDesc g = graph_;
        std::erase(g.find(bus->id)->mixMinus.exclude, id);
        if (auto* e = edgeOf(g, id, bus->id)) {
            e->gainDb = db;
        } else {
            removeEdge(g, id, bus->id);  // the automatic one, if any
            g.edges.push_back({id, bus->id, db, mainPan(id)});
        }
        commit(std::move(g), true);
    }

    void resetSend(NodeId id, int row) {
        const auto* bus = auxAt(row);
        if (!bus) return;
        GraphDesc g = graph_;
        auto* b = g.find(bus->id);
        if (b->mixMinus.follows && g.reaches(id, b->mixMinus.follows)) {
            std::erase(b->mixMinus.exclude, id);
            removeEdge(g, id, bus->id);  // back to automatic
        } else if (auto* e = edgeOf(g, id, bus->id)) {
            e->gainDb = 0;
        } else {
            return;
        }
        commit(std::move(g));
    }

    // Output nodes exist while something feeds them.
    static void pruneOutputs(GraphDesc& g) {
        std::erase_if(g.nodes, [&](const NodeDesc& n) {
            if (n.kind != NodeKind::Output) return false;
            for (const auto& e : g.edges)
                if (e.to == n.id) return false;
            return true;
        });
    }
    static NodeId freeId(const GraphDesc& g, NodeId from) {
        NodeId id = from;
        for (const auto& n : g.nodes) id = std::max(id, n.id + 1);
        return id;
    }
    NodeId outputNode(GraphDesc& g, int ioChannel) const {
        for (const auto& n : g.nodes)
            if (n.kind == NodeKind::Output && n.ioChannel == ioChannel) return n.id;
        NodeDesc o;
        o.id = freeId(g, 4000);
        o.kind = NodeKind::Output;
        o.ioChannel = ioChannel;
        o.channels = std::min(2, deviceOutputs_ - ioChannel);
        o.name = outputName(ioChannel, o.channels).toStdString();
        g.nodes.push_back(o);
        return o.id;
    }
    // The first output pair after 1-2 that nothing feeds yet; -1 if none.
    int freeOutputPair() const {
        for (int ch = 2; ch < deviceOutputs_; ch += 2) {
            bool used = false;
            for (const auto& n : graph_.nodes)
                if (n.kind == NodeKind::Output && n.ioChannel == ch)
                    for (const auto& e : graph_.edges) used |= e.to == n.id;
            if (!used) return ch;
        }
        return -1;
    }

    void assignMenu(NodeId id, QPoint at) {
        const auto* n = graph_.find(id);
        if (!n) return;
        auto* menu = styledMenu(this);
        menu->addAction(QString::fromStdString(n->name) + " goes to")->setEnabled(false);
        for (const auto& b : graph_.nodes) {
            if (b.kind != NodeKind::Bus || b.id == id || isAux(b)) continue;
            auto* act = menu->addAction(QString::fromStdString(b.name));
            act->setCheckable(true);
            act->setChecked(std::any_of(graph_.edges.begin(), graph_.edges.end(),
                                        [&](const EdgeDesc& e) {
                                            return e.from == id && e.to == b.id && !e.derived;
                                        }));
            if (graph_.reaches(b.id, id)) {
                act->setEnabled(false);  // would feed itself
                act->setText(act->text() + "  (feeds this)");
            }
            const NodeId to = b.id;
            connect(act, &QAction::triggered, this, [this, id, to](bool on) {
                GraphDesc g = graph_;
                if (on) g.edges.push_back({id, to, 0, mainPan(id)});
                else removeEdge(g, id, to);
                commit(std::move(g));
            });
        }
        menu->addSeparator();
        for (int ch = 0; ch < deviceOutputs_; ch += 2) {
            NodeId outId = 0;
            for (const auto& o : graph_.nodes)
                if (o.kind == NodeKind::Output && o.ioChannel == ch) outId = o.id;
            auto* act = menu->addAction(
                outputName(ch, std::min(2, deviceOutputs_ - ch)) +
                (ch == 0 ? "  (main)" : ""));
            act->setCheckable(true);
            act->setChecked(outId && std::any_of(graph_.edges.begin(), graph_.edges.end(),
                                                 [&](const EdgeDesc& e) {
                                                     return e.from == id && e.to == outId;
                                                 }));
            connect(act, &QAction::triggered, this, [this, id, ch](bool on) {
                GraphDesc g = graph_;
                const NodeId o = outputNode(g, ch);
                if (on) g.edges.push_back({id, o, 0, 0});
                else removeEdge(g, id, o);
                pruneOutputs(g);
                commit(std::move(g));
            });
        }
        menu->popup(at);
    }

    void stripMenu(NodeId id, QPoint at) {
        const auto* n = graph_.find(id);
        if (!n) return;
        const QString name = QString::fromStdString(n->name);
        auto* menu = styledMenu(this);
        if (n->kind == NodeKind::Strip) {
            connect(menu->addAction("New mix-minus for " + name),
                    &QAction::triggered, this, [this, id] { addBus(BusKind::MixMinus, id); });
            menu->addSeparator();
        }
        connect(menu->addAction("Rename " + name + "…"), &QAction::triggered, this,
                [this, id] { renameNode(id); });
        if (n->kind == NodeKind::Bus && id != programBus(graph_))
            connect(menu->addAction("Delete " + name + "…"), &QAction::triggered,
                    this, [this, id] { deleteBus(id); });
        menu->popup(at);
    }

    void newBusMenu(QPoint at) {
        auto* menu = styledMenu(this);
        connect(menu->addAction("Aux bus  (sends, monitors)"), &QAction::triggered,
                this, [this] { addBus(BusKind::Aux, 0); });
        connect(menu->addAction("Group bus  (into Program)"), &QAction::triggered,
                this, [this] { addBus(BusKind::Group, 0); });
        auto* mm = menu->addMenu("Mix-minus for");
        mm->setStyleSheet(menu->styleSheet());
        for (const auto& n : graph_.nodes)
            if (n.kind == NodeKind::Strip) {
                const NodeId id = n.id;
                connect(mm->addAction(QString::fromStdString(n.name)),
                        &QAction::triggered, this,
                        [this, id] { addBus(BusKind::MixMinus, id); });
            }
        menu->popup(at);
    }

    enum class BusKind { Aux, Group, MixMinus };
    void addBus(BusKind kind, NodeId minus) {
        GraphDesc g = graph_;
        const NodeId program = programBus(g);
        NodeDesc b;
        b.id = freeId(g, 3000);
        b.kind = NodeKind::Bus;
        b.channels = 2;
        int count = 1;  // of this kind, for the default name
        for (const auto& n : g.nodes)
            if (n.kind == NodeKind::Bus && n.id != program && !n.mixMinus.follows)
                count += (kind == BusKind::Group) == g.reaches(n.id, program);
        switch (kind) {
        case BusKind::Aux: b.name = "Aux " + std::to_string(count); break;
        case BusKind::Group: b.name = "Group " + std::to_string(count); break;
        case BusKind::MixMinus:
            b.name = "MM " + g.find(minus)->name;
            b.mixMinus.follows = program;
            b.mixMinus.exclude = {minus};
            break;
        }
        g.nodes.push_back(b);
        if (kind == BusKind::Group && program) {
            g.edges.push_back({b.id, program, 0, 0});
        } else if (const int ch = freeOutputPair(); ch >= 0) {
            // Ready to use: straight onto the next free output pair.
            g.edges.push_back({b.id, outputNode(g, ch), 0, 0});
        }
        commit(std::move(g));
    }

    void renameNode(NodeId id) {
        const auto* n = graph_.find(id);
        if (!n) return;
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, "Rename", "Name:", QLineEdit::Normal,
            QString::fromStdString(n->name), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        GraphDesc g = graph_;
        g.find(id)->name = name.toStdString();
        commit(std::move(g));
    }

    void deleteBus(NodeId id) {
        const auto* n = graph_.find(id);
        if (!n) return;
        const QString name = QString::fromStdString(n->name);
        QMessageBox box(QMessageBox::Warning, "Delete " + name,
                        "Delete " + name + "?",
                        QMessageBox::Cancel, this);
        box.setInformativeText("Everything routed through it stops reaching "
                               "its destinations (with a short fade).");
        auto* del = box.addButton("Delete", QMessageBox::DestructiveRole);
        box.exec();
        if (box.clickedButton() != del) return;
        GraphDesc g = graph_;
        std::erase_if(g.nodes, [&](const NodeDesc& x) { return x.id == id; });
        std::erase_if(g.edges, [&](const EdgeDesc& e) {
            return e.from == id || e.to == id;
        });
        for (auto& x : g.nodes)
            if (x.mixMinus.follows == id) x.mixMinus = {};
        pruneOutputs(g);
        commit(std::move(g));
    }

    // The input/generator node feeding a strip: its level is the strip's
    // input gain (applied before any processing).
    static NodeId sourceOf(const GraphDesc& g, NodeId strip) {
        for (const auto& e : g.edges)
            if (e.to == strip)
                if (const auto* f = g.find(e.from);
                    f && (f->kind == NodeKind::Input || f->kind == NodeKind::Generator))
                    return f->id;
        return 0;
    }

    StripWidget* makeStrip(const NodeDesc& n, bool master) {
        auto* s = new StripWidget(QString::fromStdString(n.name), n.channels,
                                  master);
        s->setLevel(n.faderDb, n.mute);
        const auto id = n.id;
        const NodeId source = sourceOf(pendingGraph_, n.id);
        s->setHasGain(source != 0);
        if (source) {
            if (const auto* src = pendingGraph_.find(source)) s->gain()->setGain(src->faderDb);
            gainSource_[id] = source;
            s->gain()->onGain = [this, source](float dB) {
                client_.setLevel(source, dB, false);
                if (auto* srcNode = graph_.find(source)) srcNode->faderDb = dB;
            };
        }
        s->onChange = [this, id](float db, bool mute) {
            client_.setLevel(id, db, mute);
            if (auto* n = graph_.find(id)) {  // so the next edit keeps it
                n->faderDb = db;
                n->mute = mute;
            }
        };
        s->inserts()->onAdd = [this, id](QPoint at) { openBrowser(id, {}, at); };
        s->sends()->onToggle = [this, id](int row) { toggleSend(id, row); };
        s->sends()->onLevel = [this, id](int row, float db) {
            setSendLevel(id, row, db);
        };
        s->sends()->onReset = [this, id](int row) { resetSend(id, row); };
        s->assign()->onClick = [this, id](QPoint at) { assignMenu(id, at); };
        s->onContext = [this, id](QPoint at) { stripMenu(id, at); };
        s->inserts()->onSlot = [this, id](int index, QPoint at) {
            const auto* n = graph_.find(id);
            if (!n) return;
            QMenu menu(this);
            menu.setStyleSheet(
                "QMenu { background: #232733; border: 1px solid #46506a;"
                " color: #e8ecf4; padding: 4px; }"
                "QMenu::item { padding: 5px 18px; border-radius: 4px; }"
                "QMenu::item:selected { background: #2a3a37; }");
            const auto names = insertNames(*n);
            const auto& ins = n->processing.inserts[std::size_t(index)];
            QAction* isolate = nullptr;
            QAction* window = nullptr;
            if (ins.type == InsertDesc::Type::Plugin) {
                window = menu.addAction("Open plugin window");
                if (!ins.sandboxed) {
                    window->setEnabled(false);
                    window->setText("Open plugin window (run isolated first)");
                }
                isolate = menu.addAction("Run isolated (crash-safe, +1 block latency)");
                isolate->setCheckable(true);
                isolate->setChecked(ins.sandboxed);
                menu.addSeparator();
            }
            auto* remove = menu.addAction("Remove " + names.value(index));
            QAction* chosen = menu.exec(at);
            if (chosen && chosen == window) {
                client_.openEditor(id, index);
            } else if (chosen == remove) {
                removeInsert(id, index);
            } else if (chosen && chosen == isolate) {
                GraphDesc g = graph_;
                g.find(id)->processing.inserts[std::size_t(index)].sandboxed =
                    isolate->isChecked();
                client_.setGraph(g);
            }
        };
        strips_[id] = s;
        return s;
    }

    MixClient client_;
    QElapsedTimer uptime_ = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    bool loggedSnapshot_ = false;
    HealthBar* health_;
    QHBoxLayout* bodyLayout_;
    QScrollArea* scroll_;
    QWidget* masterHost_;
    QLabel* offline_;
    QLabel* banner_;
    QLabel* warnings_;
    QLabel* events_;
    QTimer eventsTimer_;
    QMap<quint32, StripWidget*> strips_;
    GraphDesc graph_, pendingGraph_;
    QMap<quint32, quint32> gainSource_;  // strip -> its source node
    QJsonArray catalog_;
    bool scanning_ = false;
    QMap<quint32, QMap<QString, QPair<qint64, qint64>>> rtIssues_;  // allocs, missed
    QStringList loadFailures_;
    QPointer<QWidget> browser_;
    double rate_ = 48000;
    int deviceOutputs_ = 2;
    QTimer sendTimer_;
};

}  // namespace pm::ui

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("ProdMesh Mix");
    app.setOrganizationName("ProdMesh");
    app.setStyle("Fusion");
    app.setStyleSheet(pm::ui::theme::kAppStyle);

    QCommandLineParser p;
    p.addHelpOption();
    p.addOptions({
        {"socket", "Engine socket name", "name", pm::proto::kDefaultSocket},
        {"screenshot", "Save a PNG of the window after --after ms, then quit",
         "path"},
        {"after", "Delay before --screenshot", "ms", "1500"},
        {"open-browser", "Dev aid: open the plugin browser on this strip "
                         "after connecting, filtered by --browser-search",
         "node"},
        {"browser-search", "Dev aid: text for --open-browser", "text"},
    });
    p.process(app);

    pm::ui::MainWindow win(p.value("socket"));
    win.resize(1240, 640);
    win.show();
    if (p.isSet("open-browser"))
        QTimer::singleShot(1200, &win, [&] {
            win.openBrowser(p.value("open-browser").toUInt(),
                            p.value("browser-search"));
        });

    if (p.isSet("screenshot")) {
        QTimer::singleShot(p.value("after").toInt(), &win, [&] {
            QPixmap shot = win.grab();
            if (auto* b = win.browser()) {  // composite the popup on top
                QPainter painter(&shot);
                painter.drawPixmap(win.mapFromGlobal(b->mapToGlobal(QPoint())),
                                   b->grab());
            }
            shot.save(p.value("screenshot"));
            app.quit();
        });
    }
    return app.exec();
}
