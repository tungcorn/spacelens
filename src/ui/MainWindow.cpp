#include "ui/MainWindow.hpp"

#include "ui/CleanupReviewDialog.hpp"
#include "ui/IndexBrowserPage.hpp"

#include "core/Classification.hpp"
#include "core/CleanupRevalidation.hpp"
#include "core/FileTime.hpp"
#include "core/ReclaimAnalysis.hpp"
#include "core/SafetyPolicy.hpp"
#include "core/SizeFormatter.hpp"
#include "core/SizeParse.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/ExplorerIntegration.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextEdit>
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

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_session(std::make_unique<ScanSession>(this))
{
    setWindowTitle(QStringLiteral("SpaceLens"));
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

    m_tabs = new QTabWidget(central);
    m_tabs->addTab(buildLiveScanPage(), QStringLiteral("Live Scan"));

    m_indexPage = new IndexBrowserPage(m_reviewController, m_tabs);
    m_tabs->addTab(m_indexPage, QStringLiteral("Indexed"));
    connect(m_indexPage, &IndexBrowserPage::statusMessage, this,
            &MainWindow::onIndexStatusMessage);
    connect(m_indexPage, &IndexBrowserPage::showReviewRequested, this,
            &MainWindow::onShowReview);

    outer->addWidget(m_tabs, 1);

    m_statusLabel = new QLabel(central);
    statusBar()->addWidget(m_statusLabel, 1);
}

