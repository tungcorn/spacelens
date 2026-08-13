#include "ui/DuplicateFilesDialog.hpp"

#include "core/CleanupRevalidation.hpp"
#include "core/SizeFormatter.hpp"
#include "core/SizeParse.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/ExplorerIntegration.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace spacelens {
namespace {

QString groupLabel(const DuplicateGroup& group)
{
    const auto& first = group.instances.empty() || group.instances.front().paths.empty()
                            ? std::wstring{}
                            : group.instances.front().paths.front().path;
    return QStringLiteral("%1\n  %2 × %3 independent · redundant %4 · %5")
        .arg(QString::fromStdWString(first))
        .arg(QString::fromStdString(SizeFormatter::format(group.logicalSize)))
        .arg(group.distinctIdentityCount)
        .arg(QString::fromStdString(
            SizeFormatter::format(group.potentialRedundantLogicalBytes)))
        .arg(QString::fromStdString(group.verification));
}

QString groupDetails(const DuplicateGroup& group)
{
    QStringList lines;
    lines << QStringLiteral("Content SHA-256: %1")
                 .arg(group.contentSha256Hex.empty()
                          ? QStringLiteral("(same file identity; no content hash)")
                          : QString::fromStdString(group.contentSha256Hex));
    lines << QStringLiteral("Logical size: %1").arg(
        QString::fromStdString(SizeFormatter::format(group.logicalSize)));
    lines << QStringLiteral("Verification: %1")
                 .arg(QString::fromStdString(group.verification));
    lines << QStringLiteral("Independent content instances: %1")
                 .arg(group.distinctIdentityCount);
    lines << QStringLiteral("Path count: %1").arg(group.pathCount);
    lines << QStringLiteral("Hard-link aliases: %1").arg(group.hardLinkAliasPathCount);
    lines << QStringLiteral("Potential redundant logical bytes: %1")
                 .arg(QString::fromStdString(
                     SizeFormatter::format(group.potentialRedundantLogicalBytes)));
    lines << QStringLiteral("");
    for (const auto& instance : group.instances) {
        lines << QStringLiteral("Identity: %1")
                     .arg(QString::fromStdString(
                         duplicateIdentityKey(instance.identity).empty()
                             ? std::string("unavailable")
                             : duplicateIdentityKey(instance.identity)));
        for (const auto& path : instance.paths) {
            lines << QStringLiteral("  %1%2")
                         .arg(QString::fromStdWString(path.path))
                         .arg(path.hardLinkAlias ? QStringLiteral("  [hard-link alias]")
                                                 : QString());
        }
    }
    lines << QStringLiteral("");
    lines << QStringLiteral(
        "Planning only — this is not permission to delete, move, or link files.");
    return lines.join(QLatin1Char('\n'));
}

}  // namespace

DuplicateFilesDialog::DuplicateFilesDialog(CleanupReviewController& review,
                                           std::wstring rootPath,
                                           std::uint64_t indexAgeMs,
                                           std::string indexedAtIso,
                                           QWidget* parent)
    : QDialog(parent)
    , m_review(review)
    , m_rootPath(std::move(rootPath))
    , m_indexAgeMs(indexAgeMs)
    , m_indexedAtIso(std::move(indexedAtIso))
{
    setWindowTitle(QStringLiteral("Duplicate Files"));
    resize(880, 600);

    auto* root = new QVBoxLayout(this);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);

    auto* filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel(QStringLiteral("Minimum size:"), this));
    m_minSize = new QLineEdit(QStringLiteral("1MB"), this);
    m_minSize->setMaximumWidth(120);
    filterRow->addWidget(m_minSize);
    filterRow->addWidget(new QLabel(
        QStringLiteral("Root: ") + QString::fromStdWString(m_rootPath), this));
    filterRow->addStretch(1);
    root->addLayout(filterRow);

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
        QStringLiteral("Planning only — no files will be deleted, moved, or linked. "
                       "Same-size files are not duplicates until full SHA-256 "
                       "verification succeeds."),
        this);
    note->setStyleSheet(QStringLiteral("color: #666;"));
    root->addWidget(note);

    auto* buttons = new QHBoxLayout();
    m_findButton = new QPushButton(QStringLiteral("Find Duplicates"), this);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    m_revealButton = new QPushButton(QStringLiteral("Show in Explorer"), this);
    m_copyPathsButton = new QPushButton(QStringLiteral("Copy Path(s)"), this);
    m_copyGroupButton = new QPushButton(QStringLiteral("Copy Group Details"), this);
    m_addReviewButton =
        new QPushButton(QStringLiteral("Add to Cleanup Review"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(m_findButton);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_revealButton);
    buttons->addWidget(m_copyPathsButton);
    buttons->addWidget(m_copyGroupButton);
    buttons->addWidget(m_addReviewButton);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    connect(m_list, &QListWidget::itemSelectionChanged, this,
            &DuplicateFilesDialog::onSelectionChanged);
    connect(m_findButton, &QPushButton::clicked, this,
            &DuplicateFilesDialog::onFind);
    connect(m_cancelButton, &QPushButton::clicked, this,
            &DuplicateFilesDialog::onCancel);
    connect(m_revealButton, &QPushButton::clicked, this,
            &DuplicateFilesDialog::onReveal);
    connect(m_copyPathsButton, &QPushButton::clicked, this,
            &DuplicateFilesDialog::onCopyPaths);
    connect(m_copyGroupButton, &QPushButton::clicked, this,
            &DuplicateFilesDialog::onCopyGroup);
    connect(m_addReviewButton, &QPushButton::clicked, this,
            &DuplicateFilesDialog::onAddToReview);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(&m_session, &DuplicateDetectionSession::progressUpdated, this,
            &DuplicateFilesDialog::onProgress);
    connect(&m_session, &DuplicateDetectionSession::finished, this,
            &DuplicateFilesDialog::onFinished);

    refresh();
}

