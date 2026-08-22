#pragma once

#include "app/ZaloPreviewProvider.hpp"
#include "app/ZaloStorageSession.hpp"
#include "core/ZaloStorageInspector.hpp"

#include <QWidget>

#include <optional>

class QLabel;
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

    void buildUi();
    void clearReport();
    void applyReport(const ZaloStorageReport& report);
    void updateActionState();
    void showEmptyState(const QString& title, const QString& body,
                        bool showAction);

    QString m_selectedRoot;
    ZaloStorageSession* m_session = nullptr;
    ZaloPreviewProvider m_previewProvider;
    std::optional<ZaloStorageReport> m_report;

    QStackedWidget* m_stack = nullptr;
    EmptyStateWidget* m_empty = nullptr;
    QTableWidget* m_entries = nullptr;
    MetricStrip* m_metrics = nullptr;
    QLabel* m_rootSummary = nullptr;
    QLabel* m_reportSummary = nullptr;
    QPushButton* m_chooseButton = nullptr;
    QPushButton* m_reviewButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
};

}  // namespace spacelens
