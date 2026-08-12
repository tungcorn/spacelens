#include "ui/MainWindow.hpp"

#include "core/SizeFormatter.hpp"
#include "platform/windows/ExplorerIntegration.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace spacelens {
namespace {

QString formatElapsed(double seconds)
{
    return QStringLiteral("%1 s").arg(seconds, 0, 'f', 1);
}

QString fromWide(const std::wstring& s)
{
    return QString::fromStdWString(s);
}

std::wstring toWide(const QString& s)
{
    return s.toStdWString();
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_session(std::make_unique<ScanSession>(this))
{
    setWindowTitle(QStringLiteral("SpaceLens"));
    resize(1100, 700);
    buildUi();
    updateActionState();
    setStatusMessage(QStringLiteral("Select a folder to scan."));

    connect(m_session.get(), &ScanSession::progressUpdated, this,
            &MainWindow::onProgress);
    connect(m_session.get(), &ScanSession::finished, this,
            &MainWindow::onScanFinished);
}

MainWindow::~MainWindow()
{
    // ScanSession destructor cancels + joins; ensure UI does not outlive it oddly.
    if (m_session) {
        m_session->cancel();
    }
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* rootLayout = new QVBoxLayout(central);

    auto* topRow = new QHBoxLayout();
    m_pathEdit = new QLineEdit(central);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(QStringLiteral("No folder selected"));

    m_selectButton = new QPushButton(QStringLiteral("Select Folder…"), central);
    m_scanButton = new QPushButton(QStringLiteral("Scan"), central);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), central);
    m_copyPathButton = new QPushButton(QStringLiteral("Copy Path"), central);
    m_openExplorerButton = new QPushButton(QStringLiteral("Open in Explorer"), central);

    topRow->addWidget(m_pathEdit, 1);
    topRow->addWidget(m_selectButton);
    topRow->addWidget(m_scanButton);
    topRow->addWidget(m_cancelButton);
    topRow->addWidget(m_copyPathButton);
    topRow->addWidget(m_openExplorerButton);
    rootLayout->addLayout(topRow);

    auto* statsRow = new QHBoxLayout();
    m_filesLabel = new QLabel(QStringLiteral("Files: 0"), central);
    m_foldersLabel = new QLabel(QStringLiteral("Folders: 0"), central);
    m_processedLabel = new QLabel(
        QStringLiteral("Processed: %1")
            .arg(QString::fromStdString(SizeFormatter::format(0))),
        central);
    m_elapsedLabel = new QLabel(QStringLiteral("Elapsed: 0.0 s"), central);
    m_errorsLabel = new QLabel(QStringLiteral("Errors: 0"), central);

    statsRow->addWidget(m_filesLabel);
    statsRow->addWidget(m_foldersLabel);
    statsRow->addWidget(m_processedLabel);
    statsRow->addWidget(m_elapsedLabel);
    statsRow->addWidget(m_errorsLabel);
    statsRow->addStretch(1);
    rootLayout->addLayout(statsRow);

    auto* splitter = new QSplitter(Qt::Horizontal, central);

    m_folderTree = new QTreeWidget(splitter);
    m_folderTree->setHeaderLabels(
        {QStringLiteral("Folder"), QStringLiteral("Size"), QStringLiteral("Files")});
    m_folderTree->setUniformRowHeights(true);
    m_folderTree->setAlternatingRowColors(true);
    m_folderTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_folderTree->setExpandsOnDoubleClick(true);

    m_largestList = new QListWidget(splitter);
    m_largestList->setAlternatingRowColors(true);
    m_largestList->setContextMenuPolicy(Qt::CustomContextMenu);

    splitter->addWidget(m_folderTree);
    splitter->addWidget(m_largestList);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    rootLayout->addWidget(splitter, 1);

    m_statusLabel = new QLabel(central);
    statusBar()->addWidget(m_statusLabel, 1);

    connect(m_selectButton, &QPushButton::clicked, this, &MainWindow::onSelectFolder);
    connect(m_scanButton, &QPushButton::clicked, this, &MainWindow::onScan);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(m_copyPathButton, &QPushButton::clicked, this,
            &MainWindow::onCopySelectedPath);
    connect(m_openExplorerButton, &QPushButton::clicked, this,
            &MainWindow::onOpenSelectedInExplorer);
    connect(m_folderTree, &QTreeWidget::customContextMenuRequested, this,
            &MainWindow::onFolderContextMenu);
    connect(m_largestList, &QListWidget::customContextMenuRequested, this,
            &MainWindow::onLargestContextMenu);
}

void MainWindow::updateActionState()
{
    const bool running = m_session && m_session->isRunning();
    const bool hasPath = !m_rootPath.isEmpty();
    m_selectButton->setEnabled(!running);
    m_scanButton->setEnabled(hasPath && !running);
    m_cancelButton->setEnabled(running);
    m_copyPathButton->setEnabled(hasPath || m_lastResult.has_value());
    m_openExplorerButton->setEnabled(hasPath || m_lastResult.has_value());
}

