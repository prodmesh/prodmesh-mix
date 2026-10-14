// Mixer widgets: a channel strip with a custom-painted meter and fader.
//
// Interaction rules (console conventions, not DAW quirks):
//   - dragging moves the fader relative to where you grabbed it; clicking
//     the track never jumps it (no accidental level changes on air)
//   - Option/Alt while dragging or scrolling = fine (x0.1)
//   - Option-click resets any control to its zero (fader/gain/send 0 dB,
//     pan centre); an Option-press that moves is a fine drag instead.
//     Double-click does the same. Scroll = 0.5 dB per notch
//   - the clip light latches until clicked
#pragma once

#include <QContextMenuEvent>
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

// Option-click = reset. Armed on an Option-press, disarmed once the
// pointer moves (then it is a fine drag), fired on release.
struct AltClick {
    bool armed = false;
    QPointF at;
    void press(QMouseEvent* e) {
        armed = e->modifiers() & Qt::AltModifier;
        at = e->position();
    }
    // True while a would-be click has not moved yet (ignore the move).
    bool holding(QMouseEvent* e) {
        if (!armed) return false;
        if ((e->position() - at).manhattanLength() < 3) return true;
        armed = false;
        return false;
    }
    bool release() {
        const bool fire = armed;
        armed = false;
        return fire;
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
        alt_.press(e);  // Option-click anywhere on the fader = 0 dB
        if (capRect().adjusted(-6, -4, 6, 4).contains(e->pos())) {
            dragging_ = true;
            grabY_ = e->position().y();
            grabPos_ = FaderLaw::toPos(db_);
            update();
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (alt_.holding(e) || !dragging_) return;
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
        if (alt_.release()) commit(0);
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
    AltClick alt_;
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
    // Drag a slot up or down: `from` ends up at index `to`.
    std::function<void(int from, int to)> onMove;

    InsertList() {
        setStyleSheet("background: transparent;");
        setFixedHeight(kRows * (kRowH + kGap) - kGap);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }
    void setInserts(const QStringList& names, const QList<bool>& inEngine = {}) {
        names_ = names;
        inEngine_ = inEngine;
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
            QRect r = row(i);
            if (i == drag_) {  // the dragged slot follows the hand
                p.setOpacity(0.35);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#262b38"));
                p.drawRoundedRect(r, 4, 4);
                p.setOpacity(1);
                r.moveTop(std::clamp(dragY_ - grabOffset_, 0, height() - kRowH));
            }
            p.setPen(Qt::NoPen);
            p.setBrush(i == hover_ ? QColor("#2c3241") : QColor("#262b38"));
            p.drawRoundedRect(r, 4, 4);
            p.setBrush(notes_.contains(i) ? theme::warn : theme::accent);
            p.drawRoundedRect(QRect(r.left(), r.top(), 3, r.height()), 1.5, 1.5);
            // In-engine (not isolated) plugins are the exception: a dot.
            const bool inEngine = inEngine_.value(i, false);
            if (inEngine) {
                p.setPen(Qt::NoPen);
                p.setBrush(theme::text);
                p.drawEllipse(QPoint(r.right() - 7, r.center().y() + 1), 2, 2);
            }
            p.setPen(theme::textHi);
            p.drawText(r.adjusted(8, 0, inEngine ? -14 : -4, 0), Qt::AlignVCenter,
                       p.fontMetrics().elidedText(names_[i], Qt::ElideRight,
                                                  r.width() - (inEngine ? 22 : 12)));
        }
        if (drag_ >= 0 && dropIndex() != drag_) {  // where it will land
            const int at = dropIndex();
            const QRect t = row(at);
            const int y = at < drag_ ? t.top() - 1 : t.bottom() + 1;
            p.setPen(Qt::NoPen);
            p.setBrush(theme::accent);
            p.drawRoundedRect(QRect(0, std::max(0, y - 1), width(), 2), 1, 1);
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

    // Where the dragged slot lands: the slot under its centre.
    int dropIndex() const {
        const int shown = std::min(int(names_.size()), kRows - 1);
        const int centre = dragY_ - grabOffset_ + kRowH / 2;
        return std::clamp(centre / (kRowH + kGap), 0, shown - 1);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (press_ >= 0 && press_ < addRow() && drag_ < 0 &&
            std::abs(e->pos().y() - pressPos_.y()) > 4 && onMove) {
            drag_ = press_;
            setCursor(Qt::ClosedHandCursor);
        }
        if (drag_ >= 0) {
            dragY_ = e->pos().y();
            update();
            return;
        }
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
                                       notes_.value(i, names_[i]) +
                                           "\nClick for options; drag to reorder.",
                                       this);
                    return true;
                }
            QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }
    // A click (press + release without moving) opens; a drag reorders.
    void mousePressEvent(QMouseEvent* e) override {
        press_ = -1;
        for (int i = 0; i <= addRow(); ++i)
            if (row(i).contains(e->pos())) press_ = i;
        pressPos_ = e->pos();
        if (press_ >= 0) grabOffset_ = e->pos().y() - row(press_).top();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        const QPoint g = e->globalPosition().toPoint();
        if (drag_ >= 0) {
            const int from = drag_, to = dropIndex();
            drag_ = press_ = -1;
            setCursor(Qt::PointingHandCursor);
            update();
            if (to != from && onMove) onMove(from, to);
            return;
        }
        const int i = press_;
        press_ = -1;
        if (i < 0 || !row(i).contains(e->pos())) return;
        if (i == addRow()) {
            if (onAdd) onAdd(g);
        } else if (onSlot) {
            onSlot(i, g);
        }
    }

private:
    QStringList names_;
    QList<bool> inEngine_;
    QMap<int, QString> notes_;
    int hover_ = -1;
    int press_ = -1, drag_ = -1, dragY_ = 0, grabOffset_ = 0;
    QPoint pressPos_;
};

// Input gain (trim) before any processing: a compact drag field.
// Drag up/down (Option = fine), scroll, double-click = 0 dB.
class GainField : public QWidget {
public:
    std::function<void(float)> onGain;
    static constexpr float kMin = -20, kMax = 40;

