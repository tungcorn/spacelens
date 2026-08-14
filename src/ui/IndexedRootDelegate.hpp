#pragma once

#include <QStyledItemDelegate>

namespace spacelens {

/// Three-line indexed-root row: folder name, size · age, textual health.
class IndexedRootDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    enum Role {
        MetaRole = Qt::UserRole + 1,
        StatusRole = Qt::UserRole + 2,
        UnavailableRole = Qt::UserRole + 3
    };

    explicit IndexedRootDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
};

}  // namespace spacelens
