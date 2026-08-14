#include "ui/MetricStrip.hpp"

#include "ui/UiTheme.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace spacelens {

MetricStrip::MetricStrip(QWidget* parent)
    : QWidget(parent)
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, kUiSpace4, 0, kUiSpace4);
    row->setSpacing(kUiSpace24);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void MetricStrip::ensureSlotCount(int count)
{
    auto* row = qobject_cast<QHBoxLayout*>(layout());
    if (row == nullptr) {
        return;
    }
    while (static_cast<int>(m_slots.size()) < count) {
        auto* cell = new QWidget(this);
        auto* col = new QVBoxLayout(cell);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(2);
        Slot slot;
        slot.label = new QLabel(cell);
        slot.label->setObjectName(QStringLiteral("slMetricLabel"));
        slot.value = new QLabel(cell);
        slot.value->setObjectName(QStringLiteral("slMetricValue"));
        col->addWidget(slot.label);
        col->addWidget(slot.value);
        row->addWidget(cell);
        m_slots.push_back(slot);
    }
    if (row->itemAt(row->count() - 1) == nullptr ||
        row->itemAt(row->count() - 1)->widget() != nullptr) {
        row->addStretch(1);
    }
    for (int i = 0; i < static_cast<int>(m_slots.size()); ++i) {
        if (auto* item = row->itemAt(i)) {
            if (item->widget() != nullptr) {
                item->widget()->setVisible(i < count);
            }
        }
    }
}

void MetricStrip::setItems(const std::vector<Item>& items)
{
    ensureSlotCount(static_cast<int>(items.size()));
    for (std::size_t i = 0; i < items.size(); ++i) {
        m_slots[i].label->setText(items[i].label.toUpper());
        m_slots[i].value->setText(items[i].value);
        const QString tip = items[i].tooltip.isEmpty() ? items[i].value
                                                       : items[i].tooltip;
        m_slots[i].label->setToolTip(tip);
        m_slots[i].value->setToolTip(tip);
    }
}

}  // namespace spacelens
