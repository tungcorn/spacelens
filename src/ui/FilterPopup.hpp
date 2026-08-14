#pragma once

#include <QFrame>
#include <QToolButton>

class QFormLayout;
class QPushButton;
class QVBoxLayout;

namespace spacelens {

/// Compact filter form shown as a popup under a Filters button.
class FilterPanel final : public QWidget {
    Q_OBJECT

public:
    explicit FilterPanel(QWidget* parent = nullptr);

    [[nodiscard]] QFormLayout* form() const { return m_form; }

signals:
    void resetRequested();
    void applyRequested();

private:
    QFormLayout* m_form = nullptr;
};

class FilterButton final : public QToolButton {
    Q_OBJECT

public:
    explicit FilterButton(QWidget* parent = nullptr);

    void setPanel(FilterPanel* panel);
    void setActiveCount(int count);
    [[nodiscard]] int activeCount() const { return m_activeCount; }

    void showPanel();

signals:
    void panelAboutToShow();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateLabel();

    FilterPanel* m_panel = nullptr;
    QFrame* m_popup = nullptr;
    int m_activeCount = 0;
};

}  // namespace spacelens
