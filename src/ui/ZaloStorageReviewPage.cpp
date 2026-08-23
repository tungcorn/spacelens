#include "ui/ZaloStorageReviewPage.hpp"

#include "core/SizeFormatter.hpp"
#include "ui/EmptyStateWidget.hpp"
#include "ui/MetricStrip.hpp"
#include "ui/PageHeader.hpp"
#include "ui/UiTheme.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimeZone>
#include <QVBoxLayout>

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace spacelens {
namespace {

QString formatBytes(ByteSize bytes)
{
    return QString::fromStdString(SizeFormatter::format(bytes));
}

QString formatKnownBytes(ByteSize bytes, bool known)
{
    return known ? formatBytes(bytes) : QStringLiteral("Unknown");
}

QString formatOptionalBytes(const std::optional<ByteSize>& bytes)
{
    return bytes.has_value() ? formatBytes(*bytes) : QStringLiteral("Unknown");
}

QString formatPartialBytes(const std::optional<ByteSize>& exact,
                           const std::optional<ByteSize>& partial)
{
    if (exact.has_value()) {
        return formatBytes(*exact);
    }
    if (partial.has_value()) {
        return QStringLiteral("Partial · %1").arg(formatBytes(*partial));
    }
    return QStringLiteral("Unknown");
}

QString storageStatus(ZaloStorageStatus status)
{
    return QString::fromUtf8(toString(status));
}

QString formatFileAge(uint64_t ticks)
{
    if (ticks == 0) {
        return QStringLiteral("Unknown");
    }
    // Windows file time: 100-nanosecond intervals since January 1, 1601 (UTC).
    // Unix epoch offset: 116,444,736,000,000,000 100ns units
    if (ticks < 116444736000000000ULL) {
        return QStringLiteral("Unknown");
    }
    const uint64_t msecsSinceEpoch = (ticks - 116444736000000000ULL) / 10000ULL;
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(msecsSinceEpoch), QTimeZone::UTC).toLocalTime();
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

bool sendFileToRecycleBin(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    std::wstring doubleNullPath = path;
    doubleNullPath.push_back(L'\0');

    SHFILEOPSTRUCTW fileOp{};
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = doubleNullPath.data();
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    return (::SHFileOperationW(&fileOp) == 0 && !fileOp.fAnyOperationsAborted);
}

void revealInExplorer(const std::wstring& path)
{
    if (path.empty()) {
        return;
    }
    const std::wstring param = L"/select,\"" + path + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOW);
}

}  // namespace

ZaloStorageReviewPage::ZaloStorageReviewPage(QWidget* parent)
    : QWidget(parent)
    , m_session(new ZaloStorageSession(this))
{
    buildUi();
    connect(m_session, &ZaloStorageSession::statusMessage, this,
            &ZaloStorageReviewPage::onSessionStatus);
    connect(m_session, &ZaloStorageSession::finished, this,
            &ZaloStorageReviewPage::onFinished);
    updateActionState();
}

ZaloStorageReviewPage::~ZaloStorageReviewPage()
{
    if (m_session) {
        m_session->cancel();
    }
}

