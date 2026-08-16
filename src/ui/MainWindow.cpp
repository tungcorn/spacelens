#include "ui/MainWindow.hpp"

#include "ui/CleanupReviewDialog.hpp"
#include "ui/EmptyStateWidget.hpp"
#include "ui/FilterPopup.hpp"
#include "ui/IndexBrowserPage.hpp"
#include "ui/MetricStrip.hpp"
#include "ui/PageHeader.hpp"
#include "ui/PropertyInspector.hpp"
#include "ui/UiTheme.hpp"

#include "core/Classification.hpp"
#include "core/CleanupRevalidation.hpp"
#include "core/FileTime.hpp"
#include "core/OrdinaryLocation.hpp"
#include "core/ReclaimAnalysis.hpp"
#include "core/SafetyPolicy.hpp"
#include "core/SizeFormatter.hpp"
#include "ui/OrdinaryLocationsDialog.hpp"
#include "core/SizeParse.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/ExplorerIntegration.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace spacelens {
namespace {

QString fromWide(const std::wstring& s)
{
    return QString::fromStdWString(s);
}

std::wstring toWide(const QString& s)
{
    return s.toStdWString();
}

QString formatElapsed(double seconds)
{
    return QStringLiteral("%1 s").arg(seconds, 0, 'f', 1);
}

void addLocationSafety(PropertyInspector& inspector,
                       const LocationSafetyAssessment& assessment)
{
    if (assessment.safety == LocationSafety::Unknown &&
        assessment.source == LocationSafetySource::Unknown) {
        return;
    }
    inspector.addRow(QStringLiteral("Location safety"),
                     QString::fromUtf8(toString(assessment.safety)));
    switch (assessment.source) {
    case LocationSafetySource::BuiltInProtected:
        inspector.addRow(QStringLiteral("Source"),
                         QStringLiteral("Built-in protected rule"));
        break;
    case LocationSafetySource::BuiltInSensitive:
        inspector.addRow(QStringLiteral("Source"),
                         QStringLiteral("Built-in sensitive rule"));
        break;
    case LocationSafetySource::BuiltInOrdinary:
        inspector.addRow(QStringLiteral("Source"),
                         QStringLiteral("Built-in ordinary location"));
        break;
    case LocationSafetySource::UserDeclaredOrdinary:
        inspector.addRow(QStringLiteral("Source"),
                         QStringLiteral("User-declared ordinary location"));
        if (!assessment.declarationPath.empty()) {
            inspector.addRow(
                QStringLiteral("Declared root"),
                QString::fromStdWString(assessment.declarationPath));
        }
        inspector.addNote(
            QStringLiteral("This does not mark files as safe to remove."));
        break;
    case LocationSafetySource::Unknown:
        break;
    }
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_session(std::make_unique<ScanSession>(this))
{
    setWindowTitle(QStringLiteral("SpaceLens"));
    setWindowIcon(QIcon(QStringLiteral(":/assets/icon.png")));
    resize(1280, 800);

    const auto reviewStatus = m_reviewController.openDefault();
    m_revalidationSession = std::make_unique<CleanupRevalidationSession>(
        m_reviewController, this);
    m_maintenanceSession =
        std::make_unique<MaintenanceSession>(m_reviewController, this);

    buildUi();
    updateActionState();
    if (!reviewStatus.ok) {
        setStatusMessage(QStringLiteral("Cleanup Review state unavailable: %1")
                             .arg(QString::fromStdString(reviewStatus.message)));
    } else {
        setStatusMessage(QStringLiteral("Select a folder to scan."));
    }

    connect(m_session.get(), &ScanSession::progressUpdated, this,
            &MainWindow::onProgress);
    connect(m_session.get(), &ScanSession::finished, this,
            &MainWindow::onScanFinished);

    const QString devScan = qEnvironmentVariable("SPACELENS_DEV_SCAN_PATH");
    if (!devScan.isEmpty() && QDir(devScan).exists()) {
        m_rootPath = QDir::toNativeSeparators(devScan);
        if (m_pathEdit) {
            m_pathEdit->setText(m_rootPath);
        }
        updateActionState();
        onScan();
    }
}

MainWindow::~MainWindow()
{
    if (m_session) {
        m_session->cancel();
    }
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* nav = new QWidget(central);
    nav->setObjectName(QStringLiteral("slWorkspaceBar"));
    auto* navLayout = new QHBoxLayout(nav);
    navLayout->setContentsMargins(kUiSpace16, kUiSpace8, kUiSpace16, kUiSpace8);
    navLayout->setSpacing(kUiSpace8);

    auto* group = new QWidget(nav);
    group->setObjectName(QStringLiteral("slWorkspaceGroup"));
    auto* groupLayout = new QHBoxLayout(group);
    groupLayout->setContentsMargins(2, 2, 2, 2);
    groupLayout->setSpacing(0);
    m_navLive = new QPushButton(QStringLiteral("Live Scan"), group);
    m_navIndexed = new QPushButton(QStringLiteral("Indexed"), group);
    m_navLive->setObjectName(QStringLiteral("slWorkspace"));
    m_navIndexed->setObjectName(QStringLiteral("slWorkspace"));
    m_navLive->setProperty("slId", QStringLiteral("live"));
    m_navIndexed->setProperty("slId", QStringLiteral("indexed"));
    m_navLive->setCheckable(true);
    m_navIndexed->setCheckable(true);
    m_navLive->setChecked(true);
    m_navLive->setAutoExclusive(true);
    m_navIndexed->setAutoExclusive(true);
    m_navLive->setFocusPolicy(Qt::TabFocus);
    m_navIndexed->setFocusPolicy(Qt::TabFocus);
    groupLayout->addWidget(m_navLive);
    groupLayout->addWidget(m_navIndexed);
    navLayout->addWidget(group, 0, Qt::AlignVCenter);
    navLayout->addStretch(1);

    auto* navMore = new QToolButton(nav);
    markTertiaryButton(navMore);
    navMore->setText(QStringLiteral("More"));
    navMore->setPopupMode(QToolButton::InstantPopup);
    navMore->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* moreMenu = new QMenu(navMore);
    moreMenu->addAction(QStringLiteral("User-declared ordinary locations…"),
                        this, &MainWindow::onOrdinaryLocations);
    navMore->setMenu(moreMenu);
    navLayout->addWidget(navMore, 0, Qt::AlignVCenter);
    outer->addWidget(nav);

    auto* rule = new QFrame(central);
    rule->setObjectName(QStringLiteral("slHairline"));
    rule->setFrameShape(QFrame::NoFrame);
    outer->addWidget(rule);

    m_pages = new QStackedWidget(central);
    m_pages->setObjectName(QStringLiteral("slPages"));
    m_pages->addWidget(buildLiveScanPage());

    m_indexPage = new IndexBrowserPage(m_reviewController, m_pages);
    m_pages->addWidget(m_indexPage);
    connect(m_indexPage, &IndexBrowserPage::statusMessage, this,
            &MainWindow::onIndexStatusMessage);
    connect(m_indexPage, &IndexBrowserPage::showReviewRequested, this,
            &MainWindow::onShowReview);

    outer->addWidget(m_pages, 1);

    connect(m_navLive, &QPushButton::clicked, this, &MainWindow::onLiveWorkspace);
    connect(m_navIndexed, &QPushButton::clicked, this,
            &MainWindow::onIndexedWorkspace);
    auto* liveShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+1")), this);
    auto* indexedShortcut =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+2")), this);
    connect(liveShortcut, &QShortcut::activated, this,
            &MainWindow::onLiveWorkspace);
    connect(indexedShortcut, &QShortcut::activated, this,
            &MainWindow::onIndexedWorkspace);

    m_statusLabel = new QLabel(central);
    m_trustLabel = new QLabel(QStringLiteral("Read-only analysis"), central);
    m_trustLabel->setObjectName(QStringLiteral("slTrust"));
    m_trustLabel->setToolTip(
        QStringLiteral("CLI and analysis surfaces do not mutate analyzed files.\n"
                       "Maintenance requires explicit GUI authorization."));
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_trustLabel);
}

QWidget* MainWindow::buildLiveScanPage()
{
    auto* page = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(page);
    applyPageMargins(page);

    auto* header = new PageHeader(page);
    header->setTitle(QStringLiteral("Live Scan"));
    header->setSubtitle(
        QStringLiteral("Analyze a folder and see where its storage goes."));
    m_showReviewButton =
        new QPushButton(QStringLiteral("Cleanup Review"), header);
    markSecondaryButton(m_showReviewButton);
    header->commands()->addWidget(m_showReviewButton);
    rootLayout->addWidget(header);

    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(kUiSpace8);
    m_pathEdit = new QLineEdit(page);
    m_pathEdit->setObjectName(QStringLiteral("slLivePath"));
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(QStringLiteral("No folder selected"));
    m_selectButton = new QPushButton(QStringLiteral("Choose Folder"), page);
    markSecondaryButton(m_selectButton);
    m_scanButton = new QPushButton(QStringLiteral("Scan"), page);
    markPrimaryButton(m_scanButton);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), page);
    m_upButton = new QPushButton(QStringLiteral("Up"), page);
    markSecondaryButton(m_upButton);
    m_upButton->setToolTip(QStringLiteral("Go to the parent folder"));
    topRow->addWidget(m_pathEdit, 1);
    topRow->addWidget(m_selectButton);
    topRow->addWidget(m_scanButton);
    topRow->addWidget(m_cancelButton);
    topRow->addWidget(m_upButton);
    rootLayout->addLayout(topRow);

    m_scanProgress = new QProgressBar(page);
    m_scanProgress->setTextVisible(false);
    m_scanProgress->setFixedHeight(4);
    m_scanProgress->setMaximum(0);
    m_scanProgress->setVisible(false);
    rootLayout->addWidget(m_scanProgress);

    m_metrics = new MetricStrip(page);
    rootLayout->addWidget(m_metrics);
    updateLiveMetrics();

    auto* findRow = new QHBoxLayout();
    findRow->setSpacing(kUiSpace8);
    m_searchEdit = new QLineEdit(page);
    m_searchEdit->setPlaceholderText(QStringLiteral("Search results…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_filterButton = new FilterButton(page);
    m_filterButton->setObjectName(QStringLiteral("slLiveFilter"));
    auto* panel = new FilterPanel(m_filterButton);
    m_kindFilter = new QComboBox(panel);
    m_kindFilter->setObjectName(QStringLiteral("slLiveKind"));
    m_kindFilter->addItems({QStringLiteral("All"), QStringLiteral("Folders"),
                            QStringLiteral("Files")});
    m_minSizeFilter = new QLineEdit(panel);
    m_minSizeFilter->setObjectName(QStringLiteral("slLiveMinSize"));
    m_minSizeFilter->setPlaceholderText(QStringLiteral("Any"));
    m_extFilter = new QLineEdit(panel);
    m_extFilter->setObjectName(QStringLiteral("slLiveExt"));
    m_extFilter->setPlaceholderText(QStringLiteral("Any"));
    m_classFilter = new QComboBox(panel);
    m_classFilter->setObjectName(QStringLiteral("slLiveClass"));
    m_classFilter->addItem(QStringLiteral("Any"), QString());
    m_classFilter->addItem(QStringLiteral("BuildArtifact"),
                           QStringLiteral("BuildArtifact"));
    m_classFilter->addItem(QStringLiteral("DependencyDirectory"),
                           QStringLiteral("DependencyDirectory"));
    m_classFilter->addItem(QStringLiteral("DownloadedAiModel"),
                           QStringLiteral("DownloadedAiModel"));
    m_classFilter->addItem(QStringLiteral("UserData"), QStringLiteral("UserData"));
    m_classFilter->addItem(QStringLiteral("Unknown"), QStringLiteral("Unknown"));
    panel->form()->addRow(QStringLiteral("Type"), m_kindFilter);
    panel->form()->addRow(QStringLiteral("Minimum size"), m_minSizeFilter);
    panel->form()->addRow(QStringLiteral("Extension"), m_extFilter);
    panel->form()->addRow(QStringLiteral("Class"), m_classFilter);
    m_filterButton->setPanel(panel);
    m_sortFilter = new QComboBox(page);
    m_sortFilter->addItem(QStringLiteral("Sort: Size"), 0);
    m_sortFilter->addItem(QStringLiteral("Sort: Name"), 1);
    findRow->addWidget(m_searchEdit, 1);
    findRow->addWidget(m_filterButton);
    findRow->addWidget(m_sortFilter);
    rootLayout->addLayout(findRow);

    m_breadcrumbBar = new QWidget(page);
    m_breadcrumbLayout = new QHBoxLayout(m_breadcrumbBar);
    m_breadcrumbLayout->setContentsMargins(0, 0, 0, 0);
    m_breadcrumbLayout->setSpacing(kUiSpace4);
    m_breadcrumbLayout->addStretch(1);
    rootLayout->addWidget(m_breadcrumbBar);

    m_liveStack = new QStackedWidget(page);
    m_liveEmpty = new EmptyStateWidget(m_liveStack);
    m_liveEmpty->setTitle(QStringLiteral("Select a folder to analyze"));
    m_liveEmpty->setBody(
        QStringLiteral("SpaceLens scans it read-only and shows where storage "
                       "is being used."));
    m_liveEmpty->setActionText(QStringLiteral("Select folder"));
    m_liveEmpty->setActionVisible(true);

    m_liveWork = new QWidget(m_liveStack);
    auto* workLayout = new QVBoxLayout(m_liveWork);
    workLayout->setContentsMargins(0, 0, 0, 0);
    workLayout->setSpacing(kUiSpace8);

    auto* splitter = new QSplitter(Qt::Horizontal, m_liveWork);
    m_listing = new QTableWidget(0, 5, splitter);
    m_listing->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Type"),
         QStringLiteral("Class"), QStringLiteral("Modified")});
    m_listing->setAlternatingRowColors(true);
    m_listing->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_listing->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_listing->setContextMenuPolicy(Qt::CustomContextMenu);
    m_listing->setShowGrid(false);
    m_listing->setWordWrap(false);
    m_listing->verticalHeader()->setVisible(false);
    m_listing->verticalHeader()->setDefaultSectionSize(24);
    m_listing->horizontalHeader()->setStretchLastSection(false);
    m_listing->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_listing->setColumnWidth(1, 88);
    m_listing->setColumnWidth(2, 64);
    m_listing->setColumnWidth(3, 140);
    m_listing->setColumnWidth(4, 126);
    m_listing->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto* detailsHost = new QWidget(splitter);
    auto* detailsLayout = new QVBoxLayout(detailsHost);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(kUiSpace8);

    auto* actionRow = new QHBoxLayout();
    m_explorerButton = new QPushButton(QStringLiteral("Show in Explorer"), detailsHost);
    markSecondaryButton(m_explorerButton);
    m_openButton = new QPushButton(QStringLiteral("Open"), detailsHost);
    markSecondaryButton(m_openButton);
    m_itemMoreButton = new QToolButton(detailsHost);
    markTertiaryButton(m_itemMoreButton);
    m_itemMoreButton->setText(QStringLiteral("More"));
    m_itemMoreButton->setPopupMode(QToolButton::InstantPopup);
    m_itemMoreButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_openFolderButton = new QPushButton(QStringLiteral("Open Folder"), detailsHost);
    m_copyPathButton = new QPushButton(QStringLiteral("Copy Path"), detailsHost);
    m_rescanButton =
        new QPushButton(QStringLiteral("Rescan This Location"), detailsHost);
    m_openFolderButton->hide();
    m_copyPathButton->hide();
    m_rescanButton->hide();
    auto* moreMenu = new QMenu(m_itemMoreButton);
    moreMenu->addAction(QStringLiteral("Copy Path"), this, &MainWindow::onCopyPath);
    moreMenu->addAction(QStringLiteral("Copy Name"), this, &MainWindow::onCopyName);
    moreMenu->addAction(QStringLiteral("Open containing folder"), this,
                        &MainWindow::onOpenFolder);
    moreMenu->addSeparator();
    moreMenu->addAction(QStringLiteral("Rescan this location"), this,
                        &MainWindow::onRescanLocation);
    m_itemMoreButton->setMenu(moreMenu);
    m_addReviewButton =
        new QPushButton(QStringLiteral("Add to Cleanup Review"), detailsHost);
    markSecondaryButton(m_addReviewButton);
    actionRow->addWidget(m_explorerButton);
    actionRow->addWidget(m_openButton);
    actionRow->addWidget(m_itemMoreButton);
    actionRow->addStretch(1);
    actionRow->addWidget(m_addReviewButton);
    detailsLayout->addLayout(actionRow);

    m_details = new PropertyInspector(detailsHost);
    detailsLayout->addWidget(m_details, 1);

    m_largestLabel = new QLabel(QStringLiteral("Largest files"), detailsHost);
    m_largestLabel->setObjectName(QStringLiteral("slHint"));
    m_largestList = new QTableWidget(0, 2, detailsHost);
    m_largestList->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Size")});
    m_largestList->setAlternatingRowColors(true);
    m_largestList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_largestList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_largestList->setShowGrid(false);
    m_largestList->setWordWrap(false);
    m_largestList->verticalHeader()->setVisible(false);
    m_largestList->verticalHeader()->setDefaultSectionSize(24);
    m_largestList->horizontalHeader()->setStretchLastSection(false);
    m_largestList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_largestList->setColumnWidth(1, 88);
    m_largestList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_largestList->setMaximumHeight(148);
    m_largestList->setMinimumHeight(88);
    m_largestLabel->hide();
    m_largestList->hide();

    m_selectionLabel = new QLabel(QStringLiteral("Selected: 0 items"), detailsHost);
    m_selectionLabel->setObjectName(QStringLiteral("slHint"));
    detailsLayout->addWidget(m_largestLabel);
    detailsLayout->addWidget(m_largestList);
    detailsLayout->addWidget(m_selectionLabel);

    splitter->addWidget(m_listing);
    splitter->addWidget(detailsHost);
    splitter->setStretchFactor(0, 7);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({900, 380});
    workLayout->addWidget(splitter, 1);

    m_liveStack->addWidget(m_liveEmpty);
    m_liveStack->addWidget(m_liveWork);
    rootLayout->addWidget(m_liveStack, 1);

    connect(m_selectButton, &QPushButton::clicked, this, &MainWindow::onSelectFolder);
    connect(m_liveEmpty, &EmptyStateWidget::actionClicked, this, [this]() {
        if (m_rootPath.isEmpty()) {
            onSelectFolder();
        } else {
            onScan();
        }
    });
    connect(m_scanButton, &QPushButton::clicked, this, &MainWindow::onScan);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(m_upButton, &QPushButton::clicked, this, &MainWindow::onNavigateUp);
    connect(m_listing, &QTableWidget::itemActivated, this,
            [this](QTableWidgetItem*) { onListingActivated(); });
    connect(m_listing, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::onSelectionChanged);
    connect(m_largestList, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::onSelectionChanged);
    connect(m_listing, &QTableWidget::customContextMenuRequested, this,
            &MainWindow::onContextMenu);
    connect(m_kindFilter, &QComboBox::currentIndexChanged, this,
            &MainWindow::onFilterChanged);
    connect(m_classFilter, &QComboBox::currentIndexChanged, this,
            &MainWindow::onFilterChanged);
    connect(m_sortFilter, &QComboBox::currentIndexChanged, this,
            &MainWindow::onFilterChanged);
    connect(m_extFilter, &QLineEdit::textChanged, this, &MainWindow::onFilterChanged);
    connect(m_minSizeFilter, &QLineEdit::textChanged, this,
            &MainWindow::onFilterChanged);
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            &MainWindow::onFilterChanged);
    connect(panel, &FilterPanel::resetRequested, this, [this]() {
        resetLiveFilters();
    });
    connect(panel, &FilterPanel::applyRequested, this,
            &MainWindow::onFilterChanged);
    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::onOpen);
    connect(m_openFolderButton, &QPushButton::clicked, this,
            &MainWindow::onOpenFolder);
    connect(m_explorerButton, &QPushButton::clicked, this,
            &MainWindow::onShowInExplorer);
    connect(m_copyPathButton, &QPushButton::clicked, this, &MainWindow::onCopyPath);
    connect(m_addReviewButton, &QPushButton::clicked, this,
            &MainWindow::onAddToReview);
    connect(m_showReviewButton, &QPushButton::clicked, this,
            &MainWindow::onShowReview);
    connect(m_rescanButton, &QPushButton::clicked, this,
            &MainWindow::onRescanLocation);

    updateLiveEmptyState();
    return page;
}

