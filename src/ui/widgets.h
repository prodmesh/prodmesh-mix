// Mixer widgets: a channel strip with a custom-painted meter and fader.
//
// Interaction rules (console conventions, not DAW quirks):
//   - dragging moves the fader relative to where you grabbed it; clicking
//     the track never jumps it (no accidental level changes on air)
//   - Option/Alt while dragging or scrolling = fine (x0.1)
//   - double-click the fader = 0 dB; scroll = 0.5 dB per notch
//   - the clip light latches until clicked
#pragma once

#include <QElapsedTimer>
#include <QFontDatabase>
#include <QHelpEvent>
#include <QMap>
#include <QToolTip>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <functional>

#include "ui/theme.h"

namespace pm::ui {

inline constexpr float kFaderMinDb = -120.0f;  // bottom of travel = off

// Console fader law: 0 dB at 80% of travel, most resolution around it.
struct FaderLaw {
    static constexpr float db[] = {10, 5, 0, -5, -10, -20, -30, -40, -60, -90};
    static constexpr float pos[] = {1.0f, 0.9f, 0.8f, 0.7f, 0.6f,
                                    0.45f, 0.32f, 0.22f, 0.08f, 0.0f};
    static constexpr int n = 10;

    static float toPos(float d) {
        if (d >= db[0]) return 1;
        if (d <= db[n - 1]) return 0;
        for (int i = 1; i < n; ++i)
            if (d >= db[i])
                return pos[i] + (d - db[i]) / (db[i - 1] - db[i]) *
                                    (pos[i - 1] - pos[i]);
        return 0;
    }
    static float toDb(float p) {
        if (p >= 1) return db[0];
        if (p <= 0) return kFaderMinDb;
        for (int i = 1; i < n; ++i)
            if (p >= pos[i])
                return db[i] + (p - pos[i]) / (pos[i - 1] - pos[i]) *
                                   (db[i - 1] - db[i]);
        return kFaderMinDb;
    }
};

inline QString fmtDb(float d) {
    if (d <= -89.95f) return QStringLiteral("−∞");
    QString s = QString::number(double(d), 'f', 1);
    return d > 0.05f ? "+" + s : s.replace('-', QChar(0x2212));
}

// Meter + fader + scale, painted as one so they always line up.
class FaderMeter : public QWidget {
public:
    std::function<void(float)> onLevel;  // user moved the fader

    explicit FaderMeter(int channels, QWidget* parent = nullptr)
        : QWidget(parent), channels_(channels) {
        setMinimumHeight(220);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setFixedWidth(channels == 2 ? 64 : 58);
        setCursor(Qt::PointingHandCursor);
    }

    void setDb(float d) {
        if (dragging_) return;  // the operator's hand wins
        db_ = d;
        update();
    }
    float db() const { return db_; }

    // Values in dBFS, called at the daemon's meter rate (~30 Hz).
    void setMeter(const float peak[2], const float rms[2]) {
        const qint64 now = clock_.elapsed();
        const float dt = lastMeter_ ? float(now - lastMeter_) / 1000.0f : 0;
        lastMeter_ = now;
        for (int c = 0; c < channels_; ++c) {
            // 20 dB/s fall, instant rise: readable and honest about peaks.
            peak_[c] = std::max(peak[c], peak_[c] - 20.0f * dt);
            rms_[c] = std::max(rms[c], rms_[c] - 20.0f * dt);
            if (peak[c] >= hold_[c] || now - holdAt_[c] > 1500) {
                hold_[c] = peak[c];
                holdAt_[c] = now;
            }
            if (peak[c] >= -0.1f) clip_ = true;
        }
        update();
    }

    // Empty the meter at once (no fall-off), e.g. when the engine is gone.
    void clearMeter() {
        for (int c = 0; c < 2; ++c) peak_[c] = rms_[c] = hold_[c] = -120;
        update();
    }

    QSize sizeHint() const override { return {width(), 320}; }

protected:
    // Meter scale -60..+6 dBFS, linear in dB.
    static constexpr float kMeterTop = 6, kMeterBottom = -60;

    QRect travel() const { return rect().adjusted(0, 18, 0, -10); }

