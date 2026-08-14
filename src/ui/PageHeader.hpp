#pragma once

#include <QWidget>

class QHBoxLayout;
class QLabel;

namespace spacelens {

/// Title + optional subtitle + trailing command row.
class PageHeader final : public QWidget {
    Q_OBJECT

public:
    explicit PageHeader(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setSubtitle(const QString& subtitle);
    [[nodiscard]] QHBoxLayout* commands() const { return m_commands; }

private:
    QLabel* m_title = nullptr;
    QLabel* m_subtitle = nullptr;
    QHBoxLayout* m_commands = nullptr;
};

}  // namespace spacelens
