#include "ui/TreemapWidget.hpp"

#include "core/SizeFormatter.hpp"
#include "core/index/IndexOverview.hpp"

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
    setMinimumHeight(140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0xF4, 0xF5, 0xF7));
    setPalette(pal);
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
    if (item.isOther) {
        return QColor(0xB0, 0xB6, 0xBE);
    }
    const std::string& c = item.classification;
    if (c == "BuildArtifact") {
        return QColor(0x5B, 0x8F, 0xF9);
    }
    if (c == "DependencyDirectory") {
        return QColor(0x7C, 0x6C, 0xF0);
    }
    if (c == "PackageCache" || c == "IdeCache") {
        return QColor(0x3D, 0xB8, 0xA0);
    }
    if (c == "DownloadedAiModel") {
        return QColor(0xE0, 0x8A, 0x3D);
    }
    if (c == "LogData" || c == "TemporaryData") {
        return QColor(0x6A, 0xA8, 0x4F);
    }
    if (c == "Archive") {
        return QColor(0xC4, 0x7A, 0xC0);
    }
    if (c == "UserData") {
        return QColor(0x4A, 0x90, 0xC8);
    }
    if (c == "SystemData" || c == "ApplicationData") {
        return QColor(0x8A, 0x8F, 0x98);
    }
    if (item.kind == IndexEntryKind::Directory) {
        return QColor(0x6E, 0x9E, 0xCF);
    }
    // File by extension family (hash of name).
    std::size_t h = 0;
    for (wchar_t ch : item.name) {
        h = h * 131u + static_cast<std::size_t>(ch);
    }
    const int hue = static_cast<int>(h % 360);
    return QColor::fromHsv(hue, 70, 200);
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
    p.fillRect(rect(), palette().color(QPalette::Window));

    if (m_nodes.empty()) {
        p.setPen(QColor(0x66, 0x66, 0x66));
        p.drawText(rect().adjusted(12, 12, -12, -12),
                   Qt::AlignCenter | Qt::TextWordWrap, m_emptyMessage);
        return;
    }

    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        const auto& n = m_nodes[i];
        QRectF r = n.rect.adjusted(0.5, 0.5, -0.5, -0.5);
        if (r.width() < 1.0 || r.height() < 1.0) {
            continue;
        }

        QColor fill = n.fill;
        if (static_cast<int>(i) == m_hoverIndex) {
            fill = mix(fill, QColor(Qt::white), 0.18);
        }
        if (static_cast<int>(i) == m_selectedIndex) {
            fill = mix(fill, QColor(0x1A, 0x56, 0xDB), 0.22);
        }
        p.fillRect(r, fill);

        QPen border(static_cast<int>(i) == m_selectedIndex
                        ? QColor(0x1A, 0x56, 0xDB)
                        : QColor(0xFF, 0xFF, 0xFF, 180));
        border.setWidth(static_cast<int>(i) == m_selectedIndex ? 2 : 1);
        p.setPen(border);
        p.drawRect(r);

        // Labels only when large enough.
        if (r.width() >= 56.0 && r.height() >= 28.0) {
            QString name = n.item.isOther
                               ? QStringLiteral("Other")
                               : fromWide(n.item.name);
            if (name.isEmpty()) {
                name = fromWide(n.item.path);
            }
            const QString size =
                QString::fromStdString(SizeFormatter::format(n.item.sizeBytes));
            p.setPen(QColor(0x1A, 0x1A, 0x1A));
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
                p.setPen(QColor(0x33, 0x33, 0x33));
                p.drawText(textRect, Qt::AlignBottom | Qt::AlignLeft, size);
            }
        }
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

}  // namespace spacelens
