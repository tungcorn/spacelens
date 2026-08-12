#pragma once

#include "app/ScanSession.hpp"
#include "core/ScanTypes.hpp"

#include <QMainWindow>
#include <QString>

#include <memory>
#include <optional>

class QLineEdit;
class QPushButton;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QListWidget;
class QListWidgetItem;

namespace spacelens {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onSelectFolder();
    void onScan();
    void onCancel();
    void onProgress(const spacelens::ScanProgress& progress);
    void onScanFinished(spacelens::ScanState state);
    void onFolderContextMenu(const QPoint& pos);
    void onLargestContextMenu(const QPoint& pos);
    void onCopySelectedPath();
    void onOpenSelectedInExplorer();

private:
    void buildUi();
    void updateActionState();
    void setStatusMessage(const QString& message);
    void clearResults();
    void populateResults(const ScanResult& result);
    void populateFolderItem(QTreeWidgetItem* item,
                            const DirectoryTree& tree,
                            DirIndex dirIndex);
    [[nodiscard]] QString selectedPath() const;
    void copyPathToClipboard(const QString& path);

    QString m_rootPath;
    std::unique_ptr<ScanSession> m_session;
    std::optional<ScanResult> m_lastResult;

    QLineEdit* m_pathEdit = nullptr;
    QPushButton* m_selectButton = nullptr;
    QPushButton* m_scanButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_copyPathButton = nullptr;
    QPushButton* m_openExplorerButton = nullptr;

    QLabel* m_filesLabel = nullptr;
    QLabel* m_foldersLabel = nullptr;
    QLabel* m_processedLabel = nullptr;
    QLabel* m_elapsedLabel = nullptr;
    QLabel* m_errorsLabel = nullptr;
    QLabel* m_statusLabel = nullptr;

    QTreeWidget* m_folderTree = nullptr;
    QListWidget* m_largestList = nullptr;
};

}  // namespace spacelens
