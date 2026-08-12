#include "app/Application.hpp"

#include "ui/MainWindow.hpp"

namespace spacelens {

Application::Application(int& argc, char** argv)
    : m_qtApp(argc, argv)
{
    QApplication::setApplicationName(QStringLiteral("SpaceLens"));
    QApplication::setOrganizationName(QStringLiteral("SpaceLens"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    m_mainWindow = std::make_unique<MainWindow>();
    m_mainWindow->show();
}

Application::~Application() = default;

int Application::run()
{
    return m_qtApp.exec();
}

}  // namespace spacelens
