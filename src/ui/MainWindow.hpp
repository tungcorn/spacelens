#pragma once

#include "app/CleanupRevalidationSession.hpp"
#include "app/MaintenanceSession.hpp"
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
class QTableWidget;
class QTableWidgetItem;
class QComboBox;
class QHBoxLayout;
class QWidget;
class QStackedWidget;
class QProgressBar;
class QToolButton;

namespace spacelens {

class IndexBrowserPage;
class ZaloStorageReviewPage;

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
    void onListingActivated();
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
    void onOrdinaryLocations();
    void onLiveWorkspace();
    void onIndexedWorkspace();
    void onZaloWorkspace();
    void onZaloStatusMessage(const QString& message);

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
    void updateLiveEmptyState();
    void updateLiveMetrics();
    void updateLiveFilterCount();
    void resetLiveFilters();
    [[nodiscard]] int liveActiveFilterCount() const;
    [[nodiscard]] std::vector<RowRef> selectedRows() const;
    [[nodiscard]] std::optional<RowRef> singleSelectedRow() const;
    [[nodiscard]] RowRef rowFromItem(const QTableWidgetItem* item) const;
    void appendListingRow(QTableWidget* table, const RowRef& row,
                          const QString& type, const QString& classification,
                          const QString& modified);
    [[nodiscard]] bool rowPassesFilter(const RowRef& row,
                                       const Classification& cls) const;
    void copyText(const QString& text);
    [[nodiscard]] Classification classifyRow(const RowRef& row) const;

    QString m_rootPath;
    std::unique_ptr<ScanSession> m_session;
    std::optional<ScanResult> m_lastResult;
    ScanProgress m_lastProgress{};
    DirIndex m_currentDir = InvalidDirIndex;
    CleanupReviewController m_reviewController;
    std::unique_ptr<CleanupRevalidationSession> m_revalidationSession;
    std::unique_ptr<MaintenanceSession> m_maintenanceSession;

    QStackedWidget* m_pages = nullptr;
    QPushButton* m_navLive = nullptr;
    QPushButton* m_navIndexed = nullptr;
    QPushButton* m_navZalo = nullptr;
    IndexBrowserPage* m_indexPage = nullptr;
    ZaloStorageReviewPage* m_zaloPage = nullptr;

    QLineEdit* m_pathEdit = nullptr;
    QPushButton* m_selectButton = nullptr;
    QPushButton* m_scanButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_upButton = nullptr;
    QProgressBar* m_scanProgress = nullptr;

    QWidget* m_breadcrumbBar = nullptr;
    QHBoxLayout* m_breadcrumbLayout = nullptr;

    QLineEdit* m_searchEdit = nullptr;
    QComboBox* m_kindFilter = nullptr;
    QLineEdit* m_extFilter = nullptr;
    QLineEdit* m_minSizeFilter = nullptr;
    QComboBox* m_classFilter = nullptr;
    QComboBox* m_sortFilter = nullptr;

    class MetricStrip* m_metrics = nullptr;
    class EmptyStateWidget* m_liveEmpty = nullptr;
    class FilterButton* m_filterButton = nullptr;
    QStackedWidget* m_liveStack = nullptr;
    QWidget* m_liveWork = nullptr;
    QLabel* m_largestLabel = nullptr;
    QLabel* m_selectionLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_trustLabel = nullptr;
    QToolButton* m_itemMoreButton = nullptr;

    QTableWidget* m_listing = nullptr;
    QTableWidget* m_largestList = nullptr;
    class PropertyInspector* m_details = nullptr;

    QPushButton* m_openButton = nullptr;
    QPushButton* m_openFolderButton = nullptr;
    QPushButton* m_explorerButton = nullptr;
    QPushButton* m_copyPathButton = nullptr;
    QPushButton* m_addReviewButton = nullptr;
    QPushButton* m_showReviewButton = nullptr;
    QPushButton* m_rescanButton = nullptr;
};

}  // namespace spacelens
