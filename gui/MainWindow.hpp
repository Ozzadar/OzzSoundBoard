#pragma once

#include <filesystem>

#include <QMainWindow>

#include "core/SoundLibrary.hpp"
#include "ipc/Protocol.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(std::filesystem::path configPath, std::filesystem::path socketPath);

private:
    void loadLibrary();
    void populate();
    void addRow(const QString& name, const QString& path);
    bool commit();
    void addSounds();
    void removeSelected();
    void copyDeckCommand();
    void send(ozzsb::Verb verb, const QString& name = {});
    void sendForButton(QWidget* button, ozzsb::Verb verb);
    void refreshStatus();
    void refreshInputs();
    void showInputs(const QList<QStringList>& inputs);

    std::filesystem::path configPath_;
    std::filesystem::path socketPath_;
    ozzsb::SoundLibrary library_;
    bool populating_ = false;

    QTableWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
    QComboBox* input_ = nullptr;
    QString inputsSignature_;
    QDoubleSpinBox* micGain_ = nullptr;
    QDoubleSpinBox* clipsToMicGain_ = nullptr;
    QDoubleSpinBox* clipsToMonitorGain_ = nullptr;
};
