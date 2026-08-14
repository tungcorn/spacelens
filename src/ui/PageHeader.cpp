#include "ui/PageHeader.hpp"

#include "ui/UiTheme.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace spacelens {

PageHeader::PageHeader(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(kUiSpace16);

    auto* text = new QVBoxLayout();
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(2);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("slPageTitle"));
    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("slPageSubtitle"));
    m_subtitle->setWordWrap(true);
    text->addWidget(m_title);
    text->addWidget(m_subtitle);
    root->addLayout(text, 1);

    m_commands = new QHBoxLayout();
    m_commands->setContentsMargins(0, 0, 0, 0);
    m_commands->setSpacing(kUiSpace8);
    m_commands->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    root->addLayout(m_commands);
}

void PageHeader::setTitle(const QString& title)
{
    m_title->setText(title);
}

void PageHeader::setSubtitle(const QString& subtitle)
{
    m_subtitle->setText(subtitle);
    m_subtitle->setVisible(!subtitle.isEmpty());
}

}  // namespace spacelens
