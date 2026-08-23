#include "ui/ZaloStorageReviewPage.hpp"

#include "core/SafetyPolicy.hpp"
#include "core/SizeFormatter.hpp"
#include "ui/EmptyStateWidget.hpp"
#include "ui/MetricStrip.hpp"
#include "ui/PageHeader.hpp"
#include "ui/UiTheme.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimeZone>
#include <QUrl>
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

    // 1. Normalize path separators to standard Windows backslashes
    const std::wstring normalized = normalizePathForPolicy(path);
    if (normalized.empty()) {
        return false;
    }

    // 2. Safety policy gate: Forbid deleting OS, Program Files, drive roots, or system directories
    const LocationSafety safety = classifyLocation(normalized);
    if (isMutationDisallowed(safety)) {
        return false;
    }

    // 3. Verify path exists and query file attributes
    const DWORD attrs = ::GetFileAttributesW(normalized.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    // 4. Critical guard: Refuse to delete directory trees via single entry deletion
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    // 5. Double null-terminated buffer strictly required by SHFileOperationW
    std::vector<wchar_t> doubleNullPath(normalized.begin(), normalized.end());
    doubleNullPath.push_back(L'\0');
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

void openPlayableFile(const std::wstring& path, const ZaloEntry* entry = nullptr)
{
    if (path.empty()) {
        return;
    }
    const QString origPath = QString::fromStdWString(path);
    if (!QFileInfo::exists(origPath)) {
        revealInExplorer(path);
        return;
    }

    QFileInfo origFi(origPath);
    QString ext = origFi.suffix().toLower();

    // Check if wrapped payload or needs extension
    bool isWrapped = false;
    ByteSize offset = 0;
    if (entry != nullptr && entry->contentIdentification.has_value()) {
        const auto& cid = *entry->contentIdentification;
        if (cid.wrapper && cid.payloadOffset > 0) {
            isWrapped = true;
            offset = cid.payloadOffset;
        }
        if (ext.isEmpty()) {
            switch (cid.type) {
            case ZaloContentType::Mp4: ext = QStringLiteral("mp4"); break;
            case ZaloContentType::Mov: ext = QStringLiteral("mov"); break;
            case ZaloContentType::Jpeg: ext = QStringLiteral("jpg"); break;
            case ZaloContentType::Png: ext = QStringLiteral("png"); break;
            case ZaloContentType::Webp: ext = QStringLiteral("webp"); break;
            case ZaloContentType::Gif: ext = QStringLiteral("gif"); break;
            case ZaloContentType::Zip: ext = QStringLiteral("zip"); break;
            case ZaloContentType::Pdf: ext = QStringLiteral("pdf"); break;
            default: break;
            }
        }
    }

    if (ext.isEmpty() && entry != nullptr && entry->humanIdentity.has_value()) {
        if (entry->humanIdentity->previewKind == ZaloPreviewKind::VideoContactSheet) {
            ext = QStringLiteral("mp4");
        } else if (entry->humanIdentity->previewKind == ZaloPreviewKind::Image) {
            ext = QStringLiteral("jpg");
        }
    }

    if (ext.isEmpty() && entry != nullptr) {
        if (entry->categoryAlias == "video") {
            ext = QStringLiteral("mp4");
        } else if (entry->categoryAlias == "photo") {
            ext = QStringLiteral("jpg");
        }
    }

    // If original file already has extension and is not wrapped, open directly
    if (!origFi.suffix().isEmpty() && !isWrapped) {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(origPath))) {
            revealInExplorer(path);
        }
        return;
    }

    const QString targetExt = !ext.isEmpty() ? ext : QStringLiteral("mp4");
    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/SpaceLens/playable");
    QDir().mkpath(tempDir);
    const QString tempFilePath = tempDir + QStringLiteral("/") + origFi.fileName() + QStringLiteral(".") + targetExt;

    if (!QFileInfo::exists(tempFilePath)) {
        if (!isWrapped) {
            // Fast hard link (instant, 0 disk cost)
            if (!::CreateHardLinkW(tempFilePath.toStdWString().c_str(), path.c_str(), nullptr)) {
                ::CopyFileW(path.c_str(), tempFilePath.toStdWString().c_str(), FALSE);
            }
        } else {
            // Extract pure payload
            QFile src(origPath);
            if (src.open(QIODevice::ReadOnly)) {
                src.seek(offset);
                QFile dst(tempFilePath);
                if (dst.open(QIODevice::WriteOnly)) {
                    dst.write(src.readAll());
                    dst.close();
                }
                src.close();
            }
        }
    }

    const QString toOpen = QFileInfo::exists(tempFilePath) ? tempFilePath : origPath;
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(toOpen))) {
        ::ShellExecuteW(nullptr, L"open", toOpen.toStdWString().c_str(), nullptr, nullptr, SW_SHOW);
    }
}

}  // namespace