    int meterY(float d) const {
        const QRect t = travel();
        const float f = (std::clamp(d, kMeterBottom, kMeterTop) - kMeterBottom) /
                        (kMeterTop - kMeterBottom);
        return t.bottom() - int(f * float(t.height()));
    }
    int faderY(float d) const {
        const QRect t = travel();
        return t.bottom() - int(FaderLaw::toPos(d) * float(t.height()));
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect t = travel();

        // Clip light.
        const QRect clipR(2, 2, meterWidth(), 10);
        p.setPen(Qt::NoPen);
        p.setBrush(clip_ ? theme::clip : theme::control);
        p.drawRoundedRect(clipR, 2, 2);

        // Meter bars.
        const int bw = channels_ == 2 ? 7 : 10;
        for (int c = 0; c < channels_; ++c) {
            const int x = 2 + c * (bw + 2);
            const QRect bar(x, t.top(), bw, t.height());
            p.setBrush(theme::control);
            p.drawRect(bar);
            drawSegment(p, x, bw, kMeterBottom, std::min(peak_[c], -18.0f),
                        theme::accent);
            if (peak_[c] > -18) drawSegment(p, x, bw, -18,
                                            std::min(peak_[c], -6.0f),
                                            theme::warn);
            if (peak_[c] > -6) drawSegment(p, x, bw, -6, peak_[c], theme::clip);
            // RMS as a brighter core over the peak bar.
            p.setBrush(QColor(255, 255, 255, 40));
            const int ry = meterY(rms_[c]);
            p.drawRect(QRect(x, ry, bw, t.bottom() - ry));
            // The meter has its own dBFS scale, distinct from the fader's:
            // marks at 0, -6 and -18 dBFS, in the colour of the zone above.
            for (const auto& [d, col] : {std::pair{0.0f, theme::clip},
                                         std::pair{-6.0f, theme::clip},
                                         std::pair{-18.0f, theme::warn}}) {
                QColor m = col;
                m.setAlpha(150);
                p.setBrush(m);
                p.drawRect(QRect(x, meterY(d), bw, 1));
            }
            if (hold_[c] > kMeterBottom) {
                p.setBrush(hold_[c] > -6 ? theme::clip : theme::textMax);
                p.drawRect(QRect(x, meterY(hold_[c]), bw, 2));
            }
        }

        // Fader scale ticks between meter and fader.
        const int sx = meterWidth() + 6;
        QFont f = font();
        f.setPixelSize(9);
        p.setFont(f);
        for (float d : {10.0f, 5.0f, 0.0f, -5.0f, -10.0f, -20.0f, -30.0f,
                        -40.0f, -60.0f}) {
            const int y = faderY(d);
            p.setPen(d == 0 ? theme::textHi : theme::text);
            const QString label =
                d > 0 ? "+" + QString::number(int(d)) : QString::number(int(-d));
            p.drawText(QRect(sx - 2, y - 6, 20, 12),
                       Qt::AlignRight | Qt::AlignVCenter, label);
            p.setPen(theme::grid);
            p.drawLine(sx + 20, y, sx + 24, y);
        }

        // Fader track + cap.
        const int fx = width() - 14;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#0e1015"));
        p.drawRoundedRect(QRect(fx - 2, t.top(), 4, t.height()), 2, 2);
        const QRect cap = capRect();
        p.setBrush(dragging_ ? theme::borderHi : theme::border);
        p.drawRoundedRect(cap, 3, 3);
        p.setPen(QPen(db_ > -0.05f && db_ < 0.05f ? theme::accent : theme::textMax, 2));
        const int cy = cap.center().y();
        p.drawLine(cap.left() + 3, cy, cap.right() - 3, cy);
    }

    QRect capRect() const {
        const int fx = width() - 14;
        return QRect(fx - 10, faderY(db_) - 12, 20, 24);
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (QRect(2, 2, meterWidth(), 10).contains(e->pos())) {
            clip_ = false;  // acknowledge the clip
            update();
            return;
        }
        if (capRect().adjusted(-6, -4, 6, 4).contains(e->pos())) {
            dragging_ = true;
            grabY_ = e->position().y();
            grabPos_ = FaderLaw::toPos(db_);
            update();
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!dragging_) return;
        const float scale = e->modifiers() & Qt::AltModifier ? 0.1f : 1.0f;
        const float dp = float(grabY_ - e->position().y()) /
                         float(travel().height()) * scale;
        if (scale != lastScale_) {  // re-anchor so switching modes never jumps
            grabY_ = e->position().y();
            grabPos_ = FaderLaw::toPos(db_);
            lastScale_ = scale;
            return;
        }
        commit(FaderLaw::toDb(std::clamp(grabPos_ + dp, 0.0f, 1.0f)));
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        dragging_ = false;
        lastScale_ = 1;
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (capRect().adjusted(-6, -4, 6, 4).contains(e->pos())) commit(0);
    }
    void wheelEvent(QWheelEvent* e) override {
        const float step = e->modifiers() & Qt::AltModifier ? 0.1f : 0.5f;
        const int notches = e->angleDelta().y() / 120;
        if (!notches) return;
        float d = db_ <= -89.95f ? -90.0f : db_;
        commit(std::clamp(d + step * float(notches), kFaderMinDb, 10.0f));
    }

private:
    int meterWidth() const { return channels_ == 2 ? 16 : 10; }