QWidget* MainWindow::buildLiveScanPage()
{
    auto* page = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(page);

    auto* topRow = new QHBoxLayout();
    m_pathEdit = new QLineEdit(page);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(QStringLiteral("No folder selected"));
    m_selectButton = new QPushButton(QStringLiteral("Select Folder…"), page);
    m_scanButton = new QPushButton(QStringLiteral("Scan"), page);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), page);
    m_upButton = new QPushButton(QStringLiteral("Up"), page);
    topRow->addWidget(m_pathEdit, 1);
    topRow->addWidget(m_selectButton);
    topRow->addWidget(m_scanButton);
    topRow->addWidget(m_cancelButton);
    topRow->addWidget(m_upButton);
    rootLayout->addLayout(topRow);

    m_breadcrumbBar = new QWidget(page);
    m_breadcrumbLayout = new QHBoxLayout(m_breadcrumbBar);
    m_breadcrumbLayout->setContentsMargins(0, 0, 0, 0);
    m_breadcrumbLayout->addStretch(1);
    rootLayout->addWidget(m_breadcrumbBar);

    auto* filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel(QStringLiteral("Kind:"), page));
    m_kindFilter = new QComboBox(page);
    m_kindFilter->addItems({QStringLiteral("All"), QStringLiteral("Folders"),
                            QStringLiteral("Files")});
    filterRow->addWidget(m_kindFilter);
    filterRow->addWidget(new QLabel(QStringLiteral("Min size:"), page));
    m_minSizeFilter = new QLineEdit(page);
    m_minSizeFilter->setPlaceholderText(QStringLiteral("e.g. 10MB"));
    m_minSizeFilter->setMaximumWidth(100);
    filterRow->addWidget(m_minSizeFilter);
    filterRow->addWidget(new QLabel(QStringLiteral("Ext:"), page));
    m_extFilter = new QLineEdit(page);
    m_extFilter->setPlaceholderText(QStringLiteral("gguf"));
    m_extFilter->setMaximumWidth(80);
    filterRow->addWidget(m_extFilter);
    filterRow->addWidget(new QLabel(QStringLiteral("Class:"), page));
    m_classFilter = new QComboBox(page);
    m_classFilter->addItem(QStringLiteral("Any"), QString());
    m_classFilter->addItem(QStringLiteral("BuildArtifact"),
                           QStringLiteral("BuildArtifact"));
    m_classFilter->addItem(QStringLiteral("DependencyDirectory"),
                           QStringLiteral("DependencyDirectory"));
    m_classFilter->addItem(QStringLiteral("DownloadedAiModel"),
                           QStringLiteral("DownloadedAiModel"));
    m_classFilter->addItem(QStringLiteral("UserData"), QStringLiteral("UserData"));
    m_classFilter->addItem(QStringLiteral("Unknown"), QStringLiteral("Unknown"));
    filterRow->addWidget(m_classFilter);
    filterRow->addStretch(1);
    rootLayout->addLayout(filterRow);

    auto* statsRow = new QHBoxLayout();
    m_filesLabel = new QLabel(QStringLiteral("Files: 0"), page);
    m_foldersLabel = new QLabel(QStringLiteral("Folders: 0"), page);
    m_processedLabel = new QLabel(
        QStringLiteral("Processed: %1")
            .arg(QString::fromStdString(SizeFormatter::format(0))),
        page);
    m_elapsedLabel = new QLabel(QStringLiteral("Elapsed: 0.0 s"), page);
    m_errorsLabel = new QLabel(QStringLiteral("Errors: 0"), page);
    m_selectionLabel = new QLabel(QStringLiteral("Selected: 0 items"), page);
    statsRow->addWidget(m_filesLabel);
    statsRow->addWidget(m_foldersLabel);
    statsRow->addWidget(m_processedLabel);
    statsRow->addWidget(m_elapsedLabel);
    statsRow->addWidget(m_errorsLabel);
    statsRow->addStretch(1);
    statsRow->addWidget(m_selectionLabel);
    rootLayout->addLayout(statsRow);

    auto* actionRow = new QHBoxLayout();
    m_openButton = new QPushButton(QStringLiteral("Open"), page);
    m_openFolderButton = new QPushButton(QStringLiteral("Open Folder"), page);
    m_explorerButton = new QPushButton(QStringLiteral("Show in Explorer"), page);
    m_copyPathButton = new QPushButton(QStringLiteral("Copy Path"), page);
    m_addReviewButton =
        new QPushButton(QStringLiteral("Add to Cleanup Review"), page);
    m_showReviewButton =
        new QPushButton(QStringLiteral("Cleanup Review…"), page);
    m_rescanButton =
        new QPushButton(QStringLiteral("Rescan This Location"), page);
    actionRow->addWidget(m_openButton);
    actionRow->addWidget(m_openFolderButton);
    actionRow->addWidget(m_explorerButton);
    actionRow->addWidget(m_copyPathButton);
    actionRow->addWidget(m_addReviewButton);
    actionRow->addWidget(m_showReviewButton);
    actionRow->addWidget(m_rescanButton);
    actionRow->addStretch(1);
    rootLayout->addLayout(actionRow);

    auto* splitter = new QSplitter(Qt::Horizontal, page);

    m_listing = new QListWidget(splitter);
    m_listing->setAlternatingRowColors(true);
    m_listing->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_listing->setContextMenuPolicy(Qt::CustomContextMenu);
    m_listing->setUniformItemSizes(true);

    auto* rightSplit = new QSplitter(Qt::Vertical, splitter);
    m_largestList = new QListWidget(rightSplit);
    m_largestList->setAlternatingRowColors(true);
    m_largestList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_details = new QTextEdit(rightSplit);
    m_details->setReadOnly(true);
    rightSplit->setStretchFactor(0, 2);
    rightSplit->setStretchFactor(1, 3);

    splitter->addWidget(m_listing);
    splitter->addWidget(rightSplit);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    rootLayout->addWidget(splitter, 1);

    connect(m_selectButton, &QPushButton::clicked, this, &MainWindow::onSelectFolder);
    connect(m_scanButton, &QPushButton::clicked, this, &MainWindow::onScan);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(m_upButton, &QPushButton::clicked, this, &MainWindow::onNavigateUp);
    connect(m_listing, &QListWidget::itemActivated, this,
            &MainWindow::onItemActivated);
    connect(m_listing, &QListWidget::itemSelectionChanged, this,
            &MainWindow::onSelectionChanged);
    connect(m_largestList, &QListWidget::itemSelectionChanged, this,
            &MainWindow::onSelectionChanged);
    connect(m_listing, &QListWidget::customContextMenuRequested, this,
            &MainWindow::onContextMenu);
    connect(m_kindFilter, &QComboBox::currentIndexChanged, this,
            &MainWindow::onFilterChanged);
    connect(m_classFilter, &QComboBox::currentIndexChanged, this,
            &MainWindow::onFilterChanged);
    connect(m_extFilter, &QLineEdit::textChanged, this, &MainWindow::onFilterChanged);
    connect(m_minSizeFilter, &QLineEdit::textChanged, this,
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
    m_upButton->setEnabled(hasResult && m_currentDir != InvalidDirIndex &&
                           m_lastResult->tree.dir(m_currentDir).parent !=
                               InvalidDirIndex &&
                           !running);
    m_openButton->setEnabled(one && !running);
    m_openFolderButton->setEnabled(one && !running);
    m_explorerButton->setEnabled(one && !running);
    m_copyPathButton->setEnabled(any && !running);
    m_addReviewButton->setEnabled(any && hasResult && !running);
    m_showReviewButton->setEnabled(true);
    m_rescanButton->setEnabled(hasResult && !running);
}

