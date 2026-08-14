#pragma once

#include "core/index/IndexQuery.hpp"
#include "core/treemap/TreemapLayout.hpp"

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

namespace spacelens {

/// Display model for one treemap cell (immutable snapshot for the GUI thread).
struct TreemapDisplayItem {
    std::wstring path;
    std::wstring name;
    ByteSize sizeBytes = 0;
    IndexEntryKind kind = IndexEntryKind::File;
    std::string classification;
    std::string candidateStrength;
    bool isOther = false;
    std::uint64_t otherItemCount = 0;
};

/// Native squarified treemap. Layout runs outside paintEvent; painting iterates
/// prepared rectangles only. No SQL, no filesystem access.
class TreemapWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TreemapWidget(QWidget* parent = nullptr);

    /// Replace the hierarchy children used for layout. Empty → empty state.
    void setItems(std::vector<TreemapDisplayItem> items, ByteSize parentTotalBytes);

    void clear();

    void setSelectedPath(const std::wstring& path);
    void clearSelection();

    [[nodiscard]] std::optional<TreemapDisplayItem> selectedItem() const;
    [[nodiscard]] std::optional<TreemapDisplayItem> itemAt(const QPoint& pos) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    void itemClicked(const TreemapDisplayItem& item);
    void itemDoubleClicked(const TreemapDisplayItem& item);
    void selectionCleared();

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct PaintedNode {
        TreemapNode layout{};
        TreemapDisplayItem item{};
        QRectF rect;
        QColor fill;
    };

    void relayout();
    void scheduleRelayout();
    [[nodiscard]] int hitIndex(const QPoint& pos) const;
    [[nodiscard]] QColor colorFor(const TreemapDisplayItem& item) const;
    void updateTooltip(int index);

    std::vector<TreemapDisplayItem> m_items;
    std::vector<PaintedNode> m_nodes;
    ByteSize m_parentTotal = 0;
    std::wstring m_selectedPath;
    int m_hoverIndex = -1;
    int m_selectedIndex = -1;
    QSize m_laidOutSize;
    bool m_layoutDirty = true;
    QString m_emptyMessage = QStringLiteral("No storage to visualize.");
};

}  // namespace spacelens
