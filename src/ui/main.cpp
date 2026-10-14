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
#include <QMenu>
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
    void setFormat(double rate, int block) {
        format_ = QString("%1 kHz · %2 samples")
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
            health_->setFormat(rate_, eng["block"].toInt());
            GraphDesc g;
            if (proto::graphFromJson(m["graph"].toObject(), g, nullptr)) {
                if (!sameLayout(g, graph_) || strips_.isEmpty()) build(g);
                graph_ = g;
                const auto delay = m["delay"].toObject();
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
            if (auto* s = strips_.value(quint32(m["id"].toDouble())))
                s->setLevel(float(m["faderDb"].toDouble()), m["mute"].toBool());
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

        auto* row = new QWidget;
        row->setObjectName("root");
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(24);

        auto section = [&](const QString& title, NodeKind kind, NodeId skip) {
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
            v->addLayout(strips, 1);
            if (count) h->addWidget(col);
            else delete col;
        };
        const NodeId program = programBus(g);
        section("Inputs", NodeKind::Strip, 0);
        section("Buses", NodeKind::Bus, program);
        h->addStretch(1);
        scroll_->setWidget(row);

        if (const auto* p = g.find(program))
            masterHost_->layout()->addWidget(makeStrip(*p, true));
    }

    StripWidget* makeStrip(const NodeDesc& n, bool master) {
        auto* s = new StripWidget(QString::fromStdString(n.name), n.channels,
                                  master);
        s->setLevel(n.faderDb, n.mute);
        const auto id = n.id;
        s->onChange = [this, id](float db, bool mute) {
            client_.setLevel(id, db, mute);
            if (auto* n = graph_.find(id)) {  // so the next edit keeps it
                n->faderDb = db;
                n->mute = mute;
            }
        };
        s->inserts()->onAdd = [this, id](QPoint at) { openBrowser(id, {}, at); };
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
    GraphDesc graph_;
    QJsonArray catalog_;
    bool scanning_ = false;
    QMap<quint32, QMap<QString, QPair<qint64, qint64>>> rtIssues_;  // allocs, missed
    QStringList loadFailures_;
    QPointer<QWidget> browser_;
    double rate_ = 48000;
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