void DuplicateFilesDialog::onFind()
{
    const auto parsed = parseSize(m_minSize->text().toStdWString());
    if (!parsed.error.empty()) {
        showStatus(QStringLiteral("Invalid minimum size: %1")
                       .arg(QString::fromStdString(parsed.error)));
        return;
    }
    DuplicateScanOptions options;
    options.minimumSize = parsed.bytes;
    if (!m_session.start(m_rootPath, options)) {
        showStatus(QStringLiteral("A duplicate scan is already running."));
        return;
    }
    m_progress->setVisible(true);
    m_progress->setRange(0, 0);
    showStatus(QStringLiteral("Finding index candidates and verifying contents…"));
    updateActionState();
}

void DuplicateFilesDialog::onCancel()
{
    m_session.cancel();
    showStatus(QStringLiteral("Cancelling duplicate scan…"));
}

void DuplicateFilesDialog::onSelectionChanged()
{
    const auto index = selectedGroupIndex();
    if (!index || *index >= m_result.groups.size()) {
        m_details->setPlainText(
            QStringLiteral("Select a verified group to inspect SHA-256, "
                           "identities, and hard-link aliases."));
        updateActionState();
        return;
    }
    m_details->setPlainText(groupDetails(m_result.groups[*index]));
    updateActionState();
}

void DuplicateFilesDialog::onReveal()
{
    const auto paths = selectedPaths();
    if (paths.empty()) {
        return;
    }
    if (!revealInExplorer(paths.front()) && !openInExplorer(paths.front())) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("Could not reveal path."));
    }
}

