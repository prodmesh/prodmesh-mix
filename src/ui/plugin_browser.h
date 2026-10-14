// Plugin picker: a popup with a search field over the engine's catalog.
// Type to filter (name or vendor), arrows to move, Enter to insert, Esc to
// close. Effects only; one row per format.
#pragma once

#include <QDialog>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QVBoxLayout>
#include <functional>

namespace pm::ui {

// Name bright, vendor and format quiet: the eye lands on the name.
class PluginRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (opt.state & QStyle::State_Selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(QColor("#2a3a37"));
            p->drawRoundedRect(opt.rect.adjusted(0, 1, 0, -1), 4, 4);
        }
        const QRect r = opt.rect.adjusted(8, 0, -8, 0);
        QFont f = opt.font;
        f.setPixelSize(13);
        p->setFont(f);
        p->setPen(QColor("#e8ecf4"));
        const QString name = idx.data(Qt::DisplayRole).toString();
        p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, name);
        const int nameW = QFontMetrics(f).horizontalAdvance(name);
        f.setPixelSize(11);
        p->setFont(f);
        p->setPen(QColor("#8a92a6"));
        p->drawText(r.adjusted(nameW + 10, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                    idx.data(Qt::UserRole + 1).toString());
        p->setPen(QColor("#5a6172"));
        p->drawText(r, Qt::AlignVCenter | Qt::AlignRight,
                    idx.data(Qt::UserRole + 2).toString());
        p->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return {200, 28};
    }
};

class PluginBrowser : public QDialog {
public:
    std::function<void(const QJsonObject&)> onPick;

    PluginBrowser(const QJsonArray& catalog, bool scanning, QWidget* parent)
        : QDialog(parent, Qt::Popup) {
        setAttribute(Qt::WA_DeleteOnClose);
        setFixedSize(380, 440);
        setStyleSheet(
            "QDialog { background: #1d2129; border: 1px solid #46506a;"
            " border-radius: 8px; }"
            "QLineEdit { background: #14161c; border: 1px solid #323848;"
            " border-radius: 5px; padding: 6px 8px; color: #e8ecf4;"
            " font-size: 13px; }"
            "QLineEdit:focus { border-color: #2fbf9b; }"
            "QListWidget { background: transparent; border: none;"
            " color: #c8cede; font-size: 12px; outline: 0; }"
            "QListWidget::item { padding: 5px 6px; border-radius: 4px; }"
            "QListWidget::item:selected { background: #2a3a37;"
            " color: #e8ecf4; }");
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(10, 10, 10, 10);
        v->setSpacing(8);
        search_ = new QLineEdit;
        search_->setPlaceholderText("Search plugins");
        v->addWidget(search_);
        list_ = new QListWidget;
        list_->setItemDelegate(new PluginRowDelegate(list_));
        list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        list_->setStyleSheet(
            "QScrollBar:vertical { width: 8px; background: transparent; }"
            "QScrollBar::handle:vertical { background: #323848;"
            " border-radius: 4px; min-height: 30px; }"
            "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }");
        v->addWidget(list_, 1);
        status_ = new QLabel;
        status_->setStyleSheet("color: #8a92a6; font-size: 11px;");
        v->addWidget(status_);

        // One row per plugin: when it is installed as both CLAP and VST3,
        // offer CLAP (its threading contract is the stricter of the two).
        QMap<QString, QJsonObject> byName;
        for (const auto& item : catalog) {
            const auto o = item.toObject();
            const auto key = o["vendor"].toString() + "\x1f" + o["name"].toString();
            if (!byName.contains(key) || o["format"].toString() == "clap")
                byName[key] = o;
        }
        for (const auto& o : byName) entries_.append(o);
        std::sort(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
            return a["name"].toString().compare(b["name"].toString(),
                                                Qt::CaseInsensitive) < 0;
        });
        scanning_ = scanning;
        connect(search_, &QLineEdit::textChanged, this, [this] { refill(); });
        connect(list_, &QListWidget::itemActivated, this,
                [this](QListWidgetItem*) { pick(); });
        search_->installEventFilter(this);
        refill();
        search_->setFocus();
    }

    void setSearch(const QString& text) { search_->setText(text); }

protected:
    // Arrows and Enter work while typing.
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == search_ && e->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(e)->key();
            if (key == Qt::Key_Down || key == Qt::Key_Up) {
                const int row = list_->currentRow() + (key == Qt::Key_Down ? 1 : -1);
                if (row >= 0 && row < list_->count()) list_->setCurrentRow(row);
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                pick();
                return true;
            }
        }
        return QDialog::eventFilter(o, e);
    }

private:
    void refill() {
        list_->clear();
        const auto q = search_->text().trimmed();
        for (int i = 0; i < entries_.size(); ++i) {
            const auto& p = entries_[i];
            const auto name = p["name"].toString();
            const auto vendor = p["vendor"].toString();
            if (!q.isEmpty() && !name.contains(q, Qt::CaseInsensitive) &&
                !vendor.contains(q, Qt::CaseInsensitive))
                continue;
            auto* it = new QListWidgetItem(name);
            it->setData(Qt::UserRole, i);
            it->setData(Qt::UserRole + 1, vendor);
            it->setData(Qt::UserRole + 2, p["format"].toString().toUpper());
            list_->addItem(it);
        }
        if (list_->count()) list_->setCurrentRow(0);
        status_->setText(
            entries_.isEmpty()
                ? (scanning_ ? "Scanning installed plugins…" : "No plugins found")
                : QString("%1 of %2 plugins%3")
                      .arg(list_->count())
                      .arg(entries_.size())
                      .arg(scanning_ ? " · rescanning…" : ""));
    }

    void pick() {
        auto* it = list_->currentItem();
        if (!it) return;
        const auto entry = entries_[it->data(Qt::UserRole).toInt()];
        close();
        if (onPick) onPick(entry);
    }

    QLineEdit* search_;
    QListWidget* list_;
    QLabel* status_;
    QList<QJsonObject> entries_;
    bool scanning_ = false;
};

}  // namespace pm::ui
