#pragma once

#include "app/IndexSession.hpp"
#include "core/CleanupReview.hpp"
#include "core/index/IndexCatalog.hpp"
#include "core/index/IndexQuery.hpp"

#include <QWidget>

#include <optional>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QTextEdit;
class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;
class QSpinBox;

namespace spacelens {

/// Indexed-roots browser: list/status/query/refresh/rebuild via core APIs only.
/// No SQLite usage in this translation unit; no delete/move; no treemap.
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
    void onAddToReview();
    void onBuildFinished(spacelens::IndexBuildState state);
    void onRefreshFinished(spacelens::IndexRefreshOutcome outcome);
    void onSessionStatus(const QString& message);

private:
    struct HitRow {
        IndexHit hit;
        std::uint64_t indexAgeMs = 0;
        std::string indexedAtIso;
    };

    void buildUi();
    void updateActionState();
    void clearHits();
    void updateInspector();
    [[nodiscard]] std::optional<IndexRootSummary> selectedRoot() const;
    [[nodiscard]] std::vector<HitRow> selectedHits() const;
    void setBusy(bool busy);

    CleanupReview& m_review;
    IndexSession* m_session = nullptr;

    std::vector<IndexRootSummary> m_roots;
    std::vector<HitRow> m_hits;
    std::optional<IndexRootSummary> m_activeRoot;

    QListWidget* m_rootsList = nullptr;
    QListWidget* m_hitsList = nullptr;
    QTextEdit* m_inspector = nullptr;
    QLabel* m_rootMeta = nullptr;
    QLabel* m_queryMeta = nullptr;

    QComboBox* m_kindFilter = nullptr;
    QLineEdit* m_minSizeFilter = nullptr;
    QLineEdit* m_extFilter = nullptr;
    QComboBox* m_classFilter = nullptr;
    QComboBox* m_strengthFilter = nullptr;
    QSpinBox* m_limitSpin = nullptr;

    QPushButton* m_reloadButton = nullptr;
    QPushButton* m_queryButton = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_rebuildButton = nullptr;
    QPushButton* m_indexNewButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_addReviewButton = nullptr;
};

}  // namespace spacelens
