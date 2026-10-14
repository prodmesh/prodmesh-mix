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
#include "ui/widgets.h"

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
        if (connected_) {
            p.setPen(theme::text);
            p.drawText(QRect(pill.right() + 12, 0, 200, height()),
                       Qt::AlignVCenter, format_);
        }

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
    bool connected_ = false;
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
            health_->setFormat(eng["sampleRate"].toDouble(),
                               eng["block"].toInt());
            GraphDesc g;
            if (proto::graphFromJson(m["graph"].toObject(), g, nullptr))
                build(g);
            setOffline(false);
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
    QMap<quint32, StripWidget*> strips_;
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
    });
    p.process(app);

    pm::ui::MainWindow win(p.value("socket"));
    win.resize(1240, 640);
    win.show();

    if (p.isSet("screenshot")) {
        QTimer::singleShot(p.value("after").toInt(), &win, [&] {
            win.grab().save(p.value("screenshot"));
            app.quit();
        });
    }
    return app.exec();
}
