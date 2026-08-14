#pragma once

#include <QDialog>

#include <cstdint>
#include <optional>
#include <vector>

class QCheckBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTextEdit;

namespace spacelens {

class CleanupReviewController;
class CleanupRevalidationSession;
class MaintenanceSession;

class CleanupReviewDialog final : public QDialog {
    Q_OBJECT

public:
    CleanupReviewDialog(CleanupReviewController& controller,
                        CleanupRevalidationSession& session,
                        MaintenanceSession& maintenance,
                        QWidget* parent = nullptr);
    ~CleanupReviewDialog() override;

    void refresh();

private slots:
    void onRemoveSelected();
    void onClear();
    void onCopyPlan();
    void onExportJson();
    void onOpen();
    void onReveal();
    void onRefreshEvidence();
    void onRevalidateAll();
    void onCancelRevalidate();
    void onMoveToRecycleBin();
    void onMaintenanceHistory();
    void onSelectionChanged();
    void onRevalidationProgress(quint64 probed, quint64 total);
    void onRevalidationFinished(bool completed, const QString& message);
    void onMaintenancePlanReady(bool ok, const QString& message);
    void onMaintenanceProgress(quint64 done, quint64 total);
    void onMaintenanceFinished(bool completed, const QString& message);

private:
    [[nodiscard]] std::vector<std::uint64_t> selectedIds() const;
    [[nodiscard]] std::optional<std::uint64_t> singleSelectedId() const;
    void updateActionState();
    void showStatus(const QString& message);
    void confirmAndExecute();
    void showCompletionSummary();

    CleanupReviewController& m_controller;
    CleanupRevalidationSession& m_session;
    MaintenanceSession& m_maintenance;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QListWidget* m_list = nullptr;
    QTextEdit* m_details = nullptr;
    QProgressBar* m_progress = nullptr;
    QCheckBox* m_redact = nullptr;
    QPushButton* m_revalidateButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_refreshEvidenceButton = nullptr;
    QPushButton* m_openButton = nullptr;
    QPushButton* m_revealButton = nullptr;
    QPushButton* m_removeButton = nullptr;
    QPushButton* m_clearButton = nullptr;
    QPushButton* m_recycleButton = nullptr;
    QPushButton* m_historyButton = nullptr;
};

}  // namespace spacelens
