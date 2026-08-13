#include "ui/CleanupReviewDialog.hpp"

#include "app/CleanupRevalidationSession.hpp"
#include "core/CleanupPlan.hpp"
#include "core/CleanupReview.hpp"
#include "core/CleanupReviewStore.hpp"
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
    return QStringLiteral("[%1] %2\n  %3 · %4%5%6")
        .arg(QString::fromUtf8(toString(item.kind)))
        .arg(QString::fromStdWString(item.path))
        .arg(QString::fromStdString(SizeFormatter::format(item.sizeAtSelection)))
        .arg(QString::fromUtf8(toString(item.validation.state)))
        .arg(reasonText)
        .arg(overlapSuppressed ? QStringLiteral(" · overlap suppressed")
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
               "Validation: %11\n"
               "Reasons: %12\n"
               "Identity matched: %13\n"
               "Direct metadata unchanged: %14\n"
               "Recursive evidence revalidated: %15\n"
               "Overlap: %16\n"
               "Diffs: %17"
               "%18\n\n"
               "Planning only — this is not permission to delete or move.")
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

}  // namespace

CleanupReviewDialog::CleanupReviewDialog(CleanupReviewController& controller,
                                         CleanupRevalidationSession& session,
                                         QWidget* parent)
    : QDialog(parent)
    , m_controller(controller)
    , m_session(session)
{
    setWindowTitle(QStringLiteral("Cleanup Review"));
    resize(860, 560);

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
        QStringLiteral("Planning only — no files will be deleted or moved."),
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
    auto* clearBtn = new QPushButton(QStringLiteral("Clear Review"), this);
    auto* copyBtn = new QPushButton(QStringLiteral("Copy Plan"), this);
    auto* exportBtn = new QPushButton(QStringLiteral("Export JSON"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(m_revalidateButton);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_refreshEvidenceButton);
    buttons->addWidget(m_openButton);
    buttons->addWidget(m_revealButton);
    buttons->addWidget(m_removeButton);
    buttons->addWidget(clearBtn);
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
    connect(clearBtn, &QPushButton::clicked, this,
            &CleanupReviewDialog::onClear);
    connect(copyBtn, &QPushButton::clicked, this,
            &CleanupReviewDialog::onCopyPlan);
    connect(exportBtn, &QPushButton::clicked, this,
            &CleanupReviewDialog::onExportJson);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(&m_session, &CleanupRevalidationSession::progressUpdated, this,
            &CleanupReviewDialog::onRevalidationProgress);
    connect(&m_session, &CleanupRevalidationSession::finished, this,
            &CleanupReviewDialog::onRevalidationFinished);

    if (m_session.isRunning()) {
        m_progress->setVisible(true);
        m_progress->setRange(0, static_cast<int>(m_session.total()));
        m_progress->setValue(static_cast<int>(m_session.probed()));
        showStatus(QStringLiteral("Revalidating metadata…"));
    }

    refresh();
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
    m_summary->setText(
        QStringLiteral(
            "Selected: %1 items — Unique logical size: %2\n%3")
            .arg(review.size())
            .arg(QString::fromStdString(
                SizeFormatter::format(review.totalLogicalSize())))
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
    m_session.cancel();
    showStatus(QStringLiteral("Cancelling revalidation…"));
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
    const bool running = m_session.isRunning();
    const bool hasItems = !m_controller.review().empty();
    const bool single = singleSelectedId().has_value();
    const bool any = !selectedIds().empty();
    m_revalidateButton->setEnabled(!running && hasItems);
    m_cancelButton->setEnabled(running);
    m_refreshEvidenceButton->setEnabled(!running && single);
    m_openButton->setEnabled(single);
    m_revealButton->setEnabled(single);
    m_removeButton->setEnabled(!running && any);
}

void CleanupReviewDialog::showStatus(const QString& message)
{
    m_status->setText(message);
}

}  // namespace spacelens
