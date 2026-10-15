// The engine's menu-bar icon (mixd --menu-bar, as the app starts it).
//
// While the engine runs there is always something on screen that says so
// and can stop it — no "ghost audio" after the window is gone. The icon is
// a monochrome template (it follows the menu bar's appearance); a small red
// dot is added while recording, the universal record colour.
#pragma once

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTimer>
#include <functional>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace pm::daemon {

// An accessory process: a menu-bar item, no Dock icon or app menu.
inline void becomeAccessory() {
#if defined(__APPLE__)
    auto* cls = reinterpret_cast<id>(objc_getClass("NSApplication"));
    using Shared = id (*)(id, SEL);
    using SetPolicy = BOOL (*)(id, SEL, long);
    id app = reinterpret_cast<Shared>(objc_msgSend)(cls, sel_registerName("sharedApplication"));
    reinterpret_cast<SetPolicy>(objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 1);
#endif
}
inline void bringToFront() {
#if defined(__APPLE__)
    auto* cls = reinterpret_cast<id>(objc_getClass("NSApplication"));
    using Shared = id (*)(id, SEL);
    using Activate = void (*)(id, SEL, BOOL);
    id app = reinterpret_cast<Shared>(objc_msgSend)(cls, sel_registerName("sharedApplication"));
    reinterpret_cast<Activate>(objc_msgSend)(app, sel_registerName("activateIgnoringOtherApps:"), YES);
#endif
}

class EngineTray : public QObject {
public:
    // status(): {"project", "recording", "seconds", "xruns", "device"}.
    std::function<QJsonObject()> status;
    std::function<void()> onQuit;

    EngineTray() {
        becomeAccessory();
        icon_.setToolTip("ProdMesh Mix engine");
        header_ = menu_.addAction("ProdMesh Mix engine — running");
        header_->setEnabled(false);
        project_ = menu_.addAction("");
        project_->setEnabled(false);
        rec_ = menu_.addAction("");
        rec_->setEnabled(false);
        health_ = menu_.addAction("");
        health_->setEnabled(false);
        menu_.addSeparator();
        auto* open = menu_.addAction("Open ProdMesh Mix");
        connect(open, &QAction::triggered, this, [] { openApp(); });
        menu_.addSeparator();
        auto* quit = menu_.addAction("Quit ProdMesh Mix…");
        connect(quit, &QAction::triggered, this, [this] { confirmQuit(); });
        icon_.setContextMenu(&menu_);
        refresh();
        icon_.show();
        timer_.setInterval(1000);
        connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
        timer_.start();
    }

    // The app bundle this engine belongs to (it lives in its MacOS folder),
    // or the one beside it in a build tree.
    static void openApp() {
        // .../ProdMeshMix.app/Contents/Helpers/ProdMesh Mix Engine.app/Contents/MacOS
        const QString dir = QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
        QString app;
        const auto at = dir.indexOf(".app/Contents/Helpers/");
        if (at >= 0)
            app = dir.left(at + 4);
        else if (QFileInfo::exists(dir + "/ProdMeshMix.app"))
            app = dir + "/ProdMeshMix.app";
        if (!app.isEmpty()) QProcess::startDetached("/usr/bin/open", {app});
    }

private:
    static QIcon makeIcon(bool recording) {
        // Three fader slots with caps: a console, in 18 pt.
        const qreal dpr = 2;
        QPixmap pm(int(18 * dpr), int(18 * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        const qreal xs[] = {3.5, 9, 14.5}, caps[] = {11, 5.5, 8.5};
        for (int i = 0; i < 3; ++i) {
            p.drawRoundedRect(QRectF(xs[i] - 0.75, 2.5, 1.5, 13), 0.75, 0.75);
            p.drawRoundedRect(QRectF(xs[i] - 2.5, caps[i] - 1.5, 5, 3), 1, 1);
        }
        if (recording) {
            p.setBrush(QColor("#e5484d"));
            p.drawEllipse(QPointF(14.5, 14.5), 3.2, 3.2);
        }
        p.end();
        QIcon icon(pm);
        icon.setIsMask(!recording);  // template: follows light / dark menu bars
        return icon;
    }
    void refresh() {
        const QJsonObject s = status ? status() : QJsonObject{};
        const bool recording = s["recording"].toBool();
        if (recording != lastRecording_ || first_) {
            icon_.setIcon(makeIcon(recording));
            lastRecording_ = recording;
            first_ = false;
        }
        const QString project = s["project"].toString();
        project_->setText(project.isEmpty() ? "No project" : "Project: " + project);
        const int sec = int(s["seconds"].toDouble());
        rec_->setText(recording ? QString("● Recording  %1:%2:%3")
                                      .arg(sec / 3600, 2, 10, QChar('0'))
                                      .arg(sec / 60 % 60, 2, 10, QChar('0'))
                                      .arg(sec % 60, 2, 10, QChar('0'))
                                : QString("Not recording"));
        const qint64 xruns = qint64(s["xruns"].toDouble());
        health_->setText(QString("%1 · %2").arg(s["device"].toString(),
                                                 xruns == 1 ? "1 dropout"
                                                            : QString("%1 dropouts").arg(xruns)));
        icon_.setToolTip("ProdMesh Mix engine — " +
                         (recording ? QString("recording") : QString("running")));
    }
    void confirmQuit() {
        bringToFront();
        const QJsonObject s = status ? status() : QJsonObject{};
        QMessageBox box(QMessageBox::Warning, "Quit ProdMesh Mix",
                        "Quit ProdMesh Mix and stop the audio engine?",
                        QMessageBox::Cancel);
        box.setInformativeText(
            QString("Program audio stops on every output") +
            (s["recording"].toBool() ? ", and the recording is finished and saved." : "."));
        auto* quit = box.addButton("Quit", QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == quit && onQuit) onQuit();
    }

    QSystemTrayIcon icon_;
    QMenu menu_;
    QAction *header_, *project_, *rec_, *health_;
    QTimer timer_;
    bool lastRecording_ = false, first_ = true;
};

}  // namespace pm::daemon
