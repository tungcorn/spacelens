#include "app/CleanupRevalidationSession.hpp"

#include "core/OrdinaryLocation.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"

#include <QMetaObject>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace spacelens {
namespace {

FileTimeTicks nowFileTimeTicks()
{
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

}  // namespace

CleanupRevalidationSession::CleanupRevalidationSession(
    CleanupReviewController& controller, QObject* parent)
    : QObject(parent)
    , m_controller(controller)
{
}

CleanupRevalidationSession::~CleanupRevalidationSession()
{
    cancel();
    joinWorker();
    m_controller.setReviewMutationsBlocked(false);
}

bool CleanupRevalidationSession::isRunning() const
{
    std::lock_guard lock(m_mutex);
    return m_running;
}

quint64 CleanupRevalidationSession::probed() const
{
    std::lock_guard lock(m_mutex);
    return m_probed;
}

quint64 CleanupRevalidationSession::total() const
{
    std::lock_guard lock(m_mutex);
    return m_total;
}

bool CleanupRevalidationSession::start()
{
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
    }
    joinWorker();

    std::vector<CleanupCandidate> snapshot = m_controller.review().items();
    OrdinaryLocationPolicy locationPolicy = m_controller.ordinaryLocationPolicy();
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
        m_running = true;
        m_probed = 0;
        m_total = snapshot.size();
    }
    m_controller.setReviewMutationsBlocked(true);

    m_worker = std::jthread([this, snapshot = std::move(snapshot),
                             locationPolicy = std::move(locationPolicy)](
                                std::stop_token stop) {
        WindowsCleanupMetadataReader reader;
        const FileTimeTicks checkedAt = nowFileTimeTicks();
        CleanupRevalidationPassResult result;
        result.updates.reserve(snapshot.size());

        for (const auto& item : snapshot) {
            if (stop.stop_requested()) {
                result.completed = false;
                result.updates.clear();
                QMetaObject::invokeMethod(
                    this,
                    [this, result = std::move(result)]() mutable {
                        onWorkerFinished(std::move(result),
                                         QStringLiteral(
                                             "Revalidation cancelled. "
                                             "Partial results were discarded."));
                    },
                    Qt::QueuedConnection);
                return;
            }

            auto one = revalidateCleanupCandidate(item, reader, locationPolicy);
            CleanupValidationReplacement update;
            update.id = item.id;
            update.expectedPath = item.path;
            update.current = std::move(one.current);
            update.checkedAt = checkedAt;
            result.updates.push_back(std::move(update));
            ++result.probedCount;

            {
                std::lock_guard lock(m_mutex);
                m_probed = result.probedCount;
            }
            const quint64 probed = result.probedCount;
            const quint64 total = snapshot.size();
            QMetaObject::invokeMethod(
                this,
                [this, probed, total]() { emit progressUpdated(probed, total); },
                Qt::QueuedConnection);
        }

        result.completed = true;
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result)]() mutable {
                onWorkerFinished(std::move(result), {});
            },
            Qt::QueuedConnection);
    });
    return true;
}

void CleanupRevalidationSession::cancel()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
    }
}

void CleanupRevalidationSession::joinWorker()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

void CleanupRevalidationSession::onWorkerFinished(
    CleanupRevalidationPassResult result, QString message)
{
    bool completed = result.completed;
    if (completed) {
        const auto status =
            m_controller.replaceValidationBatch(result.updates);
        if (!status.ok) {
            completed = false;
            message = QString::fromStdString(
                status.message.empty()
                    ? "Failed to persist revalidation results."
                    : status.message);
        } else if (message.isEmpty()) {
            message = QStringLiteral("Revalidated %1 item(s).")
                          .arg(result.probedCount);
        }
    }

    {
        std::lock_guard lock(m_mutex);
        m_running = false;
        m_probed = result.probedCount;
    }
    m_controller.setReviewMutationsBlocked(false);
    emit finished(completed, message);
}

}  // namespace spacelens
