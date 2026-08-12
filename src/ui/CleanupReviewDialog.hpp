#pragma once

#include "core/CleanupReview.hpp"

#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;

namespace spacelens {

class CleanupReviewDialog final : public QDialog {
    Q_OBJECT

public:
    explicit CleanupReviewDialog(CleanupReview& review, QWidget* parent = nullptr);

    void refresh();

private slots:
    void onRemoveSelected();
    void onClear();
    void onCopyReport();
    void onOpen();
    void onReveal();

private:
    CleanupReview& m_review;
    QLabel* m_summary = nullptr;
    QListWidget* m_list = nullptr;
};

}  // namespace spacelens
