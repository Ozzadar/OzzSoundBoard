#include "MainWindow.hpp"

#include <chrono>
#include <format>
#include <set>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QSignalBlocker>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "ipc/Socket.hpp"

namespace {

enum Column { NameColumn = 0, PathColumn, PreviewColumn, PlayColumn, ColumnCount };

constexpr auto kRequestTimeout = std::chrono::milliseconds(1500);

QString toQString(const std::string& text) { return QString::fromStdString(text); }

QDoubleSpinBox* makeGain(QWidget* parent) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(0.0, 4.0);
    spin->setSingleStep(0.05);
    spin->setDecimals(2);
    return spin;
}

}  // namespace

MainWindow::MainWindow(std::filesystem::path configPath, std::filesystem::path socketPath)
    : configPath_(std::move(configPath)), socketPath_(std::move(socketPath)) {
    setWindowTitle("OzzSoundBoard");
    resize(860, 520);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    table_ = new QTableWidget(0, ColumnCount, central);
    table_->setHorizontalHeaderLabels({"Name", "File", "", ""});
    table_->horizontalHeader()->setSectionResizeMode(NameColumn, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(PathColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(PreviewColumn, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(PlayColumn, QHeaderView::ResizeToContents);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(table_, &QTableWidget::itemChanged, this, [this] {
        if (!populating_) {
            commit();
        }
    });
    layout->addWidget(table_);

    auto* buttons = new QHBoxLayout();
    auto* add = new QPushButton("Add…", central);
    auto* remove = new QPushButton("Remove", central);
    auto* copy = new QPushButton("Copy deck command", central);
    auto* stopAll = new QPushButton("Stop all", central);
    copy->setToolTip("Copy the Stream Deck Run Command for the selected sound");
    connect(add, &QPushButton::clicked, this, &MainWindow::addSounds);
    connect(remove, &QPushButton::clicked, this, &MainWindow::removeSelected);
    connect(copy, &QPushButton::clicked, this, &MainWindow::copyDeckCommand);
    connect(stopAll, &QPushButton::clicked, this, [this] { send(ozzsb::Verb::Stop); });
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(copy);
    buttons->addStretch();
    buttons->addWidget(stopAll);
    layout->addLayout(buttons);

    auto* gains = new QFormLayout();
    input_ = new QComboBox(central);
    input_->setToolTip("Microphone mixed into OzzSoundBoard Microphone");
    connect(input_, &QComboBox::currentIndexChanged, this, [this] { commit(); });
    gains->addRow("Microphone", input_);
    micGain_ = makeGain(central);
    clipsToMicGain_ = makeGain(central);
    clipsToMonitorGain_ = makeGain(central);
    gains->addRow("Microphone volume", micGain_);
    gains->addRow("Sounds into microphone", clipsToMicGain_);
    gains->addRow("Sounds through speakers", clipsToMonitorGain_);
    for (auto* spin : {micGain_, clipsToMicGain_, clipsToMonitorGain_}) {
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this] { commit(); });
    }
    layout->addLayout(gains);

    setCentralWidget(central);
    status_ = new QLabel(this);
    statusBar()->addWidget(status_, 1);

    loadLibrary();
    populate();

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::refreshStatus);
    timer->start(3000);
    refreshStatus();
}

void MainWindow::loadLibrary() {
    auto library = ozzsb::SoundLibrary::load(configPath_);
    if (!library) {
        QMessageBox::warning(this, "OzzSoundBoard", toQString(library.error()));
        return;
    }
    library_ = std::move(*library);
}

void MainWindow::populate() {
    populating_ = true;
    table_->setRowCount(0);
    for (const auto& [name, path] : library_.sounds()) {
        addRow(toQString(name), toQString(path.string()));
    }
    showInputs({});
    micGain_->setValue(library_.gains().mic);
    clipsToMicGain_->setValue(library_.gains().clipsToMic);
    clipsToMonitorGain_->setValue(library_.gains().clipsToMonitor);
    populating_ = false;
}

void MainWindow::addRow(const QString& name, const QString& path) {
    const int row = table_->rowCount();
    table_->insertRow(row);
    table_->setItem(row, NameColumn, new QTableWidgetItem(name));
    table_->setItem(row, PathColumn, new QTableWidgetItem(path));

    auto* preview = new QPushButton("Preview", table_);
    preview->setToolTip("Play through your speakers only");
    connect(preview, &QPushButton::clicked, this, [this, preview] { sendForButton(preview, ozzsb::Verb::Preview); });
    table_->setCellWidget(row, PreviewColumn, preview);

    auto* play = new QPushButton("Play", table_);
    play->setToolTip("Play into the virtual microphone and your speakers");
    connect(play, &QPushButton::clicked, this, [this, play] { sendForButton(play, ozzsb::Verb::Play); });
    table_->setCellWidget(row, PlayColumn, play);
}

bool MainWindow::commit() {
    ozzsb::SoundLibrary edited;
    std::set<std::string> seen;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const std::string name = table_->item(row, NameColumn)->text().trimmed().toStdString();
        const std::string path = table_->item(row, PathColumn)->text().trimmed().toStdString();
        std::expected<void, std::string> result = edited.set(name, path);
        if (result && !seen.insert(name).second) {
            result = std::unexpected(std::format("\"{}\" is used twice", name));
        }
        if (!result) {
            QMessageBox::warning(this, "OzzSoundBoard", toQString(result.error()));
            populate();
            return false;
        }
    }
    edited.gains() = {static_cast<float>(micGain_->value()), static_cast<float>(clipsToMicGain_->value()),
                      static_cast<float>(clipsToMonitorGain_->value())};
    edited.setInput(input_->currentData().toString().toStdString());

    if (auto saved = edited.save(configPath_); !saved) {
        QMessageBox::warning(this, "OzzSoundBoard", toQString(saved.error()));
        return false;
    }
    library_ = std::move(edited);
    send(ozzsb::Verb::Reload);
    return true;
}

