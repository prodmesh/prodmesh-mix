// The top bar: what a live operator glances at all show long.
//
//   identity    product, engine connection, device and format
//   clock       time of day (large); recording elapsed and its health
//   loudness    program loudness per BS.1770 against the show's target:
//               integrated (large), momentary, short-term, true peak
//   marker      drop a marker into the take (also the M key)
//   health      DSP load and dropouts
//
// Colour only for meaning: accent = on target / OK, amber = off target or
// needs attention, red = over the true-peak limit, clipping, dropouts,
// recording. With no engine, every engine-derived number disappears.
#pragma once

#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <functional>

#include "ui/theme.h"

namespace pm::ui {

inline QFont monoFont(int px, QFont::Weight w = QFont::Normal) {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPixelSize(px);
    f.setWeight(w);
    return f;
}

inline QFont uiFont(const QWidget* w, int px, QFont::Weight weight = QFont::Normal,
                    double spacing = 0) {
    QFont f = w->font();
    f.setPixelSize(px);
    f.setWeight(weight);
    if (spacing) f.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    return f;
}

inline QString hms(double seconds) {
    const int s = std::max(0, int(seconds));
    return QString("%1:%2:%3")
        .arg(s / 3600, 2, 10, QChar('0'))
        .arg(s / 60 % 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}

// Time of day, large; the take underneath.
class ClockBlock : public QWidget {
public:
    ClockBlock() {
        setStyleSheet("background: transparent;");
        setFixedWidth(196);
        tick_.setInterval(250);
        connect(&tick_, &QTimer::timeout, this, [this] { update(); });
        tick_.start();
    }
    void setTransport(const QJsonObject& t) {
        transport_ = t;
        update();
    }
    void setRecording(const QJsonObject& r, bool connected) {
        connected_ = connected;
        rec_ = r;
        QStringList tip;
        if (connected && r["on"].toBool()) {
            const double hours = r["diskSecondsLeft"].toDouble() / 3600.0;
            tip << "Recording take " + r["take"].toString()
                << QString("%1 tracks").arg(r["tracks"].toInt())
                << QString("%1 of disk left").arg(hours >= 1 ? QString("%1 h").arg(int(hours))
                                                             : QString("%1 min").arg(int(hours * 60)));
            if (r["droppedFrames"].toDouble() > 0)
                tip << QString("%1 s of gaps (disk too slow)")
                           .arg(r["droppedSeconds"].toDouble(), 0, 'f', 1);
            if (!r["error"].toString().isEmpty()) tip << r["error"].toString();
        }
        setToolTip(tip.join("\n"));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(monoFont(24, QFont::Medium));
        p.setPen(theme::textMax);
        p.drawText(QRect(0, 4, width(), 30), Qt::AlignLeft | Qt::AlignVCenter,
                   QTime::currentTime().toString("HH:mm:ss"));
        const QRect line(0, 36, width(), 18);
        if (connected_ && transport_["mode"].toString() == "playback") {
            // Playback replaces recording: say so where REC would be.
            const bool playing = transport_["playing"].toBool();
            p.setFont(uiFont(this, 11, QFont::DemiBold, 0.5));
            p.setPen(playing ? theme::accentHi : theme::warn);
            p.drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                       QString(playing ? "▶ " : "■ ") + hms(transport_["position"].toDouble()) +
                           "  /  " + hms(transport_["length"].toDouble()));
            return;
        }
        if (!connected_ || !rec_["on"].toBool()) {
            p.setFont(uiFont(this, 11));
            p.setPen(theme::text);
            p.drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                       connected_ ? "Not recording" : "");
            return;
        }
        // "● REC 01:23:45" — red is the universal record colour; the
        // detail turns amber when the disk or the writer needs attention.
        const bool attention = rec_["diskSecondsLeft"].toDouble() < 7200 ||
                               rec_["droppedFrames"].toDouble() > 0 ||
                               rec_["ringFill"].toDouble() > 0.5 ||
                               !rec_["error"].toString().isEmpty();
        p.setPen(Qt::NoPen);
        p.setBrush(theme::clip);
        p.drawEllipse(QPointF(5, line.center().y() + 1), 4, 4);
        p.setFont(uiFont(this, 11, QFont::DemiBold, 0.5));
        p.setPen(QColor("#ffd9db"));
        p.drawText(line.adjusted(14, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   "REC " + hms(rec_["seconds"].toDouble()));
        if (attention) {
            p.setPen(theme::warn);
            p.drawText(line.adjusted(112, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                       "check disk");
        }
    }

private:
    QTimer tick_;
    QJsonObject rec_, transport_;
    bool connected_ = false;
};

// Program loudness against the show's target. Click for targets / reset.
class LoudnessBlock : public QWidget {
public:
    std::function<void(QPoint globalPos)> onClick;

    LoudnessBlock() {
        setStyleSheet("background: transparent;");
        setFixedWidth(330);
        setCursor(Qt::PointingHandCursor);
    }
    void setLoudness(const QJsonObject& m) {
        l_ = m;
        valid_ = true;
        const double target = m["target"].toDouble(), limit = m["tpLimit"].toDouble();
        setToolTip(QString("Program loudness (ITU-R BS.1770-4), measured for %1.\n"
                           "Target %2 LUFS, true peak limit %3 dBTP.\n"
                           "Integrated: the whole show so far (gated). Momentary: 400 ms. "
                           "Short-term: 3 s.\nClick for targets, or to reset the measurement.")
                       .arg(hms(m["seconds"].toDouble()))
                       .arg(target, 0, 'f', 0)
                       .arg(limit, 0, 'f', 0));
        update();
    }
    void clear() {
        valid_ = false;
        update();
    }

protected:
    static QString num(double v) {
        if (v <= -119) return QStringLiteral("—");
        QString s = QString::number(v, 'f', 1);
        return s.replace('-', QChar(0x2212));
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(underMouse() ? QColor("#1d2129") : QColor("#191c23"));
        p.drawRoundedRect(rect().adjusted(0, 4, 0, -4), 6, 6);
        if (!valid_) {
            p.setFont(uiFont(this, 11));
            p.setPen(theme::text);
            p.drawText(rect(), Qt::AlignCenter, "Loudness — no engine");
            return;
        }
        const double target = l_["target"].toDouble(), limit = l_["tpLimit"].toDouble();
        const double I = l_["i"].toDouble(-120), M = l_["m"].toDouble(-120),
                     S = l_["s"].toDouble(-120), tpMax = l_["tpMax"].toDouble(-120);
        const bool measured = I > -119;
        const double delta = I - target;
        // Within 1 LU: on target. Beyond 2 LU: amber (A/85's tolerance).
        const QColor iColor = !measured ? theme::text
                              : std::fabs(delta) <= 1 ? theme::accentHi
                              : std::fabs(delta) <= 2 ? theme::textMax
                                                      : theme::warn;
        // Integrated, large.
        p.setFont(monoFont(24, QFont::Medium));
        p.setPen(iColor);
        p.drawText(QRect(12, 6, 100, 30), Qt::AlignLeft | Qt::AlignVCenter, num(I));
        p.setFont(uiFont(this, 8, QFont::DemiBold, 1));
        p.setPen(theme::text);
        p.drawText(QRect(12, 36, 110, 12), Qt::AlignLeft | Qt::AlignVCenter, "LUFS INTEGRATED");
        // Target and distance from it.
        p.setFont(uiFont(this, 10));
        p.drawText(QRect(12, 47, 110, 12), Qt::AlignLeft | Qt::AlignVCenter,
                   measured ? QString("target %1 · %2%3 LU")
                                  .arg(num(target))
                                  .arg(delta >= 0 ? "+" : "")
                                  .arg(num(delta))
                            : QString("target %1").arg(num(target)));

        // Momentary / short-term / true peak.
        const int x = 128;
        auto pair = [&](int y, const QString& k, const QString& v, const QColor& c) {
            p.setFont(uiFont(this, 9, QFont::DemiBold, 0.8));
            p.setPen(theme::text);
            p.drawText(QRect(x, y, 30, 14), Qt::AlignLeft | Qt::AlignVCenter, k);
            p.setFont(monoFont(12));
            p.setPen(c);
            p.drawText(QRect(x + 24, y, 60, 14), Qt::AlignLeft | Qt::AlignVCenter, v);
        };
        pair(9, "M", num(M), theme::textHi);
        pair(25, "S", num(S), theme::textHi);
        const bool over = tpMax > limit;
        p.setFont(uiFont(this, 9, QFont::DemiBold, 0.8));
        p.setPen(theme::text);
        p.drawText(QRect(x + 84, 9, 30, 14), Qt::AlignLeft | Qt::AlignVCenter, "TP");
        p.setFont(monoFont(12));
        p.setPen(over ? theme::clip : theme::textHi);
        p.drawText(QRect(x + 106, 9, 70, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   num(tpMax));
        p.setFont(uiFont(this, 9));
        p.setPen(over ? theme::clip : theme::text);
        p.drawText(QRect(x + 84, 25, 110, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   over ? QString("over %1 dBTP").arg(num(limit))
                        : QString("max · limit %1").arg(num(limit)));

        // Short-term bar against the target: target-18 .. target+9 LU.
        const QRectF bar(x, 45, width() - x - 12, 6);
        const double lo = target - 18, hi = target + 9;
        auto px = [&](double v) {
            return bar.left() + bar.width() * std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
        };
        p.setPen(Qt::NoPen);
        p.setBrush(theme::control);
        p.drawRoundedRect(bar, 3, 3);
        if (S > lo) {
            const double ds = S - target;
            p.setBrush(std::fabs(ds) <= 2 ? theme::accent : ds > 0 ? theme::warn
                                                                   : theme::faint);
            p.drawRoundedRect(QRectF(bar.left(), bar.top(), px(S) - bar.left(), bar.height()),
                              3, 3);
        }
        if (M > lo) {  // momentary: a tick riding on the bar
            p.setBrush(theme::textMax);
            p.drawRect(QRectF(px(M) - 1, bar.top() - 2, 2, bar.height() + 4));
        }
        p.setBrush(theme::textHi);  // the target
        p.drawRect(QRectF(px(target) - 0.5, bar.top() - 4, 1, bar.height() + 8));
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (onClick) onClick(e->globalPosition().toPoint());
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    QJsonObject l_;
    bool valid_ = false;
};

// The whole top bar. Identity is painted on the left, health on the right;
// the clock, loudness and marker blocks sit between them.
class TopBar : public QWidget {
public:
    std::function<void(QPoint)> onLoudnessMenu;
    std::function<void()> onMarker;

    TopBar() {
        setFixedHeight(64);
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(kLeft, 0, kRight, 0);
        h->setSpacing(20);
        clock_ = new ClockBlock;
        loud_ = new LoudnessBlock;
        marker_ = new QPushButton("◆  Marker");
        marker_->setCursor(Qt::PointingHandCursor);
        marker_->setFixedHeight(32);
        marker_->setToolTip("Drop a marker into the take, for post (M)");
        marker_->setStyleSheet(
            "QPushButton { background: #1d2129; border: 1px solid #323848; border-radius: 6px;"
            " color: #c8cede; font-size: 12px; font-weight: 600; padding: 0 14px; }"
            "QPushButton:hover { border-color: #46506a; color: #e8ecf4; }"
            "QPushButton:disabled { color: #5a6172; }");
        h->addWidget(clock_);
        h->addWidget(loud_);
        h->addWidget(marker_);
        h->addStretch(1);
        loud_->onClick = [this](QPoint at) { if (onLoudnessMenu) onLoudnessMenu(at); };
        connect(marker_, &QPushButton::clicked, this, [this] { if (onMarker) onMarker(); });
        flash_.setSingleShot(true);
        connect(&flash_, &QTimer::timeout, this, [this] { refreshMarker(); });
        setConnected(false);
    }

    void setConnected(bool c) {
        connected_ = c;
        if (!c) loud_->clear();
        clock_->setRecording(rec_, c);
        marker_->setEnabled(c);
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
        rec_ = h["recording"].toObject();
        clock_->setRecording(rec_, connected_);
        update();
    }
    void setLoudness(const QJsonObject& m) {
        if (connected_) loud_->setLoudness(m);
    }
    void setTransport(const QJsonObject& t) { clock_->setTransport(t); }
    // The newest marker flashes on the button for a moment.
    void setMarkers(int count, const QString& newest) {
        const bool added = count > markers_;
        markers_ = count;
        if (added && !newest.isEmpty()) {
            marker_->setText("◆  " + newest);
            flash_.start(1500);
        } else {
            refreshMarker();
        }
    }

protected:
    static constexpr int kLeft = 300, kRight = 230;

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), theme::bg);
        p.setPen(theme::grid);
        p.drawLine(0, height() - 1, width(), height() - 1);

        // Identity: product, connection pill; device and format beneath.
        p.setFont(uiFont(this, 14, QFont::DemiBold));
        p.setPen(theme::textMax);
        p.drawText(QRect(16, 8, 120, 24), Qt::AlignVCenter, "ProdMesh Mix");
        p.setFont(uiFont(this, 11, QFont::Medium));
        const QString state = connected_ ? "Engine connected" : "Engine not reachable";
        const QColor col = connected_ ? theme::accent : theme::clip;
        const int pw = p.fontMetrics().horizontalAdvance(state) + 30;
        const QRect pill(126, 10, pw, 20);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(col.red(), col.green(), col.blue(), 36));
        p.drawRoundedRect(pill, 10, 10);
        p.setBrush(col);
        p.drawEllipse(QPoint(pill.left() + 11, pill.center().y() + 1), 4, 4);
        p.setPen(col);
        p.drawText(pill.adjusted(20, 0, 0, 0), Qt::AlignVCenter, state);
        if (connected_) {
            p.setFont(uiFont(this, 11));
            p.setPen(theme::text);
            p.drawText(QRect(16, 36, kLeft - 36, 18), Qt::AlignVCenter,
                       p.fontMetrics().elidedText(format_, Qt::ElideMiddle, kLeft - 36));
        }

        // Health: only while it is live.
        if (!connected_) return;
        const int right = width() - 16;
        p.setFont(uiFont(this, 11, QFont::Medium));
        const QString load = QString("DSP %1%  peak %2%")
                                 .arg(int(std::round(meanLoad_ * 100)))
                                 .arg(int(std::round(worstLoad_ * 100)));
        p.setPen(theme::textHi);
        p.drawText(QRect(right - 200, 10, 200, 18), Qt::AlignRight | Qt::AlignVCenter, load);
        const QRect bar(right - 200, 32, 200, 6);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::control);
        p.drawRoundedRect(bar, 3, 3);
        p.setBrush(worstLoad_ > 0.7 ? theme::clip : worstLoad_ > 0.5 ? theme::warn
                                                                     : theme::accent);
        p.drawRoundedRect(QRect(bar.left(), bar.top(),
                                int(std::min(1.0, meanLoad_) * bar.width()), bar.height()),
                          3, 3);
        p.setBrush(theme::textMax);
        p.drawRect(QRect(bar.left() + int(std::min(1.0, worstLoad_) * bar.width()) - 1,
                         bar.top() - 2, 2, bar.height() + 4));
        QString status = xruns_ == 1 ? "1 dropout" : QString("%1 dropouts").arg(xruns_);
        if (rtViolations_ > 0) status += QString(" · %1 RT violations").arg(rtViolations_);
        p.setPen(xruns_ || rtViolations_ ? theme::clip : theme::text);
        p.drawText(QRect(right - 200, 42, 200, 16), Qt::AlignRight | Qt::AlignVCenter, status);
    }

private:
    void refreshMarker() {
        marker_->setText(markers_ ? QString("◆  Marker  ·  %1").arg(markers_)
                                  : QStringLiteral("◆  Marker"));
    }

    ClockBlock* clock_;
    LoudnessBlock* loud_;
    QPushButton* marker_;
    QTimer flash_;
    QJsonObject rec_;
    QString format_;
    bool connected_ = false;
    int markers_ = 0;
    double meanLoad_ = 0, worstLoad_ = 0;
    qint64 xruns_ = 0, rtViolations_ = 0;
};

}  // namespace pm::ui
