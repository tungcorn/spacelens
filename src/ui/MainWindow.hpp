#pragma once

#include "app/CleanupRevalidationSession.hpp"
#include "app/ScanSession.hpp"
#include "core/Classification.hpp"
#include "core/CleanupReviewStore.hpp"
#include "core/ScanTypes.hpp"
#include "core/Types.hpp"

#include <QMainWindow>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

class QLineEdit;
class QPushButton;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QTextEdit;
class QComboBox;
class QHBoxLayout;
class QWidget;
class QTabWidget;

namespace spacelens {

class IndexBrowserPage;

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
    void onNavigateUp();
    void onBreadcrumbClicked(int segmentIndex);
    void onItemActivated(QListWidgetItem* item);
    void onSelectionChanged();
    void onFilterChanged();
    void onOpen();
    void onOpenFolder();
    void onShowInExplorer();
    void onCopyPath();
    void onCopyName();
    void onCopyDetails();
    void onAddToReview();
    void onShowReview();
    void onRescanLocation();
    void onContextMenu(const QPoint& pos);
    void onIndexStatusMessage(const QString& message);

private:
    enum class RowKind { Directory, File };

    struct RowRef {
        RowKind kind = RowKind::File;
        DirIndex dir = InvalidDirIndex;
        FileIndex file = InvalidFileIndex;
        QString path;
        QString name;
        ByteSize size = 0;
    };

    void buildUi();
    [[nodiscard]] QWidget* buildLiveScanPage();
    void updateActionState();
    void setStatusMessage(const QString& message);
    void clearResults();
    void populateFromResult();
    void rebuildBreadcrumb();
    void refreshCurrentListing();
    void updateDetails();
    void updateSelectionSummary();
    [[nodiscard]] std::vector<RowRef> selectedRows() const;
    [[nodiscard]] std::optional<RowRef> singleSelectedRow() const;
    [[nodiscard]] RowRef rowFromItem(const QListWidgetItem* item) const;
    [[nodiscard]] bool rowPassesFilter(const RowRef& row,
                                       const Classification& cls) const;
    void copyText(const QString& text);
    [[nodiscard]] Classification classifyRow(const RowRef& row) const;

    QString m_rootPath;
    std::unique_ptr<ScanSession> m_session;
    std::optional<ScanResult> m_lastResult;
    DirIndex m_currentDir = InvalidDirIndex;
    CleanupReviewController m_reviewController;
    std::unique_ptr<CleanupRevalidationSession> m_revalidationSession;

    QTabWidget* m_tabs = nullptr;
    IndexBrowserPage* m_indexPage = nullptr;

    QLineEdit* m_pathEdit = nullptr;
    QPushButton* m_selectButton = nullptr;
    QPushButton* m_scanButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_upButton = nullptr;

    QWidget* m_breadcrumbBar = nullptr;
    QHBoxLayout* m_breadcrumbLayout = nullptr;

    QComboBox* m_kindFilter = nullptr;
    QLineEdit* m_extFilter = nullptr;
    QLineEdit* m_minSizeFilter = nullptr;
    QComboBox* m_classFilter = nullptr;

    QLabel* m_filesLabel = nullptr;
    QLabel* m_foldersLabel = nullptr;
    QLabel* m_processedLabel = nullptr;
    QLabel* m_elapsedLabel = nullptr;
    QLabel* m_errorsLabel = nullptr;
    QLabel* m_selectionLabel = nullptr;
    QLabel* m_statusLabel = nullptr;

    QListWidget* m_listing = nullptr;
    QListWidget* m_largestList = nullptr;
    QTextEdit* m_details = nullptr;

    QPushButton* m_openButton = nullptr;
    QPushButton* m_openFolderButton = nullptr;
    QPushButton* m_explorerButton = nullptr;
    QPushButton* m_copyPathButton = nullptr;
    QPushButton* m_addReviewButton = nullptr;
    QPushButton* m_showReviewButton = nullptr;
    QPushButton* m_rescanButton = nullptr;
};

}  // namespace spacelens
