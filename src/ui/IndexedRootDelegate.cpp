#include "ui/IndexedRootDelegate.hpp"

#include "ui/UiTheme.hpp"

#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace spacelens {

IndexedRootDelegate::IndexedRootDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

void IndexedRootDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    const QStyle* style =
        opt.widget != nullptr ? opt.widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

    const QPalette pal = opt.palette;
    const bool selected = (opt.state & QStyle::State_Selected) != 0;
    const bool unavailable = index.data(UnavailableRole).toBool();
    const QColor titleColor =
        selected ? pal.color(QPalette::HighlightedText)
                 : (unavailable ? mutedTextColor(pal)
                                : pal.color(QPalette::WindowText));
    QColor metaColor =
        selected ? pal.color(QPalette::HighlightedText) : mutedTextColor(pal);
    if (selected) {
        metaColor.setAlpha(230);
    }

    const QString path = index.data(Qt::DisplayRole).toString();
    const QString title = displayFolderName(path);
    const QString meta = index.data(MetaRole).toString();
    const QString status = index.data(StatusRole).toString();

    if (selected) {
        const QRect accent(opt.rect.left(), opt.rect.top() + 6, 3,
                           opt.rect.height() - 12);
        painter->fillRect(accent, pal.color(QPalette::HighlightedText));
    }

    QRect textRect = opt.rect.adjusted(kUiSpace12, 7, -kUiSpace12, -7);
    QFont titleFont = opt.font;
    titleFont.setBold(true);
    QFont metaFont = opt.font;
    if (metaFont.pointSizeF() > 0) {
        metaFont.setPointSizeF(metaFont.pointSizeF() - 1.0);
    }

    painter->save();
    painter->setPen(titleColor);
    painter->setFont(titleFont);
    const QString elided =
        QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, textRect.width());
    painter->drawText(textRect, Qt::AlignTop | Qt::AlignLeft, elided);

    painter->setPen(metaColor);
    painter->setFont(metaFont);
    QString second = meta;
    if (!status.isEmpty()) {
        second += QStringLiteral("\n") + status;
    }
    painter->drawText(textRect, Qt::AlignBottom | Qt::AlignLeft | Qt::TextWordWrap,
                      second);
    painter->restore();
}

QSize IndexedRootDelegate::sizeHint(const QStyleOptionViewItem& option,
                                    const QModelIndex&) const
{
    return {option.rect.width() > 0 ? option.rect.width() : kUiRootPaneHint,
            kUiRootRowHeight};
}

}  // namespace spacelens
