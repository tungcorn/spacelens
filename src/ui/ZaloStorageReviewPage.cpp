#include "ui/ZaloStorageReviewPage.hpp"

#include "core/SizeFormatter.hpp"
#include "ui/EmptyStateWidget.hpp"
#include "ui/MetricStrip.hpp"
#include "ui/PageHeader.hpp"
#include "ui/UiTheme.hpp"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimeZone>
#include <QVBoxLayout>

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

struct ItemDisplayRow {
    const ZaloAccountReport* account = nullptr;
    const ZaloEntry* entry = nullptr;
    ByteSize physicalImpact = 0;
    QString exactCopyLabel;
};

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
        QStringLiteral("Review bounded human-recognizable identity, physical cluster impact, and exact copies."));

    m_chooseButton = new QPushButton(QStringLiteral("Choose Root"), header);
    m_chooseButton->setObjectName(QStringLiteral("slZaloChooseRoot"));
    markSecondaryButton(m_chooseButton);
    m_reviewButton = new QPushButton(QStringLiteral("Review"), header);
    m_reviewButton->setObjectName(QStringLiteral("slZaloReview"));
    m_reviewButton->setToolTip(
        QStringLiteral("Run a read-only bounded review; no filesystem changes are made."));
    markPrimaryButton(m_reviewButton);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), header);
    m_cancelButton->setObjectName(QStringLiteral("slZaloCancel"));
    markSecondaryButton(m_cancelButton);
    m_cancelButton->hide();
    header->commands()->addWidget(m_chooseButton);
    header->commands()->addWidget(m_reviewButton);
    header->commands()->addWidget(m_cancelButton);
    rootLayout->addWidget(header);

    auto* rootRow = new QHBoxLayout();
    rootRow->setSpacing(kUiSpace8);
    m_rootSummary = new QLabel(
        QStringLiteral("Default discovery is ready. Native locations stay hidden."),
        this);
    m_rootSummary->setObjectName(QStringLiteral("slZaloRootSummary"));
    m_rootSummary->setWordWrap(true);
    rootRow->addWidget(m_rootSummary, 1);
    rootLayout->addLayout(rootRow);

    m_metrics = new MetricStrip(this);
    rootLayout->addWidget(m_metrics);

    m_reportSummary = new QLabel(this);
    m_reportSummary->setObjectName(QStringLiteral("slZaloSummary"));
    m_reportSummary->setWordWrap(true);
    m_reportSummary->hide();
    rootLayout->addWidget(m_reportSummary);

    m_stack = new QStackedWidget(this);
    m_empty = new EmptyStateWidget(m_stack);
    m_empty->setTitle(QStringLiteral("Ready for read-only review"));
    m_empty->setBody(QStringLiteral(
        "Choose an explicit root, or use Review for bounded default discovery. "
        "Deterministic human identity, visual thumbnails, and physical cluster impact are shown."));
    m_empty->setActionText(QStringLiteral("Choose Root"));
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
    m_entries->setSelectionMode(QAbstractItemView::SingleSelection);
    m_entries->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_entries->setShowGrid(false);
    m_entries->setWordWrap(false);
    m_entries->setIconSize(QSize(96, 60));
    m_entries->verticalHeader()->setVisible(false);
    m_entries->verticalHeader()->setDefaultSectionSize(68);
    m_entries->horizontalHeader()->setStretchLastSection(false);
    m_entries->horizontalHeader()->setDefaultSectionSize(110);
    m_entries->setColumnWidth(ColPreview, 104);
    m_entries->setColumnWidth(ColName, 220);
    m_entries->setColumnWidth(ColSummary, 240);
    m_entries->setColumnWidth(ColPhysicalImpact, 110);
    m_entries->setColumnWidth(ColLogical, 90);
    m_entries->setColumnWidth(ColAllocated, 90);
    m_entries->setColumnWidth(ColExactCopy, 110);
    m_entries->setColumnWidth(ColAge, 130);
    m_entries->setColumnWidth(ColConfidence, 90);
    m_entries->setColumnWidth(ColCategory, 85);
    m_entries->setColumnWidth(ColRootAccount, 110);
    m_entries->setColumnWidth(ColEntry, 80);

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
    connect(m_empty, &EmptyStateWidget::actionClicked, this,
            &ZaloStorageReviewPage::onChooseRoot);
}

void ZaloStorageReviewPage::clearReport()
{
    m_report.reset();
    m_entries->setRowCount(0);
    m_reportSummary->clear();
    m_reportSummary->hide();
    m_metrics->setItems({});
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
    clearReport();
    m_rootSummary->setText(
        QStringLiteral("Explicit root selected. Native location hidden."));
    showEmptyState(QStringLiteral("Ready to review"),
                   QStringLiteral("Press Review to inspect this exact root in read-only mode."),
                   false);
    updateActionState();
    emit statusMessage(QStringLiteral("Explicit root selected. Ready to review."));
}