void MainWindow::setStatusMessage(const QString& message)
{
    m_statusLabel->setText(message);
}

void MainWindow::clearResults()
{
    m_folderTree->clear();
    m_largestList->clear();
    m_lastResult.reset();
}

void MainWindow::onSelectFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select folder to analyze"),
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
    m_filesLabel->setText(
        QStringLiteral("Files: %1")
            .arg(progress.filesSeen));
    m_foldersLabel->setText(
        QStringLiteral("Folders: %1")
            .arg(progress.directoriesSeen));
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

    const auto& progress = result->progress;
    onProgress(progress);

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

    populateResults(*result);
    m_lastResult = std::move(result);
}

void MainWindow::populateResults(const ScanResult& result)
{
    m_folderTree->clear();
    m_largestList->clear();

    if (result.tree.empty()) {
        return;
    }

    const DirIndex root = result.tree.root();
    auto* rootItem = new QTreeWidgetItem(m_folderTree);
    populateFolderItem(rootItem, result.tree, root);
    rootItem->setExpanded(true);
    m_folderTree->resizeColumnToContents(0);
    m_folderTree->resizeColumnToContents(1);

    for (const auto& file : result.largestFiles) {
        const QString text = QStringLiteral("%1  —  %2")
                                 .arg(QString::fromStdString(
                                     SizeFormatter::format(file.size)),
                                      fromWide(file.path));
        auto* item = new QListWidgetItem(text, m_largestList);
        item->setData(Qt::UserRole, fromWide(file.path));
        item->setToolTip(fromWide(file.path));
    }
}

void MainWindow::populateFolderItem(QTreeWidgetItem* item,
                                    const DirectoryTree& tree,
                                    DirIndex dirIndex)
{
    const DirectoryNode& node = tree.dir(dirIndex);
    item->setText(0, fromWide(node.name));
    item->setText(1, QString::fromStdString(SizeFormatter::format(node.recursiveSize)));
    item->setText(2, QString::number(node.totalFileCount));
    item->setData(0, Qt::UserRole, fromWide(tree.pathOfDirectory(dirIndex)));
    item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);

    // Sort children by recursive size descending for readability.
    auto children = tree.largestChildDirectories(dirIndex, node.children.size());
    for (const DirIndex child : children) {
        auto* childItem = new QTreeWidgetItem(item);
        populateFolderItem(childItem, tree, child);
    }
}

QString MainWindow::selectedPath() const
{
    if (const auto* folder = m_folderTree->currentItem()) {
        const QString path = folder->data(0, Qt::UserRole).toString();
        if (!path.isEmpty()) {
            return path;
        }
    }
    if (const auto* fileItem = m_largestList->currentItem()) {
        const QString path = fileItem->data(Qt::UserRole).toString();
        if (!path.isEmpty()) {
            return path;
        }
    }
    return m_rootPath;
}

void MainWindow::copyPathToClipboard(const QString& path)
{
    if (path.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(path);
    setStatusMessage(QStringLiteral("Copied: %1").arg(path));
}

void MainWindow::onCopySelectedPath()
{
    copyPathToClipboard(selectedPath());
}

void MainWindow::onOpenSelectedInExplorer()
{
    const QString path = selectedPath();
    if (path.isEmpty()) {
        return;
    }
    if (!openInExplorer(toWide(path))) {
        // Fall back to reveal for files.
        if (!revealInExplorer(toWide(path))) {
            QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                                 QStringLiteral("Could not open Explorer for:\n%1")
                                     .arg(path));
        }
    }
}

void MainWindow::onFolderContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_folderTree->itemAt(pos);
    if (!item) {
        return;
    }
    m_folderTree->setCurrentItem(item);

    QMenu menu(this);
    menu.addAction(QStringLiteral("Open in Explorer"), this,
                   &MainWindow::onOpenSelectedInExplorer);
    menu.addAction(QStringLiteral("Copy Path"), this,
                   &MainWindow::onCopySelectedPath);
    menu.exec(m_folderTree->viewport()->mapToGlobal(pos));
}

void MainWindow::onLargestContextMenu(const QPoint& pos)
{
    QListWidgetItem* item = m_largestList->itemAt(pos);
    if (!item) {
        return;
    }
    m_largestList->setCurrentItem(item);

    QMenu menu(this);
    menu.addAction(QStringLiteral("Show in Explorer"), this, [this]() {
        const QString path = selectedPath();
        if (!revealInExplorer(toWide(path))) {
            QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                                 QStringLiteral("Could not reveal:\n%1").arg(path));
        }
    });
    menu.addAction(QStringLiteral("Copy Path"), this,
                   &MainWindow::onCopySelectedPath);
    menu.exec(m_largestList->viewport()->mapToGlobal(pos));
}

}  // namespace spacelens