void ZaloStorageReviewPage::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    applyPageMargins(this);

    auto* header = new PageHeader(this);
    header->setTitle(QStringLiteral("Zalo Storage Review"));
    header->setSubtitle(
        QStringLiteral("Review human-recognizable content, visual previews, and safely manage storage."));

    m_chooseButton = new QPushButton(QStringLiteral("Choose Root"), header);
    m_chooseButton->setObjectName(QStringLiteral("slZaloChooseRoot"));
    markSecondaryButton(m_chooseButton);

    m_reviewButton = new QPushButton(QStringLiteral("Review"), header);
    m_reviewButton->setObjectName(QStringLiteral("slZaloReview"));
    m_reviewButton->setToolTip(
        QStringLiteral("Run bounded auto-discovery and review Zalo files."));
    markPrimaryButton(m_reviewButton);

    m_cleanFileNoiseButton = new QPushButton(QStringLiteral("Clean fileNoise"), header);
    m_cleanFileNoiseButton->setObjectName(QStringLiteral("slZaloCleanNoise"));
    m_cleanFileNoiseButton->setToolTip(
        QStringLiteral("Safely move ephemeral transit cache files to Recycle Bin."));
    markSecondaryButton(m_cleanFileNoiseButton);
    m_cleanFileNoiseButton->hide();

    m_deleteButton = new QPushButton(QStringLiteral("Delete Selected"), header);
    m_deleteButton->setObjectName(QStringLiteral("slZaloDeleteSelected"));
    m_deleteButton->setToolTip(
        QStringLiteral("Move selected Zalo files safely to the Windows Recycle Bin (Del)."));
    markSecondaryButton(m_deleteButton);
    m_deleteButton->setEnabled(false);
    m_deleteButton->hide();

    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), header);
    m_cancelButton->setObjectName(QStringLiteral("slZaloCancel"));
    markSecondaryButton(m_cancelButton);
    m_cancelButton->hide();

    header->commands()->addWidget(m_chooseButton);
    header->commands()->addWidget(m_reviewButton);
    header->commands()->addWidget(m_cleanFileNoiseButton);
    header->commands()->addWidget(m_deleteButton);
    header->commands()->addWidget(m_cancelButton);
    rootLayout->addWidget(header);

    auto* rootRow = new QHBoxLayout();
    rootRow->setSpacing(kUiSpace8);
    m_rootSummary = new QLabel(
        QStringLiteral("Auto-discovery is ready. Double-click or right-click any item to view/delete."),
        this);
    m_rootSummary->setObjectName(QStringLiteral("slZaloRootSummary"));
    m_rootSummary->setWordWrap(true);
    rootRow->addWidget(m_rootSummary, 1);
    rootLayout->addLayout(rootRow);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setObjectName(QStringLiteral("slZaloProgress"));
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(4);
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(false);
    rootLayout->addWidget(m_progressBar);

    m_metrics = new MetricStrip(this);
    rootLayout->addWidget(m_metrics);

    m_reportSummary = new QLabel(this);
    m_reportSummary->setObjectName(QStringLiteral("slZaloSummary"));
    m_reportSummary->setWordWrap(true);
    m_reportSummary->hide();
    rootLayout->addWidget(m_reportSummary);

    m_stack = new QStackedWidget(this);
    m_empty = new EmptyStateWidget(m_stack);
    m_empty->setTitle(QStringLiteral("Ready for review"));
    m_empty->setBody(QStringLiteral(
        "Click Review for automatic Zalo storage discovery. "
        "Deterministic human identity, visual previews, and safe actions will be shown."));
    m_empty->setActionText(QStringLiteral("Review"));
    m_empty->setActionVisible(true);

    m_entries = new QTableWidget(0, ColCount, m_stack);
    m_entries->setObjectName(QStringLiteral("slZaloEntries"));
    m_entries->setHorizontalHeaderLabels({
        QStringLiteral("Preview"),
        QStringLiteral("Item / Title"),
        QStringLiteral("Identity Summary"),
        QStringLiteral("Physical Impact"),
        QStringLiteral("Logical"),
        QStringLiteral("Allocated"),
        QStringLiteral("Exact Copy"),
        QStringLiteral("Modified"),
        QStringLiteral("Confidence"),
        QStringLiteral("Category"),
        QStringLiteral("Account"),
        QStringLiteral("Entry ID")});
    m_entries->setAlternatingRowColors(true);
    m_entries->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_entries->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_entries->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_entries->setShowGrid(false);
    m_entries->setWordWrap(false);
    m_entries->setIconSize(QSize(96, 60));
    m_entries->verticalHeader()->setVisible(false);
    m_entries->verticalHeader()->setDefaultSectionSize(68);
    m_entries->horizontalHeader()->setStretchLastSection(false);
    m_entries->horizontalHeader()->setDefaultSectionSize(110);
    m_entries->setColumnWidth(ColPreview, 104);
    m_entries->setColumnWidth(ColName, 240);
    m_entries->setColumnWidth(ColSummary, 240);
    m_entries->setColumnWidth(ColPhysicalImpact, 110);
    m_entries->setColumnWidth(ColLogical, 90);
    m_entries->setColumnWidth(ColAllocated, 90);
    m_entries->setColumnWidth(ColExactCopy, 110);
    m_entries->setColumnWidth(ColAge, 130);
    m_entries->setColumnWidth(ColConfidence, 90);
    m_entries->setColumnWidth(ColCategory, 140);
    m_entries->setColumnWidth(ColRootAccount, 110);
    m_entries->setColumnWidth(ColEntry, 80);

    m_entries->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_entries, &QTableWidget::customContextMenuRequested, this,
            &ZaloStorageReviewPage::onTableContextMenu);
    connect(m_entries, &QTableWidget::cellDoubleClicked, this,
            &ZaloStorageReviewPage::onCellDoubleClicked);
    connect(m_entries, &QTableWidget::itemSelectionChanged, this,
            &ZaloStorageReviewPage::onSelectionChanged);

    auto* deleteShortcut = new QShortcut(QKeySequence::Delete, m_entries);
    connect(deleteShortcut, &QShortcut::activated, this,
            &ZaloStorageReviewPage::onDeleteSelected);

    m_stack->addWidget(m_empty);
    m_stack->addWidget(m_entries);
    m_stack->setCurrentWidget(m_empty);
    rootLayout->addWidget(m_stack, 1);

    connect(m_chooseButton, &QPushButton::clicked, this,
            &ZaloStorageReviewPage::onChooseRoot);
    connect(m_reviewButton, &QPushButton::clicked, this,
            &ZaloStorageReviewPage::onReview);
    connect(m_cancelButton, &QPushButton::clicked, this,
            &ZaloStorageReviewPage::onCancel);
    connect(m_cleanFileNoiseButton, &QPushButton::clicked, this,
            &ZaloStorageReviewPage::onCleanFileNoise);
    connect(m_deleteButton, &QPushButton::clicked, this,
            &ZaloStorageReviewPage::onDeleteSelected);
    connect(m_empty, &EmptyStateWidget::actionClicked, this,
            &ZaloStorageReviewPage::onReview);
}

