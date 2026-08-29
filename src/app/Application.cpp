#include "app/Application.hpp"

#include "ui/MainWindow.hpp"
#include "ui/UiTheme.hpp"

#include <QDir>
#include <QIcon>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#ifndef SPACELENS_VERSION_STRING
#define SPACELENS_VERSION_STRING "0.1.9"
#endif

namespace spacelens {
namespace {

void scheduleVisualCapture(QWidget& window, const QString& dir)
{
    QDir().mkpath(dir);
    const QString size = qEnvironmentVariable("SPACELENS_VISUAL_SIZE",
                                              QStringLiteral("1280x720"));
    const QStringList parts = size.split(QLatin1Char('x'));
    const int width = parts.value(0).toInt() > 0 ? parts.value(0).toInt() : 1280;
    const int height = parts.value(1).toInt() > 0 ? parts.value(1).toInt() : 720;
    const int delayMs =
        qEnvironmentVariableIsSet("SPACELENS_DEV_SCAN_PATH") ? 2800 : 700;
    QTimer::singleShot(delayMs, [&window, dir, width, height]() {
        window.resize(width, height);
        window.grab().save(dir + QStringLiteral("/workspace-live.png"));
        for (QPushButton* button : window.findChildren<QPushButton*>()) {
            if (button->property("slId").toString() == QLatin1String("indexed")) {
                button->click();
                break;
            }
        }
        QTimer::singleShot(250, [&window, dir]() {
            for (QListWidget* list : window.findChildren<QListWidget*>()) {
                if (list->count() > 0) {
                    list->setCurrentRow(0);
                    break;
                }
            }
            QTimer::singleShot(1200, [&window, dir]() {
                window.grab().save(dir + QStringLiteral("/workspace-indexed.png"));
                qApp->quit();
            });
        });
    });
}

}  // namespace

Application::Application(int& argc, char** argv)
    : m_qtApp(argc, argv)
{
    QApplication::setApplicationName(QStringLiteral("SpaceLens"));
    QApplication::setOrganizationName(QStringLiteral("SpaceLens"));
    QApplication::setApplicationVersion(QStringLiteral(SPACELENS_VERSION_STRING));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/icon.png")));
    applyApplicationChrome(m_qtApp);
    const QString visualTheme = qEnvironmentVariable("SPACELENS_VISUAL_THEME");
    if (!visualTheme.isEmpty()) {
        applyVisualReviewPalette(m_qtApp, visualTheme);
    }

    m_mainWindow = std::make_unique<MainWindow>();
    m_mainWindow->show();

    const QString captureDir = qEnvironmentVariable("SPACELENS_VISUAL_CAPTURE");
    if (!captureDir.isEmpty()) {
        scheduleVisualCapture(*m_mainWindow, captureDir);
    }
}

Application::~Application() = default;

int Application::run()
{
    return m_qtApp.exec();
}

}  // namespace spacelens