void MainWindow::setStatusMessage(const QString& message)
{
    m_statusLabel->setText(message);
}

void MainWindow::clearResults()
{
    m_listing->clear();
    m_largestList->clear();
    m_details->clear();
    m_lastResult.reset();
    m_currentDir = InvalidDirIndex;
    rebuildBreadcrumb();
    updateSelectionSummary();
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
    m_filesLabel->setText(QStringLiteral("Files: %1").arg(progress.filesSeen));
    m_foldersLabel->setText(
        QStringLiteral("Folders: %1").arg(progress.directoriesSeen));
    m_processedLabel->setText(
        QStringLiteral("Processed: %1").arg(QString::fromStdString(
            SizeFormatter::format(progress.bytesSeen))));
    m_elapsedLabel->setText(
        QStringLiteral("Elapsed: %1").arg(formatElapsed(progress.elapsedSeconds)));
    const auto errors = progress.accessDenied + progress.reparsePointsSkipped +
                        progress.otherErrors;
    m_errorsLabel->setText(
        QStringLiteral("Errors: %1 (denied %2, reparse %3, other %4)")
            .arg(errors)
            .arg(progress.accessDenied)
            .arg(progress.reparsePointsSkipped)
            .arg(progress.otherErrors));
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
    m_listing->clear();
    m_largestList->clear();
    if (!m_lastResult || m_lastResult->tree.empty()) {
        return;
    }
    m_currentDir = m_lastResult->tree.root();
    rebuildBreadcrumb();
    refreshCurrentListing();

    for (const auto& file : m_lastResult->largestFiles) {
        const QString text =
            QStringLiteral("%1  —  %2")
                .arg(QString::fromStdString(SizeFormatter::format(file.size)),
                     fromWide(file.path));
        auto* item = new QListWidgetItem(text, m_largestList);
        item->setData(Qt::UserRole, static_cast<int>(RowKind::File));
        item->setData(Qt::UserRole + 1, static_cast<uint>(file.fileIndex));
        item->setData(Qt::UserRole + 2, fromWide(file.path));
        item->setData(Qt::UserRole + 3,
                      file.fileIndex != InvalidFileIndex
                          ? fromWide(m_lastResult->tree.file(file.fileIndex).name)
                          : QFileInfo(fromWide(file.path)).fileName());
        item->setData(Qt::UserRole + 4, QVariant::fromValue<qulonglong>(file.size));
    }
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

void MainWindow::refreshCurrentListing()
{
    m_listing->clear();
    if (!m_lastResult || m_lastResult->tree.empty() ||
        m_currentDir == InvalidDirIndex) {
        return;
    }
    const auto& tree = m_lastResult->tree;
    const auto& node = tree.dir(m_currentDir);

    auto children = tree.largestChildDirectories(m_currentDir, node.children.size());
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
        const QString text =
            QStringLiteral("[DIR]  %1    %2    %3")
                .arg(row.name,
                     QString::fromStdString(SizeFormatter::format(row.size)),
                     QString::fromUtf8(toString(cls.category)));
        auto* item = new QListWidgetItem(text, m_listing);
        item->setData(Qt::UserRole, static_cast<int>(RowKind::Directory));
        item->setData(Qt::UserRole + 1, static_cast<uint>(child));
        item->setData(Qt::UserRole + 2, row.path);
        item->setData(Qt::UserRole + 3, row.name);
        item->setData(Qt::UserRole + 4, QVariant::fromValue<qulonglong>(row.size));
    }

    // Files sorted by size desc.
    std::vector<FileIndex> files = node.files;
    std::sort(files.begin(), files.end(), [&](FileIndex a, FileIndex b) {
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
        const QString text =
            QStringLiteral("[FILE] %1    %2    %3")
                .arg(row.name,
                     QString::fromStdString(SizeFormatter::format(row.size)),
                     QString::fromUtf8(toString(cls.category)));
        auto* item = new QListWidgetItem(text, m_listing);
        item->setData(Qt::UserRole, static_cast<int>(RowKind::File));
        item->setData(Qt::UserRole + 1, static_cast<uint>(fi));
        item->setData(Qt::UserRole + 2, row.path);
        item->setData(Qt::UserRole + 3, row.name);
        item->setData(Qt::UserRole + 4, QVariant::fromValue<qulonglong>(row.size));
    }
    updateDetails();
    updateSelectionSummary();
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
    if (m_lastResult) {
        refreshCurrentListing();
    }
}

void MainWindow::onItemActivated(QListWidgetItem* item)
{
    if (!item || !m_lastResult) {
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

MainWindow::RowRef MainWindow::rowFromItem(const QListWidgetItem* item) const
{
    RowRef row;
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
    for (QListWidgetItem* item : m_listing->selectedItems()) {
        out.push_back(rowFromItem(item));
    }
    if (out.empty()) {
        for (QListWidgetItem* item : m_largestList->selectedItems()) {
            out.push_back(rowFromItem(item));
        }
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
    const auto rowOpt = singleSelectedRow();
    if (!rowOpt || !m_lastResult) {
        if (m_lastResult && m_currentDir != InvalidDirIndex) {
            // Show current directory summary when nothing selected.
            RowRef cur;
            cur.kind = RowKind::Directory;
            cur.dir = m_currentDir;
            cur.path = fromWide(m_lastResult->tree.pathOfDirectory(m_currentDir));
            cur.name = fromWide(m_lastResult->tree.dir(m_currentDir).name);
            cur.size = m_lastResult->tree.dir(m_currentDir).recursiveSize;
            // fall through using cur
            const auto cls = classifyRow(cur);
            const auto safety = classifyLocation(toWide(cur.path));
            const auto& node = m_lastResult->tree.dir(m_currentDir);
            QString text;
            text += QStringLiteral("Name\n%1\n\n").arg(cur.name);
            text += QStringLiteral("Full path\n%1\n\n").arg(cur.path);
            text += QStringLiteral("Recursive size\n%1\n\n")
                        .arg(QString::fromStdString(SizeFormatter::format(cur.size)));
            text += QStringLiteral("Direct file size\n%1\n\n")
                        .arg(QString::fromStdString(
                            SizeFormatter::format(node.directFileSize)));
            text += QStringLiteral("Files (recursive)\n%1\n\n")
                        .arg(node.totalFileCount);
            text += QStringLiteral("Child directories\n%1\n\n")
                        .arg(node.childDirCount);
            text += QStringLiteral("Classification\n%1 (%2)\n%3\n\n")
                        .arg(QString::fromUtf8(toString(cls.category)),
                             QString::fromUtf8(toString(cls.confidence)),
                             QString::fromStdString(cls.reason));
            text += QStringLiteral("Location safety\n%1\n\n")
                        .arg(QString::fromUtf8(toString(safety)));
            if (safety == LocationSafety::Protected) {
                text += QStringLiteral(
                    "Protected system location\n"
                    "SpaceLens will not manage deletion for this location.\n");
            }
            m_details->setPlainText(text);
        }
        return;
    }
    const RowRef& row = *rowOpt;
    const auto cls = classifyRow(row);
    const auto safety = classifyLocation(toWide(row.path));
    QString text;
    text += QStringLiteral("Name\n%1\n\n").arg(row.name);
    text += QStringLiteral("Full path\n%1\n\n").arg(row.path);
    text += QStringLiteral("Type\n%1\n\n")
                .arg(row.kind == RowKind::Directory ? QStringLiteral("Directory")
                                                    : QStringLiteral("File"));
    text += QStringLiteral("Logical size\n%1\n\n")
                .arg(QString::fromStdString(SizeFormatter::format(row.size)));

    if (row.kind == RowKind::Directory && row.dir != InvalidDirIndex) {
        const auto& node = m_lastResult->tree.dir(row.dir);
        text += QStringLiteral("Direct file size\n%1\n\n")
                    .arg(QString::fromStdString(
                        SizeFormatter::format(node.directFileSize)));
        text += QStringLiteral("Files (recursive)\n%1\n\n").arg(node.totalFileCount);
        text += QStringLiteral("Child directories\n%1\n\n").arg(node.childDirCount);
        if (node.newestDescendantWrite != 0) {
            text += QStringLiteral(
                "Newest descendant write\n(FILETIME ticks) %1\n\n")
                        .arg(node.newestDescendantWrite);
        }
    } else if (row.kind == RowKind::File && row.file != InvalidFileIndex) {
        const auto& file = m_lastResult->tree.file(row.file);
        const QFileInfo fi(row.name);
        text += QStringLiteral("Extension\n%1\n\n").arg(fi.suffix());
        if (file.lastWriteTime != 0) {
            text += QStringLiteral("Last write (FILETIME ticks)\n%1\n\n")
                        .arg(file.lastWriteTime);
        }
        if (file.lastAccessTime != 0) {
            text += QStringLiteral(
                        "Last access (FILETIME ticks, advisory only)\n%1\n\n")
                        .arg(file.lastAccessTime);
        }
        text += QStringLiteral("Attributes\n0x%1\n\n")
                    .arg(file.attributes, 0, 16);
    }

    text += QStringLiteral("Classification\n%1\nConfidence\n%2\nMatched rule\n%3\n"
                           "Reason\n%4\n\n")
                .arg(QString::fromUtf8(toString(cls.category)),
                     QString::fromUtf8(toString(cls.confidence)),
                     QString::fromStdString(cls.ruleId),
                     QString::fromStdString(cls.reason));
    text += QStringLiteral("Location safety\n%1\n\n")
                .arg(QString::fromUtf8(toString(safety)));
    if (safety == LocationSafety::Protected) {
        text += QStringLiteral(
            "Protected system location\n"
            "SpaceLens will not manage deletion for this location.\n\n");
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
    text += QStringLiteral("Reclaimability\n%1\nCandidate strength\n%2\n%3\n")
                .arg(QString::fromUtf8(toString(reclaim.reclaimability)),
                     QString::fromUtf8(toString(reclaim.strength)),
                     QString::fromStdString(reclaim.explanation));
    text += QStringLiteral(
        "\nNote: classification and reclaim strength are not permission to delete.");
    m_details->setPlainText(text);
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
        c.capturedSafety = classifyLocation(c.path);
        const auto analysis =
            analyzeItem(c.path, c.kind, c.sizeAtSelection, c.lastWriteTime,
                        c.classification, c.capturedSafety, addedAt);
        c.capturedReclaimability = analysis.reclaimability;
        c.capturedCandidateStrength = analysis.strength;
        prepareCleanupCandidateForAdd(c, reader, addedAt);

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
    QListWidgetItem* item = m_listing->itemAt(pos);
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