void ZaloStorageReviewPage::clearReport()
{
    m_report.reset();
    m_displayRows.clear();
    m_entries->setRowCount(0);
    m_reportSummary->clear();
    m_reportSummary->hide();
    m_metrics->setItems({});
    m_cleanFileNoiseButton->hide();
    m_deleteButton->hide();
    m_deleteButton->setEnabled(false);
    m_deleteButton->setText(QStringLiteral("Delete Selected"));
}

void ZaloStorageReviewPage::showEmptyState(const QString& title,
                                           const QString& body,
                                           bool showAction)
{
    m_empty->setTitle(title);
    m_empty->setBody(body);
    m_empty->setActionVisible(showAction);
    m_stack->setCurrentWidget(m_empty);
}

void ZaloStorageReviewPage::updateActionState()
{
    const bool running = m_session != nullptr && m_session->isRunning();
    m_chooseButton->setEnabled(!running);
    m_reviewButton->setEnabled(!running);
    m_cancelButton->setEnabled(running);
    m_cancelButton->setVisible(running);
    m_cleanFileNoiseButton->setEnabled(!running);
    if (m_progressBar != nullptr) {
        m_progressBar->setVisible(running);
    }
    if (running) {
        m_deleteButton->setEnabled(false);
    } else {
        onSelectionChanged();
    }
}

void ZaloStorageReviewPage::onChooseRoot()
{
    const QString root = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select Zalo storage root"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (root.isEmpty()) {
        return;
    }

    m_selectedRoot = root;
    m_rootSummary->setText(
        QStringLiteral("Selected custom root: %1").arg(m_selectedRoot));
    onReview();
}

void ZaloStorageReviewPage::onReview()
{
    if (m_session == nullptr || m_session->isRunning()) {
        return;
    }

    clearReport();
    updateActionState();
    m_rootSummary->setText(
        QStringLiteral("Scanning Zalo storage across drives... Please wait."));
    showEmptyState(
        QStringLiteral("Scanning Zalo storage..."),
        QStringLiteral("Discovering accounts, verifying file identity, and analyzing media and cache footprint. This may take a moment for large collections..."),
        false);
    emit statusMessage(QStringLiteral("Scanning Zalo storage..."));

    m_session->start(m_selectedRoot);
}

