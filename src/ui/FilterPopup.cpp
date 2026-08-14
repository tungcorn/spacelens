#include "ui/FilterPopup.hpp"

#include "ui/UiTheme.hpp"

#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace spacelens {

FilterPanel::FilterPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kUiSpace12, kUiSpace12, kUiSpace12, kUiSpace12);
    root->setSpacing(kUiSpace8);

    m_form = new QFormLayout();
    m_form->setContentsMargins(0, 0, 0, 0);
    m_form->setHorizontalSpacing(kUiSpace12);
    m_form->setVerticalSpacing(kUiSpace8);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    root->addLayout(m_form);

    auto* actions = new QHBoxLayout();
    auto* reset = new QPushButton(QStringLiteral("Reset"), this);
    auto* apply = new QPushButton(QStringLiteral("Apply"), this);
    markPrimaryButton(apply);
    actions->addWidget(reset);
    actions->addStretch(1);
    actions->addWidget(apply);
    root->addLayout(actions);

    connect(reset, &QPushButton::clicked, this, &FilterPanel::resetRequested);
    connect(apply, &QPushButton::clicked, this, &FilterPanel::applyRequested);
}

FilterButton::FilterButton(QWidget* parent)
    : QToolButton(parent)
{
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setFocusPolicy(Qt::TabFocus);
    updateLabel();

    m_popup = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    m_popup->setObjectName(QStringLiteral("slFilterPopup"));
    m_popup->setFrameShape(QFrame::StyledPanel);
    m_popup->setAutoFillBackground(true);
    auto* popupLayout = new QVBoxLayout(m_popup);
    popupLayout->setContentsMargins(0, 0, 0, 0);
    m_popup->hide();
    m_popup->installEventFilter(this);

    connect(this, &QToolButton::clicked, this, &FilterButton::showPanel);
}

void FilterButton::setPanel(FilterPanel* panel)
{
    m_panel = panel;
    if (m_panel == nullptr || m_popup == nullptr || m_popup->layout() == nullptr) {
        return;
    }
    m_panel->setParent(m_popup);
    m_popup->layout()->addWidget(m_panel);
    connect(m_panel, &FilterPanel::applyRequested, m_popup, &QWidget::hide);
}

void FilterButton::setActiveCount(int count)
{
    m_activeCount = count < 0 ? 0 : count;
    updateLabel();
}

void FilterButton::updateLabel()
{
    if (m_activeCount > 0) {
        setText(QStringLiteral("Filters %1").arg(m_activeCount));
    } else {
        setText(QStringLiteral("Filters"));
    }
}

void FilterButton::showPanel()
{
    if (m_popup == nullptr || m_panel == nullptr) {
        return;
    }
    emit panelAboutToShow();
    m_popup->adjustSize();
    const QPoint pos = mapToGlobal(QPoint(0, height()));
    m_popup->move(pos);
    m_popup->show();
    m_popup->raise();
    m_panel->setFocus(Qt::PopupFocusReason);
}

bool FilterButton::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_popup && event->type() == QEvent::Hide) {
        setDown(false);
    }
    return QToolButton::eventFilter(watched, event);
}

}  // namespace spacelens
