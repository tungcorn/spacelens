#include "ui/CleanupReviewDialog.hpp"

#include "app/CleanupRevalidationSession.hpp"
#include "app/MaintenanceSession.hpp"
#include "ui/MaintenanceHistoryDialog.hpp"
#include "core/CleanupPlan.hpp"
#include "core/CleanupReview.hpp"
#include "core/CleanupReviewStore.hpp"
#include "core/Maintenance.hpp"
#include "core/SizeFormatter.hpp"
#include "platform/windows/ExplorerIntegration.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QTextEdit>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <map>
#include <unordered_set>

namespace spacelens {
namespace {

std::wstring currentUserProfile()
{
    return QDir::homePath().toStdWString();
}

QString formatStateCounts(const CleanupReview& review)
{
    std::map<std::string, int> counts;
    for (const auto& item : review.items()) {
        ++counts[toString(item.validation.state)];
    }
    QStringList parts;
    for (const auto& [name, count] : counts) {
        parts << QStringLiteral("%1 %2")
                     .arg(count)
                     .arg(QString::fromStdString(name));
    }
    return parts.join(QStringLiteral(" · "));
}

QString itemLabel(const CleanupCandidate& item, bool overlapSuppressed)
{
    const auto reasons = validationReasonNames(item.validation.reasons);
    QString reasonText;
    if (!reasons.empty() && reasons.front() != "None") {
        reasonText = QStringLiteral(" — %1").arg(
            QString::fromStdString(reasons.front()));
    }
    return QStringLiteral("[%1] %2\n  %3 · %4%5%6%7")
        .arg(QString::fromUtf8(toString(item.kind)))
        .arg(QString::fromStdWString(item.path))
        .arg(QString::fromStdString(SizeFormatter::format(item.sizeAtSelection)))
        .arg(QString::fromUtf8(toString(item.validation.state)))
        .arg(reasonText)
        .arg(overlapSuppressed ? QStringLiteral(" · overlap suppressed")
                               : QString())
        .arg(item.lifecycle == CleanupItemLifecycle::Recycled
                 ? QStringLiteral(" · Recycled")
                 : QString());
}

QString itemDetails(const CleanupCandidate& item, bool overlapSuppressed)
{
    const auto reasons = validationReasonNames(item.validation.reasons);
    QStringList reasonList;
    for (const auto& reason : reasons) {
        reasonList << QString::fromStdString(reason);
    }

    QStringList diffs;
    for (const auto& diff : item.validation.diffs) {
        diffs << QStringLiteral("%1: %2 → %3")
                     .arg(QString::fromUtf8(toString(diff.kind)))
                     .arg(QString::fromStdString(diff.captured))
                     .arg(QString::fromStdString(diff.current));
    }

    QString directoryNote;
    if (item.kind != ItemKind::File) {
        directoryNote = QStringLiteral(
            "\nDirectory note: object identity/direct metadata can be checked, "
            "but recursive contents were not revalidated.");
    }

    return QStringLiteral(
               "Path: %1\n"
               "Kind: %2\n"
               "Captured size: %3\n"
               "Classification: %4 (%5)\n"
               "Reclaimability: %6\n"
               "Candidate strength: %7\n"
               "Captured safety: %8\n"
               "Current safety: %9\n"
               "Identity: %10\n"
               "Lifecycle: %11\n"
               "Validation: %12\n"
               "Reasons: %13\n"
               "Identity matched: %14\n"
               "Direct metadata unchanged: %15\n"
               "Recursive evidence revalidated: %16\n"
               "Overlap: %17\n"
               "Diffs: %18"
               "%19\n\n"
               "Recycle Bin moves require a fresh preflight, explicit confirmation, "
               "and a final identity guard. This screen does not permanently delete.")
        .arg(QString::fromStdWString(item.path))
        .arg(QString::fromUtf8(toString(item.kind)))
        .arg(QString::fromStdString(SizeFormatter::format(item.sizeAtSelection)))
        .arg(QString::fromUtf8(toString(item.classification.category)))
        .arg(QString::fromUtf8(toString(item.classification.confidence)))
        .arg(QString::fromUtf8(toString(item.capturedReclaimability)))
        .arg(QString::fromUtf8(toString(item.capturedCandidateStrength)))
        .arg(QString::fromUtf8(toString(item.capturedSafety)))
        .arg(QString::fromUtf8(toString(item.currentEvidence.safety)))
        .arg(QString::fromUtf8(toString(identityOf(item).source)))
        .arg(QString::fromUtf8(toString(item.lifecycle)))
        .arg(QString::fromUtf8(toString(item.validation.state)))
        .arg(reasonList.join(QStringLiteral(", ")))
        .arg(item.validation.objectIdentityMatched ? QStringLiteral("yes")
                                                   : QStringLiteral("no"))
        .arg(item.validation.directMetadataUnchanged ? QStringLiteral("yes")
                                                     : QStringLiteral("no"))
        .arg(item.validation.recursiveEvidenceRevalidated
                 ? QStringLiteral("yes")
                 : QStringLiteral("no"))
        .arg(overlapSuppressed
                 ? QStringLiteral("suppressed by selected ancestor")
                 : QStringLiteral("counted in unique size"))
        .arg(diffs.isEmpty() ? QStringLiteral("none")
                             : diffs.join(QStringLiteral("; ")))
        .arg(directoryNote);
}

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

CleanupReviewDialog::CleanupReviewDialog(CleanupReviewController& controller,
                                         CleanupRevalidationSession& session,
                                         MaintenanceSession& maintenance,
                                         QWidget* parent)
    : QDialog(parent)
    , m_controller(controller)
    , m_session(session)
    , m_maintenance(maintenance)
{
    setWindowTitle(QStringLiteral("Cleanup Review"));
    resize(960, 560);

    auto* root = new QVBoxLayout(this);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_details = new QTextEdit(this);
    m_details->setReadOnly(true);
    splitter->addWidget(m_list);
    splitter->addWidget(m_details);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto* note = new QLabel(
        QStringLiteral(
            "Cleanup Review is planning-only until you confirm Move to Recycle "
            "Bin. That path is Recycle Bin only — not permanent deletion."),
        this);
    note->setStyleSheet(QStringLiteral("color: #666;"));
    root->addWidget(note);

    m_redact = new QCheckBox(
        QStringLiteral("Redact %USERPROFILE% paths in copy/export"), this);
    m_redact->setChecked(true);
    root->addWidget(m_redact);

    auto* buttons = new QHBoxLayout();
    m_revalidateButton = new QPushButton(QStringLiteral("Revalidate All"), this);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    m_refreshEvidenceButton =
        new QPushButton(QStringLiteral("Refresh Evidence"), this);
    m_openButton = new QPushButton(QStringLiteral("Open"), this);
    m_revealButton = new QPushButton(QStringLiteral("Show in Explorer"), this);
    m_removeButton = new QPushButton(QStringLiteral("Remove from Review"), this);
    m_clearButton = new QPushButton(QStringLiteral("Clear Review"), this);
    m_recycleButton =
        new QPushButton(QStringLiteral("Move to Recycle Bin…"), this);
    m_historyButton =
        new QPushButton(QStringLiteral("Maintenance History"), this);
    auto* copyBtn = new QPushButton(QStringLiteral("Copy Plan"), this);
    auto* exportBtn = new QPushButton(QStringLiteral("Export JSON"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(m_revalidateButton);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_refreshEvidenceButton);
    buttons->addWidget(m_openButton);
    buttons->addWidget(m_revealButton);
    buttons->addWidget(m_removeButton);
    buttons->addWidget(m_clearButton);
    buttons->addWidget(m_recycleButton);
    buttons->addWidget(m_historyButton);
    buttons->addWidget(copyBtn);
    buttons->addWidget(exportBtn);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    connect(m_list, &QListWidget::itemSelectionChanged, this,
            &CleanupReviewDialog::onSelectionChanged);
    connect(m_revalidateButton, &QPushButton::clicked, this,
            &CleanupReviewDialog::onRevalidateAll);
    connect(m_cancelButton, &QPushButton::clicked, this,
            &CleanupReviewDialog::onCancelRevalidate);
    connect(m_refreshEvidenceButton, &QPushButton::clicked, this,
            &CleanupReviewDialog::onRefreshEvidence);
    connect(m_openButton, &QPushButton::clicked, this,
            &CleanupReviewDialog::onOpen);
    connect(m_revealButton, &QPushButton::clicked, this,
            &CleanupReviewDialog::onReveal);
    connect(m_removeButton, &QPushButton::clicked, this,
            &CleanupReviewDialog::onRemoveSelected);
    connect(m_clearButton, &QPushButton::clicked, this,
            &CleanupReviewDialog::onClear);
    connect(m_recycleButton, &QPushButton::clicked, this,
            &CleanupReviewDialog::onMoveToRecycleBin);
    connect(m_historyButton, &QPushButton::clicked, this,
            &CleanupReviewDialog::onMaintenanceHistory);
    connect(copyBtn, &QPushButton::clicked, this,
            &CleanupReviewDialog::onCopyPlan);
    connect(exportBtn, &QPushButton::clicked, this,
            &CleanupReviewDialog::onExportJson);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(&m_session, &CleanupRevalidationSession::progressUpdated, this,
            &CleanupReviewDialog::onRevalidationProgress);
    connect(&m_session, &CleanupRevalidationSession::finished, this,
            &CleanupReviewDialog::onRevalidationFinished);
    connect(&m_maintenance, &MaintenanceSession::planReady, this,
            &CleanupReviewDialog::onMaintenancePlanReady);
    connect(&m_maintenance, &MaintenanceSession::progressUpdated, this,
            &CleanupReviewDialog::onMaintenanceProgress);
    connect(&m_maintenance, &MaintenanceSession::finished, this,
            &CleanupReviewDialog::onMaintenanceFinished);

    if (m_session.isRunning()) {
        m_progress->setVisible(true);
        m_progress->setRange(0, static_cast<int>(m_session.total()));
        m_progress->setValue(static_cast<int>(m_session.probed()));
        showStatus(QStringLiteral("Revalidating metadata…"));
    }
    if (m_maintenance.isExecuting()) {
        m_progress->setVisible(true);
        m_progress->setRange(0, static_cast<int>(m_maintenance.total()));
        m_progress->setValue(static_cast<int>(m_maintenance.progressed()));
        showStatus(QStringLiteral("Moving eligible files to the Recycle Bin…"));
    }

    refresh();
}

CleanupReviewDialog::~CleanupReviewDialog()
{
    m_maintenance.abortIfNotExecuting();
}

void CleanupReviewDialog::refresh()
{
    const auto selected = selectedIds();
    m_list->clear();
    const auto& review = m_controller.review();
    const auto plan = m_controller.buildCleanupPlan();
    std::unordered_set<std::uint64_t> suppressed;
    for (const auto& planned : plan.items) {
        if (planned.suppressed) {
            suppressed.insert(planned.id);
        }
    }
    const auto counts = formatStateCounts(review);
    std::uint64_t recycled = 0;
    for (const auto& item : review.items()) {
        if (item.lifecycle == CleanupItemLifecycle::Recycled) {
            ++recycled;
        }
    }
    m_summary->setText(
        QStringLiteral(
            "Selected: %1 items — Unique logical size: %2 — Recycled: %3\n%4")
            .arg(review.size())
            .arg(QString::fromStdString(
                SizeFormatter::format(review.totalLogicalSize())))
            .arg(recycled)
            .arg(counts.isEmpty() ? QStringLiteral("No validation states yet.")
                                  : counts));

    for (const auto& item : review.items()) {
        auto* row = new QListWidgetItem(
            itemLabel(item, suppressed.contains(item.id)), m_list);
        row->setData(Qt::UserRole, QVariant::fromValue(item.id));
        row->setData(Qt::UserRole + 1, QString::fromStdWString(item.path));
        row->setData(Qt::UserRole + 2, suppressed.contains(item.id));
        if (std::find(selected.begin(), selected.end(), item.id) !=
            selected.end()) {
            row->setSelected(true);
        }
    }
    if (m_list->selectedItems().isEmpty() && m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
    onSelectionChanged();
    updateActionState();
}

void CleanupReviewDialog::onRemoveSelected()
{
    for (const auto id : selectedIds()) {
        const auto status = m_controller.removeById(id);
        if (!status.ok) {
            showStatus(QString::fromStdString(status.message));
            refresh();
            return;
        }
    }
    refresh();
}

void CleanupReviewDialog::onClear()
{
    const auto status = m_controller.clear();
    if (!status.ok) {
        showStatus(QString::fromStdString(status.message));
        return;
    }
    refresh();
}

void CleanupReviewDialog::onCopyPlan()
{
    CleanupPlanOptions options;
    if (m_redact->isChecked()) {
        options.userProfilePath = currentUserProfile();
    }
    QApplication::clipboard()->setText(QString::fromStdString(
        m_controller.buildCleanupPlan(options).toText(options)));
    showStatus(QStringLiteral("Cleanup plan copied. Planning only."));
}

void CleanupReviewDialog::onExportJson()
{
    const auto path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Cleanup Plan"), {},
        QStringLiteral("JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    CleanupPlanOptions options;
    if (m_redact->isChecked()) {
        options.userProfilePath = currentUserProfile();
    }
    const auto json = m_controller.buildCleanupPlan(options).toJson(options);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("Could not open export file."));
        return;
    }
    file.write(QByteArray::fromStdString(json));
    if (!file.commit()) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("Could not write export file."));
        return;
    }
    showStatus(QStringLiteral("Exported planning-only JSON."));
}

void CleanupReviewDialog::onOpen()
{
    const auto id = singleSelectedId();
    if (!id) {
        return;
    }
    const auto item = m_controller.review().findById(*id);
    if (!item) {
        return;
    }
    if (!openWithDefaultApp(item->path) && !openInExplorer(item->path)) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("Could not open path."));
    }
}

