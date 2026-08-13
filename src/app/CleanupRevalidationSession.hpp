#pragma once

#include "core/CleanupRevalidation.hpp"
#include "core/CleanupReviewStore.hpp"

#include <QObject>

#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace spacelens {

/// Sequential metadata-only revalidation. One probe at a time, queued GUI
/// delivery, cooperative cancel. Cancellation discards the partial batch.
/// Does not mutate the analyzed filesystem.
class CleanupRevalidationSession final : public QObject {
    Q_OBJECT

public:
    explicit CleanupRevalidationSession(CleanupReviewController& controller,
                                        QObject* parent = nullptr);
    ~CleanupRevalidationSession() override;

    CleanupRevalidationSession(const CleanupRevalidationSession&) = delete;
    CleanupRevalidationSession& operator=(const CleanupRevalidationSession&) = delete;

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] quint64 probed() const;
    [[nodiscard]] quint64 total() const;

    /// Snapshot current review items and start. False if already running.
    bool start();
    void cancel();

signals:
    void progressUpdated(quint64 probed, quint64 total);
    void finished(bool completed, const QString& message);

private:
    void joinWorker();
    void onWorkerFinished(CleanupRevalidationPassResult result, QString message);

    CleanupReviewController& m_controller;
    std::jthread m_worker;
    mutable std::mutex m_mutex;
    bool m_running = false;
    quint64 m_probed = 0;
    quint64 m_total = 0;
};

}  // namespace spacelens
