#pragma once

#include "core/CleanupReviewStore.hpp"
#include "core/Maintenance.hpp"

#include <QObject>

#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace spacelens {

/// GUI-only Recycle Bin execution. Prepare on a worker, wait for explicit
/// human confirmation, then recycle one file at a time. Persist happens on
/// the GUI thread. Does not empty the Recycle Bin.
class MaintenanceSession final : public QObject {
    Q_OBJECT

public:
    explicit MaintenanceSession(CleanupReviewController& controller,
                                QObject* parent = nullptr);
    ~MaintenanceSession() override;

    MaintenanceSession(const MaintenanceSession&) = delete;
    MaintenanceSession& operator=(const MaintenanceSession&) = delete;

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] bool isExecuting() const;
    [[nodiscard]] bool awaitingConfirmation() const;
    [[nodiscard]] quint64 progressed() const;
    [[nodiscard]] quint64 total() const;
    [[nodiscard]] MaintenancePlan lastPlan() const;
    [[nodiscard]] MaintenanceReceipt lastReceipt() const;

    /// Snapshot selected review ids and probe them. False if busy.
    bool startPrepare(std::vector<std::uint64_t> selectedIds);
    /// After the confirmation dialog, recycle eligible items sequentially.
    bool startExecute(FileTimeTicks confirmedAt);
    /// Human declined the confirmation, or the review dialog closed.
    void abortIfNotExecuting();
    void cancel();

signals:
    void planReady(bool ok, const QString& message);
    void progressUpdated(quint64 done, quint64 total);
    void finished(bool completed, const QString& message);

private:
    void joinWorker();
    void onPrepareFinished(MaintenancePlan plan, QString message, bool cancelled);
    void onExecuteFinished(MaintenanceReceipt receipt, QString message);

    CleanupReviewController& m_controller;
    std::jthread m_worker;
    mutable std::mutex m_mutex;
    bool m_running = false;
    bool m_executing = false;
    bool m_awaitingConfirm = false;
    quint64 m_progressed = 0;
    quint64 m_total = 0;
    FileTimeTicks m_requestedAt = 0;
    MaintenancePlan m_plan{};
    MaintenanceReceipt m_receipt{};
};

}  // namespace spacelens