void CleanupReviewDialog::onReveal()
{
    const auto id = singleSelectedId();
    if (!id) {
        return;
    }
    const auto item = m_controller.review().findById(*id);
    if (!item) {
        return;
    }
    if (!revealInExplorer(item->path) && !openInExplorer(item->path)) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("Could not reveal path."));
    }
}

void CleanupReviewDialog::onRefreshEvidence()
{
    const auto id = singleSelectedId();
    if (!id) {
        return;
    }
    const auto status = m_controller.refreshEvidence(*id);
    if (!status.ok) {
        showStatus(QString::fromStdString(status.message));
        return;
    }
    if (!status.changed) {
        showStatus(QStringLiteral(
            "Refresh Evidence needs current metadata. Revalidate first. "
            "This never authorizes deletion."));
        return;
    }
    showStatus(QStringLiteral(
        "Captured evidence replaced with current metadata. "
        "This is not permission to delete."));
    refresh();
}

void CleanupReviewDialog::onRevalidateAll()
{
    if (!m_session.start()) {
        showStatus(QStringLiteral("Revalidation is already running."));
        return;
    }
    m_progress->setVisible(true);
    m_progress->setRange(0, static_cast<int>(m_controller.review().size()));
    m_progress->setValue(0);
    showStatus(QStringLiteral("Revalidating metadata…"));
    updateActionState();
}

