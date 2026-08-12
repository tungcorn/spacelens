#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spacelens {

/// Axis-aligned rectangle in layout space (widget coordinates after mapping).
struct TreemapRect {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

struct TreemapBounds {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    [[nodiscard]] double area() const noexcept { return w * h; }
    [[nodiscard]] bool empty() const noexcept
    {
        return w <= 0.0 || h <= 0.0;
    }
};

/// One input weight for layout. Zero/negative weights are ignored by helpers.
struct TreemapWeightItem {
    /// Stable identity for selection sync (absolute path, or Other sentinel).
    std::wstring key;
    double weight = 0.0;
    /// Optional stable tie-break (path). Compared lexicographically when weights equal.
    std::wstring tieBreak;
};

/// One laid-out node. `sourceIndex` indexes the input vector passed to layout
/// (after aggregation filtering). `isOther` marks a visualization-only bucket.
struct TreemapNode {
    TreemapRect rect{};
    std::size_t sourceIndex = 0;
    std::wstring key;
    double weight = 0.0;
    bool isOther = false;
    /// When isOther: how many real items were folded in.
    std::uint64_t otherItemCount = 0;
};

/// Default cap for visible rectangles before "Other" aggregation.
inline constexpr std::size_t kDefaultTreemapMaxVisible = 48;

/// Prepare layout inputs: drop non-positive weights, sort size DESC then
/// tieBreak ASC (deterministic), optionally fold the tail into a single Other
/// item. Returns items ready for `layoutSquarified` (already sorted).
///
/// Other is visualization-only: it is never a filesystem path or cleanup target.
[[nodiscard]] std::vector<TreemapWeightItem> prepareTreemapWeights(
    std::vector<TreemapWeightItem> items,
    std::size_t maxVisible = kDefaultTreemapMaxVisible,
    std::uint64_t* outOtherCount = nullptr);

/// Squarified treemap layout (Bruls, Huizing, van Wijk).
///
/// Preconditions:
/// - `items` should be non-negative weights; zeros are skipped
/// - preferred: already sorted by weight DESC (prepareTreemapWeights does this)
///
/// Postconditions (when bounds non-empty and total positive weight > 0):
/// - no overlapping interiors
/// - every rectangle contained in bounds (within numeric epsilon)
/// - areas proportional to weights within relative tolerance of total
/// - deterministic for the same inputs
[[nodiscard]] std::vector<TreemapNode> layoutSquarified(
    const std::vector<TreemapWeightItem>& items, TreemapBounds bounds);

/// Geometry invariants for tests / debug asserts.
struct TreemapGeometryReport {
    bool ok = true;
    std::string error;
    double totalArea = 0.0;
    double boundsArea = 0.0;
    double coveredArea = 0.0;
    bool anyOverlap = false;
    bool anyOutOfBounds = false;
};

[[nodiscard]] TreemapGeometryReport validateTreemapGeometry(
    const std::vector<TreemapNode>& nodes, TreemapBounds bounds,
    double areaTolerance = 0.02, double boundsEpsilon = 1e-6);

}  // namespace spacelens
