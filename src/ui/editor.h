// The Editor view: a project's takes on a timeline. First cut is for
// review and navigation — every recorded source and the program, markers,
// a cursor — drawn from the waveform overviews the recorder writes beside
// each WAV (record/peaks.h), never from the audio itself, so an 8-hour take
// opens at once and a live take grows on screen without touching the disk
// the recorder is writing to.
//
//   Cmd-scroll / pinch   zoom around the pointer      R / T   zoom out / in
//   scroll (trackpad) or Shift-scroll: move in time    Home / End
//   click                set the cursor               F       zoom to fit
//
// Colour: accent = signal; red = clipped peaks and the record head (red
// means recording everywhere); markers are white flags.
#pragma once

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "record/peaks.h"
#include "ui/theme.h"
#include "ui/toolbar.h"

namespace pm::ui {

// One recorded track: its overview, plus coarser copies (x8 per level) so
// drawing any zoom touches a handful of values per pixel.
class TrackPeaks {
public:
    QString name, role;
    bool open(const QString& path) {
        if (!reader_.open(path.toStdString())) return false;
        levels_.clear();
        extend();
        return true;
    }
    bool refresh() {
        if (!reader_.refresh()) return false;
        extend();
        return true;
    }
    double seconds() const { return reader_.seconds(); }
    double rate() const { return reader_.header().sampleRate; }
    std::uint32_t bucket() const { return reader_.header().bucket; }
    int channels() const { return reader_.channels(); }

    // Min/max over [t0, t1) seconds, all channels combined; false if empty.
    bool range(double t0, double t1, float& lo, float& hi) const {
        const double perBucket = bucket() / rate();
        auto b0 = std::int64_t(std::floor(t0 / perBucket));
        auto b1 = std::int64_t(std::ceil(t1 / perBucket));
        b0 = std::max<std::int64_t>(b0, 0);
        b1 = std::min<std::int64_t>(b1, std::int64_t(reader_.buckets()));
        if (b1 <= b0) return false;
        // The coarsest level that still has >= 1 entry inside the range.
        int level = 0;
        std::int64_t span = 1;
        while (level < int(levels_.size()) && (b1 - b0) >= span * 8 * 2) {
            ++level;
            span *= 8;
        }
        lo = 1;
        hi = -1;
        if (level == 0) {
            for (auto b = b0; b < b1; ++b)
                for (int c = 0; c < channels(); ++c) {
                    lo = std::min(lo, reader_.lo(std::size_t(b), c));
                    hi = std::max(hi, reader_.hi(std::size_t(b), c));
                }
            return true;
        }
        const auto& L = levels_[std::size_t(level - 1)];
        const auto e0 = b0 / span, e1 = std::min<std::int64_t>((b1 + span - 1) / span,
                                                                std::int64_t(L.lo.size()));
        for (auto e = e0; e < e1; ++e) {
            lo = std::min(lo, L.lo[std::size_t(e)]);
            hi = std::max(hi, L.hi[std::size_t(e)]);
        }
        return e1 > e0;
    }

private:
    struct Level {
        std::vector<float> lo, hi;  // channels combined
        std::size_t covered = 0;    // level-0 buckets folded in (whole groups)
    };
    // Fold newly arrived buckets into the coarser levels.
    void extend() {
        if (levels_.empty()) levels_.resize(6);  // x8 .. x262144
        std::size_t span = 8;
        for (std::size_t l = 0; l < levels_.size(); ++l, span *= 8) {
            auto& L = levels_[l];
            const std::size_t whole = reader_.buckets() / span;
            // Recompute the last (possibly partial) entry and add new ones.
            const std::size_t from = L.lo.empty() ? 0 : L.lo.size() - 1;
            L.lo.resize(std::max(whole, from));
            L.hi.resize(L.lo.size());
            for (std::size_t e = from; e < whole; ++e) {
                float lo = 1, hi = -1;
                if (l == 0) {
                    for (std::size_t b = e * 8; b < e * 8 + 8; ++b)
                        for (int c = 0; c < channels(); ++c) {
                            lo = std::min(lo, reader_.lo(b, c));
                            hi = std::max(hi, reader_.hi(b, c));
                        }
                } else {
                    const auto& P = levels_[l - 1];
                    for (std::size_t p = e * 8; p < e * 8 + 8 && p < P.lo.size(); ++p) {
                        lo = std::min(lo, P.lo[p]);
                        hi = std::max(hi, P.hi[p]);
                    }
                }
                L.lo[e] = lo;
                L.hi[e] = hi;
            }
        }
    }
    rec::PeakReader reader_;
    std::vector<Level> levels_;
};

struct TakeMarker {
    double t = 0;
    QString label;
};

// A take on disk: Takes/<name>/take.json, the .peaks files, markers.csv.
struct Take {
    QString dir, name;
    QDateTime started;
    std::vector<std::unique_ptr<TrackPeaks>> tracks;
    std::vector<TakeMarker> markers;
    qint64 markersSize = -1;