void CleanupReviewDialog::onCancelRevalidate()
{
    if (m_maintenance.isRunning()) {
        m_maintenance.cancel();
        showStatus(QStringLiteral("Cancelling Recycle Bin operation…"));
        return;
    }
    m_session.cancel();
    showStatus(QStringLiteral("Cancelling revalidation…"));
}

void CleanupReviewDialog::onMoveToRecycleBin()
{
    if (m_session.isRunning() || m_maintenance.isRunning()) {
        showStatus(QStringLiteral(
            "Wait for the current review or Recycle Bin operation to finish."));
        return;
    }
    const auto ids = selectedIds();
    if (ids.empty()) {
        showStatus(QStringLiteral(
            "Select one or more review items before moving to the Recycle Bin."));
        return;
    }
    if (!m_maintenance.startPrepare(ids)) {
        showStatus(QStringLiteral(
            "Could not start Recycle Bin preflight. Review may be busy."));
        return;
    }
    m_progress->setVisible(true);
    m_progress->setRange(0, 0);
    showStatus(QStringLiteral(
        "Checking selected files for Recycle Bin eligibility…"));
    updateActionState();
}

void CleanupReviewDialog::onMaintenancePlanReady(bool ok, const QString& message)
{
    m_progress->setVisible(false);
    showStatus(message);
    updateActionState();
    if (!ok) {
        return;
    }
    confirmAndExecute();
}

