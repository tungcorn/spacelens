#include "app/MaintenanceSession.hpp"

#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/LocationVolume.hpp"
#include "platform/windows/RecycleAdapter.hpp"

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

class ProgressRecycle final : public IRecycleOperation {
public:
    ProgressRecycle(WindowsRecycleAdapter& adapter,
                    MaintenanceSession& session,
                    quint64 total)
        : m_adapter(adapter)
        , m_session(session)
        , m_total(total)
    {
    }

    MaintenanceItemReceipt recycle(const MaintenancePlanItem& item) override
    {
        auto receipt = m_adapter.recycle(item);
        ++m_done;
        const quint64 done = m_done;
        const quint64 total = m_total;
        QMetaObject::invokeMethod(
            &m_session,
            [session = &m_session, done, total]() {
                emit session->progressUpdated(done, total);
            },
            Qt::QueuedConnection);
        return receipt;
    }

private:
    WindowsRecycleAdapter& m_adapter;
    MaintenanceSession& m_session;
    quint64 m_done = 0;
    quint64 m_total = 0;
};

}  // namespace

MaintenanceSession::MaintenanceSession(CleanupReviewController& controller,
                                       QObject* parent)
    : QObject(parent)
    , m_controller(controller)
{
}

MaintenanceSession::~MaintenanceSession()
{
    cancel();
    joinWorker();
    m_controller.setReviewMutationsBlocked(false);
}

bool MaintenanceSession::isRunning() const
{
    std::lock_guard lock(m_mutex);
    return m_running;
}

bool MaintenanceSession::isExecuting() const
{
    std::lock_guard lock(m_mutex);
    return m_executing;
}

bool MaintenanceSession::awaitingConfirmation() const
{
    std::lock_guard lock(m_mutex);
    return m_awaitingConfirm;
}

quint64 MaintenanceSession::progressed() const
{
    std::lock_guard lock(m_mutex);
    return m_progressed;
}

quint64 MaintenanceSession::total() const
{
    std::lock_guard lock(m_mutex);
    return m_total;
}

MaintenancePlan MaintenanceSession::lastPlan() const
{
    std::lock_guard lock(m_mutex);
    return m_plan;
}

MaintenanceReceipt MaintenanceSession::lastReceipt() const
{
    std::lock_guard lock(m_mutex);
    return m_receipt;
}

bool MaintenanceSession::startPrepare(std::vector<std::uint64_t> selectedIds)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
    }
    if (m_controller.reviewMutationsBlocked()) {
        return false;
    }
    joinWorker();

    CleanupReview snapshot = m_controller.review();
    WindowsCleanupMetadataReader probe;
    WindowsVolumeIdentityReader volumes;
    OrdinaryLocationPolicy locationPolicy =
        m_controller.refreshOrdinaryLocations(probe, volumes);
    const FileTimeTicks requestedAt = nowFileTimeTicks();
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
        m_running = true;
        m_executing = false;
        m_awaitingConfirm = false;
        m_progressed = 0;
        m_total = selectedIds.size();
        m_requestedAt = requestedAt;
        m_plan = {};
        m_receipt = {};
    }
    m_controller.setReviewMutationsBlocked(true);

    m_worker = std::jthread([this, snapshot = std::move(snapshot),
                             selectedIds = std::move(selectedIds),
                             locationPolicy = std::move(locationPolicy)](
                                std::stop_token stop) {
        WindowsCleanupMetadataReader reader;
        WindowsRecycleAdapter adapter;
        auto plan = prepareMaintenancePlan(snapshot, selectedIds, reader,
                                           "maintenance-v1", locationPolicy);
        applyRecycleAvailability(
            plan, [&](const MaintenancePlanItem& item, std::string* detail) {
                return adapter.volumeCanRecycle(item.path, item.logicalSize,
                                                detail);
            });
        if (stop.stop_requested()) {
            QMetaObject::invokeMethod(
                this,
                [this, plan = std::move(plan)]() mutable {
                    onPrepareFinished(std::move(plan),
                                      QStringLiteral(
                                          "Recycle Bin preparation cancelled."),
                                      true);
                },
                Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, plan = std::move(plan)]() mutable {
                onPrepareFinished(std::move(plan), {}, false);
            },
            Qt::QueuedConnection);
    });
    return true;
}

