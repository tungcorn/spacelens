#include "ui/TreemapWidget.hpp"

#include "core/SizeFormatter.hpp"
#include "core/index/IndexOverview.hpp"
#include "ui/UiTheme.hpp"

#include <QContextMenuEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace spacelens {
namespace {

QString fromWide(const std::wstring& s)
{
    return QString::fromStdWString(s);
}

QColor mix(const QColor& a, const QColor& b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

}  // namespace

TreemapWidget::TreemapWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(true);
}

QSize TreemapWidget::sizeHint() const
{
    return {480, 220};
}

QSize TreemapWidget::minimumSizeHint() const
{
    return {200, 120};
}

void TreemapWidget::setItems(std::vector<TreemapDisplayItem> items,
                             ByteSize parentTotalBytes)
{
    m_items = std::move(items);
    m_parentTotal = parentTotalBytes;
    m_hoverIndex = -1;
    m_selectedIndex = -1;
    if (!m_selectedPath.empty()) {
        for (std::size_t i = 0; i < m_items.size(); ++i) {
            if (!m_items[i].isOther && m_items[i].path == m_selectedPath) {
                m_selectedIndex = static_cast<int>(i);
                break;
            }
        }
    }
    m_layoutDirty = true;
    relayout();
    update();
}

void TreemapWidget::clear()
{
    m_items.clear();
    m_nodes.clear();
    m_parentTotal = 0;
    m_selectedPath.clear();
    m_hoverIndex = -1;
    m_selectedIndex = -1;
    m_layoutDirty = true;
    update();
}

void TreemapWidget::setSelectedPath(const std::wstring& path)
{
    m_selectedPath = path;
    m_selectedIndex = -1;
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        if (!m_nodes[i].item.isOther && m_nodes[i].item.path == path) {
            m_selectedIndex = static_cast<int>(i);
            break;
        }
    }
    // Path may refer to a pre-layout item not yet in nodes.
    if (m_selectedIndex < 0) {
        for (std::size_t i = 0; i < m_items.size(); ++i) {
            if (!m_items[i].isOther && m_items[i].path == path) {
                // Will map after relayout via key match in painted nodes.
                break;
            }
        }
        for (std::size_t i = 0; i < m_nodes.size(); ++i) {
            if (m_nodes[i].layout.key == path) {
                m_selectedIndex = static_cast<int>(i);
                break;
            }
        }
    }
    update();
}

void TreemapWidget::clearSelection()
{
    m_selectedPath.clear();
    m_selectedIndex = -1;
    update();
}

std::optional<TreemapDisplayItem> TreemapWidget::selectedItem() const
{
    if (m_selectedIndex < 0 ||
        m_selectedIndex >= static_cast<int>(m_nodes.size())) {
        return std::nullopt;
    }
    return m_nodes[static_cast<std::size_t>(m_selectedIndex)].item;
}

std::optional<TreemapDisplayItem> TreemapWidget::itemAt(const QPoint& pos) const
{
    const int i = hitIndex(pos);
    if (i < 0) {
        return std::nullopt;
    }
    return m_nodes[static_cast<std::size_t>(i)].item;
}

void TreemapWidget::scheduleRelayout()
{
    m_layoutDirty = true;
}

