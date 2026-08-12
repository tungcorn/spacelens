#include "core/treemap/TreemapLayout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace spacelens {
namespace {

constexpr const wchar_t* kOtherKey = L"__spacelens_treemap_other__";

[[nodiscard]] double positiveWeight(double w) noexcept
{
    return (w > 0.0 && std::isfinite(w)) ? w : 0.0;
}

/// Worst aspect ratio for a row laid along a side of length `side`.
/// Lower is better; perfect square → 1.
[[nodiscard]] double worstAspect(const std::vector<double>& row, double rowWeight,
                                 double side) noexcept
{
    if (row.empty() || side <= 0.0 || rowWeight <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double s2 = side * side;
    double maxW = 0.0;
    double minW = std::numeric_limits<double>::infinity();
    for (double w : row) {
        maxW = std::max(maxW, w);
        minW = std::min(minW, w);
    }
    // max( s^2 * rmax / row^2 , row^2 / (s^2 * rmin) )
    const double a = (s2 * maxW) / (rowWeight * rowWeight);
    const double b = (rowWeight * rowWeight) / (s2 * minW);
    return std::max(a, b);
}

struct LayoutState {
    TreemapBounds remaining{};
    std::vector<TreemapNode> out;
    double scale = 1.0;  // area units per weight unit
};

void fillNodeMeta(TreemapNode& node, std::size_t idx,
                  const TreemapWeightItem& it)
{
    node.sourceIndex = idx;
    node.key = it.key;
    node.weight = it.weight;
    node.isOther = (it.key == kOtherKey);
    if (node.isOther) {
        std::uint64_t count = 0;
        for (wchar_t ch : it.tieBreak) {
            if (ch < L'0' || ch > L'9') {
                count = 0;
                break;
            }
            count = count * 10ULL + static_cast<std::uint64_t>(ch - L'0');
        }
        node.otherItemCount = count;
    }
}

/// Place a row against the shortest side of the remaining rectangle.
/// Landscape (w >= h): vertical strip on the left; items stacked top→bottom.
/// Portrait  (h >  w): horizontal strip on the top; items stacked left→right.
void layoutRow(LayoutState& st, const std::vector<std::size_t>& indices,
               const std::vector<TreemapWeightItem>& items, double rowWeight)
{
    if (indices.empty() || rowWeight <= 0.0 || st.remaining.empty()) {
        return;
    }

    const bool landscape = st.remaining.w >= st.remaining.h;
    if (landscape) {
        // Vertical strip: thickness along X, items along Y (height = short side).
        const double side = st.remaining.h;
        if (side <= 0.0) {
            return;
        }
        double thickness = (rowWeight * st.scale) / side;
        // Last row / FP clamp: never exceed remaining width.
        if (thickness > st.remaining.w) {
            thickness = st.remaining.w;
        }
        // Identify last positive-weight item so residual height can absorb FP error.
        std::size_t lastPos = indices.size();
        for (std::size_t i = indices.size(); i > 0; --i) {
            if (positiveWeight(items[indices[i - 1]].weight) > 0.0) {
                lastPos = i - 1;
                break;
            }
        }
        double cursor = 0.0;
        for (std::size_t ri = 0; ri < indices.size(); ++ri) {
            const std::size_t idx = indices[ri];
            const auto& it = items[idx];
            const double w = positiveWeight(it.weight);
            if (w <= 0.0) {
                continue;
            }
            double len = (w * st.scale) / thickness;
            if (ri == lastPos) {
                len = std::max(0.0, side - cursor);
            }
            TreemapNode node;
            fillNodeMeta(node, idx, it);
            node.rect.x = st.remaining.x;
            node.rect.y = st.remaining.y + cursor;
            node.rect.w = thickness;
            node.rect.h = len;
            st.out.push_back(node);
            cursor += len;
        }
        st.remaining.x += thickness;
        st.remaining.w -= thickness;
        if (st.remaining.w < 1e-12) {
            st.remaining.w = 0.0;
        }
    } else {
        // Horizontal strip: thickness along Y, items along X (width = short side).
        const double side = st.remaining.w;
        if (side <= 0.0) {
            return;
        }
        double thickness = (rowWeight * st.scale) / side;
        if (thickness > st.remaining.h) {
            thickness = st.remaining.h;
        }
        std::size_t lastPos = indices.size();
        for (std::size_t i = indices.size(); i > 0; --i) {
            if (positiveWeight(items[indices[i - 1]].weight) > 0.0) {
                lastPos = i - 1;
                break;
            }
        }
        double cursor = 0.0;
        for (std::size_t ri = 0; ri < indices.size(); ++ri) {
            const std::size_t idx = indices[ri];
            const auto& it = items[idx];
            const double w = positiveWeight(it.weight);
            if (w <= 0.0) {
                continue;
            }
            double len = (w * st.scale) / thickness;
            if (ri == lastPos) {
                len = std::max(0.0, side - cursor);
            }
            TreemapNode node;
            fillNodeMeta(node, idx, it);
            node.rect.x = st.remaining.x + cursor;
            node.rect.y = st.remaining.y;
            node.rect.w = len;
            node.rect.h = thickness;
            st.out.push_back(node);
            cursor += len;
        }
        st.remaining.y += thickness;
        st.remaining.h -= thickness;
        if (st.remaining.h < 1e-12) {
            st.remaining.h = 0.0;
        }
    }
}

void squarify(LayoutState& st, const std::vector<std::size_t>& order,
              const std::vector<TreemapWeightItem>& items)
{
    std::size_t start = 0;
    const std::size_t n = order.size();
    while (start < n && !st.remaining.empty()) {
        const double side = std::min(st.remaining.w, st.remaining.h);
        if (side <= 0.0) {
            break;
        }

        std::vector<std::size_t> row;
        std::vector<double> rowWeights;
        double rowWeight = 0.0;

        while (start < n) {
            const std::size_t idx = order[start];
            const double w = positiveWeight(items[idx].weight);
            if (w <= 0.0) {
                ++start;
                continue;
            }

            std::vector<double> trialWeights = rowWeights;
            trialWeights.push_back(w);
            const double trialRowWeight = rowWeight + w;
            const double currentWorst = worstAspect(rowWeights, rowWeight, side);
            const double trialWorst =
                worstAspect(trialWeights, trialRowWeight, side);

            if (row.empty() || trialWorst <= currentWorst) {
                row.push_back(idx);
                rowWeights.push_back(w);
                rowWeight = trialRowWeight;
                ++start;
            } else {
                break;
            }
        }

        if (row.empty()) {
            break;
        }
        // If this consumes the rest of the input, force the strip to fill the
        // remaining rectangle so extreme weight ratios do not leave a sliver.
        if (start >= n) {
            const bool landscape = st.remaining.w >= st.remaining.h;
            if (landscape) {
                // thickness along X should become remaining.w
                // layoutRow clamps thickness to remaining.w already; bump scale
                // for this row only by adjusting rowWeight target via thickness.
                // Easiest: set remaining so thickness formula yields full width:
                // thickness = rowWeight * scale / h  => set by temporarily
                // scaling — instead just expand after layout. See post-pass below.
            }
        }
        layoutRow(st, row, items, rowWeight);
    }

    // Absorb leftover empty sliver caused by FP into the last node (if any).
    if (!st.out.empty() && !st.remaining.empty()) {
        auto& last = st.out.back();
        if (st.remaining.w > 0.0 && st.remaining.w <= 1.0) {
            last.rect.w += st.remaining.w;
            st.remaining.w = 0.0;
        }
        if (st.remaining.h > 0.0 && st.remaining.h <= 1.0) {
            last.rect.h += st.remaining.h;
            st.remaining.h = 0.0;
        }
        // Larger leftover (extreme skew): expand last strip node to fill.
        if (st.remaining.w > 0.0 || st.remaining.h > 0.0) {
            const double x2 = st.remaining.x + st.remaining.w;
            const double y2 = st.remaining.y + st.remaining.h;
            // If last node shares an edge with remaining, grow into it.
            if (std::abs(last.rect.x + last.rect.w - st.remaining.x) < 1e-6 &&
                st.remaining.w > 0.0) {
                last.rect.w += st.remaining.w;
                st.remaining.w = 0.0;
            } else if (std::abs(last.rect.y + last.rect.h - st.remaining.y) < 1e-6 &&
                       st.remaining.h > 0.0) {
                last.rect.h += st.remaining.h;
                st.remaining.h = 0.0;
            }
            (void)x2;
            (void)y2;
        }
    }
}

[[nodiscard]] bool rectsOverlap(const TreemapRect& a, const TreemapRect& b,
                                double eps) noexcept
{
    // Treat shared edges as non-overlapping (touching is fine).
    return a.x + a.w > b.x + eps && b.x + b.w > a.x + eps &&
           a.y + a.h > b.y + eps && b.y + b.h > a.y + eps;
}

[[nodiscard]] bool rectInside(const TreemapRect& r, const TreemapBounds& b,
                              double eps) noexcept
{
    return r.x + eps >= b.x && r.y + eps >= b.y &&
           r.x + r.w <= b.x + b.w + eps && r.y + r.h <= b.y + b.h + eps &&
           r.w >= -eps && r.h >= -eps;
}

}  // namespace

std::vector<TreemapWeightItem> prepareTreemapWeights(
    std::vector<TreemapWeightItem> items, std::size_t maxVisible,
    std::uint64_t* outOtherCount)
{
    if (outOtherCount) {
        *outOtherCount = 0;
    }

    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const TreemapWeightItem& it) {
                                   return positiveWeight(it.weight) <= 0.0;
                               }),
                items.end());

    std::stable_sort(items.begin(), items.end(),
                     [](const TreemapWeightItem& a, const TreemapWeightItem& b) {
                         if (a.weight != b.weight) {
                             return a.weight > b.weight;
                         }
                         if (a.tieBreak != b.tieBreak) {
                             return a.tieBreak < b.tieBreak;
                         }
                         return a.key < b.key;
                     });

    if (maxVisible == 0) {
        maxVisible = 1;
    }

    if (items.size() <= maxVisible) {
        return items;
    }

    const std::size_t keep = maxVisible - 1;
    double otherWeight = 0.0;
    std::uint64_t otherCount = 0;
    for (std::size_t i = keep; i < items.size(); ++i) {
        otherWeight += positiveWeight(items[i].weight);
        ++otherCount;
    }
    items.resize(keep);
    if (otherWeight > 0.0 && otherCount > 0) {
        TreemapWeightItem other;
        other.key = kOtherKey;
        other.weight = otherWeight;
        other.tieBreak = std::to_wstring(otherCount);
        items.push_back(std::move(other));
        if (outOtherCount) {
            *outOtherCount = otherCount;
        }
    }
    return items;
}