void MainWindow::updateActionState()
{
    const bool running = m_session && m_session->isRunning();
    const bool hasPath = !m_rootPath.isEmpty();
    const bool hasResult = m_lastResult.has_value();
    const auto selected = selectedRows();
    const bool one = selected.size() == 1;
    const bool any = !selected.empty();

    m_selectButton->setEnabled(!running);
    m_scanButton->setEnabled(hasPath && !running);
    m_cancelButton->setEnabled(running);
    m_cancelButton->setVisible(running);
    if (m_scanProgress) {
        m_scanProgress->setVisible(running);
        if (running) {
            m_scanProgress->setMaximum(0);
        }
    }
    m_upButton->setEnabled(hasResult && m_currentDir != InvalidDirIndex &&
                           m_lastResult->tree.dir(m_currentDir).parent !=
                               InvalidDirIndex &&
                           !running);
    m_openButton->setEnabled(one && !running);
    m_openFolderButton->setEnabled(one && !running);
    m_explorerButton->setEnabled(one && !running);
    m_copyPathButton->setEnabled(any && !running);
    m_itemMoreButton->setEnabled((any || hasResult) && !running);
    m_addReviewButton->setEnabled(any && hasResult && !running);
    m_showReviewButton->setEnabled(true);
    m_rescanButton->setEnabled(hasResult && !running);
    updateLiveEmptyState();
}

