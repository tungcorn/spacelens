#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

namespace spacelens {

/// Compact empty / ready / zero-result prompt. No hero illustration.
class EmptyStateWidget final : public QWidget {
    Q_OBJECT

public:
    explicit EmptyStateWidget(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setBody(const QString& body);
    void setActionText(const QString& text);
    void setActionVisible(bool visible);

signals:
    void actionClicked();

private:
    QLabel* m_title = nullptr;
    QLabel* m_body = nullptr;
    QPushButton* m_action = nullptr;
};

}  // namespace spacelens
