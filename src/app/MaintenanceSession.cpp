#include "app/MaintenanceSession.hpp"

#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/LocationVolume.hpp"
#include "platform/windows/RecycleAdapter.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>

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

class SessionJournal final : public IMaintenanceJournal {
public:
    SessionJournal(MaintenanceSession& session, CleanupReviewController& controller)
        : m_session(session)
        , m_controller(controller)
    {
    }

    bool checkpointItem(std::uint64_t operationId,
                        const MaintenanceItemReceipt& item,
                        ByteSize recycledLogicalBytes) override
    {
        if (QThread::currentThread() == m_session.thread()) {
            return m_controller
                .checkpointMaintenance(operationId, item, recycledLogicalBytes)
                .ok;
        }
        bool ok = false;
        QMetaObject::invokeMethod(
            &m_session,
            [this, operationId, item, recycledLogicalBytes, &ok]() {
                ok = m_controller
                         .checkpointMaintenance(operationId, item,
                                                recycledLogicalBytes)
                         .ok;
            },
            Qt::BlockingQueuedConnection);
        return ok;
    }

private:
    MaintenanceSession& m_session;
    CleanupReviewController& m_controller;
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
    std::vector<MaintenanceReceipt> history;
    if (!m_controller.loadMaintenanceReceipts(history).ok) {
        return false;
    }
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
        m_operationId = 0;
        m_plan = {};
        m_receipt = {};
    }
    m_controller.setReviewMutationsBlocked(true);

    startWorker([this, snapshot = std::move(snapshot),
                 selectedIds = std::move(selectedIds),
                 locationPolicy = std::move(locationPolicy),
                 history = std::move(history),
                 requestedAt](std::stop_token stop) {
        WindowsCleanupMetadataReader reader;
        WindowsRecycleAdapter adapter;
        auto plan = prepareMaintenancePlan(snapshot, selectedIds, reader,
                                           "maintenance-v2", locationPolicy,
                                           history);
        plan.preparedAt = requestedAt;
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
    FileTimeTicks preparedAt = 0;
    bool running = false;
    bool executing = false;
    bool awaiting = false;
    {
        std::lock_guard lock(m_mutex);
        running = m_running;
        executing = m_executing;
        awaiting = m_awaitingConfirm;
        plan = m_plan;
        requestedAt = m_requestedAt;
        preparedAt = m_plan.preparedAt;
    }
    const auto now = nowFileTimeTicks();
    const auto gate = evaluateMaintenanceConfirmGate(
        running, executing, awaiting, preparedAt, now);
    if (gate == MaintenanceConfirmGate::StalePlan) {
        expirePreparedPlan();
        return false;
    }
    if (gate != MaintenanceConfirmGate::Ok) {
        return false;
    }
    joinWorker();
    {
        std::lock_guard lock(m_mutex);
        running = m_running;
        executing = m_executing;
        awaiting = m_awaitingConfirm;
        preparedAt = m_plan.preparedAt;
    }
    const auto confirmGate = evaluateMaintenanceConfirmGate(
        running, executing, awaiting, preparedAt, nowFileTimeTicks());
    if (confirmGate == MaintenanceConfirmGate::StalePlan) {
        expirePreparedPlan();
        return false;
    }
    if (confirmGate != MaintenanceConfirmGate::Ok) {
        return false;
    }

    MaintenanceReceipt seed;
    seed.requestedAt = requestedAt;
    seed.confirmedAt = confirmedAt;
    seed.status = MaintenanceOperationStatus::Executing;
    seed.selectedCount = plan.selectedCount;
    seed.eligibleCount = plan.eligibleCount;
    seed.selectedLogicalBytes = plan.selectedLogicalBytes;
    seed.eligibleLogicalBytes = plan.eligibleLogicalBytes;
    const auto began = m_controller.beginMaintenance(seed);
    if (!began.ok) {
        {
            std::lock_guard lock(m_mutex);
            m_running = false;
            m_executing = false;
            m_awaitingConfirm = false;
        }
        m_controller.setReviewMutationsBlocked(false);
        emit finished(false,
                      QString::fromStdString(
                          began.message.empty()
                              ? "Failed to persist the Recycle Bin operation."
                              : began.message));
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_executing = true;
        m_awaitingConfirm = false;
        m_progressed = 0;
        m_total = plan.eligibleCount;
        m_operationId = seed.operationId;
    }

    WindowsCleanupMetadataReader executeProbe;
    WindowsVolumeIdentityReader executeVolumes;
    OrdinaryLocationPolicy executePolicy =
        m_controller.refreshOrdinaryLocations(executeProbe, executeVolumes);

    startWorker([this, plan = std::move(plan), confirmedAt, requestedAt,
                 operationId = seed.operationId,
                 executePolicy = std::move(executePolicy)](std::stop_token stop) {
        WindowsCleanupMetadataReader reader;
        WindowsRecycleAdapter adapter;
        ProgressRecycle recycle(adapter, *this, plan.eligibleCount);
        SessionJournal journal(*this, m_controller);
        auto receipt = executeMaintenancePlan(
            plan, reader, recycle, confirmedAt,
            [&]() { return stop.stop_requested(); }, executePolicy, &journal,
            operationId,
            [&](const MaintenancePlanItem& item, std::string* detail) {
                return adapter.volumeCanRecycle(item.path, item.logicalSize,
                                                detail);
            });
        receipt.requestedAt = requestedAt;
        receipt.operationId = operationId;
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

void MaintenanceSession::startWorker(std::function<void(std::stop_token)> body)
{
    m_workerFinished.store(false, std::memory_order_release);
    try {
        m_worker = std::jthread(
            [this, body = std::move(body)](std::stop_token stop) {
                struct Done {
                    std::atomic<bool>& flag;
                    ~Done() noexcept
                    {
                        flag.store(true, std::memory_order_release);
                    }
                } done{m_workerFinished};
                body(stop);
            });
    } catch (...) {
        m_workerFinished.store(true, std::memory_order_release);
        throw;
    }
}

void MaintenanceSession::expirePreparedPlan()
{
    {
        std::lock_guard lock(m_mutex);
        m_running = false;
        m_executing = false;
        m_awaitingConfirm = false;
    }
    m_controller.setReviewMutationsBlocked(false);
    emit finished(
        false,
        QStringLiteral(
            "The prepared Recycle Bin plan expired. Prepare again."));
}

void MaintenanceSession::joinWorker()
{
    if (!m_worker.joinable()) {
        return;
    }
    m_worker.request_stop();
    // joinable() stays true until join(). Pump until the worker has finished
    // every BlockingQueuedConnection persist so join() cannot deadlock the GUI.
    if (QThread::currentThread() == thread()) {
        while (!m_workerFinished.load(std::memory_order_acquire)) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents,
                                            50);
        }
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    }
    if (m_worker.joinable()) {
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
    const auto completedWrite = m_controller.completeMaintenance(receipt);
    bool completed = completedWrite.ok;
    if (!completedWrite.ok) {
        if (receipt.status == MaintenanceOperationStatus::Completed ||
            receipt.status == MaintenanceOperationStatus::Executing) {
            receipt.status = MaintenanceOperationStatus::Uncertain;
        }
        message = QString::fromStdString(
            completedWrite.message.empty()
                ? "Failed to persist the Recycle Bin completion."
                : completedWrite.message);
    } else if (message.isEmpty()) {
        message = QStringLiteral(
                      "Recycled: %1\nBlocked before mutation: %2\nFailed: %3\n"
                      "Cancelled: %4\nUncertain: %5\nRecycled logical size: %6 "
                      "bytes.\nThe Recycle Bin still occupies storage.")
                      .arg(receipt.recycled)
                      .arg(receipt.blocked)
                      .arg(receipt.failed)
                      .arg(receipt.cancelled)
                      .arg(receipt.uncertain)
                      .arg(receipt.recycledLogicalBytes);
        if (receipt.unexpectedPermanentRemoval) {
            message += QStringLiteral(
                "\nWARNING: an item left the source path without Recycle Bin "
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
