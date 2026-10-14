// The scene bar: stored mixes, recalled like a console — select, then GO.
//
//   click a scene      select it as next (the live mix does not change)
//   GO                 recall the selected scene (with its fade time)
//   double-click       recall it now
//   click it again     menu: recall, update, rename, fade time, move, delete
//
// The live scene is filled; "edited" means the mix has changed since it
// was recalled or stored. Undo recall puts back the mix from just before
// the last recall.
#pragma once

#include <QApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <functional>

#include "ui/theme.h"

namespace pm::ui {

class SceneBar : public QWidget {
public:
    std::function<void(int id)> onRecall;
    std::function<void(int id, QPoint globalPos)> onMenu;
    std::function<void()> onStore, onUndo;

    SceneBar() {
        setAttribute(Qt::WA_StyledBackground);
        setObjectName("scenebar");
        setStyleSheet("#scenebar { background: #171a21; border-bottom: 1px solid #2a2e39; }");
        setFixedHeight(42);
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(16, 6, 16, 6);
        h->setSpacing(6);
        auto* label = new QLabel("SCENES");
        label->setStyleSheet("color: #8a92a6; font-size: 10px; font-weight: 600;"
                             " letter-spacing: 1.5px; padding-right: 6px;");
        h->addWidget(label);
        chips_ = new QHBoxLayout;
        chips_->setSpacing(6);
        h->addLayout(chips_);
        store_ = button("+ Store", "Store the current mix as a new scene");
        h->addWidget(store_);
        h->addStretch(1);
        undo_ = button("Undo recall", "Back to the mix from just before the last recall");
        h->addWidget(undo_);
        go_ = new QPushButton("GO");
        go_->setCursor(Qt::PointingHandCursor);
        go_->setFixedHeight(28);
        go_->setMinimumWidth(120);
        h->addWidget(go_);
        connect(store_, &QPushButton::clicked, this, [this] { if (onStore) onStore(); });
        connect(undo_, &QPushButton::clicked, this, [this] { if (onUndo) onUndo(); });
        connect(go_, &QPushButton::clicked, this, [this] {
            if (selected_ && onRecall) onRecall(selected_);
        });
        menuTimer_.setSingleShot(true);
        connect(&menuTimer_, &QTimer::timeout, this, [this] {
            if (onMenu) onMenu(menuFor_, menuAt_);
        });
        refresh();
    }

    void setScenes(const QJsonObject& m) {
        scenes_ = m["scenes"].toArray();
        live_ = m["current"].toInt();
        modified_ = m["modified"].toBool();
        canUndo_ = m["canUndo"].toBool();
        bool found = false;
        for (const auto& v : scenes_) found |= v.toObject()["id"].toInt() == selected_;
        if (!found) selected_ = 0;
        rebuild();
    }

private:
    static QPushButton* button(const QString& text, const QString& tip) {
        auto* b = new QPushButton(text);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setFixedHeight(28);
        b->setStyleSheet(
            "QPushButton { background: transparent; border: 1px dashed #323848;"
            " border-radius: 5px; color: #8a92a6; font-size: 11px; padding: 0 10px; }"
            "QPushButton:hover { border-color: #46506a; color: #e8ecf4; }");
        return b;
    }

    static QString fadeText(double ms) {
        if (ms <= 0) return {};
        return ms < 1000 ? QString("%1 ms").arg(int(ms))
                         : QString("%1 s").arg(ms / 1000.0, 0, 'g', 2);
    }

    // One chip per scene. Clicks come through clicked() (mouse, keyboard
    // and accessibility alike); a double-click recalls.
    class Chip : public QPushButton {
    public:
        std::function<void()> onDouble;
        using QPushButton::QPushButton;

    protected:
        void mouseDoubleClickEvent(QMouseEvent*) override {
            if (onDouble) onDouble();
        }
    };