std::vector<TreemapNode> layoutSquarified(
    const std::vector<TreemapWeightItem>& items, TreemapBounds bounds)
{
    std::vector<TreemapNode> empty;
    if (bounds.empty()) {
        return empty;
    }

    double totalWeight = 0.0;
    std::vector<std::size_t> order;
    order.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const double w = positiveWeight(items[i].weight);
        if (w <= 0.0) {
            continue;
        }
        totalWeight += w;
        order.push_back(i);
    }
    if (totalWeight <= 0.0 || order.empty()) {
        return empty;
    }

    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) {
                         const double wa = positiveWeight(items[a].weight);
                         const double wb = positiveWeight(items[b].weight);
                         if (wa != wb) {
                             return wa > wb;
                         }
                         if (items[a].tieBreak != items[b].tieBreak) {
                             return items[a].tieBreak < items[b].tieBreak;
                         }
                         return items[a].key < items[b].key;
                     });

    LayoutState st;
    st.remaining = bounds;
    st.scale = bounds.area() / totalWeight;
    st.out.reserve(order.size());
    squarify(st, order, items);
    return st.out;
}

TreemapGeometryReport validateTreemapGeometry(const std::vector<TreemapNode>& nodes,
                                              TreemapBounds bounds,
                                              double areaTolerance,
                                              double boundsEpsilon)
{
    TreemapGeometryReport report;
    report.boundsArea = bounds.area();
    if (bounds.empty()) {
        report.ok = nodes.empty();
        if (!report.ok) {
            report.error = "non-empty layout for empty bounds";
        }
        return report;
    }

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        report.coveredArea += n.rect.w * n.rect.h;
        report.totalArea += n.rect.w * n.rect.h;
        if (!rectInside(n.rect, bounds, boundsEpsilon)) {
            report.anyOutOfBounds = true;
            report.ok = false;
            report.error = "rectangle out of bounds";
        }
        for (std::size_t j = i + 1; j < nodes.size(); ++j) {
            if (rectsOverlap(n.rect, nodes[j].rect, boundsEpsilon)) {
                report.anyOverlap = true;
                report.ok = false;
                report.error = "rectangle overlap";
            }
        }
    }

    if (!nodes.empty() && report.boundsArea > 0.0) {
        const double rel =
            std::abs(report.coveredArea - report.boundsArea) / report.boundsArea;
        if (rel > std::max(areaTolerance, 0.05)) {
            report.ok = false;
            if (report.error.empty()) {
                std::ostringstream oss;
                oss << "area conservation failed rel=" << rel;
                report.error = oss.str();
            }
        }
    }
    return report;
}

}  // namespace spacelens
