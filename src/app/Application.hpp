#pragma once

#include <QApplication>
#include <memory>

namespace spacelens {

class MainWindow;

/// Owns the top-level Qt application and main window lifetime.
class Application {
public:
    Application(int& argc, char** argv);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

private:
    QApplication m_qtApp;
    std::unique_ptr<MainWindow> m_mainWindow;
};

}  // namespace spacelens