void MainWindow::addSounds() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Add sounds", QString(), "Audio (*.wav *.flac *.ogg *.oga *.opus *.mp3 *.aiff *.aif);;All files (*)");
    if (files.isEmpty()) {
        return;
    }
    populating_ = true;
    for (const QString& file : files) {
        QString name = QFileInfo(file).completeBaseName();
        for (int suffix = 2; !table_->findItems(name, Qt::MatchExactly).isEmpty(); ++suffix) {
            name = QStringLiteral("%1 %2").arg(QFileInfo(file).completeBaseName()).arg(suffix);
        }
        addRow(name, file);
    }
    populating_ = false;
    commit();
}

void MainWindow::removeSelected() {
    std::set<int> rows;
    for (const QModelIndex& index : table_->selectionModel()->selectedRows()) {
        rows.insert(index.row());
    }
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        table_->removeRow(*it);
    }
    if (!rows.empty()) {
        commit();
    }
}

void MainWindow::copyDeckCommand() {
    const int row = table_->currentRow();
    if (row < 0) {
        statusBar()->showMessage("Select a sound first", 3000);
        return;
    }
    QString name = table_->item(row, NameColumn)->text();
    name.replace('\'', "'\\''");
    // OpenDeck runs commands through sh without the login PATH, so prefer the absolute binary path.
    QString binary = QStandardPaths::findExecutable("ozzsoundboard");
    if (binary.isEmpty()) {
        binary = "ozzsoundboard";
    }
    const QString command = QStringLiteral("%1 play '%2'").arg(binary, name);
    QApplication::clipboard()->setText(command);
    statusBar()->showMessage("Copied: " + command, 4000);
}

void MainWindow::sendForButton(QWidget* button, ozzsb::Verb verb) {
    const int row = table_->indexAt(button->pos()).row();
    if (row >= 0) {
        send(verb, table_->item(row, NameColumn)->text());
    }
}

void MainWindow::send(ozzsb::Verb verb, const QString& name) {
    auto response = ozzsb::sendRequest(socketPath_, ozzsb::Request{verb, name.toStdString()}, kRequestTimeout);
    if (!response) {
        statusBar()->showMessage(toQString(response.error()), 5000);
    } else if (!response->ok) {
        statusBar()->showMessage(toQString(response->message), 5000);
    }
}

void MainWindow::refreshInputs() {
    auto response = ozzsb::sendRequest(socketPath_, ozzsb::Request{ozzsb::Verb::Inputs, {}}, kRequestTimeout);
    QList<QStringList> inputs;
    if (response && response->ok) {
        for (const QString& line : toQString(response->body).split('\n', Qt::SkipEmptyParts)) {
            QStringList fields = line.split('\t');
            while (fields.size() < 3) {
                fields << QString();
            }
            inputs << fields;
        }
    }
    showInputs(inputs);
}

// inputs: {node name, description, flags} per microphone, as reported by the daemon.
void MainWindow::showInputs(const QList<QStringList>& inputs) {
    const QString configured = toQString(library_.input());
    QString signature = configured;
    for (const QStringList& input : inputs) {
        signature += '\n' + input.join('\t');
    }
    if (signature == inputsSignature_ && input_->count() > 0) {
        return;
    }
    inputsSignature_ = signature;

    const QSignalBlocker blocker(input_);
    input_->clear();
    QString systemDefault;
    for (const QStringList& input : inputs) {
        if (input[2].split(',').contains("default")) {
            systemDefault = input[1];
        }
    }
    input_->addItem(systemDefault.isEmpty() ? "Automatic (follow system default)"
                                            : QStringLiteral("Automatic (system default: %1)").arg(systemDefault),
                    QString());
    bool configuredFound = configured.isEmpty();
    for (const QStringList& input : inputs) {
        QString label = input[1];
        if (input[2].split(',').contains("active")) {
            label += "  ●";
        }
        input_->addItem(label, input[0]);
        configuredFound = configuredFound || input[0] == configured;
    }
    if (!configuredFound) {
        input_->addItem(QStringLiteral("%1 (not connected)").arg(configured), configured);
    }
    input_->setCurrentIndex(std::max(0, input_->findData(configured)));
}

void MainWindow::refreshStatus() {
    refreshInputs();
    auto response = ozzsb::sendRequest(socketPath_, ozzsb::Request{ozzsb::Verb::Status, {}}, kRequestTimeout);
    if (!response || !response->ok) {
        status_->setText("Daemon not running. Start it with: systemctl --user start ozzsoundboard");
        return;
    }
    QStringList parts;
    for (const QString& line : toQString(response->body).split('\n', Qt::SkipEmptyParts)) {
        const QStringList fields = line.split('\t');
        if (fields.size() >= 3 && fields[0] == "mic_capture") {
            parts << "mic: " + fields[2];
        } else if (fields.size() >= 2 && (fields[0] == "mic_capture" || fields[0] == "monitor" || fields[0] == "voices")) {
            parts << fields[0] + ": " + fields[1];
        } else if (fields.size() >= 3 && fields[0] == "virtual_mic") {
            parts << QStringLiteral("\"%1\": %2").arg(fields[1], fields[2]);
        }
    }
    status_->setText("Daemon running · " + parts.join(" · "));
}