void ZaloStorageReviewPage::onCancel()
{
    if (m_session != nullptr && m_session->isRunning()) {
        m_session->cancel();
        m_rootSummary->setText(QStringLiteral("Cancelling scan..."));
        showEmptyState(QStringLiteral("Cancelling..."),
                       QStringLiteral("Stopping Zalo storage inspection..."),
                       false);
        emit statusMessage(QStringLiteral("Cancelling Zalo review..."));
    }
}

void ZaloStorageReviewPage::onSessionStatus(const QString& message)
{
    emit statusMessage(message);
}

void ZaloStorageReviewPage::onFinished(spacelens::ZaloStorageStatus status)
{
    updateActionState();

    if (status == ZaloStorageStatus::Cancelled) {
        emit statusMessage(QStringLiteral("Zalo review cancelled."));
        m_rootSummary->setText(
            QStringLiteral("Review cancelled. Click Review to scan again."));
        showEmptyState(QStringLiteral("Review cancelled"),
                       QStringLiteral("Inspection was cancelled before completion. Click Review to scan again."),
                       true);
        return;
    }

    auto report = m_session->takeReport();
    if (!report.has_value()) {
        emit statusMessage(QStringLiteral("No report available."));
        m_rootSummary->setText(
            QStringLiteral("Inspection incomplete. Click Review to retry."));
        showEmptyState(
            QStringLiteral("Inspection incomplete"),
            QStringLiteral("The review session did not produce a report. Status: %1. Click Review to retry.")
                .arg(storageStatus(status)),
            true);
        return;
    }

    m_report = std::move(*report);
    applyReport(*m_report);
    emit statusMessage(QStringLiteral("Zalo review complete: %1.")
                           .arg(storageStatus(m_report->status)));
}