void MainWindow::setStatusMessage(const QString& message)
{
    m_statusLabel->setText(message);
}

void MainWindow::clearResults()
{
    m_listing->setRowCount(0);
    m_largestList->setRowCount(0);
    m_details->clear();
    m_largestLabel->setVisible(false);
    m_largestList->setVisible(false);
    m_lastResult.reset();
    m_lastProgress = {};
    m_currentDir = InvalidDirIndex;
    rebuildBreadcrumb();
    updateLiveMetrics();
    updateSelectionSummary();
    updateLiveEmptyState();
}

void MainWindow::onSelectFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select folder to analyze"),
        m_rootPath.isEmpty() ? QStringLiteral("C:/") : m_rootPath,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty()) {
        return;
    }
    m_rootPath = QDir::toNativeSeparators(dir);
    m_pathEdit->setText(m_rootPath);
    updateActionState();
    updateLiveEmptyState();
    setStatusMessage(QStringLiteral("Ready to scan: %1").arg(m_rootPath));
}

void MainWindow::onScan()
{
    if (m_rootPath.isEmpty() || !m_session) {
        return;
    }
    clearResults();
    ScanOptions options;
    options.topFileCount = 100;
    if (!m_session->start(m_rootPath, options)) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("A scan is already running."));
        return;
    }
    updateActionState();
    setStatusMessage(QStringLiteral("Scanning…"));
}