    double seconds() const {
        double s = 0;
        for (const auto& t : tracks) s = std::max(s, t->seconds());
        return s;
    }
    bool load(const QString& takeDir) {
        dir = takeDir;
        name = QFileInfo(takeDir).fileName();
        tracks.clear();
        QFile f(takeDir + "/take.json");
        if (f.open(QIODevice::ReadOnly)) {
            const auto doc = QJsonDocument::fromJson(f.readAll()).object();
            started = QDateTime::fromString(doc["started"].toString(), Qt::ISODateWithMs);
            for (const auto& v : doc["tracks"].toArray()) {
                const auto o = v.toObject();
                auto t = std::make_unique<TrackPeaks>();
                t->name = o["name"].toString();
                t->role = o["role"].toString();
                const QString wav = takeDir + "/" + o["file"].toString();
                if (t->open(QString::fromStdString(rec::peaksPathFor(wav.toStdString()))))
                    tracks.push_back(std::move(t));
            }
        } else {  // older takes: whatever overviews are there
            for (const auto& fi : QDir(takeDir).entryInfoList({"*.peaks"}, QDir::Files,
                                                              QDir::Name)) {
                auto t = std::make_unique<TrackPeaks>();
                t->name = fi.completeBaseName().mid(3);
                if (t->open(fi.filePath())) tracks.push_back(std::move(t));
            }
        }
        // The broadcast itself first, then the sources in recording order.
        std::stable_partition(tracks.begin(), tracks.end(),
                              [](const auto& t) { return t->role == "program"; });
        loadMarkers();
        return !tracks.empty();
    }
    bool refresh() {
        bool any = false;
        for (auto& t : tracks) any |= t->refresh();
        any |= loadMarkers();
        return any;
    }
    // markers.csv: index,take_time,take_seconds,sample,wall_clock,"label"
    bool loadMarkers() {
        QFile f(dir + "/markers.csv");
        if (!f.exists() || f.size() == markersSize) return false;
        markersSize = f.size();
        markers.clear();
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return true;
        f.readLine();  // header
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            const auto parts = line.split(',');
            if (parts.size() < 6) continue;
            QString label = line.section(',', 5);
            if (label.startsWith('"') && label.endsWith('"')) label = label.mid(1, label.size() - 2);
            markers.push_back({parts[2].toDouble(), label});
        }
        return true;
    }
};

class Timeline : public QWidget {
public:
    static constexpr int kHeader = 150, kRuler = 30, kLane = 64, kGap = 4;
    std::function<void()> onViewChanged;  // zoom / scroll / follow changed

