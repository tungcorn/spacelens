#pragma once

#include "core/Duplicates.hpp"

#include <QObject>

#include <mutex>
#include <stop_token>
#include <thread>

namespace spacelens {

/// Sequential index-backed duplicate scan. One worker, queued GUI delivery,
/// cooperative cancel. Does not mutate the analyzed filesystem.
class DuplicateDetectionSession final : public QObject {
    Q_OBJECT

public:
    explicit DuplicateDetectionSession(QObject* parent = nullptr);
    ~DuplicateDetectionSession() override;

    DuplicateDetectionSession(const DuplicateDetectionSession&) = delete;
    DuplicateDetectionSession& operator=(const DuplicateDetectionSession&) = delete;

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] DuplicateScanProgress progress() const;
    [[nodiscard]] DuplicateDetectionResult lastResult() const;

    bool start(std::wstring rootPath, DuplicateScanOptions options);
    void cancel();

signals:
    void progressUpdated(quint64 filesDone, quint64 filesTotal, quint64 bytesDone,
                         quint64 bytesTotal);
    void finished(bool completed, const QString& message);

private:
    void joinWorker();
    void onWorkerFinished(DuplicateDetectionResult result, QString message);

    std::jthread m_worker;
    mutable std::mutex m_mutex;
    bool m_running = false;
    DuplicateScanProgress m_progress{};
    DuplicateDetectionResult m_lastResult{};
};

}  // namespace spacelens
