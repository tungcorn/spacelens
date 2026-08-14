#include "ui/EmptyStateWidget.hpp"

#include "ui/UiTheme.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace spacelens {

EmptyStateWidget::EmptyStateWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kUiSpace24, kUiSpace24, kUiSpace24, kUiSpace24);
    root->setSpacing(kUiSpace8);
    root->addStretch(1);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("slEmptyTitle"));
    m_title->setAlignment(Qt::AlignHCenter);
    m_title->setWordWrap(true);

    m_body = new QLabel(this);
    m_body->setObjectName(QStringLiteral("slEmptyBody"));
    m_body->setAlignment(Qt::AlignHCenter);
    m_body->setWordWrap(true);

    m_action = new QPushButton(this);
    markPrimaryButton(m_action);
    m_action->setVisible(false);
    m_action->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* actionRow = new QHBoxLayout();
    actionRow->addStretch(1);
    actionRow->addWidget(m_action);
    actionRow->addStretch(1);

    root->addWidget(m_title);
    root->addWidget(m_body);
    root->addSpacing(kUiSpace8);
    root->addLayout(actionRow);
    root->addStretch(2);

    connect(m_action, &QPushButton::clicked, this,
            &EmptyStateWidget::actionClicked);
}

void EmptyStateWidget::setTitle(const QString& title)
{
    m_title->setText(title);
}

void EmptyStateWidget::setBody(const QString& body)
{
    m_body->setText(body);
    m_body->setVisible(!body.isEmpty());
}

void EmptyStateWidget::setActionText(const QString& text)
{
    m_action->setText(text);
}

void EmptyStateWidget::setActionVisible(bool visible)
{
    m_action->setVisible(visible);
}

}  // namespace spacelens