    Timeline() {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void setTake(Take* t, bool live) {
        take_ = t;
        live_ = live;
        top_ = 0;
        if (t && !live) fit();
        else if (t) followLive_ = true;
        update();
    }
    void setLive(bool live) {
        live_ = live;
        update();
    }
    Take* take() const { return take_; }

    double secondsPerPixel() const { return spp_; }
    double startTime() const { return t0_; }
    double cursor() const { return cursor_; }
    bool following() const { return followLive_ && live_; }
    double width_s() const { return spp_ * laneWidth(); }

    void setStart(double t) {
        t0_ = std::max(0.0, t);
        update();
    }
    void zoomBy(double factor, double anchorX = -1) {
        if (!take_) return;
        const double ax = anchorX < 0 ? laneWidth() / 2.0 : anchorX - kHeader;
        const double at = t0_ + ax * spp_;
        const double minSpp = take_->tracks.empty()
                                  ? 0.001
                                  : take_->tracks[0]->bucket() / take_->tracks[0]->rate() / 4;
        spp_ = std::clamp(spp_ * factor, minSpp, 60.0);
        t0_ = std::max(0.0, at - ax * spp_);
        update();
        if (onViewChanged) onViewChanged();
    }
    void fit() {
        if (!take_) return;
        const double len = std::max(1.0, take_->seconds());
        spp_ = len * 1.02 / std::max(100, laneWidth());
        t0_ = 0;
        followLive_ = false;
        update();
        if (onViewChanged) onViewChanged();
    }
    void setFollow(bool on) {
        followLive_ = on;
        tick();
        if (onViewChanged) onViewChanged();
    }
    // Called when new audio arrives: keep the record head in view.
    void tick() {
        if (take_ && following()) {
            const double head = take_->seconds();
            if (head <= width_s() * 0.92)
                t0_ = 0;  // the whole take fits: show it from the start
            else if (head > t0_ + width_s() * 0.92 || head < t0_)
                t0_ = std::max(0.0, head - width_s() * 0.75);
        }
        update();
    }
    int laneWidth() const { return std::max(1, width() - kHeader); }
    int contentHeight() const {
        return take_ ? kRuler + int(take_->tracks.size()) * (kLane + kGap) + kGap : kRuler;
    }

protected:
    double timeAt(double x) const { return t0_ + (x - kHeader) * spp_; }
    double xAt(double t) const { return kHeader + (t - t0_) / spp_; }

    // Ruler steps that read well: 1 s .. 1 h.
    double tickStep() const {
        static const double steps[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300,
                                       600, 900, 1800, 3600, 7200};
        for (double s : steps)
            if (s / spp_ >= 90) return s;
        return 7200;
    }
    static QString fmt(double t, double step) {
        const int s = int(t);
        QString out = hms(t);
        if (step < 1) out += QString(".%1").arg(int(std::round((t - s) * 10)) % 10);
        return out;
    }
    // Level in dB on the lane: quiet speech stays visible (-48 dB floor).
    static double dbScale(float v) {
        const double a = std::fabs(v);
        if (a < 0.0039) return 0;  // -48 dB
        return std::clamp((20 * std::log10(a) + 48) / 48, 0.0, 1.0);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), theme::bg);
        if (!take_) {
            p.setPen(theme::text);
            p.setFont(uiFont(this, 13));
            p.drawText(rect(), Qt::AlignCenter,
                       "No recordings in this project yet.\nTakes appear here as soon as "
                       "recording starts.");
            return;
        }
        const int w = width();
        const double tEnd = timeAt(w);
        const double head = take_->seconds();

        // Lanes.
        for (std::size_t i = 0; i < take_->tracks.size(); ++i) {
            const auto& tr = *take_->tracks[i];
            const int y = kRuler + kGap + int(i) * (kLane + kGap) - top_;
            if (y + kLane < kRuler || y > height()) continue;
            const QRect lane(kHeader, y, w - kHeader, kLane);
            p.fillRect(lane, QColor("#191c23"));
            const bool program = tr.role == "program";
            // Header.
            const QRect head_r(0, y, kHeader - 1, kLane);
            p.fillRect(head_r, QColor("#1d2129"));
            p.fillRect(QRect(0, y, 3, kLane), program ? theme::accentHi : theme::accent);
            p.setFont(uiFont(this, 12, QFont::DemiBold));
            p.setPen(program ? theme::accentHi : theme::textMax);
            p.drawText(head_r.adjusted(12, 8, -8, 0), Qt::AlignLeft | Qt::AlignTop,
                       p.fontMetrics().elidedText(tr.name, Qt::ElideRight, kHeader - 20));
            p.setFont(uiFont(this, 10));
            p.setPen(theme::text);
            p.drawText(head_r.adjusted(12, 28, -8, 0), Qt::AlignLeft | Qt::AlignTop,
                       QString(program ? "program" : "source") +
                           (tr.channels() > 1 ? " · stereo" : " · mono"));

            // Waveform, mirrored around the lane's centre line, in dB.
            const double mid = y + kLane / 2.0, half = kLane / 2.0 - 3;
            p.setPen(QColor("#262b38"));
            p.drawLine(kHeader, int(mid), w, int(mid));
            const int x0 = kHeader, x1 = std::min(w, int(std::ceil(xAt(head))));
            QPainterPath shape;
            std::vector<QPointF> top, bottom;
            std::vector<int> clipped;
            for (int x = x0; x < x1; ++x) {
                float lo, hi;
                if (!tr.range(timeAt(x), timeAt(x + 1), lo, hi)) continue;
                const double up = dbScale(hi > 0 ? hi : 0), dn = dbScale(lo < 0 ? lo : 0);
                top.push_back({double(x), mid - half * up});
                bottom.push_back({double(x), mid + half * dn});
                if (hi >= 0.999f || lo <= -0.999f) clipped.push_back(x);
            }
            if (!top.empty()) {
                shape.moveTo(top.front());
                for (const auto& pt : top) shape.lineTo(pt);
                for (auto it = bottom.rbegin(); it != bottom.rend(); ++it) shape.lineTo(*it);
                shape.closeSubpath();
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setPen(Qt::NoPen);
                p.setBrush(program ? QColor(92, 224, 189, 150) : QColor(47, 191, 155, 140));
                p.drawPath(shape);
                p.setRenderHint(QPainter::Antialiasing, false);
            }
            p.setPen(theme::clip);
            for (int x : clipped) p.drawLine(x, y + 1, x, y + 5);
        }

