#include "ui/MainWindow.hpp"

#include "core/SizeFormatter.hpp"

#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace spacelens {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("SpaceLens"));
    resize(1100, 700);
    buildUi();
    updateActionState();
    setStatusMessage(QStringLiteral("Select a folder to scan."));
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* rootLayout = new QVBoxLayout(central);

    // --- Top bar ---
    auto* topRow = new QHBoxLayout();
    m_pathEdit = new QLineEdit(central);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(QStringLiteral("No folder selected"));

    m_selectButton = new QPushButton(QStringLiteral("Select Folder…"), central);
    m_scanButton = new QPushButton(QStringLiteral("Scan"), central);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), central);

    topRow->addWidget(m_pathEdit, /*stretch=*/1);
    topRow->addWidget(m_selectButton);
    topRow->addWidget(m_scanButton);
    topRow->addWidget(m_cancelButton);
    rootLayout->addLayout(topRow);

    // --- Progress / stats strip ---
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

    // --- Main splitter ---
    auto* splitter = new QSplitter(Qt::Horizontal, central);

    m_folderTree = new QTreeWidget(splitter);
    m_folderTree->setHeaderLabels({QStringLiteral("Folder"), QStringLiteral("Size")});
    m_folderTree->setUniformRowHeights(true);
    m_folderTree->setAlternatingRowColors(true);

    m_largestList = new QListWidget(splitter);
    m_largestList->setAlternatingRowColors(true);

    splitter->addWidget(m_folderTree);
    splitter->addWidget(m_largestList);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    rootLayout->addWidget(splitter, /*stretch=*/1);

    m_statusLabel = new QLabel(central);
    statusBar()->addWidget(m_statusLabel, /*stretch=*/1);

    connect(m_selectButton, &QPushButton::clicked, this, &MainWindow::onSelectFolder);
    connect(m_scanButton, &QPushButton::clicked, this, &MainWindow::onScan);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::onCancel);
}

void MainWindow::updateActionState()
{
    const bool hasPath = !m_rootPath.isEmpty();
    m_scanButton->setEnabled(hasPath);
    // Cancel is only meaningful once an async scan exists.
    m_cancelButton->setEnabled(false);
}

void MainWindow::setStatusMessage(const QString& message)
{
    m_statusLabel->setText(message);
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
    if (m_rootPath.isEmpty()) {
        return;
    }

    // Bootstrap milestone: UI shell only. Scanner is the next commit.
    QMessageBox::information(
        this,
        QStringLiteral("SpaceLens"),
        QStringLiteral(
            "Core scanner is not connected yet.\n\n"
            "Selected folder:\n%1\n\n"
            "This bootstrap build validates the CMake/Qt shell.")
            .arg(m_rootPath));

    setStatusMessage(QStringLiteral("Scanner not wired (bootstrap)."));
}

void MainWindow::onCancel()
{
    // No active scan in the bootstrap shell.
}

}  // namespace spacelens
