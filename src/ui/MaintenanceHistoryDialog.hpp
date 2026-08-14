#pragma once

#include <QDialog>

class QLabel;
class QListWidget;
class QTextEdit;

namespace spacelens {

class CleanupReviewController;

/// Inspection-only Recycle Bin history. No Restore, Delete, Retry, or Empty.
class MaintenanceHistoryDialog final : public QDialog {
    Q_OBJECT

public:
    explicit MaintenanceHistoryDialog(CleanupReviewController& controller,
                                      QWidget* parent = nullptr);

    void selectOperation(quint64 operationId);

private slots:
    void onSelectionChanged();

private:
    void refresh();

    CleanupReviewController& m_controller;
    QLabel* m_intro = nullptr;
    QListWidget* m_list = nullptr;
    QTextEdit* m_details = nullptr;
};

}  // namespace spacelens