void MainWindow::onCancel()
{
    if (m_session) {
        m_session->cancel();
        setStatusMessage(QStringLiteral("Cancelling…"));
    }
}

void MainWindow::onProgress(const ScanProgress& progress)
{
    m_lastProgress = progress;
    updateLiveMetrics();
    setStatusMessage(
        QStringLiteral("Scanning: %1").arg(fromWide(progress.currentPath)));
}

void MainWindow::onScanFinished(ScanState state)
{
    auto result = m_session->takeResult();
    updateActionState();
    if (!result) {
        setStatusMessage(QStringLiteral("Scan finished with no result."));
        return;
    }
    onProgress(result->progress);
    switch (state) {
    case ScanState::Completed:
        setStatusMessage(QStringLiteral("Scan complete."));
        break;
    case ScanState::Cancelled:
        setStatusMessage(QStringLiteral("Scan cancelled."));
        break;
    case ScanState::Failed:
        setStatusMessage(QStringLiteral("Scan failed: %1")
                             .arg(fromWide(result->errorMessage)));
        break;
    default:
        setStatusMessage(QStringLiteral("Scan finished."));
        break;
    }
    m_lastResult = std::move(result);
    populateFromResult();
    updateActionState();
}

void MainWindow::populateFromResult()
{
    m_listing->setRowCount(0);
    m_largestList->setRowCount(0);
    if (!m_lastResult || m_lastResult->tree.empty()) {
        return;
    }
    m_currentDir = m_lastResult->tree.root();
    rebuildBreadcrumb();
    refreshCurrentListing();

    for (const auto& file : m_lastResult->largestFiles) {
        RowRef row;
        row.kind = RowKind::File;
        row.file = file.fileIndex;
        row.path = fromWide(file.path);
        row.name = file.fileIndex != InvalidFileIndex
                       ? fromWide(m_lastResult->tree.file(file.fileIndex).name)
                       : QFileInfo(fromWide(file.path)).fileName();
        row.size = file.size;
        QString modified;
        if (file.fileIndex != InvalidFileIndex) {
            modified = formatFileTimeLocal(
                m_lastResult->tree.file(file.fileIndex).lastWriteTime);
        }
        appendListingRow(m_largestList, row, QStringLiteral("File"), {}, modified);
    }
    const bool hasLargest = m_largestList->rowCount() > 0;
    m_largestLabel->setVisible(hasLargest);
    m_largestList->setVisible(hasLargest);
}