        // Markers through the lanes.
        for (const auto& m : take_->markers) {
            const double x = xAt(m.t);
            if (x < kHeader || x > w) continue;
            p.setPen(QPen(QColor(232, 236, 244, 90), 1, Qt::DashLine));
            p.drawLine(QPointF(x, kRuler), QPointF(x, height()));
        }
        // Record head (live) and cursor.
        if (live_) {
            const double x = xAt(head);
            if (x >= kHeader && x <= w) {
                p.setPen(QPen(theme::clip, 2));
                p.drawLine(QPointF(x, kRuler), QPointF(x, height()));
            }
        }
        if (cursor_ >= 0) {
            const double x = xAt(cursor_);
            if (x >= kHeader && x <= w) {
                p.setPen(QPen(theme::textMax, 1));
                p.drawLine(QPointF(x, kRuler), QPointF(x, height()));
            }
        }

        // Ruler on top (drawn last so lanes scroll under it).
        p.fillRect(QRect(0, 0, w, kRuler), theme::panel);
        p.setPen(theme::grid);
        p.drawLine(0, kRuler - 1, w, kRuler - 1);
        const double step = tickStep();
        p.setFont(monoFont(10));
        for (double t = std::floor(t0_ / step) * step; t <= tEnd; t += step) {
            const double x = xAt(t);
            if (x < kHeader) continue;
            p.setPen(theme::border);
            p.drawLine(QPointF(x, kRuler - 8), QPointF(x, kRuler - 1));
            for (int k = 1; k < 4; ++k) {  // minor ticks
                const double xm = xAt(t + step * k / 4);
                if (xm < w) p.drawLine(QPointF(xm, kRuler - 4), QPointF(xm, kRuler - 1));
            }
            p.setPen(theme::text);
            p.drawText(QRectF(x + 4, 2, 120, kRuler - 10), Qt::AlignLeft | Qt::AlignVCenter,
                       fmt(t, step));
        }
        // Marker flags.
        for (const auto& m : take_->markers) {
            const double x = xAt(m.t);
            if (x < kHeader || x > w) continue;
            p.setFont(uiFont(this, 10, QFont::DemiBold));
            const int tw = p.fontMetrics().horizontalAdvance(m.label) + 12;
            const QRectF flag(x, 4, tw, 16);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor("#e8ecf4"));
            p.drawRoundedRect(flag, 3, 3);
            p.setPen(QColor("#14161c"));
            p.drawText(flag.adjusted(6, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m.label);
        }
        // Header corner: the take's wall-clock start.
        p.fillRect(QRect(0, 0, kHeader, kRuler), theme::panel);
        p.setPen(theme::text);
        p.setFont(uiFont(this, 10));
        p.drawText(QRect(12, 0, kHeader - 12, kRuler), Qt::AlignVCenter,
                   take_->started.isValid() ? "started " + take_->started.toString("HH:mm:ss")
                                            : QString());
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->position().x() < kHeader || !take_) return;
        cursor_ = std::clamp(timeAt(e->position().x()), 0.0, take_->seconds());
        update();
        if (onViewChanged) onViewChanged();
    }
    void wheelEvent(QWheelEvent* e) override {
        if (!take_) return;
        const auto d = e->pixelDelta().isNull() ? e->angleDelta() / 4 : e->pixelDelta();
        if (e->modifiers() & Qt::ControlModifier) {  // Cmd on macOS: zoom
            zoomBy(std::pow(1.0025, -d.y()), e->position().x());
            return;
        }
        int dx = d.x(), dy = d.y();
        if (e->modifiers() & Qt::ShiftModifier) std::swap(dx, dy);
        if (dx) {
            followLive_ = false;  // the operator took over
            t0_ = std::max(0.0, t0_ - dx * spp_);
        }
        if (dy) {
            const int maxTop = std::max(0, contentHeight() - height());
            top_ = std::clamp(top_ - dy, 0, maxTop);
        }
        update();
        if (onViewChanged) onViewChanged();
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::NativeGesture) {
            auto* g = static_cast<QNativeGestureEvent*>(e);
            if (g->gestureType() == Qt::ZoomNativeGesture) {
                zoomBy(1.0 / (1.0 + g->value()), g->position().x());
                return true;
            }
        }
        return QWidget::event(e);
    }
    void keyPressEvent(QKeyEvent* e) override {
        switch (e->key()) {
        case Qt::Key_T: zoomBy(0.5); break;   // Pro Tools: T in, R out
        case Qt::Key_R: zoomBy(2.0); break;
        case Qt::Key_F: fit(); break;
        case Qt::Key_Home: followLive_ = false; t0_ = 0; break;
        case Qt::Key_End:
            if (take_) t0_ = std::max(0.0, take_->seconds() - width_s() * 0.75);
            break;
        default: QWidget::keyPressEvent(e); return;
        }
        update();
        if (onViewChanged) onViewChanged();
    }
    void resizeEvent(QResizeEvent*) override { tick(); }