ZaloStorageReviewPage::ZaloStorageReviewPage(QWidget* parent)
    : QWidget(parent)
    , m_session(new ZaloStorageSession(this))
{
    buildUi();
    connect(m_session, &ZaloStorageSession::statusMessage, this,
            &ZaloStorageReviewPage::onSessionStatus);
    connect(m_session, &ZaloStorageSession::progressUpdated, this,
            &ZaloStorageReviewPage::onProgressUpdated);
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

    m_entries->installEventFilter(this);

    auto* deleteShortcut = new QShortcut(QKeySequence::Delete, m_entries);
    connect(deleteShortcut, &QShortcut::activated, this,
            &ZaloStorageReviewPage::onDeleteSelected);

    m_scanningWidget = buildScanningWidget();

    m_stack->addWidget(m_empty);
    m_stack->addWidget(m_scanningWidget);
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

QWidget* ZaloStorageReviewPage::buildScanningWidget()
{
    auto* container = new QWidget(this);
    auto* outerLayout = new QVBoxLayout(container);
    outerLayout->setContentsMargins(kUiSpace24, kUiSpace24, kUiSpace24, kUiSpace24);
    outerLayout->setAlignment(Qt::AlignCenter);

    auto* card = new QFrame(container);
    card->setObjectName(QStringLiteral("slZaloScanCard"));
    card->setMaximumWidth(720);
    card->setMinimumWidth(540);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setSpacing(kUiSpace16);
    cardLayout->setContentsMargins(kUiSpace24, kUiSpace24, kUiSpace24, kUiSpace24);

    // 1. Header: Icon + Title + Subtitle
    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(kUiSpace12);

    auto* iconLabel = new QLabel(QStringLiteral("🔍"), card);
    QFont iconFont = iconLabel->font();
    iconFont.setPixelSize(28);
    iconLabel->setFont(iconFont);
    headerRow->addWidget(iconLabel);

    auto* titleLayout = new QVBoxLayout();
    titleLayout->setSpacing(2);
    m_scanTitleLabel = new QLabel(QStringLiteral("Scanning Zalo Storage..."), card);
    QFont titleFont = m_scanTitleLabel->font();
    titleFont.setPixelSize(16);
    titleFont.setBold(true);
    m_scanTitleLabel->setFont(titleFont);
    titleLayout->addWidget(m_scanTitleLabel);

    m_scanSubtitleLabel = new QLabel(
        QStringLiteral("Discovering accounts, verifying file identity, and analyzing media and cache footprint..."),
        card);
    m_scanSubtitleLabel->setWordWrap(true);
    titleLayout->addWidget(m_scanSubtitleLabel);
    headerRow->addLayout(titleLayout, 1);
    cardLayout->addLayout(headerRow);

    // 2. Progress Bar
    m_scanProgressBar = new QProgressBar(card);
    m_scanProgressBar->setObjectName(QStringLiteral("slZaloScanProgressBar"));
    m_scanProgressBar->setFixedHeight(6);
    m_scanProgressBar->setTextVisible(false);
    m_scanProgressBar->setRange(0, 0);
    cardLayout->addWidget(m_scanProgressBar);

    // 3. Stats Strip (3 column stat blocks)
    auto* statsGrid = new QHBoxLayout();
    statsGrid->setSpacing(kUiSpace12);

    auto createStatCard = [card](const QString& title, QLabel*& valueLabel) -> QWidget* {
        auto* box = new QFrame(card);
        box->setObjectName(QStringLiteral("slZaloStatBox"));
        box->setAutoFillBackground(true);
        auto* lay = new QVBoxLayout(box);
        lay->setContentsMargins(kUiSpace12, kUiSpace8, kUiSpace12, kUiSpace8);
        lay->setSpacing(2);

        auto* t = new QLabel(title, box);
        QFont tf = t->font();
        tf.setPixelSize(10);
        tf.setWeight(QFont::DemiBold);
        t->setFont(tf);
        t->setObjectName(QStringLiteral("slZaloStatTitle"));
        lay->addWidget(t);

        valueLabel = new QLabel(QStringLiteral("0"), box);
        QFont vf = valueLabel->font();
        vf.setPixelSize(17);
        vf.setBold(true);
        valueLabel->setFont(vf);
        valueLabel->setObjectName(QStringLiteral("slZaloStatValue"));
        lay->addWidget(valueLabel);

        return box;
    };

    statsGrid->addWidget(createStatCard(QStringLiteral("FILES SCANNED"), m_scanFilesValue), 1);
    statsGrid->addWidget(createStatCard(QStringLiteral("DATA ANALYZED"), m_scanBytesValue), 1);
    statsGrid->addWidget(createStatCard(QStringLiteral("CURRENT PHASE"), m_scanPhaseValue), 1);
    cardLayout->addLayout(statsGrid);

    // 4. Discovered Categories Breakdown
    auto* catSection = new QVBoxLayout();
    catSection->setSpacing(kUiSpace8);

    auto* catTitle = new QLabel(QStringLiteral("Discovered Content Breakdown:"), card);
    QFont catTitleFont = catTitle->font();
    catTitleFont.setPixelSize(12);
    catTitleFont.setWeight(QFont::Medium);
    catTitle->setFont(catTitleFont);
    catSection->addWidget(catTitle);

    auto* chipsRow = new QHBoxLayout();
    chipsRow->setSpacing(kUiSpace8);

    auto createChip = [card](const QString& icon, const QString& name, QLabel*& badge) -> QWidget* {
        auto* chip = new QFrame(card);
        chip->setObjectName(QStringLiteral("slZaloChip"));
        auto* lay = new QHBoxLayout(chip);
        lay->setContentsMargins(kUiSpace8, kUiSpace4, kUiSpace8, kUiSpace4);
        lay->setSpacing(4);

        auto* lbl = new QLabel(QStringLiteral("%1 %2:").arg(icon, name), chip);
        QFont lf = lbl->font();
        lf.setPixelSize(11);
        lbl->setFont(lf);
        lay->addWidget(lbl);

        badge = new QLabel(QStringLiteral("0"), chip);
        QFont bf = badge->font();
        bf.setPixelSize(11);
        bf.setBold(true);
        badge->setFont(bf);
        lay->addWidget(badge);

        return chip;
    };

    chipsRow->addWidget(createChip(QStringLiteral("🖼️"), QStringLiteral("Photos"), m_scanPhotoBadge));
    chipsRow->addWidget(createChip(QStringLiteral("🎬"), QStringLiteral("Videos"), m_scanVideoBadge));
    chipsRow->addWidget(createChip(QStringLiteral("🗑️"), QStringLiteral("fileNoise"), m_scanNoiseBadge));
    chipsRow->addWidget(createChip(QStringLiteral("📦"), QStringLiteral("Cache"), m_scanCacheBadge));
    chipsRow->addWidget(createChip(QStringLiteral("📄"), QStringLiteral("Docs"), m_scanDocBadge));
    catSection->addLayout(chipsRow);
    cardLayout->addLayout(catSection);

    // 5. Current File Path Indicator
    auto* pathRow = new QHBoxLayout();
    pathRow->setSpacing(kUiSpace8);
    auto* pathIcon = new QLabel(QStringLiteral("📂"), card);
    pathRow->addWidget(pathIcon);

    m_scanCurrentPath = new QLabel(QStringLiteral("Discovering storage roots..."), card);
    QFont pathFont = m_scanCurrentPath->font();
    pathFont.setPixelSize(11);
    m_scanCurrentPath->setFont(pathFont);
    m_scanCurrentPath->setObjectName(QStringLiteral("slZaloPathIndicator"));
    pathRow->addWidget(m_scanCurrentPath, 1);
    cardLayout->addLayout(pathRow);

    // 6. Action Button (Cancel)
    auto* bottomRow = new QHBoxLayout();
    bottomRow->addStretch();
    auto* cancelScanBtn = new QPushButton(QStringLiteral("Cancel Scan"), card);
    cancelScanBtn->setObjectName(QStringLiteral("slZaloCardCancel"));
    markSecondaryButton(cancelScanBtn);
    connect(cancelScanBtn, &QPushButton::clicked, this, &ZaloStorageReviewPage::onCancel);
    bottomRow->addWidget(cancelScanBtn);
    bottomRow->addStretch();
    cardLayout->addLayout(bottomRow);

    outerLayout->addWidget(card);
    return container;
}

void ZaloStorageReviewPage::resetScanningWidget()
{
    if (m_scanTitleLabel != nullptr) {
        m_scanTitleLabel->setText(QStringLiteral("Scanning Zalo Storage..."));
    }
    if (m_scanSubtitleLabel != nullptr) {
        m_scanSubtitleLabel->setText(
            QStringLiteral("Discovering accounts, verifying file identity, and analyzing media and cache footprint..."));
    }
    if (m_scanProgressBar != nullptr) {
        m_scanProgressBar->setRange(0, 0);
        m_scanProgressBar->setTextVisible(false);
    }
    if (m_scanFilesValue != nullptr) {
        m_scanFilesValue->setText(QStringLiteral("0"));
    }
    if (m_scanBytesValue != nullptr) {
        m_scanBytesValue->setText(QStringLiteral("0 B"));
    }
    if (m_scanPhaseValue != nullptr) {
        m_scanPhaseValue->setText(QStringLiteral("Discovering roots..."));
    }
    if (m_scanPhotoBadge != nullptr) {
        m_scanPhotoBadge->setText(QStringLiteral("0"));
    }
    if (m_scanVideoBadge != nullptr) {
        m_scanVideoBadge->setText(QStringLiteral("0"));
    }
    if (m_scanNoiseBadge != nullptr) {
        m_scanNoiseBadge->setText(QStringLiteral("0"));
    }
    if (m_scanCacheBadge != nullptr) {
        m_scanCacheBadge->setText(QStringLiteral("0"));
    }
    if (m_scanDocBadge != nullptr) {
        m_scanDocBadge->setText(QStringLiteral("0"));
    }
    if (m_scanCurrentPath != nullptr) {
        m_scanCurrentPath->setText(QStringLiteral("Discovering storage roots..."));
    }
}

void ZaloStorageReviewPage::onProgressUpdated(const spacelens::ZaloScanProgress& progress)
{
    const QLocale locale;
    if (m_scanFilesValue != nullptr) {
        m_scanFilesValue->setText(locale.toString(static_cast<qulonglong>(progress.filesScanned)));
    }
    if (m_scanBytesValue != nullptr) {
        m_scanBytesValue->setText(formatBytes(progress.bytesScanned));
    }
    if (m_scanPhaseValue != nullptr && !progress.phase.empty()) {
        m_scanPhaseValue->setText(QString::fromUtf8(progress.phase));
    }
    if (m_scanPhotoBadge != nullptr) {
        m_scanPhotoBadge->setText(locale.toString(static_cast<qulonglong>(progress.photoCount)));
    }
    if (m_scanVideoBadge != nullptr) {
        m_scanVideoBadge->setText(locale.toString(static_cast<qulonglong>(progress.videoCount)));
    }
    if (m_scanNoiseBadge != nullptr) {
        m_scanNoiseBadge->setText(locale.toString(static_cast<qulonglong>(progress.fileNoiseCount)));
    }
    if (m_scanCacheBadge != nullptr) {
        m_scanCacheBadge->setText(locale.toString(static_cast<qulonglong>(progress.cacheCount)));
    }
    if (m_scanDocBadge != nullptr) {
        m_scanDocBadge->setText(locale.toString(static_cast<qulonglong>(progress.documentCount)));
    }
    if (m_scanProgressBar != nullptr) {
        if (progress.totalFilesToIdentify > 0) {
            m_scanProgressBar->setRange(0, static_cast<int>(progress.totalFilesToIdentify));
            m_scanProgressBar->setValue(static_cast<int>(progress.filesIdentified));
            m_scanProgressBar->setTextVisible(true);
        } else {
            m_scanProgressBar->setRange(0, 0);
            m_scanProgressBar->setTextVisible(false);
        }
    }
    if (m_scanCurrentPath != nullptr && !progress.currentPath.empty()) {
        const QString pathStr = QString::fromStdWString(progress.currentPath);
        m_scanCurrentPath->setText(
            m_scanCurrentPath->fontMetrics().elidedText(pathStr, Qt::ElideMiddle, 500));
        m_scanCurrentPath->setToolTip(pathStr);
    }
    if (m_rootSummary != nullptr) {
        m_rootSummary->setText(
            QStringLiteral("Scanning: %1 files · %2 · %3")
                .arg(locale.toString(static_cast<qulonglong>(progress.filesScanned)),
                     formatBytes(progress.bytesScanned),
                     QString::fromUtf8(progress.phase)));
    }
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
    resetScanningWidget();
    m_stack->setCurrentWidget(m_scanningWidget);
    m_rootSummary->setText(
        QStringLiteral("Scanning Zalo storage across drives... Please wait."));
    emit statusMessage(QStringLiteral("Scanning Zalo storage..."));

    m_session->start(m_selectedRoot);
}

void ZaloStorageReviewPage::onCancel()
{
    if (m_session != nullptr && m_session->isRunning()) {
        m_session->cancel();
        m_rootSummary->setText(QStringLiteral("Cancelling scan..."));
        if (m_scanTitleLabel != nullptr) {
            m_scanTitleLabel->setText(QStringLiteral("Cancelling scan..."));
        }
        if (m_scanSubtitleLabel != nullptr) {
            m_scanSubtitleLabel->setText(QStringLiteral("Stopping Zalo storage inspection... Please wait."));
        }
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

        QString actualFileName;
        if (!entry.nativePath.empty()) {
            actualFileName = QFileInfo(QString::fromStdWString(entry.nativePath)).fileName();
        }

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
                    hid, QSize(96, 56));
            }
        } else {
            displayName = QStringLiteral("Unknown Zalo data");
            summary = QStringLiteral("Content could not be identified safely");
        }

        // Display actual disk filename so users can immediately recognize their files
        if (!actualFileName.isEmpty()) {
            if (displayName.isEmpty() ||
                displayName == QStringLiteral("Unknown Zalo data") ||
                displayName.startsWith(QStringLiteral("MP4 Video")) ||
                displayName.startsWith(QStringLiteral("MOV Video")) ||
                displayName.startsWith(QStringLiteral("JPEG Image")) ||
                displayName.startsWith(QStringLiteral("PNG Image")) ||
                displayName.startsWith(QStringLiteral("WebP Image")) ||
                displayName.startsWith(QStringLiteral("GIF Image")) ||
                displayName.startsWith(QStringLiteral("PDF Document")) ||
                displayName.startsWith(QStringLiteral("DOCX Document")) ||
                displayName.startsWith(QStringLiteral("XLSX Document")) ||
                displayName.startsWith(QStringLiteral("PPTX Document")) ||
                displayName.startsWith(QStringLiteral("ZIP Archive")) ||
                displayName.startsWith(QStringLiteral("Zalo item"))) {
                displayName = actualFileName;
            }
        }

        const QString fullPathStr = QString::fromStdWString(entry.nativePath);

        const QString fullTooltip =
            QStringLiteral("<b>%1</b><br/>"
                           "<b>Path:</b> %2<br/>"
                           "<b>Type / Summary:</b> %3<br/>"
                           "<b>Size:</b> %4 (Physical Impact: %5)<br/>"
                           "<b>Modified:</b> %6<br/>"
                           "<hr/>"
                           "<i>• Double-click or Enter: Open / Play file<br/>"
                           "• Right-click or Del: Delete to Recycle Bin</i>")
                .arg((!actualFileName.isEmpty() ? actualFileName : displayName).toHtmlEscaped())
                .arg(fullPathStr.toHtmlEscaped())
                .arg(summary.toHtmlEscaped())
                .arg(formatBytes(entry.logicalBytes))
                .arg(formatBytes(r.physicalImpact))
                .arg(formatFileAge(entry.lastWriteTicks));

        // Preview item
        auto* previewItem = new QTableWidgetItem();
        if (!previewPixmap.isNull()) {
            previewItem->setIcon(QIcon(previewPixmap));
        }
        previewItem->setTextAlignment(Qt::AlignCenter);
        previewItem->setToolTip(fullTooltip);
        previewItem->setData(Qt::UserRole, fullPathStr);
        previewItem->setData(Qt::UserRole + 1, static_cast<qulonglong>(r.physicalImpact));
        m_entries->setItem(rowIdx, ColPreview, previewItem);

        // Name
        auto* nameItem = new QTableWidgetItem(displayName);
        nameItem->setToolTip(fullTooltip);
        m_entries->setItem(rowIdx, ColName, nameItem);

        // Summary
        auto* summaryItem = new QTableWidgetItem(summary);
        summaryItem->setToolTip(fullTooltip);
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
            categoryTooltip = QStringLiteral("Transit Cache · Safe to delete");
        } else if (entry.categoryAlias == "resource") {
            categoryLabel = QStringLiteral("resource [Chat Cache]");
            categoryTooltip = QStringLiteral("Chat media cache · Review before delete");
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
    
    QAction* openAction = menu.addAction(QStringLiteral("▶ Open / Play File"));
    QAction* revealAction = menu.addAction(QStringLiteral("📂 Reveal in File Explorer"));
    QAction* copyPathAction = menu.addAction(QStringLiteral("📋 Copy File Path"));
    menu.addSeparator();
    
    QString deleteLabel = QStringLiteral("🗑️ Delete to Recycle Bin");
    if (selected.size() > 1) {
        deleteLabel = QStringLiteral("🗑️ Delete %1 Selected to Recycle Bin").arg(selected.size());
    }
    QAction* deleteAction = menu.addAction(deleteLabel);

    QAction* chosen = menu.exec(m_entries->viewport()->mapToGlobal(pos));
    if (chosen == openAction) {
        onOpenFile();
    } else if (chosen == revealAction) {
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
        openPlayableFile(m_displayRows[row].nativePath, m_displayRows[row].entry);
    }
}