void DuplicateFilesDialog::onCopyPaths()
{
    QStringList lines;
    for (const auto& path : selectedPaths()) {
        lines << QString::fromStdWString(path);
    }
    if (lines.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    showStatus(QStringLiteral("Copied path(s). Planning only."));
}

void DuplicateFilesDialog::onCopyGroup()
{
    const auto index = selectedGroupIndex();
    if (!index || *index >= m_result.groups.size()) {
        return;
    }
    QApplication::clipboard()->setText(groupDetails(m_result.groups[*index]));
    showStatus(QStringLiteral("Copied group details. Planning only."));
}

void DuplicateFilesDialog::onAddToReview()
{
    const auto index = selectedGroupIndex();
    if (!index || *index >= m_result.groups.size()) {
        return;
    }
    const auto& group = m_result.groups[*index];
    WindowsCleanupMetadataReader reader;
    int added = 0;
    int already = 0;
    int conflicts = 0;
    for (const auto& instance : group.instances) {
        for (const auto& path : instance.paths) {
            auto candidate = cleanupCandidateFromDuplicate(
                group, instance, path, m_rootPath, m_indexAgeMs, m_indexedAtIso);
            prepareCleanupCandidateForAdd(candidate, reader);
            const auto status = m_review.add(std::move(candidate));
            if (!status.ok) {
                showStatus(QString::fromStdString(status.message));
                return;
            }
            if (status.add.result == CleanupAddResult::Added) {
                ++added;
            } else if (status.add.result == CleanupAddResult::DuplicateUpdated) {
                ++already;
            } else if (status.add.result == CleanupAddResult::IdentityConflict) {
                ++conflicts;
            }
        }
    }
    showStatus(QStringLiteral(
                   "Added to Cleanup Review: %1 new, %2 already present, "
                   "%3 identity conflicts. This does not authorize deletion.")
                   .arg(added)
                   .arg(already)
                   .arg(conflicts));
}

void DuplicateFilesDialog::onProgress(quint64 filesDone, quint64 filesTotal,
                                     quint64 bytesDone, quint64 bytesTotal)
{
    m_progress->setVisible(true);
    if (filesTotal == 0) {
        m_progress->setRange(0, 0);
    } else {
        m_progress->setRange(0, static_cast<int>(std::min<quint64>(filesTotal, 1000000)));
        m_progress->setValue(static_cast<int>(std::min(filesDone, filesTotal)));
    }
    showStatus(QStringLiteral("Duplicate scan %1 / %2 candidates · %3 / %4 processed")
                   .arg(filesDone)
                   .arg(filesTotal)
                   .arg(QString::fromStdString(SizeFormatter::format(bytesDone)))
                   .arg(QString::fromStdString(SizeFormatter::format(bytesTotal))));
}

void DuplicateFilesDialog::onFinished(bool completed, const QString& message)
{
    m_progress->setVisible(false);
    m_result = m_session.lastResult();
    if (!completed && m_result.cancelled) {
        showStatus(message + QStringLiteral(" Results are partial."));
    } else {
        showStatus(message);
    }
    refresh();
}

void DuplicateFilesDialog::refresh()
{
    m_list->clear();
    const auto& summary = m_result.summary;
    QString header = QStringLiteral(
                         "Verified groups: %1   Files: %2   Potential redundant "
                         "logical size: %3")
                         .arg(summary.verifiedGroups)
                         .arg(summary.verifiedPaths)
                         .arg(QString::fromStdString(SizeFormatter::format(
                             summary.potentialRedundantLogicalBytes)));
    if (m_result.cancelled) {
        header += QStringLiteral("\nScan cancelled — results are partial.");
    }
    if (!m_result.skipped.empty()) {
        header += QStringLiteral("\nSkipped / inconclusive: %1")
                      .arg(m_result.skipped.size());
    }
    m_summary->setText(header);

    for (std::size_t i = 0; i < m_result.groups.size(); ++i) {
        auto* row = new QListWidgetItem(groupLabel(m_result.groups[i]), m_list);
        row->setData(Qt::UserRole, static_cast<qulonglong>(i));
    }
    if (m_list->count() > 0 && m_list->selectedItems().isEmpty()) {
        m_list->setCurrentRow(0);
    }
    onSelectionChanged();
    updateActionState();
}

std::optional<std::size_t> DuplicateFilesDialog::selectedGroupIndex() const
{
    const auto items = m_list->selectedItems();
    if (items.size() != 1) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(items.front()->data(Qt::UserRole).toULongLong());
}

std::vector<std::wstring> DuplicateFilesDialog::selectedPaths() const
{
    std::vector<std::wstring> paths;
    const auto index = selectedGroupIndex();
    if (!index || *index >= m_result.groups.size()) {
        return paths;
    }
    for (const auto& instance : m_result.groups[*index].instances) {
        for (const auto& path : instance.paths) {
            paths.push_back(path.path);
        }
    }
    return paths;
}

void DuplicateFilesDialog::updateActionState()
{
    const bool running = m_session.isRunning();
    const bool singleGroup = selectedGroupIndex().has_value();
    m_findButton->setEnabled(!running);
    m_cancelButton->setEnabled(running);
    m_minSize->setEnabled(!running);
    m_revealButton->setEnabled(!running && !selectedPaths().empty());
    m_copyPathsButton->setEnabled(!running && singleGroup);
    m_copyGroupButton->setEnabled(!running && singleGroup);
    m_addReviewButton->setEnabled(!running && singleGroup);
}

void DuplicateFilesDialog::showStatus(const QString& message)
{
    m_status->setText(message);
}

}  // namespace spacelens
