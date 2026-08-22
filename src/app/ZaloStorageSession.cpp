#include "app/ZaloStorageSession.hpp"

#include <QMetaObject>

#include <utility>

namespace spacelens {
namespace {

std::wstring toWide(const QString& value)
{
    return value.toStdWString();
}

}  // namespace

ZaloStorageSession::ZaloStorageSession(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<spacelens::ZaloStorageStatus>(
        "spacelens::ZaloStorageStatus");
}

ZaloStorageSession::~ZaloStorageSession()
{
    cancel();
    joinWorker();
}

bool ZaloStorageSession::isRunning() const
{
    std::lock_guard lock(m_mutex);
    return m_running;
}

bool ZaloStorageSession::start(const QString& rootPath)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
    }

    joinWorker();

    ZaloInspectionOptions options;
    const QString selectedRoot = rootPath.trimmed();
    if (!selectedRoot.isEmpty()) {
        options.explicitRoots.push_back(toWide(selectedRoot));
        options.includeDefaultRoots = false;
    }

    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
        m_running = true;
        m_report.reset();
    }

    m_worker = std::jthread(
        [this, options = std::move(options)](std::stop_token stop) mutable {
            ZaloStorageReport report;
            try {
                report = inspectZaloStorage(options, stop);
            } catch (...) {
                report.status = ZaloStorageStatus::Error;
                report.detail = "Zalo inspection failed";
            }

            QMetaObject::invokeMethod(
                this,
                [this, report = std::move(report)]() mutable {
                    onWorkerFinished(std::move(report));
                },
                Qt::QueuedConnection);
        });
    return true;
}

void ZaloStorageSession::cancel()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
    }
}

std::optional<ZaloStorageReport> ZaloStorageSession::takeReport()
{
    std::lock_guard lock(m_mutex);
    auto report = std::move(m_report);
    m_report.reset();
    return report;
}

void ZaloStorageSession::joinWorker()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

void ZaloStorageSession::onWorkerFinished(ZaloStorageReport report)
{
    const ZaloStorageStatus status = report.status;
    {
        std::lock_guard lock(m_mutex);
        m_report = std::move(report);
        m_running = false;
    }
    emit finished(status);
}

}  // namespace spacelens
