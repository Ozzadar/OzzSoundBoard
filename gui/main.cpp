#include <QApplication>

#include "MainWindow.hpp"
#include "core/Paths.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("OzzSoundBoard");
    QApplication::setDesktopFileName("ozzsoundboard-gui");

    MainWindow window(ozzsb::defaultConfigPath(), ozzsb::defaultSocketPath());
    window.show();
    return QApplication::exec();
}
