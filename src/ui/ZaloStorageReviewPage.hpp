#pragma once

#include "app/ZaloPreviewProvider.hpp"
#include "app/ZaloStorageSession.hpp"
#include "core/ZaloStorageInspector.hpp"

#include <QWidget>

#include <optional>

class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTableWidget;

namespace spacelens {

class EmptyStateWidget;
class MetricStrip;

/// Read-only Zalo storage evidence review. Native locations never enter the
/// page's visible model; rows contain only report-local aliases, deterministic
/// human identities, visual previews, and bounded physical impact accounting.
class ZaloStorageReviewPage final : public QWidget {
    Q_OBJECT

public:
    explicit ZaloStorageReviewPage(QWidget* parent = nullptr);
    ~ZaloStorageReviewPage() override;

signals:
    void statusMessage(const QString& message);

private slots:
    void onChooseRoot();
    void onReview();
    void onCancel();
    void onSessionStatus(const QString& message);
    void onFinished(spacelens::ZaloStorageStatus status);
    void onTableContextMenu(const QPoint& pos);
    void onCellDoubleClicked(int row, int column);
    void onOpenFile();
    void onRevealInExplorer();
    void onCopyPath();
    void onDeleteSelected();
    void onCleanFileNoise();
    void onSelectionChanged();
    void onProgressUpdated(const spacelens::ZaloScanProgress& progress);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    enum Column {
        ColPreview = 0,
        ColName,
        ColSummary,
        ColPhysicalImpact,
        ColLogical,
        ColAllocated,
        ColExactCopy,
        ColAge,
        ColConfidence,
        ColCategory,
        ColRootAccount,
        ColEntry,
        ColCount
    };

    struct ItemDisplayRow {
        const ZaloAccountReport* account = nullptr;
        const ZaloEntry* entry = nullptr;
        ByteSize physicalImpact = 0;
        QString exactCopyLabel;
        std::wstring nativePath;
    };

    void buildUi();
    QWidget* buildScanningWidget();
    void resetScanningWidget();
    void clearReport();
    void applyReport(const ZaloStorageReport& report);
    void updateActionState();
    void showEmptyState(const QString& title, const QString& body,
                        bool showAction);

    QString m_selectedRoot;
    ZaloStorageSession* m_session = nullptr;
    ZaloPreviewProvider m_previewProvider;
    std::optional<ZaloStorageReport> m_report;
    std::vector<ItemDisplayRow> m_displayRows;

    QStackedWidget* m_stack = nullptr;
    EmptyStateWidget* m_empty = nullptr;
    QWidget* m_scanningWidget = nullptr;
    QLabel* m_scanTitleLabel = nullptr;
    QLabel* m_scanSubtitleLabel = nullptr;
    QProgressBar* m_scanProgressBar = nullptr;
    QLabel* m_scanPhaseValue = nullptr;
    QLabel* m_scanFilesValue = nullptr;
    QLabel* m_scanBytesValue = nullptr;
    QLabel* m_scanCurrentPath = nullptr;
    QLabel* m_scanPhotoBadge = nullptr;
    QLabel* m_scanVideoBadge = nullptr;
    QLabel* m_scanNoiseBadge = nullptr;
    QLabel* m_scanCacheBadge = nullptr;
    QLabel* m_scanDocBadge = nullptr;

    QTableWidget* m_entries = nullptr;
    MetricStrip* m_metrics = nullptr;
    QLabel* m_rootSummary = nullptr;
    QLabel* m_reportSummary = nullptr;
    QPushButton* m_chooseButton = nullptr;
    QPushButton* m_reviewButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_cleanFileNoiseButton = nullptr;
    QPushButton* m_deleteButton = nullptr;
    QProgressBar* m_progressBar = nullptr;
};

}  // namespace spacelens