    void drawSegment(QPainter& p, int x, int w, float from, float to,
                     const QColor& c) {
        if (to <= from) return;
        const int y0 = meterY(to), y1 = meterY(from);
        p.setBrush(c);
        p.drawRect(QRect(x, y0, w, y1 - y0));
    }

    void commit(float d) {
        d = std::round(d * 10.0f) / 10.0f;
        if (d < -89.95f) d = kFaderMinDb;
        if (d == db_) return;
        db_ = d;
        update();
        if (onLevel) onLevel(d);
    }

    int channels_;
    float db_ = 0;
    float peak_[2] = {-120, -120}, rms_[2] = {-120, -120};
    float hold_[2] = {-120, -120};
    qint64 holdAt_[2] = {0, 0}, lastMeter_ = 0;
    bool clip_ = false;
    bool dragging_ = false;
    double grabY_ = 0;
    float grabPos_ = 0, lastScale_ = 1;
    QElapsedTimer clock_ = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
};

// Insert slots, painted. Fixed height (3 slots + add) so faders line up
// across strips whatever their chains hold. Click a slot to act on it,
// click the last row to add.
class InsertList : public QWidget {
public:
    static constexpr int kRows = 4, kRowH = 20, kGap = 3;
    std::function<void(int index, QPoint globalPos)> onSlot;
    std::function<void(QPoint globalPos)> onAdd;

    InsertList() {
        setStyleSheet("background: transparent;");
        setFixedHeight(kRows * (kRowH + kGap) - kGap);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }
    void setInserts(const QStringList& names) {
        names_ = names;
        update();
    }
    // Per-slot notes (amber marker + tooltip): a plugin that is not
    // real-time safe, or one that failed to load.
    void setNotes(const QMap<int, QString>& notes) {
        if (notes == notes_) return;
        notes_ = notes;
        update();
    }

protected:
    QRect row(int i) const {
        return QRect(0, i * (kRowH + kGap), width(), kRowH);
    }
    int addRow() const { return std::min(int(names_.size()), kRows - 1); }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QFont f = font();
        f.setPixelSize(11);
        p.setFont(f);
        const int shown = std::min(int(names_.size()), kRows - 1);
        for (int i = 0; i < shown; ++i) {
            const QRect r = row(i);
            p.setPen(Qt::NoPen);
            p.setBrush(i == hover_ ? QColor("#2c3241") : QColor("#262b38"));
            p.drawRoundedRect(r, 4, 4);
            p.setBrush(notes_.contains(i) ? theme::warn : theme::accent);
            p.drawRoundedRect(QRect(r.left(), r.top(), 3, r.height()), 1.5, 1.5);
            p.setPen(theme::textHi);
            p.drawText(r.adjusted(8, 0, -4, 0), Qt::AlignVCenter,
                       p.fontMetrics().elidedText(names_[i], Qt::ElideRight,
                                                  r.width() - 12));
        }
        const QRect add = row(addRow());
        p.setPen(QPen(i_hoverAdd() ? theme::borderHi : theme::border, 1,
                      Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(add.adjusted(0, 0, -1, -1), 4, 4);
        p.setPen(i_hoverAdd() ? theme::textHi : theme::text);
        const QString more = names_.size() > kRows - 1
                                 ? QString("+%1 more").arg(names_.size() - (kRows - 1))
                                 : QStringLiteral("+ Insert");
        p.drawText(add, Qt::AlignCenter, more);
    }
    bool i_hoverAdd() const { return hover_ == addRow(); }

    void mouseMoveEvent(QMouseEvent* e) override {
        int h = -1;
        for (int i = 0; i < kRows; ++i)
            if (row(i).contains(e->pos())) h = i;
        if (h > addRow()) h = -1;
        if (h != hover_) {
            hover_ = h;
            update();
        }
    }
    void leaveEvent(QEvent*) override {
        hover_ = -1;
        update();
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(e);
            for (int i = 0; i < std::min(int(names_.size()), kRows - 1); ++i)
                if (row(i).contains(he->pos())) {
                    QToolTip::showText(he->globalPos(),
                                       notes_.value(i, names_[i]), this);
                    return true;
                }
            QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }
    void mousePressEvent(QMouseEvent* e) override {
        const QPoint g = e->globalPosition().toPoint();
        for (int i = 0; i <= addRow(); ++i)
            if (row(i).contains(e->pos())) {
                if (i == addRow()) {
                    if (onAdd) onAdd(g);
                } else if (onSlot) {
                    onSlot(i, g);
                }
            }
    }

private:
    QStringList names_;
    QMap<int, QString> notes_;
    int hover_ = -1;
};

class StripWidget : public QWidget {
public:
    std::function<void(float faderDb, bool mute)> onChange;