private:
    Take* take_ = nullptr;
    bool live_ = false, followLive_ = true;
    double spp_ = 0.05, t0_ = 0, cursor_ = -1;
    int top_ = 0;
};

class EditorView : public QWidget {
public:
    EditorView() {
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet("background: #14161c;");
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        auto* bar = new QWidget;
        bar->setFixedHeight(44);
        bar->setStyleSheet(
            "QWidget { background: #14161c; }"
            "QLabel { color: #8a92a6; font-size: 11px; }"
            "QComboBox { background: #1d2129; border: 1px solid #323848; border-radius: 6px;"
            " color: #e8ecf4; padding: 4px 10px; font-size: 12px; min-width: 260px; }"
            "QComboBox QAbstractItemView { background: #232733; color: #e8ecf4;"
            " selection-background-color: #2a3a37; }"
            "QPushButton { background: #1d2129; border: 1px solid #323848; border-radius: 6px;"
            " color: #c8cede; font-size: 12px; padding: 4px 12px; }"
            "QPushButton:hover { border-color: #46506a; color: #e8ecf4; }"
            "QPushButton:checked { background: #1f3b34; border-color: #2fbf9b; color: #e8ecf4; }");
        auto* h = new QHBoxLayout(bar);
        h->setContentsMargins(16, 6, 16, 6);
        h->setSpacing(8);
        auto* lbl = new QLabel("TAKE");
        lbl->setStyleSheet("color: #8a92a6; font-size: 10px; font-weight: 600;"
                           " letter-spacing: 1.5px;");
        h->addWidget(lbl);
        takes_ = new QComboBox;
        h->addWidget(takes_);
        info_ = new QLabel;
        h->addWidget(info_);
        h->addStretch(1);
        cursorLbl_ = new QLabel;
        cursorLbl_->setStyleSheet("color: #c8cede; font-size: 12px;");
        h->addWidget(cursorLbl_);
        h->addSpacing(12);
        auto* out = new QPushButton("−");
        auto* in = new QPushButton("+");
        auto* fit = new QPushButton("Fit");
        follow_ = new QPushButton("Follow live");
        follow_->setCheckable(true);
        out->setToolTip("Zoom out (R, or Cmd-scroll)");
        in->setToolTip("Zoom in (T, or Cmd-scroll)");
        fit->setToolTip("The whole take (F)");
        follow_->setToolTip("Keep the record head in view");
        for (auto* b : {out, in, fit, follow_}) {
            b->setCursor(Qt::PointingHandCursor);
            h->addWidget(b);
        }
        v->addWidget(bar);

        timeline_ = new Timeline;
        v->addWidget(timeline_, 1);
        setFocusProxy(timeline_);  // R / T / F / Home / End
        scroll_ = new QScrollBar(Qt::Horizontal);
        v->addWidget(scroll_);

        connect(out, &QPushButton::clicked, this, [this] { timeline_->zoomBy(2.0); });
        connect(in, &QPushButton::clicked, this, [this] { timeline_->zoomBy(0.5); });
        connect(fit, &QPushButton::clicked, this, [this] { timeline_->fit(); });
        connect(follow_, &QPushButton::toggled, this, [this](bool on) {
            if (!syncing_) timeline_->setFollow(on);
        });
        connect(takes_, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (!syncing_ && i >= 0) openTake(takes_->itemData(i).toString());
        });
        connect(scroll_, &QScrollBar::valueChanged, this, [this](int v) {
            if (syncing_) return;
            timeline_->setFollow(false);
            timeline_->setStart(v / 10.0);
            syncControls();
        });
        timeline_->onViewChanged = [this] { syncControls(); };