void ZaloStorageReviewPage::onOpenFile()
{
    const int row = m_entries ? m_entries->currentRow() : -1;
    if (row >= 0 && static_cast<std::size_t>(row) < m_displayRows.size()) {
        openPlayableFile(m_displayRows[row].nativePath, m_displayRows[row].entry);
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

bool ZaloStorageReviewPage::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_entries && event != nullptr && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Delete) {
            onDeleteSelected();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter ||
            keyEvent->key() == Qt::Key_Space) {
            onOpenFile();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
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
    // Strict descending sort and deduplication to prevent out-of-order deletion or multiple erasures
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    struct TargetItem {
        int row = -1;
        std::wstring path;
        ByteSize bytes = 0;
        QString name;
    };

    std::vector<TargetItem> items;
    items.reserve(rows.size());
    ByteSize totalBytes = 0;

    for (const int r : rows) {
        if (r >= 0 && static_cast<std::size_t>(r) < m_displayRows.size()) {
            std::wstring path = m_displayRows[r].nativePath;
            const ByteSize bytes = m_displayRows[r].physicalImpact;
            QString name;
            if (auto* item = m_entries->item(r, ColName)) {
                name = item->text();
            }
            if (auto* prevItem = m_entries->item(r, ColPreview)) {
                const QString storedPath = prevItem->data(Qt::UserRole).toString();
                if (!storedPath.isEmpty()) {
                    path = storedPath.toStdWString();
                }
            }
            if (!path.empty()) {
                totalBytes += bytes;
                items.push_back({r, std::move(path), bytes, std::move(name)});
            }
        }
    }

    if (items.empty()) {
        return;
    }

    const QString title = QStringLiteral("Confirm Move to Recycle Bin");
    QString message;
    if (items.size() == 1) {
        message = QStringLiteral(
                      "Move this file to the Windows Recycle Bin?\n\n%1 (%2)\n\nPath: %3\n\n"
                      "Note: You can restore this file from the Windows Recycle Bin if needed.")
                      .arg(items.front().name, formatBytes(totalBytes),
                           QString::fromStdWString(items.front().path));
    } else {
        message = QStringLiteral(
                      "Move %1 selected items (~%2) to the Windows Recycle Bin?\n\n"
                      "Note: You can restore any of these files from the Windows Recycle Bin if needed.")
                      .arg(items.size())
                      .arg(formatBytes(totalBytes));
    }

    const auto reply = QMessageBox::question(
        this, title, message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    std::size_t successCount = 0;
    for (const auto& item : items) {
        if (sendFileToRecycleBin(item.path)) {
            ++successCount;
            m_entries->removeRow(item.row);
            if (static_cast<std::size_t>(item.row) < m_displayRows.size()) {
                m_displayRows.erase(m_displayRows.begin() + item.row);
            }
        }
    }

    if (successCount == items.size()) {
        emit statusMessage(
            QStringLiteral("Successfully moved %1 item(s) to Recycle Bin.")
                .arg(successCount));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("Partial Delete"),
            QStringLiteral("Moved %1 of %2 items to Recycle Bin. Some files may be open or locked by Zalo.")
                .arg(successCount)
                .arg(items.size()));
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
    // Find all rows that belong to file-noise
    std::vector<int> candidateRows;
    ByteSize totalBytes = 0;

    for (int r = 0; r < static_cast<int>(m_displayRows.size()); ++r) {
        const auto& row = m_displayRows[r];
        if (row.entry != nullptr && row.entry->categoryAlias == "file-noise" &&
            !row.nativePath.empty()) {
            candidateRows.push_back(r);
            if (!row.entry->hardLinkAlias) {
                totalBytes += row.physicalImpact;
            }
        }
    }

    if (candidateRows.empty()) {
        QMessageBox::information(this, QStringLiteral("Clean fileNoise"),
                                 QStringLiteral("No fileNoise cache files found."));
        m_cleanFileNoiseButton->hide();
        return;
    }

    const auto reply = QMessageBox::question(
        this, QStringLiteral("Confirm Clean fileNoise"),
        QStringLiteral("Clean %1 ephemeral cache files (~%2)?\n\n"
                       "All files will be moved safely to your Windows Recycle Bin.")
            .arg(candidateRows.size())
            .arg(formatBytes(totalBytes)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    // Sort candidate row indices in descending order so removal doesn't invalidate lower indices
    std::sort(candidateRows.begin(), candidateRows.end(), std::greater<int>());

    std::size_t deletedCount = 0;
    ByteSize freedBytes = 0;
    m_entries->setUpdatesEnabled(false);

    for (const int r : candidateRows) {
        if (r >= 0 && static_cast<std::size_t>(r) < m_displayRows.size()) {
            const auto& row = m_displayRows[r];
            if (sendFileToRecycleBin(row.nativePath)) {
                ++deletedCount;
                freedBytes += row.physicalImpact;
                m_entries->removeRow(r);
                m_displayRows.erase(m_displayRows.begin() + r);
            }
        }
    }

    m_entries->setUpdatesEnabled(true);

    // Calculate remaining file-noise
    ByteSize remainingNoiseBytes = 0;
    for (const auto& row : m_displayRows) {
        if (row.entry != nullptr && row.entry->categoryAlias == "file-noise" &&
            !row.nativePath.empty() && !row.entry->hardLinkAlias) {
            remainingNoiseBytes += row.physicalImpact;
        }
    }

    if (remainingNoiseBytes > 0) {
        m_cleanFileNoiseButton->setText(
            QStringLiteral("Clean fileNoise (%1)").arg(formatBytes(remainingNoiseBytes)));
        m_cleanFileNoiseButton->show();
    } else {
        m_cleanFileNoiseButton->hide();
    }

    QMessageBox::information(
        this, QStringLiteral("Clean Complete"),
        QStringLiteral("Successfully moved %1 of %2 cache files to the Recycle Bin (~%3 freed).")
            .arg(deletedCount)
            .arg(candidateRows.size())
            .arg(formatBytes(freedBytes)));

    if (m_displayRows.empty()) {
        clearReport();
        showEmptyState(
            QStringLiteral("All items cleaned"),
            QStringLiteral("All reviewed items have been moved to the Recycle Bin."),
            true);
    } else {
        m_reportSummary->setText(
            QStringLiteral("%1 remaining entries · Sorted by Physical Impact · Right-click or double-click to reveal/delete.")
                .arg(m_displayRows.size()));
        onSelectionChanged();
    }

    emit statusMessage(
        QStringLiteral("Cleaned %1 cache file(s) (%2 freed).")
            .arg(deletedCount)
            .arg(formatBytes(freedBytes)));
}

}  // namespace spacelens