void ZaloStorageReviewPage::applyReport(const ZaloStorageReport& report)
{
    m_entries->setUpdatesEnabled(false);
    m_entries->setRowCount(0);
    m_displayRows.clear();

    // Build exact copy mapping
    std::unordered_map<std::string, QString> exactCopyMap;
    for (std::size_t m = 0; m < report.exactCopy.matches.size(); ++m) {
        const auto& match = report.exactCopy.matches[m];
        const QString matchLabel = QStringLiteral("Copy Match #%1").arg(m + 1);
        exactCopyMap[match.zaloEntryId] = matchLabel;
    }

    // Collect all entries across accounts and compute physical impact
    ByteSize totalFileNoiseBytes = 0;
    for (const auto& account : report.accounts) {
        for (const auto& entry : account.entries) {
            ItemDisplayRow row;
            row.account = &account;
            row.entry = &entry;
            row.physicalImpact = entry.allObservedPathReleaseBytes.value_or(
                entry.singlePathReleaseBytes.value_or(
                    entry.allocatedBytes.value_or(entry.logicalBytes)));
            row.nativePath = entry.nativePath;

            if (entry.categoryAlias == "file-noise" && !entry.hardLinkAlias) {
                totalFileNoiseBytes += row.physicalImpact;
            }

            const auto it = exactCopyMap.find(entry.entryId);
            if (it != exactCopyMap.end()) {
                row.exactCopyLabel = it->second;
            } else {
                row.exactCopyLabel = QStringLiteral("Unique");
            }
            m_displayRows.push_back(std::move(row));
        }
    }

    // Sort by physical impact descending by default
    std::sort(m_displayRows.begin(), m_displayRows.end(),
              [](const ItemDisplayRow& a, const ItemDisplayRow& b) {
                  if (a.physicalImpact != b.physicalImpact) {
                      return a.physicalImpact > b.physicalImpact;
                  }
                  if (a.entry->logicalBytes != b.entry->logicalBytes) {
                      return a.entry->logicalBytes > b.entry->logicalBytes;
                  }
                  return a.entry->entryId < b.entry->entryId;
              });

    for (const auto& r : m_displayRows) {
        const int rowIdx = m_entries->rowCount();
        m_entries->insertRow(rowIdx);

        const ZaloAccountReport& account = *r.account;
        const ZaloEntry& entry = *r.entry;

        const auto* content = entry.contentIdentification.has_value()
                                  ? &*entry.contentIdentification
                                  : nullptr;
        const ZaloContentConfidence confidence =
            content == nullptr ? ZaloContentConfidence::Unknown
                               : content->confidence;

        QString displayName;
        QString summary;
        QPixmap previewPixmap;

        if (entry.humanIdentity.has_value()) {
            const auto& hid = *entry.humanIdentity;
            displayName = QString::fromStdString(hid.displayName);
            summary = QString::fromStdString(hid.contentSummary);
            if (hid.previewAvailable && !entry.nativePath.empty()) {
                previewPixmap = m_previewProvider.getPreviewPixmap(
                    QString::fromStdWString(entry.nativePath),
                    hid, QSize(96, 60));
            }
        } else {
            displayName = QStringLiteral("Unknown Zalo data");
            summary = QStringLiteral("Content could not be identified safely");
        }

        const QString fullPathStr = QString::fromStdWString(entry.nativePath);

        // Preview item
        auto* previewItem = new QTableWidgetItem();
        if (!previewPixmap.isNull()) {
            previewItem->setIcon(QIcon(previewPixmap));
        }
        previewItem->setTextAlignment(Qt::AlignCenter);
        previewItem->setToolTip(fullPathStr.isEmpty() ? summary : fullPathStr);
        m_entries->setItem(rowIdx, ColPreview, previewItem);

        // Name
        auto* nameItem = new QTableWidgetItem(displayName);
        nameItem->setToolTip(fullPathStr.isEmpty() ? displayName : fullPathStr);
        m_entries->setItem(rowIdx, ColName, nameItem);

        // Summary
        auto* summaryItem = new QTableWidgetItem(summary);
        summaryItem->setToolTip(summary);
        m_entries->setItem(rowIdx, ColSummary, summaryItem);

        // Physical Impact
        auto* impactItem = new QTableWidgetItem(formatBytes(r.physicalImpact));
        impactItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        impactItem->setForeground(QColor(60, 150, 255));
        m_entries->setItem(rowIdx, ColPhysicalImpact, impactItem);

        // Logical
        auto* logItem = new QTableWidgetItem(formatBytes(entry.logicalBytes));
        logItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_entries->setItem(rowIdx, ColLogical, logItem);

        // Allocated
        auto* allocItem = new QTableWidgetItem(
            entry.allocationKnown ? formatOptionalBytes(entry.allocatedBytes)
                                 : QStringLiteral("Unknown"));
        allocItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_entries->setItem(rowIdx, ColAllocated, allocItem);

        // Exact Copy
        auto* copyItem = new QTableWidgetItem(r.exactCopyLabel);
        if (r.exactCopyLabel.startsWith(QStringLiteral("Copy Match"))) {
            copyItem->setForeground(QColor(255, 170, 50));
        }
        m_entries->setItem(rowIdx, ColExactCopy, copyItem);

        // Age / Modified
        auto* ageItem = new QTableWidgetItem(formatFileAge(entry.lastWriteTicks));
        m_entries->setItem(rowIdx, ColAge, ageItem);

        // Confidence
        auto* confItem = new QTableWidgetItem(QString::fromUtf8(toString(confidence)));
        if (confidence == ZaloContentConfidence::Verified) {
            confItem->setForeground(QColor(40, 190, 90));
        } else if (confidence == ZaloContentConfidence::Strong) {
            confItem->setForeground(QColor(70, 160, 240));
        }
        m_entries->setItem(rowIdx, ColConfidence, confItem);

        // Category with recommendation tag
        QString categoryLabel = QString::fromStdString(entry.categoryAlias);
        QString categoryTooltip = categoryLabel;
        if (entry.categoryAlias == "file-noise") {
            categoryLabel = QStringLiteral("file-noise [Cache]");
            categoryTooltip = QStringLiteral("Transit Cache · An toan de xoa (Safe to delete)");
        } else if (entry.categoryAlias == "resource") {
            categoryLabel = QStringLiteral("resource [Chat Cache]");
            categoryTooltip = QStringLiteral("Chat media cache · Xem xet xoa");
        } else if (entry.categoryAlias == "video") {
            categoryLabel = QStringLiteral("video [Downloads]");
            categoryTooltip = QStringLiteral("Video download");
        }
        auto* catItem = new QTableWidgetItem(categoryLabel);
        catItem->setToolTip(categoryTooltip);
        m_entries->setItem(rowIdx, ColCategory, catItem);

        // Root/Account
        auto* acctItem = new QTableWidgetItem(
            QString::fromStdString(account.rootAlias + "/" + account.accountAlias));
        m_entries->setItem(rowIdx, ColRootAccount, acctItem);

        // Entry ID
        auto* entryItem = new QTableWidgetItem(QString::fromStdString(entry.entryId));
        m_entries->setItem(rowIdx, ColEntry, entryItem);
    }
    m_entries->setUpdatesEnabled(true);

    if (totalFileNoiseBytes > 0) {
        m_cleanFileNoiseButton->setText(
            QStringLiteral("Clean fileNoise (%1)").arg(formatBytes(totalFileNoiseBytes)));
        m_cleanFileNoiseButton->show();
    } else {
        m_cleanFileNoiseButton->hide();
    }

    const auto& accounting = report.accounting;
    const QLocale locale;
    m_metrics->setItems({
        {QStringLiteral("Status"), storageStatus(report.status), {}},
        {QStringLiteral("Root aliases"),
         locale.toString(static_cast<qulonglong>(report.roots.size())), {}},
        {QStringLiteral("Account aliases"),
         locale.toString(static_cast<qulonglong>(report.accounts.size())), {}},
        {QStringLiteral("Entries"),
         locale.toString(static_cast<qulonglong>(m_displayRows.size())), {}},
        {QStringLiteral("Path-visible logical"),
         formatKnownBytes(accounting.pathVisibleLogicalBytes,
                          accounting.pathVisibleLogicalKnown), {}},
        {QStringLiteral("Unique logical"),
         formatKnownBytes(accounting.uniqueLogicalBytes,
                          accounting.uniqueLogicalKnown), {}},
        {QStringLiteral("Allocated"),
         formatPartialBytes(accounting.uniqueAllocatedBytes,
                            accounting.partialKnownUniqueAllocatedBytes), {}},
        {QStringLiteral("Physical impact"),
         formatPartialBytes(accounting.allObservedPathReleaseBytes,
                            accounting.partialKnownReleaseBytes),
         QStringLiteral("Physical footprint on disk.")},
    });

    m_reportSummary->setText(
        QStringLiteral("%1 · %2 root(s) · %3 account(s) · %4 entries · Sorted by Physical Impact · Right-click or double-click to reveal/delete.")
            .arg(storageStatus(report.status))
            .arg(report.roots.size())
            .arg(report.accounts.size())
            .arg(m_displayRows.size()));
    m_reportSummary->setVisible(true);

    if (m_displayRows.empty()) {
        m_deleteButton->hide();
        m_rootSummary->setText(
            QStringLiteral("Scan complete: No Zalo entries found."));
        showEmptyState(
            QStringLiteral("No Zalo entries found"),
            QStringLiteral("The review completed but found no files in the scanned directories. Click Review to scan again."),
            true);
    } else {
        m_deleteButton->show();
        m_rootSummary->setText(
            QStringLiteral("Review complete. Double-click or right-click any item to view/delete."));
        m_stack->setCurrentWidget(m_entries);
    }
    onSelectionChanged();
}