    GainField() {
        setFixedHeight(20);
        setCursor(Qt::SizeVerCursor);
        setToolTip("Input gain — before EQ, dynamics and inserts.\n"
                   "Drag up/down (hold Option for fine); Option-click for 0 dB.");
    }
    void setGain(float dB) {
        if (dragging_) return;
        gain_ = dB;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(dragging_ ? QColor("#2c3241") : QColor("#191c23"));
        p.drawRoundedRect(rect(), 4, 4);
        // A thin bar shows where in the range the gain sits.
        const float f = (gain_ - kMin) / (kMax - kMin);
        p.setBrush(QColor(47, 191, 155, 60));
        p.drawRoundedRect(QRectF(0, height() - 3, width() * f, 3), 1.5, 1.5);
        QFont lf = font();
        lf.setPixelSize(8);
        lf.setWeight(QFont::DemiBold);
        lf.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        p.setFont(lf);
        p.setPen(theme::text);
        p.drawText(rect().adjusted(5, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, "GAIN");
        QFont v = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        v.setPixelSize(10);
        p.setFont(v);
        p.setPen(gain_ != 0.0f ? theme::textMax : QColor("#5a6172"));
        p.drawText(rect().adjusted(0, 0, -5, 0), Qt::AlignVCenter | Qt::AlignRight,
                   (gain_ > 0.05f ? "+" : "") + QString::number(double(gain_), 'f', 1));
    }
    void mousePressEvent(QMouseEvent* e) override {
        alt_.press(e);
        dragging_ = true;
        grabY_ = e->position().y();
        grabGain_ = gain_;
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (alt_.holding(e) || !dragging_) return;
        const float perPx = e->modifiers() & Qt::AltModifier ? 0.02f : 0.2f;
        if (perPx != lastPerPx_) {  // re-anchor when switching fine/coarse
            grabY_ = e->position().y();
            grabGain_ = gain_;
            lastPerPx_ = perPx;
            return;
        }
        commit(grabGain_ + float(grabY_ - e->position().y()) * perPx);
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        dragging_ = false;
        lastPerPx_ = 0.2f;
        if (alt_.release()) commit(0);
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { commit(0); }
    void wheelEvent(QWheelEvent* e) override {
        const int notches = e->angleDelta().y() / 120;
        if (notches) commit(gain_ + 0.5f * float(notches));
    }

private:
    AltClick alt_;
    void commit(float dB) {
        dB = std::clamp(std::round(dB * 10.0f) / 10.0f, kMin, kMax);
        if (dB == gain_) return;
        gain_ = dB;
        update();
        if (onGain) onGain(dB);
    }
    float gain_ = 0, grabGain_ = 0, lastPerPx_ = 0.2f;
    double grabY_ = 0;
    bool dragging_ = false;
};

// "DLY 3115" / "CMP 64": a quiet label and a value that only takes colour
// when it is non-zero.
class DelayCell : public QWidget {
public:
    explicit DelayCell(const QString& label) : label_(label) {
        setFixedHeight(16);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setValue(int samples, const QColor& active, const QString& tip) {
        if (samples == value_ && tip == toolTip()) return;
        value_ = samples;
        active_ = active;
        setToolTip(tip);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#191c23"));
        p.drawRoundedRect(rect(), 3, 3);
        QFont f = font();
        f.setPixelSize(9);
        f.setWeight(QFont::DemiBold);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        p.setFont(f);
        p.setPen(theme::text);
        p.drawText(rect().adjusted(6, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   label_);
        QFont v = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        v.setPixelSize(11);
        p.setFont(v);
        p.setPen(value_ ? active_ : QColor("#5a6172"));
        p.drawText(rect().adjusted(0, 0, -6, 0), Qt::AlignVCenter | Qt::AlignRight,
                   QString::number(value_));
    }

private:
    QString label_;
    int value_ = 0;
    QColor active_;
};

// The strip's sends, as slots like the inserts (fixed height, so strips
// line up). A send goes to a bus or straight to an output. Each row:
//   dot    click = on/off ("−" = left out of this mix-minus, click to put
//          it back)
//   rest   click = menu (pre/post, reset, remove), drag up/down = level
//          (Option = fine), Option-click = 0 dB (or back to automatic on a
//          mix-minus)
// The last row adds a send.
class SendList : public QWidget {
public:
    static constexpr int kRows = 4, kRowH = 18, kGap = 2, kDotW = 16;
    enum class State { On, Muted, Auto, Minus };
    struct Row {
        quint32 dest = 0;
        QString label;
        State state = State::On;
        float db = 0;
        bool pre = false;  // pre-fader
        QString tip;
    };
    std::function<void(quint32 dest)> onToggle;
    std::function<void(quint32 dest, float db)> onLevel;
    std::function<void(quint32 dest)> onReset;
    std::function<void(quint32 dest, QPoint globalPos)> onMenu;
    std::function<void(QPoint globalPos)> onAdd;

    SendList() {
        setStyleSheet("background: transparent;");
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(kRows * (kRowH + kGap) - kGap);
    }
    void setRows(const QList<Row>& rows) {
        if (dragRow_ >= 0) return;  // never fight the operator's hand
        rows_ = rows;
        update();
    }

protected:
    QRect row(int i) const { return QRect(0, i * (kRowH + kGap), width(), kRowH); }
    int shown() const { return std::min(int(rows_.size()), kRows - 1); }
    int addRow() const { return shown(); }
    int rowAt(QPoint p) const {
        for (int i = 0; i <= addRow(); ++i)
            if (row(i).contains(p)) return i;
        return -1;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QFont f = font();
        f.setPixelSize(10);
        QFont v = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        v.setPixelSize(10);
        for (int i = 0; i < shown(); ++i) {
            const auto& r = rows_[i];
            const QRect rr = row(i);
            p.setPen(Qt::NoPen);
            p.setBrush(i == hover_ || i == dragRow_ ? QColor("#20242e")
                                                    : QColor("#191c23"));
            p.drawRoundedRect(rr, 4, 4);
            const bool sending = r.state == State::On || r.state == State::Auto;
            if (r.state != State::Minus) {  // level bar on the fader law
                p.setBrush(sending ? QColor(47, 191, 155, 60) : QColor(138, 146, 166, 40));
                const float pos = FaderLaw::toPos(r.db);
                p.drawRoundedRect(QRectF(kDotW, rr.bottom() - 2,
                                         (rr.width() - kDotW) * pos, 3), 1.5, 1.5);
            }
            const QPointF c(rr.left() + kDotW / 2.0 + 1, rr.center().y() + 0.5);
            if (r.state == State::Minus) {
                p.setPen(QPen(theme::textHi, 1.6));
                p.drawLine(QPointF(c.x() - 3.5, c.y()), QPointF(c.x() + 3.5, c.y()));
            } else if (sending) {
                p.setBrush(theme::accent);
                p.drawEllipse(c, 3.5, 3.5);
            } else {
                p.setPen(QPen(theme::text, 1.2));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(c, 3.5, 3.5);
            }
            // Only a level the operator set is a number; the dot and the
            // bar say "automatic" or "left out" (the tooltip spells it out).
            // A pre-fader send shows its tag instead (the bar still shows the
            // level; the number comes back while dragging and in the tooltip).
            const QString value =
                (r.state == State::On || r.state == State::Muted) &&
                        (!r.pre || i == dragRow_)
                    ? fmtDb(r.db) : QString();
            int vw = value.isEmpty() ? 0 : QFontMetrics(v).horizontalAdvance(value) + 5;
            if (r.pre) {  // "PRE" tag left of the value
                QFont t = font();
                t.setPixelSize(7);
                t.setWeight(QFont::Bold);
                const int tw = QFontMetrics(t).horizontalAdvance("PRE") + 6;
                const QRect tag(rr.right() - vw - tw - 1, rr.top() + 4, tw, rr.height() - 8);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#2c3241"));
                p.drawRoundedRect(tag, 2, 2);
                p.setFont(t);
                p.setPen(theme::textHi);
                p.drawText(tag, Qt::AlignCenter, "PRE");
                vw += tw + 3;
            }
            p.setFont(f);
            p.setPen(sending ? theme::textHi : theme::text);
            p.drawText(rr.adjusted(kDotW + 2, 0, -vw - 2, 0),
                       Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(r.label, Qt::ElideRight,
                                                  rr.width() - kDotW - vw - 4));
            p.setFont(v);
            p.setPen(r.state == State::On ? theme::textMax : theme::text);
            p.drawText(rr.adjusted(0, 0, -5, 0), Qt::AlignVCenter | Qt::AlignRight,
                       value);
        }
        const QRect add = row(addRow());
        const bool hot = hover_ == addRow();
        p.setPen(QPen(hot ? theme::borderHi : theme::border, 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(add).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        p.setFont(f);
        p.setPen(hot ? theme::textHi : theme::text);
        const int more = int(rows_.size()) - shown();
        p.drawText(add, Qt::AlignCenter,
                   more > 0 ? QString("+%1 more").arg(more) : QStringLiteral("+ Send"));
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(e);
            const int i = rowAt(he->pos());
            if (i >= 0 && i < shown())
                QToolTip::showText(he->globalPos(), rows_[i].tip, this);
            else if (i == addRow())
                QToolTip::showText(he->globalPos(),
                                   "Send this channel to a bus or an output "
                                   "(monitor mixes, recorders, remote feeds).", this);
            else
                QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragRow_ >= 0 && !moved_ && std::abs(e->position().y() - grabY_) < 3)
            return;  // still a click
        alt_.holding(e);  // moved: an Option-drag is a fine drag
        if (dragRow_ >= 0) {
            if (!moved_) {
                moved_ = true;
                grabY_ = e->position().y();
            }
            const float perPx = e->modifiers() & Qt::AltModifier ? 0.025f : 0.25f;
            if (perPx != lastPerPx_) {  // re-anchor when switching fine/coarse
                grabY_ = e->position().y();
                grabDb_ = rows_[dragRow_].db;
                lastPerPx_ = perPx;
                return;
            }
            float db = grabDb_ + float(grabY_ - e->position().y()) * perPx;
            db = std::clamp(std::round(db * 10.0f) / 10.0f, -60.0f, 10.0f);
            auto& r = rows_[dragRow_];
            if (db == r.db && r.state != State::Auto) return;
            r.db = db;
            if (r.state == State::Auto) r.state = State::On;
            update();
            if (onLevel) onLevel(r.dest, db);
            return;
        }
        const int h = rowAt(e->pos());
        setCursor(h < 0 ? Qt::ArrowCursor
                  : h == addRow() || e->pos().x() < kDotW + 2 ? Qt::PointingHandCursor
                                                               : Qt::SizeVerCursor);
        if (h != hover_) {
            hover_ = h;
            update();
        }
    }
    void leaveEvent(QEvent*) override {
        hover_ = -1;
        update();
    }
    void mousePressEvent(QMouseEvent* e) override {
        const int i = rowAt(e->pos());
        if (i < 0) return;
        const QPoint g = e->globalPosition().toPoint();
        if (i == addRow()) {
            if (e->button() == Qt::LeftButton && onAdd) onAdd(g);
            return;
        }
        const auto& r = rows_[i];
        if (e->button() == Qt::RightButton) {
            if (onMenu) onMenu(r.dest, g);
            return;
        }
        if (e->pos().x() < kDotW + 2) {
            if (onToggle) onToggle(r.dest);
            return;
        }
        pressRow_ = i;
        moved_ = false;
        alt_.press(e);
        grabY_ = e->position().y();
        grabDb_ = r.db;
        lastPerPx_ = 0.25f;
        if (r.state != State::Minus) dragRow_ = i;
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        const int i = pressRow_;
        const bool click = !moved_;
        const bool reset = alt_.release() && click;
        dragRow_ = pressRow_ = -1;
        update();
        if (i < 0 || i >= int(rows_.size()) || !click) return;
        if (reset) {
            if (onReset) onReset(rows_[i].dest);
        } else if (onMenu) {
            onMenu(rows_[i].dest, e->globalPosition().toPoint());
        }
    }

private:
    QList<Row> rows_;
    int hover_ = -1, dragRow_ = -1, pressRow_ = -1;
    bool moved_ = false;
    AltClick alt_;
    double grabY_ = 0;
    float grabDb_ = 0, lastPerPx_ = 0.25f;
};

// Pan (mono channels) or balance (stereo): drag left/right (Option =
// fine), scroll, double-click = centre.
class PanField : public QWidget {
public:
    std::function<void(float)> onPan;  // -1 left .. +1 right

    PanField() {
        setFixedHeight(18);
        setCursor(Qt::SizeHorCursor);
        setToolTip("Pan (balance on a stereo channel).\nDrag left/right (hold "
                   "Option for fine); Option-click to centre.");
    }
    void setPan(float p) {
        if (dragging_ || p == pan_) return;
        pan_ = p;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(dragging_ ? QColor("#2c3241") : QColor("#191c23"));
        p.drawRoundedRect(rect(), 4, 4);
        // Centre tick and a bar from centre to the position.
        const float cx = width() / 2.0f;
        const float x = cx + pan_ * (width() / 2.0f - 3);
        p.setBrush(QColor(47, 191, 155, 70));
        p.drawRect(QRectF(std::min(cx, x), height() - 3, std::fabs(x - cx), 3));
        p.setBrush(theme::border);
        p.drawRect(QRectF(cx - 0.5, height() - 5, 1, 5));
        p.setBrush(pan_ == 0 ? theme::text : theme::accent);
        p.drawRoundedRect(QRectF(x - 1.5, height() - 5, 3, 5), 1, 1);
        QFont lf = font();
        lf.setPixelSize(8);
        lf.setWeight(QFont::DemiBold);
        lf.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        p.setFont(lf);
        p.setPen(theme::text);
        p.drawText(rect().adjusted(5, -2, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, "PAN");
        QFont v = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        v.setPixelSize(10);
        p.setFont(v);
        p.setPen(pan_ != 0.0f ? theme::textMax : QColor("#5a6172"));
        p.drawText(rect().adjusted(0, -2, -5, 0), Qt::AlignVCenter | Qt::AlignRight,
                   text(pan_));
    }
    static QString text(float p) {
        const int pct = int(std::round(std::fabs(p) * 100));
        if (pct == 0) return "C";
        return (p < 0 ? "L" : "R") + QString::number(pct);
    }
    void mousePressEvent(QMouseEvent* e) override {
        alt_.press(e);
        dragging_ = true;
        grabX_ = e->position().x();
        grabPan_ = pan_;
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (alt_.holding(e) || !dragging_) return;
        const float perPx = e->modifiers() & Qt::AltModifier ? 0.002f : 0.02f;
        if (perPx != lastPerPx_) {
            grabX_ = e->position().x();
            grabPan_ = pan_;
            lastPerPx_ = perPx;
            return;
        }
        commit(grabPan_ + float(e->position().x() - grabX_) * perPx);
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        dragging_ = false;
        lastPerPx_ = 0.02f;
        if (alt_.release()) commit(0);
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { commit(0); }
    void wheelEvent(QWheelEvent* e) override {
        const int notches = e->angleDelta().y() / 120;
        if (notches) commit(pan_ + 0.05f * float(notches));
    }

private:
    void commit(float p) {
        p = std::clamp(std::round(p * 100.0f) / 100.0f, -1.0f, 1.0f);
        if (p == pan_) return;
        pan_ = p;
        update();
        if (onPan) onPan(p);
    }
    float pan_ = 0, grabPan_ = 0, lastPerPx_ = 0.02f;
    double grabX_ = 0;
    bool dragging_ = false;
    AltClick alt_;
};

// A routing button: "→ Program" (where the strip goes) or "In 3" (what
// feeds it). Click for the menu. An empty main output is amber: a channel
// that reaches nothing is almost always a mistake on air.
class AssignButton : public QWidget {
public:
    std::function<void(QPoint globalPos)> onClick;
    AssignButton(QString prefix, QString empty, bool emptyWarns)
        : prefix_(std::move(prefix)), empty_(std::move(empty)), warn_(emptyWarns) {
        setFixedHeight(20);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }
    void setText(const QString& t, const QString& tip) {
        if (t == text_ && tip == toolTip()) return;
        text_ = t;
        setToolTip(tip);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(underMouse() ? theme::borderHi : theme::border, 1));
        p.setBrush(QColor("#191c23"));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        QFont f = font();
        f.setPixelSize(10);
        f.setWeight(QFont::Medium);
        p.setFont(f);
        const bool none = text_.isEmpty();
        p.setPen(none ? (warn_ ? theme::warn : theme::text)
                      : underMouse() ? theme::textMax : theme::textHi);
        p.drawText(rect().adjusted(4, 0, -4, 0), Qt::AlignCenter,
                   p.fontMetrics().elidedText(none ? empty_ : prefix_ + text_,
                                              Qt::ElideRight, width() - 8));
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
    void mousePressEvent(QMouseEvent* e) override {
        if (onClick) onClick(mapToGlobal(QPoint(0, height())));
        e->accept();
    }

private:
    QString prefix_, empty_;
    bool warn_;
    QString text_ = "-";
};

class StripWidget : public QWidget {
public:
    std::function<void(float faderDb, bool mute)> onChange;
    std::function<void(QPoint globalPos)> onContext;  // right-click

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
        name_->setCursor(Qt::PointingHandCursor);
        name_->installEventFilter(this);
        setName(name);
        v->addWidget(name_);
        caption_ = new QLabel(this);
        caption_->setAlignment(Qt::AlignCenter);
        caption_->setStyleSheet("color: #8a92a6; font-size: 9px;");
        caption_->setFixedHeight(11);
        v->addWidget(caption_);

        input_ = new AssignButton("", "No input", false);
        v->addWidget(input_);
        gain_ = new GainField;
        v->addWidget(gain_);
        inserts_ = new InsertList;
        v->addWidget(inserts_);
        sends_ = new SendList;
        v->addWidget(sends_);
        pan_ = new PanField;
        v->addWidget(pan_);

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

        // Pro Tools-style delay view: what this chain adds, and how far this
        // channel is held back to stay in phase with others.
        auto* delays = new QVBoxLayout;
        delays->setSpacing(3);
        dly_ = new DelayCell("DLY");
        cmp_ = new DelayCell("CMP");
        delays->addWidget(dly_);
        delays->addWidget(cmp_);
        v->addLayout(delays);
        assign_ = new AssignButton("→ ", "→ nowhere", true);
        v->addWidget(assign_);

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
        name_->setToolTip(n + "\nClick to rename, delete, or make a mix-minus.");
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
    SendList* sends() { return sends_; }
    AssignButton* assign() { return assign_; }
    AssignButton* input() { return input_; }
    PanField* pan() { return pan_; }
    void setHasPan(bool on) {
        auto sp = pan_->sizePolicy();
        sp.setRetainSizeWhenHidden(true);
        pan_->setSizePolicy(sp);
        pan_->setVisible(on);
    }
    // A second line under the name ("minus Remote 1" on a mix-minus).
    void setCaption(const QString& c, const QString& tip = {}) {
        caption_->setText(caption_->fontMetrics().elidedText(
            c, Qt::ElideRight, width() - 16));
        caption_->setToolTip(tip.isEmpty() ? c : tip);
    }
    // Strips fed by a source get a gain field; buses do not.
    GainField* gain() { return gain_; }
    // Channels have an input selector and a gain; buses neither. Hidden
    // widgets keep their space, so faders line up across the console.
    void setInputControls(bool selector, bool gain) {
        for (QWidget* w : {static_cast<QWidget*>(input_), static_cast<QWidget*>(gain_)}) {
            auto sp = w->sizePolicy();
            sp.setRetainSizeWhenHidden(true);
            w->setSizePolicy(sp);
        }
        input_->setVisible(selector);
        gain_->setVisible(gain);
    }
    // chain: this node's own insert latency. comp: (destination name,
    // samples) of the delay applied on each of its outputs. arrival: total
    // latency at this node's output.
    void setDelay(int chain, int arrival,
                  const QList<QPair<QString, int>>& comp, double rate) {
        auto ms = [rate](int n) {
            return rate > 0 ? QString::number(1000.0 * n / rate, 'f', 2) + " ms"
                            : QString();
        };
        dly_->setValue(chain, theme::warn,
                       QString("Plugin delay: this chain adds %1 samples (%2)."
                               "\nTotal latency at this point: %3 samples (%4).")
                           .arg(chain).arg(ms(chain)).arg(arrival).arg(ms(arrival)));
        int most = 0;
        QStringList lines;
        for (const auto& [to, n] : comp) {
            most = std::max(most, n);
            lines << QString("%1: %2 samples (%3)").arg(to).arg(n).arg(ms(n));
        }
        cmp_->setValue(most, theme::accent,
                       most ? "Delay compensation: held back to stay in phase "
                              "with other channels' plugins.\n" + lines.join("\n")
                            : "No delay compensation needed.");
    }
    void clearMeter() { fader_->clearMeter(); }

protected:
    void contextMenuEvent(QContextMenuEvent* e) override {
        if (onContext) onContext(e->globalPos());
    }
    // Clicking the name opens the same menu (not everyone right-clicks).
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == name_ && e->type() == QEvent::MouseButtonRelease && onContext) {
            onContext(name_->mapToGlobal(QPoint(0, name_->height())));
            return true;
        }
        return QWidget::eventFilter(o, e);
    }

private:
    void showDb(float d) { readout_->setText(fmtDb(d)); }

    bool master_;
    bool syncing_ = false;
    QLabel* name_;
    QLabel* caption_;
    SendList* sends_;
    AssignButton* assign_;
    AssignButton* input_;
    PanField* pan_;
    QLabel* readout_;
    DelayCell* dly_;
    DelayCell* cmp_;
    GainField* gain_;
    InsertList* inserts_;
    FaderMeter* fader_;
    QPushButton* mute_;
};

}  // namespace pm::ui
