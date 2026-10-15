// Audio Setup: input and output device, sample rate and buffer size.
//
// The engine owns the device, so it lists them ("audioSetup") and applies
// a choice ("applyAudio") by saving it and restarting itself in place — a
// device's rate and buffer are fixed while it runs. That stops program
// audio for a moment, so Apply asks first and says what happens to the
// take. Devices are saved by UID (names can repeat), with the name for
// display when the device is not there.
#pragma once

#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <functional>

namespace pm::ui {

class AudioSetupDialog : public QDialog {
public:
    std::function<void()> onRefresh;
    std::function<void(const QJsonObject& settings)> onApply;
    std::function<bool()> recording;  // to say what happens to the take

    explicit AudioSetupDialog(QWidget* parent) : QDialog(parent) {
        setWindowTitle("Audio Setup");
        setModal(true);
        setMinimumWidth(560);
        setStyleSheet(R"(
            QDialog { background: #14161c; }
            QLabel { color: #c8cede; background: transparent; font-size: 12px; }
            QLabel#h1 { color: #e8ecf4; font-size: 18px; font-weight: 600; }
            QLabel#dim { color: #8a92a6; font-size: 11px; }
            QLabel#field { color: #8a92a6; font-size: 10px; font-weight: 600;
                           letter-spacing: 1.2px; }
            QLabel#running { color: #c8cede; background: #1a1d24; border: 1px solid #2a2e39;
                             border-radius: 6px; padding: 9px 12px; font-size: 12px; }
            QLabel#warning { color: #e0c05c; background: #2a2618; border: 1px solid #5a4d22;
                             border-radius: 6px; padding: 9px 12px; font-size: 12px; }
            QComboBox { background: #1d2129; border: 1px solid #323848; border-radius: 6px;
                        color: #e8ecf4; padding: 6px 10px; font-size: 13px; min-height: 18px; }
            QComboBox:hover { border-color: #46506a; }
            QComboBox:disabled { color: #5a6172; }
            QComboBox QAbstractItemView { background: #1d2129; color: #e8ecf4;
                                          selection-background-color: #2f6b5c; }
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

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(28, 24, 28, 22);
        root->setSpacing(14);
        auto* h1 = new QLabel("Audio Setup");
        h1->setObjectName("h1");
        root->addWidget(h1);
        running_ = new QLabel;
        running_->setObjectName("running");
        running_->setWordWrap(true);
        root->addWidget(running_);
        warning_ = new QLabel;
        warning_->setObjectName("warning");
        warning_->setWordWrap(true);
        warning_->setTextFormat(Qt::RichText);
        warning_->hide();
        root->addWidget(warning_);

        auto* form = new QFormLayout;
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(12);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto field = [](const QString& t) {
            auto* l = new QLabel(t);
            l->setObjectName("field");
            return l;
        };
        input_ = new QComboBox;
        output_ = new QComboBox;
        rate_ = new QComboBox;
        block_ = new QComboBox;
        form->addRow(field("INPUT"), input_);
        form->addRow(field("OUTPUT"), output_);
        auto* rb = new QHBoxLayout;
        rb->setSpacing(18);
        rb->addWidget(rate_, 1);
        auto* bl = field("BUFFER");
        rb->addWidget(bl);
        rb->addWidget(block_, 1);
        form->addRow(field("SAMPLE RATE"), rb);
        root->addLayout(form);
        note_ = new QLabel;
        note_->setObjectName("dim");
        note_->setWordWrap(true);
        root->addWidget(note_);

        root->addStretch(1);
        auto* buttons = new QHBoxLayout;
        auto* refresh = new QPushButton("Refresh Devices");
        refresh->setObjectName("plain");
        refresh->setToolTip("Look for devices again — e.g. after starting Dante Virtual "
                            "Soundcard or plugging in an interface");
        connect(refresh, &QPushButton::clicked, this, [this] { if (onRefresh) onRefresh(); });
        buttons->addWidget(refresh);
        buttons->addStretch(1);
        auto* cancel = new QPushButton("Cancel");
        cancel->setObjectName("plain");
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        buttons->addWidget(cancel);
        apply_ = new QPushButton("Apply…");
        apply_->setObjectName("primary");
        apply_->setDefault(true);
        connect(apply_, &QPushButton::clicked, this, [this] { confirmApply(); });
        buttons->addWidget(apply_);
        root->addLayout(buttons);

        for (auto* c : {input_, output_})
            connect(c, &QComboBox::currentIndexChanged, this, [this] {
                if (!filling_) refreshRatesAndBlocks();
            });
        for (auto* c : {rate_, block_})
            connect(c, &QComboBox::currentIndexChanged, this, [this] {
                if (!filling_) updateNotes();
            });
        setInfo({});
    }

    // From the engine's "audioSetup" message. Keeps the operator's
    // unapplied choices across a refresh.
    void setInfo(const QJsonObject& m) {
        const bool first = info_.isEmpty();
        const QString keepIn = first ? QString() : input_->currentData().toString();
        const QString keepOut = first ? QString() : output_->currentData().toString();
        const double keepRate = first ? 0 : rate_->currentData().toDouble();
        const int keepBlock = first ? 0 : block_->currentData().toInt();
        info_ = m;
        devices_ = m["devices"].toArray();
        // Read-only: operator[] on a non-const QJsonObject inserts keys.
        const QJsonObject saved = m["saved"].toObject();
        const QJsonObject run = m["running"].toObject();
        const auto locked = m["locked"].toArray().toVariantList();
        const bool sim = m["driver"].toString() == "sim";

        if (m.isEmpty()) {
            running_->setText("Asking the engine…");
        } else {
            const double rate = run["rate"].toDouble();
            const int block = run["block"].toInt();
            running_->setText(
                QString("<b>Running:</b> %1  ·  %2 kHz  ·  %3 samples (%4 ms)  ·  "
                        "%5 in / %6 out")
                    .arg(run["device"].toString().toHtmlEscaped())
                    .arg(rate / 1000.0, 0, 'g', 4)
                    .arg(block)
                    .arg(rate > 0 ? 1000.0 * block / rate : 0, 0, 'f', 1)
                    .arg(run["inputs"].toInt())
                    .arg(run["outputs"].toInt()));
        }
        const QString warn = m["warning"].toString();
        warning_->setVisible(!warn.isEmpty());
        warning_->setText("The saved devices could not be opened (" + warn.toHtmlEscaped() +
                          "), so the engine is using the system output without inputs. "
                          "Choose devices below, or start the missing one and Refresh.");

        filling_ = true;
        input_->clear();
        output_->clear();
        if (sim) {
            input_->addItem("Simulated (no device)", QString());
            output_->addItem("Simulated clock", QString());
        } else {
            input_->addItem("None — no inputs", "none");
            output_->addItem("System default output", "");
            for (const auto& v : devices_) {
                const auto d = v.toObject();
                const auto name = d["name"].toString();
                if (d["inputs"].toInt() > 0)
                    input_->addItem(QString("%1  —  %2 in").arg(name).arg(d["inputs"].toInt()),
                                    d["uid"].toString());
                if (d["outputs"].toInt() > 0)
                    output_->addItem(
                        QString("%1  —  %2 out%3")
                            .arg(name)
                            .arg(d["outputs"].toInt())
                            .arg(d["defaultOutput"].toBool() ? "  (system default)" : ""),
                        d["uid"].toString());
            }
        }
        auto select = [&](QComboBox* c, const QString& want, const QString& savedName) {
            int i = c->findData(want);
            if (i < 0) {
                // Matched by name too (the command line and older settings).
                for (int k = 0; k < c->count() && i < 0; ++k)
                    if (c->itemText(k).startsWith(want + "  —")) i = k;
            }
            if (i < 0 && !want.isEmpty() && want != "none") {
                // Saved but not there now: keep it choosable, marked.
                c->addItem(QString("%1  —  not connected").arg(savedName.isEmpty() ? want : savedName),
                           want);
                i = c->count() - 1;
            }
            c->setCurrentIndex(std::max(i, 0));
        };
        if (!sim) {
            select(input_, first ? saved["input"].toString("none") : keepIn,
                   saved["inputName"].toString());
            select(output_, first ? saved["output"].toString() : keepOut,
                   saved["outputName"].toString());
        }
        filling_ = false;
        refreshRatesAndBlocks(first ? saved["rate"].toDouble(run["rate"].toDouble(48000)) : keepRate,
                              first ? saved["block"].toInt(run["block"].toInt(128)) : keepBlock);

        input_->setEnabled(!sim && !locked.contains("input"));
        output_->setEnabled(!sim && !locked.contains("output"));
        rate_->setEnabled(!locked.contains("rate"));
        block_->setEnabled(!locked.contains("block"));
        for (auto* c : {input_, output_, rate_, block_})
            c->setToolTip(c->isEnabled() || sim ? QString()
                                                : "Set on the engine's command line");
        updateNotes();
        adjustSize();  // the warning may have appeared
    }

private:
    QJsonObject device(const QString& uid) const {
        for (const auto& v : devices_)
            if (v.toObject()["uid"].toString() == uid) return v.toObject();
        return {};
    }
    QJsonObject outputDevice() const {
        const QString uid = output_->currentData().toString();
        if (!uid.isEmpty()) return device(uid);
        for (const auto& v : devices_)
            if (v.toObject()["defaultOutput"].toBool()) return v.toObject();
        return {};
    }

    // Rates both chosen devices can run at; buffer sizes the output (the
    // clock) allows.
    void refreshRatesAndBlocks(double wantRate = 0, int wantBlock = 0) {
        if (wantRate <= 0) wantRate = rate_->currentData().toDouble();
        if (wantBlock <= 0) wantBlock = block_->currentData().toInt();
        QList<double> rates{44100, 48000, 88200, 96000, 176400, 192000};
        auto restrict = [&](const QJsonObject& d) {
            const auto r = d["rates"].toArray();
            if (r.isEmpty()) return;
            rates.removeIf([&](double x) { return !r.contains(QJsonValue(x)); });
        };
        const QJsonObject out = outputDevice();
        restrict(out);
        const QString in = input_->currentData().toString();
        if (!in.isEmpty() && in != "none") restrict(device(in));
        const int minB = std::max(out["minBlock"].toInt(16), 16);
        const int maxB = out["maxBlock"].toInt(0) > 0 ? out["maxBlock"].toInt() : 4096;

        filling_ = true;
        rate_->clear();
        for (double r : rates)
            rate_->addItem(QString("%1 kHz").arg(r / 1000.0, 0, 'g', 4), r);
        if (rates.isEmpty()) rate_->addItem("No common rate", 0.0);
        int ri = rate_->findData(wantRate);
        if (ri < 0) ri = rate_->findData(48000.0);
        rate_->setCurrentIndex(std::max(ri, 0));
        block_->clear();
        for (int b : {16, 32, 64, 128, 256, 512, 1024, 2048})
            if (b >= minB && b <= maxB) block_->addItem(QString("%1 samples").arg(b), b);
        int bi = block_->findData(wantBlock);
        if (bi < 0) bi = block_->findData(128);
        block_->setCurrentIndex(std::max(bi, 0));
        filling_ = false;
        updateNotes();
    }

    QJsonObject chosen() const {
        QJsonObject s;
        if (info_.value("driver").toString() != "sim") {
            s["input"] = input_->currentData().toString();
            s["output"] = output_->currentData().toString();
            const auto in = device(s["input"].toString());
            const auto out = device(s["output"].toString());
            if (!in.isEmpty()) s["inputName"] = in["name"];
            if (!out.isEmpty()) s["outputName"] = out["name"];
        }
        s["rate"] = rate_->currentData().toDouble();
        s["block"] = block_->currentData().toInt();
        return s;
    }

    bool changed() const {
        if (info_.isEmpty()) return false;
        const auto c = chosen();
        const auto saved = info_.value("saved").toObject();
        const auto run = info_.value("running").toObject();
        if (c["rate"].toDouble() != run["rate"].toDouble() ||
            c["block"].toInt() != run["block"].toInt())
            return true;
        if (info_.value("driver").toString() == "sim") return false;
        // A fallback is running: re-applying the saved devices is a change.
        if (!info_.value("warning").toString().isEmpty()) return true;
        return c["input"].toString() != saved["input"].toString("none") ||
               c["output"].toString() != saved["output"].toString();
    }

    void updateNotes() {
        const double rate = rate_->currentData().toDouble();
        const int block = block_->currentData().toInt();
        QStringList notes;
        if (rate > 0 && block > 0)
            notes << QString("Buffer latency %1 ms each way. Smaller is tighter for live "
                             "monitoring and costs more CPU; 128 is a good start.")
                         .arg(1000.0 * block / rate, 0, 'f', 1);
        const QString in = input_->currentData().toString();
        const QString out = output_->currentData().toString();
        const QString outUid = outputDevice()["uid"].toString();
        if (!in.isEmpty() && in != "none" && in != outUid)
            notes << "Input and output are different devices: they are combined, with the "
                     "output as the clock and the input drift-corrected into it.";
        if (in == "none" && info_.value("driver").toString() != "sim")
            notes << "No input: nothing is recorded but the program bus and test sources.";
        note_->setText(notes.join("\n\n"));
        apply_->setEnabled(changed() && rate > 0 && block > 0);
    }

    void confirmApply() {
        QMessageBox box(QMessageBox::Warning, "Apply audio settings",
                        "Restart the audio engine with these settings?",
                        QMessageBox::Cancel, this);
        const bool rec = recording && recording();
        box.setInformativeText(
            QString("Program audio stops on every output for a moment while the engine "
                    "reopens the device. The mix, scenes and plugins come back as they are.") +
            (rec ? "\n\nThe current take is finished and saved; a new take starts when "
                   "the engine is back."
                 : QString()));
        auto* go = box.addButton("Restart Engine", QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != go) return;
        if (onApply) onApply(chosen());
        accept();
    }

    QLabel *running_, *warning_, *note_;
    QComboBox *input_, *output_, *rate_, *block_;
    QPushButton* apply_;
    QJsonObject info_;
    QJsonArray devices_;
    bool filling_ = false;
};

}  // namespace pm::ui