void ZaloStorageReviewPage::onSelectionChanged()
{
    if (m_entries == nullptr || m_deleteButton == nullptr) {
        return;
    }
    const auto selected = m_entries->selectionModel() != nullptr
                              ? m_entries->selectionModel()->selectedRows()
                              : QModelIndexList{};
    const bool running = m_session != nullptr && m_session->isRunning();
    if (selected.isEmpty() || running || m_displayRows.empty()) {
        m_deleteButton->setEnabled(false);
        m_deleteButton->setText(QStringLiteral("Delete Selected"));
    } else {
        ByteSize totalBytes = 0;
        for (const auto& index : selected) {
            const int row = index.row();
            if (row >= 0 && static_cast<std::size_t>(row) < m_displayRows.size()) {
                totalBytes += m_displayRows[row].physicalImpact;
            }
        }
        m_deleteButton->setEnabled(true);
        if (selected.size() == 1) {
            m_deleteButton->setText(
                QStringLiteral("Delete Selected (%1)").arg(formatBytes(totalBytes)));
        } else {
            m_deleteButton->setText(
                QStringLiteral("Delete Selected (%1 items · %2)")
                    .arg(selected.size())
                    .arg(formatBytes(totalBytes)));
        }
    }
}

void ZaloStorageReviewPage::onTableContextMenu(const QPoint& pos)
{
    const int clickedRow = m_entries->rowAt(pos.y());
    if (clickedRow < 0 || static_cast<std::size_t>(clickedRow) >= m_displayRows.size()) {
        return;
    }

    if (m_entries->selectionModel() == nullptr) {
        return;
    }

    // If clicked row is not currently selected, select only this row
    if (!m_entries->selectionModel()->isRowSelected(clickedRow, QModelIndex())) {
        m_entries->selectRow(clickedRow);
    }

    const auto selected = m_entries->selectionModel()->selectedRows();
    const auto& rowData = m_displayRows[clickedRow];
    QMenu menu(this);
    
    QAction* revealAction = menu.addAction(QStringLiteral("📂 Reveal in File Explorer (Mở thư mục)"));
    QAction* copyPathAction = menu.addAction(QStringLiteral("📋 Copy File Path (Sao chép đường dẫn)"));
    menu.addSeparator();
    
    QString deleteLabel = QStringLiteral("🗑️ Delete to Recycle Bin (Xóa vào thùng rác)");
    if (selected.size() > 1) {
        deleteLabel = QStringLiteral("🗑️ Delete %1 Selected to Recycle Bin (Xóa %1 mục)").arg(selected.size());
    }
    QAction* deleteAction = menu.addAction(deleteLabel);

    QAction* chosen = menu.exec(m_entries->viewport()->mapToGlobal(pos));
    if (chosen == revealAction) {
        revealInExplorer(rowData.nativePath);
    } else if (chosen == copyPathAction) {
        QGuiApplication::clipboard()->setText(QString::fromStdWString(rowData.nativePath));
        emit statusMessage(QStringLiteral("Path copied to clipboard."));
    } else if (chosen == deleteAction) {
        onDeleteSelected();
    }
}