void TreemapWidget::relayout()
{
    m_nodes.clear();
    m_laidOutSize = size();
    m_layoutDirty = false;

    if (m_items.empty() || width() < 8 || height() < 8) {
        return;
    }

    std::vector<TreemapWeightItem> weights;
    weights.reserve(m_items.size());
    // Map from weight key → display item index (for non-Other).
    for (const auto& it : m_items) {
        if (it.sizeBytes == 0 && !it.isOther) {
            continue;
        }
        TreemapWeightItem w;
        if (it.isOther) {
            w.key = treemapOtherKey();
            w.tieBreak = std::to_wstring(it.otherItemCount);
        } else {
            w.key = it.path;
            w.tieBreak = it.path;
        }
        w.weight = static_cast<double>(it.sizeBytes);
        if (w.weight <= 0.0) {
            continue;
        }
        weights.push_back(std::move(w));
    }

    std::uint64_t otherCount = 0;
    auto prepared = prepareTreemapWeights(std::move(weights),
                                          kDefaultTreemapMaxVisible, &otherCount);

    // If prepare created Other and we did not have an Other display item, synthesize.
    TreemapDisplayItem otherDisplay;
    otherDisplay.isOther = true;
    otherDisplay.name = L"Other";
    otherDisplay.otherItemCount = otherCount;
    for (const auto& p : prepared) {
        if (isTreemapOtherKey(p.key)) {
            otherDisplay.sizeBytes = static_cast<ByteSize>(p.weight);
            otherDisplay.otherItemCount = otherCount;
            break;
        }
    }

    const TreemapBounds bounds{0, 0, static_cast<double>(width()),
                               static_cast<double>(height())};
    auto layout = layoutSquarified(prepared, bounds);

    m_nodes.reserve(layout.size());
    for (const auto& n : layout) {
        PaintedNode pn;
        pn.layout = n;
        pn.rect = QRectF(n.rect.x, n.rect.y, n.rect.w, n.rect.h);
        if (n.isOther) {
            pn.item = otherDisplay;
            pn.item.sizeBytes = static_cast<ByteSize>(n.weight);
            pn.item.otherItemCount = n.otherItemCount;
        } else {
            // Find matching source item by path.
            bool found = false;
            for (const auto& it : m_items) {
                if (!it.isOther && it.path == n.key) {
                    pn.item = it;
                    found = true;
                    break;
                }
            }
            if (!found) {
                pn.item.path = n.key;
                pn.item.name = n.key;
                pn.item.sizeBytes = static_cast<ByteSize>(n.weight);
            }
        }
        pn.fill = colorFor(pn.item);
        m_nodes.push_back(std::move(pn));
    }

    // Refresh selection index against painted nodes.
    m_selectedIndex = -1;
    if (!m_selectedPath.empty()) {
        for (std::size_t i = 0; i < m_nodes.size(); ++i) {
            if (!m_nodes[i].item.isOther &&
                m_nodes[i].item.path == m_selectedPath) {
                m_selectedIndex = static_cast<int>(i);
                break;
            }
        }
    }
}

QColor TreemapWidget::colorFor(const TreemapDisplayItem& item) const
{
    // Deterministic palette by category / kind — not safety coloring.
    QColor fill;
    if (item.isOther) {
        fill = QColor(0xB0, 0xB6, 0xBE);
    } else {
        const std::string& c = item.classification;
        if (c == "BuildArtifact") {
            fill = QColor(0x5B, 0x8F, 0xF9);
        } else if (c == "DependencyDirectory") {
            fill = QColor(0x7C, 0x6C, 0xF0);
        } else if (c == "PackageCache" || c == "IdeCache") {
            fill = QColor(0x3D, 0xB8, 0xA0);
        } else if (c == "DownloadedAiModel") {
            fill = QColor(0xE0, 0x8A, 0x3D);
        } else if (c == "LogData" || c == "TemporaryData") {
            fill = QColor(0x6A, 0xA8, 0x4F);
        } else if (c == "Archive") {
            fill = QColor(0xC4, 0x7A, 0xC0);
        } else if (c == "UserData") {
            fill = QColor(0x4A, 0x90, 0xC8);
        } else if (c == "SystemData" || c == "ApplicationData") {
            fill = QColor(0x8A, 0x8F, 0x98);
        } else if (item.kind == IndexEntryKind::Directory) {
            fill = QColor(0x6E, 0x9E, 0xCF);
        } else {
            std::size_t h = 0;
            for (wchar_t ch : item.name) {
                h = h * 131u + static_cast<std::size_t>(ch);
            }
            const int hue = static_cast<int>(h % 360);
            fill = QColor::fromHsv(hue, 70, 200);
        }
    }
    return adjustClassificationFill(fill, paletteIsDark(palette()));
}

