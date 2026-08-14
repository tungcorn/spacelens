#pragma once

#include <QWidget>

#include <utility>
#include <vector>

class QLabel;

namespace spacelens {

/// Compact typographic metrics. Not dashboard cards.
class MetricStrip final : public QWidget {
    Q_OBJECT

public:
    struct Item {
        QString label;
        QString value;
        QString tooltip;
    };

    explicit MetricStrip(QWidget* parent = nullptr);

    void setItems(const std::vector<Item>& items);

private:
    struct Slot {
        QLabel* label = nullptr;
        QLabel* value = nullptr;
    };

    void ensureSlotCount(int count);

    std::vector<Slot> m_slots;
};

}  // namespace spacelens