        poll_.setInterval(300);  // a live take grows; markers arrive
        connect(&poll_, &QTimer::timeout, this, [this] {
            if (take_ && take_->name == liveTake_ && take_->refresh()) {
                timeline_->tick();
                syncControls();
            }
        });
        poll_.start();
    }

    // The project folder; rescans its takes.
    void setProject(const QString& path) {
        if (path == project_) return;
        project_ = path;
        rescan(true);
    }
    // The take being recorded now (from the engine's health), or empty.
    void setLiveTake(const QString& name) {
        if (name == liveTake_) return;
        liveTake_ = name;
        rescan(take_ == nullptr || take_->name != name);
    }

private:
    void rescan(bool openNewest) {
        syncing_ = true;
        const QString keep = take_ ? take_->name : QString();
        takes_->clear();
        QStringList dirs;
        if (!project_.isEmpty())
            for (const auto& fi : QDir(project_ + "/Takes").entryInfoList(
                     QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed))
                dirs << fi.filePath();
        for (const auto& d : dirs) {
            const QString n = QFileInfo(d).fileName();
            takes_->addItem(n == liveTake_ ? n + "   ● recording" : n, d);
        }
        syncing_ = false;
        int pick = -1;
        for (int i = 0; i < takes_->count(); ++i)
            if (QFileInfo(takes_->itemData(i).toString()).fileName() ==
                (openNewest || keep.isEmpty() ? (liveTake_.isEmpty() ? QString() : liveTake_)
                                              : keep))
                pick = i;
        if (pick < 0 && takes_->count()) pick = 0;  // newest
        if (pick >= 0) {
            syncing_ = true;
            takes_->setCurrentIndex(pick);
            syncing_ = false;
            openTake(takes_->itemData(pick).toString());
        } else {
            take_.reset();
            timeline_->setTake(nullptr, false);
            syncControls();
        }
    }
    void openTake(const QString& dir) {
        auto t = std::make_unique<Take>();
        if (!t->load(dir)) t.reset();
        take_ = std::move(t);
        const bool live = take_ && take_->name == liveTake_;
        timeline_->setTake(take_.get(), live);
        syncControls();
    }
    void syncControls() {
        syncing_ = true;
        if (take_) {
            info_->setText(QString("%1 tracks · %2 · %3 markers")
                               .arg(take_->tracks.size())
                               .arg(hms(take_->seconds()))
                               .arg(take_->markers.size()));
            const double c = timeline_->cursor();
            cursorLbl_->setText(
                c < 0 ? QString()
                      : "Cursor " + hms(c) +
                            QString(".%1").arg(int((c - std::floor(c)) * 1000), 3, 10, QChar('0')) +
                            (take_->started.isValid()
                                 ? "  ·  " + take_->started.addMSecs(qint64(c * 1000))
                                                 .toString("HH:mm:ss")
                                 : QString()));
            const double len = take_->seconds();
            scroll_->setRange(0, int(std::max(0.0, len - timeline_->width_s()) * 10));
            scroll_->setPageStep(int(timeline_->width_s() * 10));
            scroll_->setValue(int(timeline_->startTime() * 10));
        } else {
            info_->setText(project_.isEmpty() ? QString() : "No takes yet");
            cursorLbl_->clear();
            scroll_->setRange(0, 0);
        }
        const bool live = take_ && take_->name == liveTake_;
        follow_->setVisible(live);
        follow_->setChecked(timeline_->following());
        syncing_ = false;
    }

    QComboBox* takes_;
    QLabel *info_, *cursorLbl_;
    QPushButton* follow_;
    Timeline* timeline_;
    QScrollBar* scroll_;
    QTimer poll_;
    QString project_, liveTake_;
    std::unique_ptr<Take> take_;
    bool syncing_ = false;
};

}  // namespace pm::ui
