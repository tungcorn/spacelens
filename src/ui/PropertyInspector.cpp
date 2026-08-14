#include "ui/PropertyInspector.hpp"

#include "ui/UiTheme.hpp"

#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QSize>
#include <QVBoxLayout>

namespace spacelens {

PropertyInspector::PropertyInspector(QWidget* parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_host = new QWidget(this);
    m_root = new QVBoxLayout(m_host);
    m_root->setContentsMargins(0, 0, kUiSpace4, 0);
    m_root->setSpacing(kUiSpace8);

    m_title = new QLabel(m_host);
    m_title->setObjectName(QStringLiteral("slRootTitle"));
    m_title->setWordWrap(true);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_title->hide();

    m_summary = new QLabel(m_host);
    m_summary->setObjectName(QStringLiteral("slMetricValue"));
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary->hide();

    m_formHost = new QWidget(m_host);
    m_form = new QFormLayout(m_formHost);
    m_form->setContentsMargins(0, 0, 0, 0);
    m_form->setHorizontalSpacing(kUiSpace12);
    m_form->setVerticalSpacing(kUiSpace8);
    m_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_formHost->hide();

    m_note = new QLabel(m_host);
    m_note->setObjectName(QStringLiteral("slHint"));
    m_note->setWordWrap(true);
    m_note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_note->hide();

    m_root->addWidget(m_title);
    m_root->addWidget(m_summary);
    m_root->addWidget(m_formHost);
    m_root->addWidget(m_note);
    m_root->addStretch(1);
    setWidget(m_host);
}

void PropertyInspector::clear()
{
    m_title->clear();
    m_title->hide();
    m_summary->clear();
    m_summary->hide();
    m_note->clear();
    m_note->hide();
    while (m_form->rowCount() > 0) {
        m_form->removeRow(0);
    }
    m_formHost->hide();
    m_plain.clear();
}

void PropertyInspector::setHeading(const QString& title, const QString& summary)
{
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
    m_summary->setText(summary);
    m_summary->setVisible(!summary.isEmpty());
    if (!title.isEmpty()) {
        m_plain += title;
        m_plain += QLatin1Char('\n');
    }
    if (!summary.isEmpty()) {
        m_plain += summary;
        m_plain += QLatin1Char('\n');
    }
    if (!m_plain.isEmpty()) {
        m_plain += QLatin1Char('\n');
    }
}

void PropertyInspector::addRow(const QString& label, const QString& value)
{
    if (isNoiseDisplayValue(value)) {
        return;
    }
    auto* valueLabel = new QLabel(value, m_formHost);
    valueLabel->setObjectName(QStringLiteral("slPropValue"));
    valueLabel->setWordWrap(true);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* nameLabel = new QLabel(label, m_formHost);
    nameLabel->setObjectName(QStringLiteral("slPropLabel"));
    nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_form->addRow(nameLabel, valueLabel);
    m_formHost->show();
    m_plain += label;
    m_plain += QLatin1Char('\n');
    m_plain += value;
    m_plain += QLatin1String("\n\n");
}

void PropertyInspector::addNote(const QString& note)
{
    if (note.trimmed().isEmpty()) {
        return;
    }
    m_note->setText(note);
    m_note->show();
    m_plain += note;
    m_plain += QLatin1Char('\n');
}

QString PropertyInspector::toPlainText() const
{
    return m_plain.trimmed();
}

QSize PropertyInspector::sizeHint() const
{
    return {280, 320};
}

QSize PropertyInspector::minimumSizeHint() const
{
    return {240, 160};
}

}  // namespace spacelens