void MainWindow::rebuildBreadcrumb()
{
    while (QLayoutItem* child = m_breadcrumbLayout->takeAt(0)) {
        if (child->widget()) {
            child->widget()->deleteLater();
        }
        delete child;
    }
    if (!m_lastResult || m_lastResult->tree.empty() ||
        m_currentDir == InvalidDirIndex) {
        m_breadcrumbLayout->addStretch(1);
        return;
    }

    std::vector<DirIndex> chain;
    for (DirIndex cur = m_currentDir; cur != InvalidDirIndex;
         cur = m_lastResult->tree.dir(cur).parent) {
        chain.push_back(cur);
    }
    std::reverse(chain.begin(), chain.end());

    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (i > 0) {
            auto* sep = new QLabel(QStringLiteral(">"), m_breadcrumbBar);
            m_breadcrumbLayout->addWidget(sep);
        }
        const DirIndex idx = chain[i];
        const QString name = fromWide(m_lastResult->tree.dir(idx).name);
        auto* btn = new QPushButton(name, m_breadcrumbBar);
        btn->setFlat(true);
        btn->setCursor(Qt::PointingHandCursor);
        m_breadcrumbLayout->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, idx]() {
            m_currentDir = idx;
            rebuildBreadcrumb();
            refreshCurrentListing();
            updateActionState();
        });
    }
    m_breadcrumbLayout->addStretch(1);
}

void MainWindow::onBreadcrumbClicked(int)
{
    // Navigation is wired per breadcrumb button.
}

void MainWindow::onNavigateUp()
{
    if (!m_lastResult || m_currentDir == InvalidDirIndex) {
        return;
    }
    const DirIndex parent = m_lastResult->tree.dir(m_currentDir).parent;
    if (parent == InvalidDirIndex) {
        return;
    }
    m_currentDir = parent;
    rebuildBreadcrumb();
    refreshCurrentListing();
    updateActionState();
}

void MainWindow::appendListingRow(QTableWidget* table, const RowRef& row,
                                 const QString& type,
                                 const QString& classification,
                                 const QString& modified)
{
    if (table == nullptr) {
        return;
    }
    const int rowIndex = table->rowCount();
    table->insertRow(rowIndex);
    auto* nameItem = new QTableWidgetItem(row.name);
    nameItem->setData(Qt::UserRole, static_cast<int>(row.kind));
    nameItem->setData(Qt::UserRole + 1,
                      static_cast<uint>(row.kind == RowKind::Directory
                                            ? row.dir
                                            : row.file));
    nameItem->setData(Qt::UserRole + 2, row.path);
    nameItem->setData(Qt::UserRole + 3, row.name);
    nameItem->setData(Qt::UserRole + 4, QVariant::fromValue<qulonglong>(row.size));
    nameItem->setToolTip(row.path);
    table->setItem(rowIndex, 0, nameItem);

    auto* sizeItem = new QTableWidgetItem(
        QString::fromStdString(SizeFormatter::format(row.size)));
    sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sizeItem->setToolTip(row.path);
    table->setItem(rowIndex, 1, sizeItem);

    if (table->columnCount() > 2) {
        table->setItem(rowIndex, 2, new QTableWidgetItem(type));
        const QString shownClass =
            isNoiseDisplayValue(classification) ? QString() : classification;
        table->setItem(rowIndex, 3, new QTableWidgetItem(shownClass));
        table->setItem(rowIndex, 4, new QTableWidgetItem(modified));
    }
}

void MainWindow::refreshCurrentListing()
{
    m_listing->setRowCount(0);
    if (!m_lastResult || m_lastResult->tree.empty() ||
        m_currentDir == InvalidDirIndex) {
        return;
    }
    const auto& tree = m_lastResult->tree;
    const auto& node = tree.dir(m_currentDir);
    const bool sortByName = m_sortFilter && m_sortFilter->currentIndex() == 1;

    auto children = tree.largestChildDirectories(m_currentDir, node.children.size());
    if (sortByName) {
        std::sort(children.begin(), children.end(), [&](DirIndex a, DirIndex b) {
            return tree.dir(a).name < tree.dir(b).name;
        });
    }
    for (const DirIndex child : children) {
        RowRef row;
        row.kind = RowKind::Directory;
        row.dir = child;
        row.path = fromWide(tree.pathOfDirectory(child));
        row.name = fromWide(tree.dir(child).name);
        row.size = tree.dir(child).recursiveSize;
        const auto cls = classifyRow(row);
        if (!rowPassesFilter(row, cls)) {
            continue;
        }
        appendListingRow(m_listing, row, QStringLiteral("Folder"),
                         QString::fromUtf8(toString(cls.category)),
                         formatFileTimeLocal(tree.dir(child).newestDescendantWrite));
    }

    std::vector<FileIndex> files = node.files;
    std::sort(files.begin(), files.end(), [&](FileIndex a, FileIndex b) {
        if (sortByName) {
            return tree.file(a).name < tree.file(b).name;
        }
        return tree.file(a).size > tree.file(b).size;
    });
    for (const FileIndex fi : files) {
        RowRef row;
        row.kind = RowKind::File;
        row.file = fi;
        row.path = fromWide(tree.pathOfFile(fi));
        row.name = fromWide(tree.file(fi).name);
        row.size = tree.file(fi).size;
        const auto cls = classifyRow(row);
        if (!rowPassesFilter(row, cls)) {
            continue;
        }
        appendListingRow(m_listing, row, QStringLiteral("File"),
                         QString::fromUtf8(toString(cls.category)),
                         formatFileTimeLocal(tree.file(fi).lastWriteTime));
    }
    updateDetails();
    updateSelectionSummary();
    updateLiveEmptyState();
}