int TreemapWidget::hitIndex(const QPoint& pos) const
{
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].rect.contains(pos)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TreemapWidget::updateTooltip(int index)
{
    if (index < 0 || index >= static_cast<int>(m_nodes.size())) {
        setToolTip(QString());
        return;
    }
    const auto& n = m_nodes[static_cast<std::size_t>(index)];
    QString tip;
    if (n.item.isOther) {
        tip = QStringLiteral("Other\n%1 smaller items\n%2 combined")
                  .arg(n.item.otherItemCount)
                  .arg(QString::fromStdString(
                      SizeFormatter::format(n.item.sizeBytes)));
        tip += QStringLiteral("\n(Click to inspect grouped items in table)");
    } else {
        tip = fromWide(n.item.name.empty() ? n.item.path : n.item.name);
        tip += QLatin1Char('\n');
        tip += QString::fromStdString(SizeFormatter::format(n.item.sizeBytes));
        if (m_parentTotal > 0) {
            const double pct = 100.0 * static_cast<double>(n.item.sizeBytes) /
                               static_cast<double>(m_parentTotal);
            tip += QStringLiteral("\n%1% of current directory")
                       .arg(pct, 0, 'f', 1);
        }
        if (!n.item.classification.empty()) {
            tip += QLatin1Char('\n');
            tip += QString::fromStdString(n.item.classification);
        }
        if (n.item.kind == IndexEntryKind::Directory) {
            tip += QStringLiteral("\n(Double-click to browse · Right-click for menu)");
        } else {
            tip += QStringLiteral("\n(Double-click to open · Right-click for menu)");
        }
    }
    setToolTip(tip);
}

void TreemapWidget::paintEvent(QPaintEvent*)
{
    if (m_layoutDirty || m_laidOutSize != size()) {
        relayout();
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    const QPalette pal = palette();
    const QColor surface = pal.color(QPalette::Base);
    p.fillRect(rect(), surface);

    if (m_nodes.empty()) {
        p.setPen(mutedTextColor(pal));
        p.drawText(rect().adjusted(12, 12, -12, -12),
                   Qt::AlignCenter | Qt::TextWordWrap, m_emptyMessage);
        return;
    }

    const QColor highlight = pal.color(QPalette::Highlight);
    QColor gutter = pal.color(QPalette::Window);
    gutter.setAlpha(200);

    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        const auto& n = m_nodes[i];
        QRectF r = n.rect.adjusted(0.5, 0.5, -0.5, -0.5);
        if (r.width() < 1.0 || r.height() < 1.0) {
            continue;
        }

        QColor fill = n.fill;
        if (static_cast<int>(i) == m_hoverIndex) {
            fill = mix(fill, surface, 0.18);
        }
        if (static_cast<int>(i) == m_selectedIndex) {
            fill = mix(fill, highlight, 0.22);
        }
        p.fillRect(r, fill);

        QPen border(gutter);
        border.setWidth(1);
        p.setPen(border);
        p.drawRect(r);

        if (r.width() >= 56.0 && r.height() >= 28.0) {
            QString name = n.item.isOther
                               ? QStringLiteral("Other")
                               : fromWide(n.item.name);
            if (name.isEmpty()) {
                name = fromWide(n.item.path);
            }
            QString size =
                QString::fromStdString(SizeFormatter::format(n.item.sizeBytes));
            if (m_parentTotal > 0 && r.width() >= 90.0) {
                const double pct = 100.0 * static_cast<double>(n.item.sizeBytes) /
                                   static_cast<double>(m_parentTotal);
                if (pct >= 1.0) {
                    size += QStringLiteral(" (%1%)").arg(pct, 0, 'f', 1);
                }
            }
            const QColor label = contrastingTextColor(fill);
            p.setPen(label);
            QFont f = font();
            f.setPointSizeF(std::max(8.0, f.pointSizeF()));
            p.setFont(f);
            QRectF textRect = r.adjusted(4, 3, -4, -3);
            const QString elided = p.fontMetrics().elidedText(
                name, Qt::ElideMiddle, static_cast<int>(textRect.width()));
            p.drawText(textRect, Qt::AlignTop | Qt::AlignLeft, elided);
            if (r.height() >= 40.0) {
                QFont fs = f;
                fs.setPointSizeF(std::max(7.5, f.pointSizeF() - 1.0));
                p.setFont(fs);
                QColor secondary = label;
                secondary.setAlpha(200);
                p.setPen(secondary);
                p.drawText(textRect, Qt::AlignBottom | Qt::AlignLeft, size);
            }
        }
    }

    // Paint crisp selection highlight border on top in a separate pass.
    if (m_selectedIndex >= 0 &&
        static_cast<std::size_t>(m_selectedIndex) < m_nodes.size()) {
        const auto& n = m_nodes[static_cast<std::size_t>(m_selectedIndex)];
        QRectF r = n.rect.adjusted(0.5, 0.5, -0.5, -0.5);
        if (r.width() >= 1.0 && r.height() >= 1.0) {
            QPen selBorder(highlight);
            selBorder.setWidth(2);
            p.setPen(selBorder);
            p.drawRect(r);
        }
    }
}

void TreemapWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event != nullptr && (event->type() == QEvent::PaletteChange ||
                             event->type() == QEvent::StyleChange)) {
        m_layoutDirty = true;
        update();
    }
}

void TreemapWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_layoutDirty = true;
    // Relayout on next paint; keep resize snappy.
    update();
}

void TreemapWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int idx = hitIndex(event->pos());
    if (idx != m_hoverIndex) {
        m_hoverIndex = idx;
        updateTooltip(idx);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void TreemapWidget::leaveEvent(QEvent* event)
{
    if (m_hoverIndex != -1) {
        m_hoverIndex = -1;
        setToolTip(QString());
        update();
    }
    QWidget::leaveEvent(event);
}

void TreemapWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int idx = hitIndex(event->pos());
        if (idx >= 0) {
            m_selectedIndex = idx;
            const auto& item = m_nodes[static_cast<std::size_t>(idx)].item;
            if (!item.isOther) {
                m_selectedPath = item.path;
            } else {
                m_selectedPath.clear();
            }
            update();
            emit itemClicked(item);
        } else {
            clearSelection();
            emit selectionCleared();
        }
    }
    QWidget::mousePressEvent(event);
}

void TreemapWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int idx = hitIndex(event->pos());
        if (idx >= 0) {
            emit itemDoubleClicked(m_nodes[static_cast<std::size_t>(idx)].item);
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TreemapWidget::contextMenuEvent(QContextMenuEvent* event)
{
    const int idx = hitIndex(event->pos());
    if (idx >= 0) {
        m_selectedIndex = idx;
        const auto& item = m_nodes[static_cast<std::size_t>(idx)].item;
        if (!item.isOther) {
            m_selectedPath = item.path;
        } else {
            m_selectedPath.clear();
        }
        update();
        emit itemClicked(item);
        emit itemContextMenuRequested(item, event->globalPos());
    } else {
        emit contextMenuRequested(event->globalPos());
    }
    event->accept();
}

void TreemapWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_selectedIndex >= 0 &&
            static_cast<std::size_t>(m_selectedIndex) < m_nodes.size()) {
            emit itemDoubleClicked(
                m_nodes[static_cast<std::size_t>(m_selectedIndex)].item);
            event->accept();
            return;
        }
    } else if (event->key() == Qt::Key_Backspace ||
               (event->key() == Qt::Key_Up &&
                (event->modifiers() & Qt::AltModifier))) {
        emit navigateUpRequested();
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Up) {
        if (!m_nodes.empty()) {
            if (m_selectedIndex > 0) {
                --m_selectedIndex;
            } else {
                m_selectedIndex = static_cast<int>(m_nodes.size() - 1);
            }
            const auto& item =
                m_nodes[static_cast<std::size_t>(m_selectedIndex)].item;
            m_selectedPath = item.isOther ? std::wstring{} : item.path;
            update();
            emit itemClicked(item);
            event->accept();
            return;
        }
    } else if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Down) {
        if (!m_nodes.empty()) {
            if (m_selectedIndex >= 0 &&
                static_cast<std::size_t>(m_selectedIndex + 1) < m_nodes.size()) {
                ++m_selectedIndex;
            } else {
                m_selectedIndex = 0;
            }
            const auto& item =
                m_nodes[static_cast<std::size_t>(m_selectedIndex)].item;
            m_selectedPath = item.isOther ? std::wstring{} : item.path;
            update();
            emit itemClicked(item);
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void TreemapWidget::focusInEvent(QFocusEvent* event)
{
    if (m_selectedIndex < 0 && !m_nodes.empty()) {
        m_selectedIndex = 0;
        const auto& item = m_nodes[0].item;
        m_selectedPath = item.isOther ? std::wstring{} : item.path;
        emit itemClicked(item);
    }
    update();
    QWidget::focusInEvent(event);
}

void TreemapWidget::focusOutEvent(QFocusEvent* event)
{
    update();
    QWidget::focusOutEvent(event);
}

}  // namespace spacelens
