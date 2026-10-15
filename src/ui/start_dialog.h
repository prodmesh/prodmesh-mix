// The Start window: new project (from a template) or a recent one.
//
// Shown by File > New / Open, and once on a genuine first run. Never on a
// reconnect: a UI that restarts mid-show comes back to the mixer.
#pragma once

#include <QButtonGroup>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <functional>

#ifndef PM_VERSION_STRING
#define PM_VERSION_STRING "dev"
#endif

namespace pm::ui {

class StartDialog : public QDialog {
public:
    std::function<void(const QString& name, const QString& templ,
                       const QString& location, bool record)> onCreate;
    std::function<void(const QString& path)> onOpen;

    explicit StartDialog(QWidget* parent) : QDialog(parent) {
        setWindowTitle("ProdMesh Mix");
        setModal(true);
        setFixedSize(780, 520);
        setStyleSheet(R"(
            QDialog { background: #14161c; }
            QLabel { color: #c8cede; background: transparent; }
            QLabel#h1 { color: #e8ecf4; font-size: 20px; font-weight: 600; }
            QLabel#brand { color: #e8ecf4; font-size: 17px; font-weight: 600; }
            QLabel#dim { color: #8a92a6; font-size: 11px; }
            QLabel#field { color: #8a92a6; font-size: 10px; font-weight: 600;
                           letter-spacing: 1.2px; }
            QWidget#side { background: #1a1d24; border-right: 1px solid #2a2e39; }
            QPushButton#nav { text-align: left; padding: 9px 14px; border: none;
                              border-radius: 6px; color: #c8cede; font-size: 13px;
                              background: transparent; }
            QPushButton#nav:checked { background: #232733; color: #e8ecf4; }
            QPushButton#nav:hover { color: #e8ecf4; }
            QLineEdit { background: #1d2129; border: 1px solid #323848; border-radius: 6px;
                        color: #e8ecf4; padding: 7px 10px; font-size: 13px;
                        selection-background-color: #2f6b5c; }
            QLineEdit:focus { border-color: #2fbf9b; }
            QListWidget { background: transparent; border: none; outline: none; }
            QListWidget::item { background: #1d2129; border: 1px solid #262a34;
                                border-radius: 8px; margin: 0 0 8px 0; }
            QListWidget::item:hover { border-color: #46506a; }
            QListWidget::item:selected { border-color: #2fbf9b; background: #1f2b2a;
                                         color: #e8ecf4; }
            QCheckBox { color: #c8cede; font-size: 12px; spacing: 8px; }
            QPushButton#plain { background: #232733; border: 1px solid #323848;
                                border-radius: 6px; color: #c8cede; padding: 7px 16px;
                                font-size: 12px; }
            QPushButton#plain:hover { border-color: #46506a; color: #e8ecf4; }
            QPushButton#primary { background: #2fbf9b; border: none; border-radius: 6px;
                                  color: #0d1a16; padding: 8px 20px; font-size: 13px;
                                  font-weight: 700; }
            QPushButton#primary:hover { background: #5ce0bd; }
            QPushButton#primary:disabled { background: #232733; color: #5a6172; }
        )");

        auto* root = new QHBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        // Side: brand and the two places to go.
        auto* side = new QWidget;
        side->setObjectName("side");
        side->setAttribute(Qt::WA_StyledBackground);
        side->setFixedWidth(210);
        auto* sv = new QVBoxLayout(side);
        sv->setContentsMargins(16, 22, 16, 18);
        sv->setSpacing(4);
        auto* brand = new QLabel("ProdMesh Mix");
        brand->setObjectName("brand");
        auto* tag = new QLabel("A broadcast console that records.");
        tag->setObjectName("dim");
        sv->addWidget(brand);
        sv->addWidget(tag);
        sv->addSpacing(22);
        newNav_ = navButton("New project");
        recentNav_ = navButton("Open recent");
        sv->addWidget(newNav_);
        sv->addWidget(recentNav_);
        sv->addStretch(1);
        auto* ver = new QLabel("v" + QString(PM_VERSION_STRING));
        ver->setObjectName("dim");
        sv->addWidget(ver);
        root->addWidget(side);

        pages_ = new QStackedWidget;
        pages_->addWidget(buildNew());
        pages_->addWidget(buildRecent());
        root->addWidget(pages_, 1);

        auto* nav = new QButtonGroup(this);
        nav->addButton(newNav_);
        nav->addButton(recentNav_);
        connect(newNav_, &QPushButton::clicked, this, [this] { showNew(); });
        connect(recentNav_, &QPushButton::clicked, this, [this] { showRecent(); });
        showNew();
    }

    void showNew() {
        newNav_->setChecked(true);
        pages_->setCurrentIndex(0);
        name_->setFocus();
        name_->selectAll();
    }
    void showRecent() {
        recentNav_->setChecked(true);
        pages_->setCurrentIndex(1);
        recent_->setFocus();
    }

    // From the daemon's "projects" message.
    void setProjects(const QJsonObject& m) {
        location_ = m["location"].toString();
        setLocation(location_);
        current_ = m["current"].toString();

        const QString keep = templates_->currentItem()
                                 ? templates_->currentItem()->data(Qt::UserRole).toString()
                                 : QString("blank");
        templates_->clear();
        for (const auto& v : m["templates"].toArray()) {
            const auto o = v.toObject();
            auto* it = addRow(templates_, o["name"].toString(),
                              o["description"].toString(), o["id"].toString());
            if (o["id"].toString() == keep) templates_->setCurrentItem(it);
        }

        recent_->clear();
        for (const auto& v : m["recent"].toArray()) {
            const auto o = v.toObject();
            const auto when = QDateTime::fromString(o["modified"].toString(), Qt::ISODate);
            const bool cur = o["path"].toString() == current_;
            addRow(recent_, o["name"].toString() + (cur ? "   ·  open now" : ""),
                   tilde(o["path"].toString()) + "   ·   " +
                       when.toString("MMM d, h:mm ap"),
                   o["path"].toString());
        }
        if (recent_->count()) recent_->setCurrentRow(0);
        emptyRecent_->setVisible(recent_->count() == 0);
        // A free default name.
        if (name_->text().isEmpty() || name_->text() == lastDefault_) {
            QStringList taken;
            for (const auto& v : m["recent"].toArray())
                taken << v.toObject()["name"].toString();
            QString n = "Untitled Show";
            for (int i = 2; taken.contains(n); ++i) n = QString("Untitled Show %1").arg(i);
            name_->setText(n);
            lastDefault_ = n;
        }
    }

private:
    // A list row: a name and a quieter second line.
    QListWidgetItem* addRow(QListWidget* list, const QString& title, const QString& sub,
                            const QString& data) {
        auto* it = new QListWidgetItem;
        it->setData(Qt::UserRole, data);
        it->setSizeHint(QSize(0, 60));
        list->addItem(it);
        auto* w = new QWidget;
        w->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(14, 9, 14, 9);
        v->setSpacing(3);
        auto* t = new QLabel(title);
        t->setStyleSheet("color: #e8ecf4; font-size: 13px; font-weight: 600;");
        auto* d = new QLabel(sub);
        d->setStyleSheet("color: #8a92a6; font-size: 11px;");
        v->addWidget(t);
        v->addWidget(d);
        list->setItemWidget(it, w);
        return it;
    }
    static QString tilde(const QString& path) {
        const QString home = QDir::homePath();
        return path.startsWith(home) ? "~" + path.mid(home.size()) : path;
    }

    QPushButton* navButton(const QString& text) {
        auto* b = new QPushButton(text);
        b->setObjectName("nav");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    }
    void setLocation(const QString& d) {
        location_ = d;
        const QString t = tilde(d);
        locLabel_->setText(locLabel_->fontMetrics().elidedText(t, Qt::ElideMiddle, 380));
        locLabel_->setToolTip(d);
    }
    static QLabel* field(const QString& text) {
        auto* l = new QLabel(text.toUpper());
        l->setObjectName("field");
        return l;
    }

    QWidget* buildNew() {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(28, 24, 28, 22);
        v->setSpacing(8);
        auto* h1 = new QLabel("New project");
        h1->setObjectName("h1");
        v->addWidget(h1);
        v->addSpacing(8);
        v->addWidget(field("Name"));
        name_ = new QLineEdit;
        v->addWidget(name_);
        v->addSpacing(8);
        v->addWidget(field("Start from"));
        templates_ = new QListWidget;
        templates_->setCursor(Qt::PointingHandCursor);
        v->addWidget(templates_, 1);
        v->addWidget(field("Location"));
        auto* loc = new QHBoxLayout;
        locLabel_ = new QLabel;
        locLabel_->setObjectName("dim");
        auto* choose = new QPushButton("Choose…");
        choose->setObjectName("plain");
        loc->addWidget(locLabel_, 1);
        loc->addWidget(choose);
        v->addLayout(loc);
        record_ = new QCheckBox("Record every source and the program, always "
                                "(recommended for live shows)");
        record_->setChecked(true);
        v->addSpacing(4);
        v->addWidget(record_);
        v->addSpacing(8);
        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto* cancel = new QPushButton("Cancel");
        cancel->setObjectName("plain");
        auto* create = new QPushButton("Create");
        create->setObjectName("primary");
        create->setDefault(true);
        buttons->addWidget(cancel);
        buttons->addWidget(create);
        v->addLayout(buttons);

        connect(choose, &QPushButton::clicked, this, [this] {
            const QString d = QFileDialog::getExistingDirectory(this, "Where to keep projects",
                                                                location_);
            if (!d.isEmpty()) setLocation(d);
        });
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(name_, &QLineEdit::textChanged, this, [create](const QString& t) {
            create->setEnabled(!t.trimmed().isEmpty() && !t.contains('/'));
        });
        connect(create, &QPushButton::clicked, this, [this] {
            const auto* t = templates_->currentItem();
            if (onCreate)
                onCreate(name_->text().trimmed(),
                         t ? t->data(Qt::UserRole).toString() : QString("blank"),
                         location_, record_->isChecked());
            accept();
        });
        return w;
    }

    QWidget* buildRecent() {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(28, 24, 28, 22);
        v->setSpacing(8);
        auto* h1 = new QLabel("Open recent");
        h1->setObjectName("h1");
        v->addWidget(h1);
        v->addSpacing(8);
        recent_ = new QListWidget;
        recent_->setCursor(Qt::PointingHandCursor);
        v->addWidget(recent_, 1);
        emptyRecent_ = new QLabel("No recent projects yet.");
        emptyRecent_->setObjectName("dim");
        v->addWidget(emptyRecent_);
        auto* buttons = new QHBoxLayout;
        auto* other = new QPushButton("Open other…");
        other->setObjectName("plain");
        buttons->addWidget(other);
        buttons->addStretch(1);
        auto* cancel = new QPushButton("Cancel");
        cancel->setObjectName("plain");
        auto* open = new QPushButton("Open");
        open->setObjectName("primary");
        buttons->addWidget(cancel);
        buttons->addWidget(open);
        v->addLayout(buttons);

        auto openSelected = [this] {
            if (auto* it = recent_->currentItem()) {
                if (onOpen) onOpen(it->data(Qt::UserRole).toString());
                accept();
            }
        };
        connect(open, &QPushButton::clicked, this, openSelected);
        connect(recent_, &QListWidget::itemDoubleClicked, this, openSelected);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(other, &QPushButton::clicked, this, [this] {
            const QString d = QFileDialog::getExistingDirectory(this, "Open project folder",
                                                                location_);
            if (!d.isEmpty()) {
                if (onOpen) onOpen(d);
                accept();
            }
        });
        return w;
    }

    QPushButton *newNav_, *recentNav_;
    QStackedWidget* pages_;
    QLineEdit* name_;
    QListWidget *templates_, *recent_;
    QLabel *locLabel_, *emptyRecent_;
    QCheckBox* record_;
    QString location_, current_, lastDefault_;
};

}  // namespace pm::ui