    void rebuild() {
        while (auto* item = chips_->takeAt(0)) {
            if (auto* w = item->widget()) w->deleteLater();  // may be mid-click
            delete item;
        }
        chipIds_.clear();
        chipWidgets_.clear();
        for (const auto& v : scenes_) {
            const auto o = v.toObject();
            const int id = o["id"].toInt();
            auto* c = new Chip;
            c->setCursor(Qt::PointingHandCursor);
            c->setFixedHeight(28);
            const QString fade = fadeText(o["fadeMs"].toDouble());
            c->setToolTip(QString("%1%2\nClick to select, GO to recall; double-click "
                                  "recalls now. Click again for options.")
                              .arg(o["name"].toString(),
                                   fade.isEmpty() ? QString() : " — " + fade + " fade"));
            connect(c, &QPushButton::clicked, this, [this, id, c] {
                if (selected_ != id) {
                    selected_ = id;
                    restyle();
                    return;
                }
                // Already selected: options, unless this click is the first
                // half of a double-click (which recalls instead).
                menuFor_ = id;
                menuAt_ = c->mapToGlobal(QPoint(0, c->height()));
                menuTimer_.start(QApplication::doubleClickInterval());
            });
            c->onDouble = [this, id] {
                menuTimer_.stop();
                selected_ = id;
                restyle();
                if (onRecall) onRecall(id);
            };
            chips_->addWidget(c);
            chipIds_ << id;
            chipWidgets_ << c;
        }
        restyle();
    }

    // Live = filled accent; selected (next) = bright outline.
    void restyle() {
        int n = 0;
        for (int i = 0; i < chipIds_.size(); ++i) {
            const auto o = scenes_[i].toObject();
            const int id = chipIds_[i];
            const bool live = id == live_, sel = id == selected_;
            QString text = QString("%1  %2").arg(++n).arg(o["name"].toString());
            if (live && modified_) text += "  · edited";
            chipWidgets_[i]->setText(text);
            chipWidgets_[i]->setStyleSheet(QString(
                "QPushButton { background: %1; border: 1px solid %2; border-radius: 5px;"
                " color: %3; font-size: 12px; font-weight: %4; padding: 0 12px; }"
                "QPushButton:hover { border-color: #5ce0bd; }")
                .arg(live ? "#1f3b34" : "#1d2129",
                     sel ? "#e8ecf4" : live ? "#2fbf9b" : "#323848",
                     live ? "#e8ecf4" : "#c8cede",
                     live ? "600" : "500"));
        }
        refresh();
    }

    void refresh() {
        undo_->setVisible(canUndo_);
        QString name;
        double fade = 0;
        for (const auto& v : scenes_)
            if (v.toObject()["id"].toInt() == selected_) {
                name = v.toObject()["name"].toString();
                fade = v.toObject()["fadeMs"].toDouble();
            }
        go_->setEnabled(selected_ != 0);
        const QString f = fadeText(fade);
        go_->setText(selected_ ? "GO  ▸  " + name + (f.isEmpty() ? "" : "  (" + f + ")")
                               : "GO");
        go_->setToolTip(selected_ ? "Recall " + name + (f.isEmpty() ? "" : " over " + f)
                                  : "Select a scene, then GO");
        go_->setStyleSheet(
            "QPushButton { background: #2fbf9b; border: none; border-radius: 5px;"
            " color: #0d1a16; font-size: 12px; font-weight: 700; padding: 0 14px; }"
            "QPushButton:hover { background: #5ce0bd; }"
            "QPushButton:disabled { background: #232733; color: #5a6172; }");
    }

    QHBoxLayout* chips_;
    QPushButton *store_, *undo_, *go_;
    QList<int> chipIds_;
    QList<QPushButton*> chipWidgets_;
    QTimer menuTimer_;
    int menuFor_ = 0;
    QPoint menuAt_;
    QJsonArray scenes_;
    int live_ = 0, selected_ = 0;
    bool modified_ = false, canUndo_ = false;
};

}  // namespace pm::ui
