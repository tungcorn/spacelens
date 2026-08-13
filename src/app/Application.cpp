#include "app/Application.hpp"

#include "ui/MainWindow.hpp"

#ifndef SPACELENS_VERSION_STRING
#define SPACELENS_VERSION_STRING "0.1.1"
#endif

namespace spacelens {

Application::Application(int& argc, char** argv)
    : m_qtApp(argc, argv)
{
    QApplication::setApplicationName(QStringLiteral("SpaceLens"));
    QApplication::setOrganizationName(QStringLiteral("SpaceLens"));
    QApplication::setApplicationVersion(QStringLiteral(SPACELENS_VERSION_STRING));

    m_mainWindow = std::make_unique<MainWindow>();
    m_mainWindow->show();
}

Application::~Application() = default;

int Application::run()
{
    return m_qtApp.exec();
}

}  // namespace spacelens
