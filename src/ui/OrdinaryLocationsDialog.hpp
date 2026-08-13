#pragma once

#include "core/CleanupReviewStore.hpp"

#include <QDialog>

class QListWidget;
class QLabel;
class QPushButton;

namespace spacelens {

class OrdinaryLocationsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit OrdinaryLocationsDialog(CleanupReviewController& controller,
                                     QWidget* parent = nullptr);

private slots:
    void onAdd();
    void onRemove();
    void onSelectionChanged();

private:
    void refresh();

    CleanupReviewController& m_controller;
    QLabel* m_intro = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_addButton = nullptr;
    QPushButton* m_removeButton = nullptr;
};

}  // namespace spacelens