bool MainWindow::rowPassesFilter(const RowRef& row, const Classification& cls) const
{
    const int kind = m_kindFilter->currentIndex();
    if (kind == 1 && row.kind != RowKind::Directory) {
        return false;
    }
    if (kind == 2 && row.kind != RowKind::File) {
        return false;
    }
    const QString ext = m_extFilter->text().trimmed();
    if (!ext.isEmpty()) {
        if (row.kind != RowKind::File) {
            return false;
        }
        QString e = ext;
        if (e.startsWith(QLatin1Char('.'))) {
            e = e.mid(1);
        }
        const QFileInfo fi(row.name);
        if (fi.suffix().compare(e, Qt::CaseInsensitive) != 0) {
            return false;
        }
    }
    const QString minText = m_minSizeFilter->text().trimmed();
    if (!minText.isEmpty()) {
        const auto parsed = parseSize(minText.toStdString());
        if (parsed.error.empty() && row.size < parsed.bytes) {
            return false;
        }
    }
    const QString classWanted = m_classFilter->currentData().toString();
    if (!classWanted.isEmpty()) {
        if (QString::fromUtf8(toString(cls.category)) != classWanted) {
            return false;
        }
    }
    const QString search = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    if (!search.isEmpty()) {
        if (!row.name.contains(search, Qt::CaseInsensitive) &&
            !row.path.contains(search, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}

Classification MainWindow::classifyRow(const RowRef& row) const
{
    if (!m_lastResult) {
        return {};
    }
    const auto& tree = m_lastResult->tree;
    if (row.kind == RowKind::Directory && row.dir != InvalidDirIndex) {
        const auto& node = tree.dir(row.dir);
        std::vector<std::wstring> children;
        for (const DirIndex c : node.children) {
            children.push_back(tree.dir(c).name);
        }
        for (const FileIndex f : node.files) {
            children.push_back(tree.file(f).name);
        }
        return classifyDirectory(node.name, toWide(row.path), children.data(),
                                 children.size());
    }
    if (row.kind == RowKind::File && row.file != InvalidFileIndex) {
        return classifyFile(tree.file(row.file).name, toWide(row.path));
    }
    return classifyFile(toWide(row.name), toWide(row.path));
}

void MainWindow::onFilterChanged()
{
    updateLiveFilterCount();
    if (m_lastResult) {
        refreshCurrentListing();
    }
}

void MainWindow::onLiveWorkspace()
{
    m_navLive->setChecked(true);
    m_navIndexed->setChecked(false);
    m_pages->setCurrentIndex(0);
    m_navLive->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::onIndexedWorkspace()
{
    m_navIndexed->setChecked(true);
    m_navLive->setChecked(false);
    m_pages->setCurrentIndex(1);
    m_navIndexed->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::updateLiveMetrics()
{
    if (m_metrics == nullptr) {
        return;
    }
    const auto& p = m_lastProgress;
    const auto errors =
        p.accessDenied + p.reparsePointsSkipped + p.otherErrors;
    const QLocale loc;
    m_metrics->setItems({
        {QStringLiteral("Files"), loc.toString(static_cast<qulonglong>(p.filesSeen)),
         {}},
        {QStringLiteral("Folders"),
         loc.toString(static_cast<qulonglong>(p.directoriesSeen)),
         {}},
        {QStringLiteral("Processed"),
         QString::fromStdString(SizeFormatter::format(p.bytesSeen)),
         {}},
        {QStringLiteral("Elapsed"), formatElapsed(p.elapsedSeconds), {}},
        {QStringLiteral("Errors"), loc.toString(static_cast<qlonglong>(errors)),
         QStringLiteral("Denied %1 · reparse %2 · other %3")
             .arg(p.accessDenied)
             .arg(p.reparsePointsSkipped)
             .arg(p.otherErrors)},
    });
}

int MainWindow::liveActiveFilterCount() const
{
    int count = 0;
    if (m_kindFilter && m_kindFilter->currentIndex() != 0) {
        ++count;
    }
    if (m_minSizeFilter && !m_minSizeFilter->text().trimmed().isEmpty()) {
        ++count;
    }
    if (m_extFilter && !m_extFilter->text().trimmed().isEmpty()) {
        ++count;
    }
    if (m_classFilter && !m_classFilter->currentData().toString().isEmpty()) {
        ++count;
    }
    return count;
}

void MainWindow::updateLiveFilterCount()
{
    if (m_filterButton) {
        m_filterButton->setActiveCount(liveActiveFilterCount());
    }
}

void MainWindow::resetLiveFilters()
{
    QSignalBlocker b1(m_kindFilter);
    QSignalBlocker b2(m_classFilter);
    QSignalBlocker b3(m_extFilter);
    QSignalBlocker b4(m_minSizeFilter);
    m_kindFilter->setCurrentIndex(0);
    m_classFilter->setCurrentIndex(0);
    m_extFilter->clear();
    m_minSizeFilter->clear();
    updateLiveFilterCount();
    onFilterChanged();
}

void MainWindow::updateLiveEmptyState()
{
    if (m_liveStack == nullptr || m_liveEmpty == nullptr) {
        return;
    }
    const bool running = m_session && m_session->isRunning();
    const bool hasResult = m_lastResult.has_value();
    const int hits = m_listing ? m_listing->rowCount() : 0;
    if (running || (hasResult && hits > 0)) {
        m_liveStack->setCurrentWidget(m_liveWork);
        if (m_largestLabel) {
            m_largestLabel->setVisible(m_largestList &&
                                       m_largestList->rowCount() > 0);
        }
        if (m_largestList) {
            m_largestList->setVisible(m_largestList->rowCount() > 0);
        }
        return;
    }
    if (hasResult && hits == 0) {
        m_liveEmpty->setTitle(QStringLiteral("No matching files"));
        m_liveEmpty->setBody(
            QStringLiteral("Adjust filters or scan another location."));
        m_liveEmpty->setActionVisible(false);
        m_liveStack->setCurrentWidget(m_liveEmpty);
        return;
    }
    if (!m_rootPath.isEmpty()) {
        m_liveEmpty->setTitle(QStringLiteral("Ready to scan"));
        m_liveEmpty->setBody(m_rootPath);
        m_liveEmpty->setActionText(QStringLiteral("Scan"));
        m_liveEmpty->setActionVisible(true);
        m_liveStack->setCurrentWidget(m_liveEmpty);
        return;
    }
    m_liveEmpty->setTitle(QStringLiteral("Select a folder to analyze"));
    m_liveEmpty->setBody(
        QStringLiteral("SpaceLens scans it read-only and shows where storage "
                       "is being used."));
    m_liveEmpty->setActionText(QStringLiteral("Select folder"));
    m_liveEmpty->setActionVisible(true);
    m_liveStack->setCurrentWidget(m_liveEmpty);
}

void MainWindow::onListingActivated()
{
    if (!m_lastResult || m_listing == nullptr) {
        return;
    }
    const int current = m_listing->currentRow();
    auto* item = current >= 0 ? m_listing->item(current, 0) : nullptr;
    if (item == nullptr) {
        return;
    }
    const auto row = rowFromItem(item);
    if (row.kind == RowKind::Directory && row.dir != InvalidDirIndex) {
        m_currentDir = row.dir;
        rebuildBreadcrumb();
        refreshCurrentListing();
        updateActionState();
    }
}

MainWindow::RowRef MainWindow::rowFromItem(const QTableWidgetItem* item) const
{
    RowRef row;
    if (item == nullptr) {
        return row;
    }
    row.kind = static_cast<RowKind>(item->data(Qt::UserRole).toInt());
    const auto index = static_cast<std::uint32_t>(item->data(Qt::UserRole + 1).toUInt());
    row.path = item->data(Qt::UserRole + 2).toString();
    row.name = item->data(Qt::UserRole + 3).toString();
    row.size = static_cast<ByteSize>(item->data(Qt::UserRole + 4).toULongLong());
    if (row.kind == RowKind::Directory) {
        row.dir = index;
    } else {
        row.file = index;
    }
    return row;
}

std::vector<MainWindow::RowRef> MainWindow::selectedRows() const
{
    std::vector<RowRef> out;
    auto collect = [&](QTableWidget* table) {
        if (table == nullptr || table->selectionModel() == nullptr) {
            return;
        }
        for (const QModelIndex& idx : table->selectionModel()->selectedRows()) {
            if (auto* item = table->item(idx.row(), 0)) {
                out.push_back(rowFromItem(item));
            }
        }
    };
    collect(m_listing);
    if (out.empty()) {
        collect(m_largestList);
    }
    return out;
}

std::optional<MainWindow::RowRef> MainWindow::singleSelectedRow() const
{
    const auto rows = selectedRows();
    if (rows.size() != 1) {
        return std::nullopt;
    }
    return rows.front();
}

void MainWindow::onSelectionChanged()
{
    updateSelectionSummary();
    updateDetails();
    updateActionState();
}

void MainWindow::updateSelectionSummary()
{
    const auto rows = selectedRows();
    ByteSize total = 0;
    for (const auto& r : rows) {
        total += r.size;
    }
    m_selectionLabel->setText(
        QStringLiteral("Selected: %1 items — %2")
            .arg(rows.size())
            .arg(QString::fromStdString(SizeFormatter::format(total))));
}

void MainWindow::updateDetails()
{
    m_details->clear();
    RowRef row;
    bool haveRow = false;
    const auto rowOpt = singleSelectedRow();
    if (rowOpt) {
        row = *rowOpt;
        haveRow = true;
    } else if (m_lastResult && m_currentDir != InvalidDirIndex) {
        row.kind = RowKind::Directory;
        row.dir = m_currentDir;
        row.path = fromWide(m_lastResult->tree.pathOfDirectory(m_currentDir));
        row.name = fromWide(m_lastResult->tree.dir(m_currentDir).name);
        row.size = m_lastResult->tree.dir(m_currentDir).recursiveSize;
        haveRow = true;
    }
    if (!haveRow || !m_lastResult) {
        return;
    }

    const auto cls = classifyRow(row);
    const auto assessment =
        m_reviewController.ordinaryLocationPolicy().classify(toWide(row.path));
    const auto safety = assessment.safety;
    m_details->setHeading(
        displayFolderName(row.name.isEmpty() ? row.path : row.name),
        QString::fromStdString(SizeFormatter::format(row.size)));
    m_details->addRow(QStringLiteral("Path"), row.path);
    m_details->addRow(QStringLiteral("Type"),
                      row.kind == RowKind::Directory ? QStringLiteral("Folder")
                                                     : QStringLiteral("File"));

    if (row.kind == RowKind::Directory && row.dir != InvalidDirIndex) {
        const auto& node = m_lastResult->tree.dir(row.dir);
        m_details->addRow(
            QStringLiteral("Direct file size"),
            QString::fromStdString(SizeFormatter::format(node.directFileSize)));
        m_details->addRow(QStringLiteral("Files"),
                          QString::number(node.totalFileCount));
        m_details->addRow(QStringLiteral("Child folders"),
                          QString::number(node.childDirCount));
        m_details->addRow(QStringLiteral("Newest write"),
                          formatFileTimeLocal(node.newestDescendantWrite));
    } else if (row.kind == RowKind::File && row.file != InvalidFileIndex) {
        const auto& file = m_lastResult->tree.file(row.file);
        const QFileInfo info(row.name);
        m_details->addRow(QStringLiteral("Extension"), info.suffix());
        m_details->addRow(QStringLiteral("Modified"),
                          formatFileTimeLocal(file.lastWriteTime));
        m_details->addRow(QStringLiteral("Last access"),
                          formatFileTimeLocal(file.lastAccessTime));
        if (file.attributes != 0) {
            m_details->addRow(QStringLiteral("Attributes"),
                              QStringLiteral("0x%1").arg(file.attributes, 0, 16));
        }
    }

    m_details->addRow(QStringLiteral("Classification"),
                      QString::fromUtf8(toString(cls.category)));
    if (!isNoiseDisplayValue(QString::fromUtf8(toString(cls.category)))) {
        m_details->addRow(QStringLiteral("Confidence"),
                          QString::fromUtf8(toString(cls.confidence)));
        m_details->addRow(QStringLiteral("Matched rule"),
                          QString::fromStdString(cls.ruleId));
        m_details->addRow(QStringLiteral("Reason"),
                          QString::fromStdString(cls.reason));
    }
    addLocationSafety(*m_details, assessment);
    if (safety == LocationSafety::Protected) {
        m_details->addNote(
            QStringLiteral("Protected system location. SpaceLens will not manage "
                           "deletion for this location."));
    }

    FileTimeTicks activity = 0;
    FileTimeTicks access = 0;
    if (row.kind == RowKind::Directory && row.dir != InvalidDirIndex) {
        activity = m_lastResult->tree.dir(row.dir).newestDescendantWrite;
    } else if (row.kind == RowKind::File && row.file != InvalidFileIndex) {
        activity = m_lastResult->tree.file(row.file).lastWriteTime;
        access = m_lastResult->tree.file(row.file).lastAccessTime;
    }
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER now{};
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    const auto reclaim = analyzeItem(
        toWide(row.path),
        row.kind == RowKind::Directory ? ItemKind::Directory : ItemKind::File,
        row.size, activity, cls, safety, now.QuadPart, access);
    m_details->addRow(QStringLiteral("Reclaimability"),
                      QString::fromUtf8(toString(reclaim.reclaimability)));
    m_details->addRow(QStringLiteral("Candidate strength"),
                      QString::fromUtf8(toString(reclaim.strength)));
    m_details->addRow(QStringLiteral("Reclaim note"),
                      QString::fromStdString(reclaim.explanation));
    m_details->addNote(
        QStringLiteral("Classification and reclaim strength are not permission "
                       "to delete."));
}

void MainWindow::copyText(const QString& text)
{
    if (text.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(text);
    setStatusMessage(QStringLiteral("Copied."));
}

void MainWindow::onOpen()
{
    const auto row = singleSelectedRow();
    if (!row) {
        return;
    }
    if (!openWithDefaultApp(toWide(row->path))) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("Could not open:\n%1").arg(row->path));
    }
}

void MainWindow::onOpenFolder()
{
    const auto row = singleSelectedRow();
    if (!row) {
        return;
    }
    if (!openParentFolder(toWide(row->path))) {
        QMessageBox::warning(
            this, QStringLiteral("SpaceLens"),
            QStringLiteral("Could not open folder for:\n%1").arg(row->path));
    }
}

void MainWindow::onShowInExplorer()
{
    const auto row = singleSelectedRow();
    if (!row) {
        return;
    }
    const auto path = toWide(row->path);
    if (row->kind == RowKind::File) {
        if (!revealInExplorer(path) && !openParentFolder(path)) {
            QMessageBox::warning(
                this, QStringLiteral("SpaceLens"),
                QStringLiteral("Could not reveal:\n%1").arg(row->path));
        }
    } else {
        if (!openInExplorer(path)) {
            QMessageBox::warning(
                this, QStringLiteral("SpaceLens"),
                QStringLiteral("Could not open Explorer:\n%1").arg(row->path));
        }
    }
}

void MainWindow::onCopyPath()
{
    const auto rows = selectedRows();
    QStringList paths;
    for (const auto& r : rows) {
        paths << r.path;
    }
    copyText(paths.join(QStringLiteral("\n")));
}

void MainWindow::onCopyName()
{
    const auto row = singleSelectedRow();
    if (!row) {
        return;
    }
    copyText(row->name);
}

void MainWindow::onCopyDetails()
{
    copyText(m_details->toPlainText());
}

void MainWindow::onAddToReview()
{
    if (!m_lastResult) {
        return;
    }
    const auto rows = selectedRows();
    int added = 0;
    int already = 0;
    int conflicts = 0;
    WindowsCleanupMetadataReader reader;
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER now{};
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    const FileTimeTicks addedAt = now.QuadPart;

    for (const auto& row : rows) {
        CleanupCandidate c;
        c.path = toWide(row.path);
        c.kind = row.kind == RowKind::Directory ? ItemKind::Directory
                                                : ItemKind::File;
        c.sizeAtSelection = row.size;
        c.classification = classifyRow(row);
        c.reasonAdded = "Added from GUI live scan selection";
        c.source = "live_scan";
        c.sourceRoot = toWide(m_rootPath);
        if (row.kind == RowKind::File && row.file != InvalidFileIndex) {
            c.lastWriteTime = m_lastResult->tree.file(row.file).lastWriteTime;
            c.attributes = m_lastResult->tree.file(row.file).attributes;
        } else if (row.kind == RowKind::Directory && row.dir != InvalidDirIndex) {
            c.lastWriteTime =
                m_lastResult->tree.dir(row.dir).newestDescendantWrite;
        }
        const auto policy = m_reviewController.ordinaryLocationPolicy();
        c.capturedSafety = effectiveLocationSafety(c.path, policy);
        const auto analysis =
            analyzeItem(c.path, c.kind, c.sizeAtSelection, c.lastWriteTime,
                        c.classification, c.capturedSafety, addedAt);
        c.capturedReclaimability = analysis.reclaimability;
        c.capturedCandidateStrength = analysis.strength;
        prepareCleanupCandidateForAdd(c, reader, policy, addedAt);

        const auto status = m_reviewController.add(std::move(c));
        if (!status.ok) {
            setStatusMessage(QString::fromStdString(status.message));
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
    setStatusMessage(
        QStringLiteral("Cleanup Review: %1 item(s) (added %2, already %3, "
                       "identity conflicts %4)")
            .arg(m_reviewController.review().size())
            .arg(added)
            .arg(already)
            .arg(conflicts));
}

void MainWindow::onShowReview()
{
    if (!m_revalidationSession || !m_maintenanceSession) {
        return;
    }
    CleanupReviewDialog dialog(m_reviewController, *m_revalidationSession,
                               *m_maintenanceSession, this);
    dialog.exec();
    setStatusMessage(QStringLiteral("Cleanup Review: %1 item(s)")
                         .arg(m_reviewController.review().size()));
}

void MainWindow::onOrdinaryLocations()
{
    OrdinaryLocationsDialog dialog(m_reviewController, this);
    dialog.exec();
    updateDetails();
}

void MainWindow::onIndexStatusMessage(const QString& message)
{
    setStatusMessage(message);
}

void MainWindow::onRescanLocation()
{
    QString path = m_rootPath;
    if (m_lastResult && m_currentDir != InvalidDirIndex) {
        path = fromWide(m_lastResult->tree.pathOfDirectory(m_currentDir));
    }
    const auto row = singleSelectedRow();
    if (row && row->kind == RowKind::Directory) {
        path = row->path;
    }
    if (path.isEmpty() || !m_session) {
        return;
    }
    m_rootPath = path;
    m_pathEdit->setText(m_rootPath);
    onScan();
}

void MainWindow::onContextMenu(const QPoint& pos)
{
    QTableWidgetItem* item = m_listing->itemAt(pos);
    if (item) {
        if (!item->isSelected()) {
            m_listing->setCurrentItem(item);
        }
    }
    QMenu menu(this);
    menu.addAction(QStringLiteral("Open"), this, &MainWindow::onOpen);
    menu.addAction(QStringLiteral("Open Folder"), this, &MainWindow::onOpenFolder);
    menu.addAction(QStringLiteral("Show in Explorer"), this,
                   &MainWindow::onShowInExplorer);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Copy Path"), this, &MainWindow::onCopyPath);
    menu.addAction(QStringLiteral("Copy Name"), this, &MainWindow::onCopyName);
    menu.addAction(QStringLiteral("Copy Details"), this, &MainWindow::onCopyDetails);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Add to Cleanup Review"), this,
                   &MainWindow::onAddToReview);
    menu.addAction(QStringLiteral("Rescan This Location"), this,
                   &MainWindow::onRescanLocation);
    menu.exec(m_listing->viewport()->mapToGlobal(pos));
}

}  // namespace spacelens