void ZaloStorageReviewPage::onCellDoubleClicked(int row, int /*column*/)
{
    if (row >= 0 && static_cast<std::size_t>(row) < m_displayRows.size()) {
        revealInExplorer(m_displayRows[row].nativePath);
    }
}

void ZaloStorageReviewPage::onRevealInExplorer()
{
    const int row = m_entries->currentRow();
    if (row >= 0 && static_cast<std::size_t>(row) < m_displayRows.size()) {
        revealInExplorer(m_displayRows[row].nativePath);
    }
}

void ZaloStorageReviewPage::onCopyPath()
{
    const int row = m_entries->currentRow();
    if (row >= 0 && static_cast<std::size_t>(row) < m_displayRows.size()) {
        QGuiApplication::clipboard()->setText(
            QString::fromStdWString(m_displayRows[row].nativePath));
        emit statusMessage(QStringLiteral("Path copied to clipboard."));
    }
}

void ZaloStorageReviewPage::onDeleteSelected()
{
    if (m_entries == nullptr || m_entries->selectionModel() == nullptr) {
        return;
    }
    const auto selectedIndices = m_entries->selectionModel()->selectedRows();
    if (selectedIndices.isEmpty()) {
        return;
    }

    std::vector<int> rows;
    rows.reserve(selectedIndices.size());
    for (const auto& idx : selectedIndices) {
        rows.push_back(idx.row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());

    ByteSize totalBytes = 0;
    std::vector<std::wstring> paths;
    paths.reserve(rows.size());
    for (const int r : rows) {
        if (r >= 0 && static_cast<std::size_t>(r) < m_displayRows.size()) {
            totalBytes += m_displayRows[r].physicalImpact;
            paths.push_back(m_displayRows[r].nativePath);
        }
    }

    if (paths.empty()) {
        return;
    }

    const QString title = QStringLiteral("Confirm Move to Recycle Bin");
    QString message;
    if (paths.size() == 1) {
        const int row = rows.front();
        const QString name = m_entries->item(row, ColName) != nullptr
                                 ? m_entries->item(row, ColName)->text()
                                 : QStringLiteral("Selected item");
        message = QStringLiteral(
                      "Move this file to the Windows Recycle Bin?\n\n%1 (%2)\n\nPath: %3")
                      .arg(name, formatBytes(totalBytes),
                           QString::fromStdWString(paths.front()));
    } else {
        message = QStringLiteral(
                      "Move %1 selected items (~%2) to the Windows Recycle Bin?\n\n"
                      "This action is safe and reversible via the Recycle Bin.")
                      .arg(paths.size())
                      .arg(formatBytes(totalBytes));
    }

    const auto reply = QMessageBox::question(
        this, title, message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    std::size_t successCount = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const int r = rows[i];
        const auto& path = paths[i];
        if (sendFileToRecycleBin(path)) {
            ++successCount;
            m_entries->removeRow(r);
            if (static_cast<std::size_t>(r) < m_displayRows.size()) {
                m_displayRows.erase(m_displayRows.begin() + r);
            }
        }
    }

    if (successCount == paths.size()) {
        emit statusMessage(
            QStringLiteral("Successfully moved %1 item(s) to Recycle Bin.")
                .arg(successCount));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("Partial Delete"),
            QStringLiteral("Moved %1 of %2 items to Recycle Bin. Some files may be locked by Zalo.")
                .arg(successCount)
                .arg(paths.size()));
    }

    if (m_displayRows.empty()) {
        clearReport();
        showEmptyState(
            QStringLiteral("All items deleted"),
            QStringLiteral("All reviewed items have been moved to the Recycle Bin. Click Review to scan again."),
            true);
    } else {
        m_reportSummary->setText(
            QStringLiteral("%1 remaining entries · Sorted by Physical Impact · Right-click or double-click to reveal/delete.")
                .arg(m_displayRows.size()));
        onSelectionChanged();
    }
}