bool MaintenanceSession::startExecute(FileTimeTicks confirmedAt)
{
    MaintenancePlan plan;
    FileTimeTicks requestedAt = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_running || m_executing || !m_awaitingConfirm) {
            return false;
        }
        plan = m_plan;
        requestedAt = m_requestedAt;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    {
        std::lock_guard lock(m_mutex);
        if (!m_running || m_executing || !m_awaitingConfirm) {
            return false;
        }
        m_executing = true;
        m_awaitingConfirm = false;
        m_progressed = 0;
        m_total = plan.eligibleCount;
    }

    WindowsCleanupMetadataReader executeProbe;
    WindowsVolumeIdentityReader executeVolumes;
    OrdinaryLocationPolicy executePolicy =
        m_controller.refreshOrdinaryLocations(executeProbe, executeVolumes);

    m_worker = std::jthread([this, plan = std::move(plan), confirmedAt,
                             requestedAt,
                             executePolicy = std::move(executePolicy)](
                                std::stop_token stop) {
        WindowsCleanupMetadataReader reader;
        WindowsRecycleAdapter adapter;
        ProgressRecycle recycle(adapter, *this, plan.eligibleCount);
        auto receipt = executeMaintenancePlan(
            plan, reader, recycle, confirmedAt,
            [&]() { return stop.stop_requested(); }, executePolicy);
        receipt.requestedAt = requestedAt;
        receipt.completedAt = nowFileTimeTicks();
        QMetaObject::invokeMethod(
            this,
            [this, receipt = std::move(receipt)]() mutable {
                onExecuteFinished(std::move(receipt), {});
            },
            Qt::QueuedConnection);
    });
    return true;
}

void MaintenanceSession::abortIfNotExecuting()
{
    bool shouldUnblock = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_executing) {
            if (m_worker.joinable()) {
                m_worker.request_stop();
            }
            return;
        }
        if (m_running) {
            m_running = false;
            m_awaitingConfirm = false;
            shouldUnblock = true;
        }
    }
    if (m_worker.joinable()) {
        m_worker.request_stop();
    }
    if (shouldUnblock) {
        m_controller.setReviewMutationsBlocked(false);
    }
}

void MaintenanceSession::cancel()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
    }
}

void MaintenanceSession::joinWorker()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

void MaintenanceSession::onPrepareFinished(MaintenancePlan plan,
                                           QString message,
                                           bool cancelled)
{
    bool aborted = false;
    {
        std::lock_guard lock(m_mutex);
        m_plan = std::move(plan);
        if (cancelled || !m_running) {
            aborted = !m_running && !cancelled;
            m_running = false;
            m_awaitingConfirm = false;
            m_executing = false;
        } else {
            m_awaitingConfirm = true;
        }
    }
    if (cancelled) {
        m_controller.setReviewMutationsBlocked(false);
        if (message.isEmpty()) {
            message = QStringLiteral("Recycle Bin preparation cancelled.");
        }
        emit planReady(false, message);
        return;
    }
    if (aborted) {
        return;
    }
    if (message.isEmpty()) {
        const auto ready = lastPlan();
        message = QStringLiteral("Prepared %1 eligible of %2 selected file(s).")
                      .arg(ready.eligibleCount)
                      .arg(ready.selectedCount);
    }
    emit planReady(true, message);
}

void MaintenanceSession::onExecuteFinished(MaintenanceReceipt receipt,
                                           QString message)
{
    const auto status = m_controller.recordMaintenance(receipt);
    bool completed = status.ok;
    if (!status.ok) {
        message = QString::fromStdString(
            status.message.empty()
                ? "Failed to persist the Recycle Bin receipt."
                : status.message);
    } else if (message.isEmpty()) {
        message = QStringLiteral(
                      "Recycle Bin: %1 recycled, %2 blocked, %3 failed, "
                      "%4 cancelled. Recycled logical size: %5 bytes. "
                      "The Recycle Bin still occupies storage.")
                      .arg(receipt.recycled)
                      .arg(receipt.blocked)
                      .arg(receipt.failed)
                      .arg(receipt.cancelled)
                      .arg(receipt.recycledLogicalBytes);
        if (receipt.unexpectedPermanentRemoval) {
            message += QStringLiteral(
                " WARNING: an item left the source path without Recycle Bin "
                "evidence. Remaining files were not attempted.");
        }
    }
    {
        std::lock_guard lock(m_mutex);
        m_receipt = std::move(receipt);
        m_running = false;
        m_executing = false;
        m_awaitingConfirm = false;
    }
    m_controller.setReviewMutationsBlocked(false);
    emit finished(completed, message);
}

}  // namespace spacelens