void CleanupReviewDialog::confirmAndExecute()
{
    const auto plan = m_maintenance.lastPlan();
    if (plan.eligibleCount == 0) {
        QStringList blocked;
        for (const auto& item : plan.items) {
            if (!item.eligible) {
                blocked << QStringLiteral("%1 — %2")
                               .arg(QString::fromStdWString(item.path))
                               .arg(QString::fromUtf8(toString(item.blockReason)));
            }
        }
        QMessageBox::information(
            this, QStringLiteral("Move to Recycle Bin"),
            QStringLiteral(
                "No selected files are eligible to move to the Recycle Bin.\n\n"
                "Selected: %1\nBlocked: %2\nSelected logical size: %3\n\n%4")
                .arg(plan.selectedCount)
                .arg(plan.blockedCount)
                .arg(QString::fromStdString(
                    SizeFormatter::format(plan.selectedLogicalBytes)))
                .arg(blocked.join(QStringLiteral("\n"))));
        m_maintenance.abortIfNotExecuting();
        updateActionState();
        return;
    }

    QStringList blocked;
    for (const auto& item : plan.items) {
        if (!item.eligible) {
            blocked << QStringLiteral("%1 — %2")
                           .arg(QString::fromStdWString(item.path))
                           .arg(QString::fromUtf8(toString(item.blockReason)));
        }
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Move to Recycle Bin"));
    box.setText(QStringLiteral("Move %1 eligible file(s) to the Recycle Bin?")
                    .arg(plan.eligibleCount));
    box.setInformativeText(
        QStringLiteral(
            "Selected files: %1 — %2\n"
            "Eligible files: %3 — %4\n"
            "Blocked files: %5\n\n"
            "Files will be moved to the Windows Recycle Bin.\n"
            "This is not permanent deletion.\n"
            "Recycle Bin contents still occupy disk space.\n"
            "SpaceLens will not empty the Recycle Bin.\n\n"
            "Blocked reasons:\n%6")
            .arg(plan.selectedCount)
            .arg(QString::fromStdString(
                SizeFormatter::format(plan.selectedLogicalBytes)))
            .arg(plan.eligibleCount)
            .arg(QString::fromStdString(
                SizeFormatter::format(plan.eligibleLogicalBytes)))
            .arg(plan.blockedCount)
            .arg(blocked.isEmpty() ? QStringLiteral("(none)")
                                   : blocked.join(QStringLiteral("\n"))));
    auto* moveButton =
        box.addButton(QStringLiteral("Move eligible files to Recycle Bin"),
                      QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != moveButton) {
        m_maintenance.abortIfNotExecuting();
        showStatus(QStringLiteral("Recycle Bin operation cancelled."));
        updateActionState();
        return;
    }
    if (!m_maintenance.startExecute(nowFileTimeTicks())) {
        showStatus(QStringLiteral("Could not start the Recycle Bin operation."));
        updateActionState();
        return;
    }
    m_progress->setVisible(true);
    m_progress->setRange(0, static_cast<int>(plan.eligibleCount));
    m_progress->setValue(0);
    showStatus(QStringLiteral("Moving eligible files to the Recycle Bin…"));
    updateActionState();
}

void CleanupReviewDialog::onMaintenanceProgress(quint64 done, quint64 total)
{
    m_progress->setVisible(true);
    m_progress->setRange(0, static_cast<int>(total));
    m_progress->setValue(static_cast<int>(done));
}

void CleanupReviewDialog::onMaintenanceHistory()
{
    MaintenanceHistoryDialog dialog(m_controller, this);
    dialog.exec();
}

void CleanupReviewDialog::showCompletionSummary()
{
    const auto receipt = m_maintenance.lastReceipt();
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Recycle Bin result"));
    box.setIcon(receipt.unexpectedPermanentRemoval ||
                        receipt.status == MaintenanceOperationStatus::Uncertain
                    ? QMessageBox::Warning
                    : QMessageBox::Information);
    box.setText(QStringLiteral("Recycle Bin operation %1")
                    .arg(QString::fromUtf8(toString(receipt.status))));
    box.setInformativeText(
        QStringLiteral(
            "Recycled: %1\n"
            "Blocked before mutation: %2\n"
            "Failed: %3\n"
            "Cancelled: %4\n"
            "Uncertain: %5\n"
            "Recycled logical size: %6\n\n"
            "The Recycle Bin still occupies storage. This is not space freed.")
            .arg(receipt.recycled)
            .arg(receipt.blocked)
            .arg(receipt.failed)
            .arg(receipt.cancelled)
            .arg(receipt.uncertain)
            .arg(QString::fromStdString(
                SizeFormatter::format(receipt.recycledLogicalBytes))));
    auto* details = box.addButton(QStringLiteral("View details"),
                                  QMessageBox::ActionRole);
    box.addButton(QStringLiteral("Close"), QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() == details) {
        MaintenanceHistoryDialog dialog(m_controller, this);
        dialog.selectOperation(receipt.operationId);
        dialog.exec();
    }
}

void CleanupReviewDialog::onMaintenanceFinished(bool completed,
                                               const QString& message)
{
    m_progress->setVisible(false);
    showStatus(message);
    if (!completed) {
        QMessageBox::warning(this, QStringLiteral("Move to Recycle Bin"),
                             message);
    } else {
        showCompletionSummary();
    }
    refresh();
}

void CleanupReviewDialog::onSelectionChanged()
{
    const auto id = singleSelectedId();
    if (!id) {
        m_details->setPlainText(
            selectedIds().empty()
                ? QStringLiteral("Select an item to inspect captured and "
                                 "current evidence.")
                : QStringLiteral("Select a single item to inspect details."));
        updateActionState();
        return;
    }
    const auto item = m_controller.review().findById(*id);
    if (!item) {
        m_details->clear();
        updateActionState();
        return;
    }
    bool overlapSuppressed = false;
    if (QListWidgetItem* row = m_list->currentItem()) {
        overlapSuppressed = row->data(Qt::UserRole + 2).toBool();
    }
    m_details->setPlainText(itemDetails(*item, overlapSuppressed));
    updateActionState();
}

void CleanupReviewDialog::onRevalidationProgress(quint64 probed, quint64 total)
{
    m_progress->setVisible(true);
    m_progress->setRange(0, static_cast<int>(total));
    m_progress->setValue(static_cast<int>(probed));
}

void CleanupReviewDialog::onRevalidationFinished(bool completed,
                                                const QString& message)
{
    m_progress->setVisible(false);
    showStatus(message);
    if (!completed && message.contains(QStringLiteral("discarded"))) {
        showStatus(message);
    }
    refresh();
}

std::vector<std::uint64_t> CleanupReviewDialog::selectedIds() const
{
    std::vector<std::uint64_t> ids;
    for (QListWidgetItem* row : m_list->selectedItems()) {
        ids.push_back(row->data(Qt::UserRole).toULongLong());
    }
    return ids;
}

std::optional<std::uint64_t> CleanupReviewDialog::singleSelectedId() const
{
    const auto ids = selectedIds();
    if (ids.size() != 1) {
        return std::nullopt;
    }
    return ids.front();
}

void CleanupReviewDialog::updateActionState()
{
    const bool revalidating = m_session.isRunning();
    const bool maintaining = m_maintenance.isRunning();
    const bool busy = revalidating || maintaining;
    const bool hasItems = !m_controller.review().empty();
    const bool single = singleSelectedId().has_value();
    const bool any = !selectedIds().empty();
    m_revalidateButton->setEnabled(!busy && hasItems);
    m_cancelButton->setEnabled(busy);
    m_refreshEvidenceButton->setEnabled(!busy && single);
    m_openButton->setEnabled(single);
    m_revealButton->setEnabled(single);
    m_removeButton->setEnabled(!busy && any);
    m_clearButton->setEnabled(!busy && hasItems);
    m_recycleButton->setEnabled(!busy && any);
    if (m_historyButton != nullptr) {
        m_historyButton->setEnabled(!m_maintenance.isExecuting());
    }
}

void CleanupReviewDialog::showStatus(const QString& message)
{
    m_status->setText(message);
}

}  // namespace spacelens