void ZaloStorageReviewPage::onCleanFileNoise()
{
    std::vector<std::wstring> pathsToDelete;
    ByteSize totalBytes = 0;
    for (const auto& row : m_displayRows) {
        if (row.entry != nullptr && row.entry->categoryAlias == "file-noise" &&
            !row.nativePath.empty()) {
            pathsToDelete.push_back(row.nativePath);
            if (!row.entry->hardLinkAlias) {
                totalBytes += row.physicalImpact;
            }
        }
    }

    if (pathsToDelete.empty()) {
        QMessageBox::information(this, QStringLiteral("Clean fileNoise"),
                                 QStringLiteral("No fileNoise cache files found."));
        return;
    }

    const auto reply = QMessageBox::question(
        this, QStringLiteral("Confirm Clean fileNoise"),
        QStringLiteral("Clean %1 ephemeral cache files (~%2)?\n\n"
                       "All files will be moved safely to your Windows Recycle Bin.")
            .arg(pathsToDelete.size())
            .arg(formatBytes(totalBytes)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    std::size_t deletedCount = 0;
    for (const auto& path : pathsToDelete) {
        if (sendFileToRecycleBin(path)) {
            ++deletedCount;
        }
    }

    QMessageBox::information(
        this, QStringLiteral("Clean Complete"),
        QStringLiteral("Successfully moved %1 of %2 cache files to the Recycle Bin.")
            .arg(deletedCount)
            .arg(pathsToDelete.size()));

    // Refresh review
    onReview();
}

}  // namespace spacelens