    StripWidget(const QString& name, int channels, bool master,
                QWidget* parent = nullptr)
        : QWidget(parent), master_(master) {
        setAttribute(Qt::WA_StyledBackground);
        setObjectName("strip");
        setStyleSheet(master
            ? "#strip { background: #1d2129; border: 1px solid #2f6b5c; border-radius: 6px; }"
            : "#strip { background: #1d2129; border: 1px solid #262a34; border-radius: 6px; }");
        setFixedWidth(master ? 104 : 84);

        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(8, 8, 8, 8);
        v->setSpacing(6);

        name_ = new QLabel(this);
        QFont nf = font();
        nf.setPixelSize(12);
        nf.setWeight(QFont::DemiBold);
        name_->setFont(nf);
        name_->setAlignment(Qt::AlignCenter);
        name_->setStyleSheet(master ? "color: #5ce0bd;" : "color: #e8ecf4;");
        name_->setToolTip(name);
        setName(name);
        v->addWidget(name_);

        inserts_ = new InsertList;
        v->addWidget(inserts_);
        latency_ = new QLabel(this);
        latency_->setAlignment(Qt::AlignCenter);
        latency_->setStyleSheet("color: #e0c05c; font-size: 10px;");
        latency_->setFixedHeight(12);
        latency_->setToolTip("Latency this path adds (plugin look-ahead), "
                             "compensated on every path that meets it");
        v->addWidget(latency_);

        fader_ = new FaderMeter(channels, this);
        v->addWidget(fader_, 1, Qt::AlignHCenter);

        readout_ = new QLabel(this);
        readout_->setAlignment(Qt::AlignCenter);
        QFont rf = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        rf.setPixelSize(12);
        readout_->setFont(rf);
        readout_->setStyleSheet("color: #c8cede;");
        v->addWidget(readout_);

        mute_ = new QPushButton("MUTE", this);
        mute_->setCheckable(true);
        mute_->setCursor(Qt::PointingHandCursor);
        mute_->setFixedHeight(26);
        mute_->setStyleSheet(
            "QPushButton { background: #232733; border: 1px solid #323848;"
            " border-radius: 4px; color: #8a92a6; font-size: 11px;"
            " font-weight: 600; letter-spacing: 1px; }"
            "QPushButton:hover { border-color: #46506a; color: #c8cede; }"
            "QPushButton:checked { background: #c2393e; border-color: #e5484d;"
            " color: #ffffff; }");
        v->addWidget(mute_);

        fader_->onLevel = [this](float d) {
            showDb(d);
            if (onChange) onChange(d, mute_->isChecked());
        };
        connect(mute_, &QPushButton::toggled, this, [this](bool on) {
            if (syncing_) return;
            if (onChange) onChange(fader_->db(), on);
        });
        showDb(0);
    }

    void setName(const QString& n) {
        name_->setText(name_->fontMetrics().elidedText(n, Qt::ElideRight,
                                                       width() - 16));
        name_->setToolTip(n);
    }

    // From the daemon (snapshot or another client's move).
    void setLevel(float faderDb, bool mute) {
        fader_->setDb(faderDb);
        showDb(fader_->db());
        syncing_ = true;
        mute_->setChecked(mute);
        syncing_ = false;
    }

    void setMeter(const float peak[2], const float rms[2]) {
        fader_->setMeter(peak, rms);
    }
    InsertList* inserts() { return inserts_; }
    void setLatency(int samples, double rate) {
        latency_->setText(samples > 0 && rate > 0
                              ? QString("+%1 ms").arg(1000.0 * samples / rate, 0, 'f', 1)
                              : QString());
    }
    void clearMeter() { fader_->clearMeter(); }

private:
    void showDb(float d) { readout_->setText(fmtDb(d)); }

    bool master_;
    bool syncing_ = false;
    QLabel* name_;
    QLabel* readout_;
    QLabel* latency_;
    InsertList* inserts_;
    FaderMeter* fader_;
    QPushButton* mute_;
};

}  // namespace pm::ui
