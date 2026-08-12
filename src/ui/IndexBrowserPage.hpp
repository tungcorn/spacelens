#pragma once

#include "app/IndexSession.hpp"
#include "core/CleanupReview.hpp"
#include "core/index/IndexCatalog.hpp"
#include "core/index/IndexOverview.hpp"
#include "core/index/IndexQuery.hpp"
#include "ui/TreemapWidget.hpp"

#include <QAbstractTableModel>
#include <QWidget>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QTextEdit;
class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QTableView;
class QButtonGroup;
class QToolButton;
class QShortcut;
class QHBoxLayout;
class QWidget;

namespace spacelens {

/// Qt model for indexed discovery hits (no QWidget-per-row).
class IndexHitTableModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColSize,
        ColType,
        ColActivity,
        ColClassification,
        ColReclaim,
        ColPath,
        ColCount
    };

    explicit IndexHitTableModel(QObject* parent = nullptr);

    void setHits(std::vector<IndexHit> hits, std::uint64_t indexAgeMs,
                 std::string indexedAtIso);
    void clear();

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;

    [[nodiscard]] const IndexHit* hitAt(int row) const;
    [[nodiscard]] int findRowByPath(const std::wstring& path) const;
    [[nodiscard]] std::uint64_t indexAgeMs() const { return m_indexAgeMs; }
    [[nodiscard]] const std::string& indexedAtIso() const
    {
        return m_indexedAtIso;
    }

private:
    std::vector<IndexHit> m_hits;
    std::uint64_t m_indexAgeMs = 0;
    std::string m_indexedAtIso;
};

/// Indexed storage discovery + overview/treemap. Qt calls core APIs only.
/// No delete/move; filesystem_mutation remains false.
class IndexBrowserPage final : public QWidget {
    Q_OBJECT

public:
    explicit IndexBrowserPage(CleanupReview& review, QWidget* parent = nullptr);
    ~IndexBrowserPage() override;

public slots:
    void reloadRoots();

signals:
    void statusMessage(const QString& message);

private slots:
    void onRootSelectionChanged();
    void onQuery();
    void onRefreshIndex();
    void onRebuildIndex();
    void onIndexNewRoot();
    void onCancelJob();
    void onHitSelectionChanged();
    void onHitDoubleClicked(const QModelIndex& index);
    void onHitsContextMenu(const QPoint& pos);
    void onAddToReview();
    void onOpenSelected();
    void onRevealSelected();
    void onOpenFolderSelected();
    void onCopyPath();
    void onCopyDetails();
    void onPresetClicked(int id);
    void onBreadcrumbClicked(int segmentIndex);
    void onBuildFinished(spacelens::IndexBuildState state);
    void onRefreshFinished(spacelens::IndexRefreshOutcome outcome);
    void onSessionStatus(const QString& message);
    void onQueryFinished(quint64 generation);
    void onTreemapItemClicked(const TreemapDisplayItem& item);
    void onTreemapItemDoubleClicked(const TreemapDisplayItem& item);

private:
    struct PendingBrowsePayload {
        IndexQueryResult discovery{};
        HierarchyChildrenResult hierarchy{};
    };

    void buildUi();
    void updateActionState();
    void clearHits();
    void updateInspector();
    void updateRootHeader();
    void updateBreadcrumb();
    void updateOverviewLabel(const StorageOverview& overview);
    void applyHierarchyResult(const HierarchyChildrenResult& hierarchy);
    void applyPresetDefaults(IndexDiscoveryPreset preset);
    [[nodiscard]] IndexQuerySpec buildQuerySpec() const;
    [[nodiscard]] std::optional<IndexRootSummary> selectedRoot() const;
    [[nodiscard]] std::vector<int> selectedRows() const;
    [[nodiscard]] std::vector<IndexHit> selectedHits() const;
    void setBusy(bool busy);
    void browseInto(const std::wstring& path);
    void focusSearch();
    [[nodiscard]] bool ensurePathExists(const std::wstring& path,
                                        QString* message) const;
    void applyQueryResult(PendingBrowsePayload payload, quint64 generation);
    void selectTablePath(const std::wstring& path);
    void showOtherInspector(const TreemapDisplayItem& item);

    CleanupReview& m_review;
    IndexSession* m_session = nullptr;

    std::vector<IndexRootSummary> m_roots;
    std::optional<IndexRootSummary> m_activeRoot;
    std::wstring m_browsePath;  // empty = index root view
    IndexDiscoveryPreset m_preset = IndexDiscoveryPreset::Largest;
    bool m_sortUserOverride = false;
    bool m_syncingSelection = false;

    /// Immediate children of the current location (hierarchy, not discovery filters).
    std::vector<IndexHit> m_hierarchyChildren;
    StorageOverview m_overview{};
    std::optional<TreemapDisplayItem> m_otherSelection;

    IndexHitTableModel* m_hitModel = nullptr;

    // Async query worker (separate from build/refresh session).
    std::jthread m_queryWorker;
    std::mutex m_queryMutex;
    std::atomic<quint64> m_queryGeneration{0};
    std::optional<PendingBrowsePayload> m_pendingBrowse;
    bool m_queryRunning = false;

    QListWidget* m_rootsList = nullptr;
    QTableView* m_hitsView = nullptr;
    TreemapWidget* m_treemap = nullptr;
    QTextEdit* m_inspector = nullptr;
    QLabel* m_rootMeta = nullptr;
    QLabel* m_overviewLabel = nullptr;
    QLabel* m_queryMeta = nullptr;
    QLabel* m_selectionMeta = nullptr;
    QLabel* m_emptyLabel = nullptr;
    QWidget* m_breadcrumbBar = nullptr;
    QHBoxLayout* m_breadcrumbLayout = nullptr;

    QButtonGroup* m_presetGroup = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QComboBox* m_kindFilter = nullptr;
    QComboBox* m_minSizeFilter = nullptr;
    QLineEdit* m_minSizeCustom = nullptr;
    QComboBox* m_activityFilter = nullptr;
    QLineEdit* m_extFilter = nullptr;
    QComboBox* m_classFilter = nullptr;
    QComboBox* m_strengthFilter = nullptr;
    QComboBox* m_sortFilter = nullptr;
    QSpinBox* m_limitSpin = nullptr;

    QPushButton* m_reloadButton = nullptr;
    QPushButton* m_queryButton = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_rebuildButton = nullptr;
    QPushButton* m_indexNewButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_addReviewButton = nullptr;
    QPushButton* m_openButton = nullptr;
    QPushButton* m_revealButton = nullptr;
    QPushButton* m_copyPathButton = nullptr;
};

}  // namespace spacelens
