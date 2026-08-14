#include "app/DuplicateDetectionSession.hpp"

#include "core/DuplicateDetection.hpp"
#include "core/SizeFormatter.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexQuery.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/FileContentHasher.hpp"

#include <QMetaObject>

namespace spacelens {

DuplicateDetectionSession::DuplicateDetectionSession(QObject* parent)
    : QObject(parent)
{
}

DuplicateDetectionSession::~DuplicateDetectionSession()
{
    cancel();
    joinWorker();
}

bool DuplicateDetectionSession::isRunning() const
{
    std::lock_guard lock(m_mutex);
    return m_running;
}

DuplicateScanProgress DuplicateDetectionSession::progress() const
{
    std::lock_guard lock(m_mutex);
    return m_progress;
}

DuplicateDetectionResult DuplicateDetectionSession::lastResult() const
{
    std::lock_guard lock(m_mutex);
    return m_lastResult;
}

bool DuplicateDetectionSession::start(std::wstring rootPath,
                                      DuplicateScanOptions options)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
    }
    joinWorker();
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
        m_running = true;
        m_progress = {};
        m_lastResult = {};
    }

    if (options.hashCachePath.empty()) {
        options.hashCachePath = spaceLensHashCachePath();
    }

    m_worker = std::jthread([this, rootPath = std::move(rootPath),
                             options = std::move(options)](std::stop_token stop) {
        const auto candidates =
            queryDuplicateSizeCandidates(rootPath, options.minimumSize);
        {
            std::lock_guard lock(m_mutex);
            m_progress.candidateFiles = candidates.candidateFiles;
            m_progress.candidateBytes = candidates.candidateBytes;
        }
        QMetaObject::invokeMethod(
            this,
            [this, files = candidates.candidateFiles,
             bytes = candidates.candidateBytes]() {
                emit progressUpdated(0, files, 0, bytes);
            },
            Qt::QueuedConnection);

        WindowsCleanupMetadataReader reader;
        WindowsFileContentHasher hasher;
        auto result = detectDuplicates(
            candidates, reader, hasher, options,
            [&stop]() { return stop.stop_requested(); },
            [this](const DuplicateScanProgress& progress) {
                {
                    std::lock_guard lock(m_mutex);
                    m_progress = progress;
                }
                const quint64 done =
                    progress.filesProbed + progress.filesFingerprinted +
                    progress.filesFullyHashed;
                QMetaObject::invokeMethod(
                    this,
                    [this, done, total = progress.candidateFiles,
                     bytes = progress.bytesRead,
                     bytesTotal = progress.candidateBytes]() {
                        emit progressUpdated(done, total, bytes, bytesTotal);
                    },
                    Qt::QueuedConnection);
            });

        QString message;
        if (!candidates.ok) {
            message = QString::fromStdString(candidates.error);
        } else if (result.cancelled) {
            message = QStringLiteral(
                "Duplicate scan cancelled. Completed groups are marked partial.");
        } else {
            message = QStringLiteral(
                          "Verified %1 group(s). Potential redundant logical "
                          "size: %2. Planning only.")
                          .arg(result.summary.verifiedGroups)
                          .arg(QString::fromStdString(SizeFormatter::format(
                              result.summary.potentialRedundantLogicalBytes)));
            if (result.summary.cacheHits > 0) {
                message += QStringLiteral(" Cache hits: %1.")
                               .arg(result.summary.cacheHits);
            }
        }

        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result), message = std::move(message)]() mutable {
                onWorkerFinished(std::move(result), message);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void DuplicateDetectionSession::cancel()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
    }
}

void DuplicateDetectionSession::joinWorker()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

void DuplicateDetectionSession::onWorkerFinished(DuplicateDetectionResult result,
                                                 QString message)
{
    const bool completed = result.completed && !result.cancelled;
    {
        std::lock_guard lock(m_mutex);
        m_running = false;
        m_progress = result.progress;
        m_lastResult = std::move(result);
    }
    emit finished(completed, message);
}

}  // namespace spacelens