void ZaloStorageReviewPage::onReview()
{
    if (m_session == nullptr) {
        return;
    }
    clearReport();
    showEmptyState(QStringLiteral("Reviewing Zalo storage"),
                   QStringLiteral("Reading bounded storage evidence…"), false);
    if (!m_session->start(m_selectedRoot)) {
        showEmptyState(QStringLiteral("Review is already running"),
                       QStringLiteral("Wait for the current read-only review to finish."),
                       false);
        updateActionState();
        return;
    }
    updateActionState();
    emit statusMessage(QStringLiteral("Reviewing Zalo storage…"));
}

void ZaloStorageReviewPage::onCancel()
{
    if (m_session != nullptr && m_session->isRunning()) {
        m_session->cancel();
        emit statusMessage(QStringLiteral("Cancelling review…"));
    }
}

void ZaloStorageReviewPage::onSessionStatus(const QString& message)
{
    emit statusMessage(message);
}

void ZaloStorageReviewPage::onFinished(ZaloStorageStatus status)
{
    const auto report = m_session->takeReport();
    updateActionState();
    if (!report.has_value()) {
        emit statusMessage(
            QStringLiteral("Review finished without a report (%1).")
                .arg(storageStatus(status)));
        return;
    }

    m_report = std::move(*report);
    applyReport(*m_report);
    emit statusMessage(QStringLiteral("Zalo review: %1.")
                           .arg(storageStatus(m_report->status)));
}

void ZaloStorageReviewPage::applyReport(const ZaloStorageReport& report)
{
    m_entries->setUpdatesEnabled(false);
    m_entries->setRowCount(0);

    // Build exact copy mapping
    std::unordered_map<std::string, QString> exactCopyMap;
    for (std::size_t m = 0; m < report.exactCopy.matches.size(); ++m) {
        const auto& match = report.exactCopy.matches[m];
        const QString matchLabel = QStringLiteral("Copy Match #%1").arg(m + 1);
        exactCopyMap[match.zaloEntryId] = matchLabel;
    }

    // Collect all entries across accounts and compute physical impact
    std::vector<ItemDisplayRow> rows;
    for (const auto& account : report.accounts) {
        for (const auto& entry : account.entries) {
            ItemDisplayRow row;
            row.account = &account;
            row.entry = &entry;
            row.physicalImpact = entry.allObservedPathReleaseBytes.value_or(
                entry.singlePathReleaseBytes.value_or(
                    entry.allocatedBytes.value_or(entry.logicalBytes)));
            
            const auto it = exactCopyMap.find(entry.entryId);
            if (it != exactCopyMap.end()) {
                row.exactCopyLabel = it->second;
            } else {
                row.exactCopyLabel = QStringLiteral("Unique");
            }
            rows.push_back(std::move(row));
        }
    }

    // Sort by physical impact descending by default
    std::sort(rows.begin(), rows.end(), [](const ItemDisplayRow& a, const ItemDisplayRow& b) {
        if (a.physicalImpact != b.physicalImpact) {
            return a.physicalImpact > b.physicalImpact;
        }
        if (a.entry->logicalBytes != b.entry->logicalBytes) {
            return a.entry->logicalBytes > b.entry->logicalBytes;
        }
        return a.entry->entryId < b.entry->entryId;
    });

    for (const auto& r : rows) {
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
            if (hid.previewAvailable) {
                previewPixmap = m_previewProvider.getPreviewPixmap(
                    QString::fromStdString(hid.previewReference),
                    hid, QSize(96, 60));
            }
        } else {
            displayName = QStringLiteral("Unknown Zalo data");
            summary = QStringLiteral("Content could not be identified safely");
        }

        // Preview item
        auto* previewItem = new QTableWidgetItem();
        if (!previewPixmap.isNull()) {
            previewItem->setIcon(QIcon(previewPixmap));
        }
        previewItem->setTextAlignment(Qt::AlignCenter);
        m_entries->setItem(rowIdx, ColPreview, previewItem);

        // Name
        auto* nameItem = new QTableWidgetItem(displayName);
        nameItem->setToolTip(displayName);
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

        // Category
        auto* catItem = new QTableWidgetItem(QString::fromStdString(entry.categoryAlias));
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

    const auto& accounting = report.accounting;
    const QLocale locale;
    m_metrics->setItems({
        {QStringLiteral("Status"), storageStatus(report.status), {}},
        {QStringLiteral("Root aliases"),
         locale.toString(static_cast<qulonglong>(report.roots.size())), {}},
        {QStringLiteral("Account aliases"),
         locale.toString(static_cast<qulonglong>(report.accounts.size())), {}},
        {QStringLiteral("Entries"),
         locale.toString(static_cast<qulonglong>(rows.size())), {}},
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
         QStringLiteral("Physical reclaimable footprint; read-only review.")},
    });

    m_reportSummary->setText(
        QStringLiteral("%1 · %2 root(s) · %3 account(s) · %4 entries · Sorted by Physical Impact")
            .arg(storageStatus(report.status))
            .arg(report.roots.size())
            .arg(report.accounts.size())
            .arg(rows.size()));
    m_reportSummary->setVisible(true);

    if (rows.empty()) {
        showEmptyState(
            QStringLiteral("No Zalo entries found"),
            QStringLiteral("The review completed but found no files in the scanned directories."),
            true);
    } else {
        m_stack->setCurrentWidget(m_entries);
    }
}

}  // namespace spacelens
