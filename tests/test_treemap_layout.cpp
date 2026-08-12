#include "TestRunner.hpp"

#include "core/treemap/TreemapLayout.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace spacelens;

namespace {

std::vector<TreemapWeightItem> weights(std::initializer_list<double> ws)
{
    std::vector<TreemapWeightItem> items;
    int i = 0;
    for (double w : ws) {
        TreemapWeightItem it;
        const std::string ascii = "k" + std::to_string(i++);
        it.key.assign(ascii.begin(), ascii.end());
        it.weight = w;
        it.tieBreak = it.key;
        items.push_back(it);
    }
    return items;
}

}  // namespace

SPACELENS_TEST(Treemap_one_item_fills_bounds)
{
    auto items = weights({100.0});
    TreemapBounds b{0, 0, 200, 100};
    auto nodes = layoutSquarified(items, b);
    SPACELENS_REQUIRE(nodes.size() == 1);
    SPACELENS_REQUIRE(std::abs(nodes[0].rect.w - 200.0) < 1e-6);
    SPACELENS_REQUIRE(std::abs(nodes[0].rect.h - 100.0) < 1e-6);
    auto report = validateTreemapGeometry(nodes, b);
    SPACELENS_REQUIRE(report.ok);
    SPACELENS_REQUIRE(!report.anyOverlap);
    SPACELENS_REQUIRE(!report.anyOutOfBounds);
}

SPACELENS_TEST(Treemap_proportional_area_two_items)
{
    auto items = weights({75.0, 25.0});
    TreemapBounds b{0, 0, 400, 100};
    auto nodes = layoutSquarified(items, b);
    SPACELENS_REQUIRE(nodes.size() == 2);
    const double total = b.area();
    double a0 = nodes[0].rect.w * nodes[0].rect.h;
    double a1 = nodes[1].rect.w * nodes[1].rect.h;
    // 75/25 split → 0.75 / 0.25 of area
    SPACELENS_REQUIRE(std::abs(a0 / total - 0.75) < 0.02);
    SPACELENS_REQUIRE(std::abs(a1 / total - 0.25) < 0.02);
    auto report = validateTreemapGeometry(nodes, b);
    SPACELENS_REQUIRE(report.ok);
}

SPACELENS_TEST(Treemap_no_overlap_and_within_bounds)
{
    auto items = weights({40, 30, 20, 10, 5, 5});
    TreemapBounds b{10, 20, 320, 240};
    auto nodes = layoutSquarified(items, b);
    SPACELENS_REQUIRE(nodes.size() == 6);
    auto report = validateTreemapGeometry(nodes, b, 0.05);
    SPACELENS_REQUIRE(report.ok);
    SPACELENS_REQUIRE(!report.anyOverlap);
    SPACELENS_REQUIRE(!report.anyOutOfBounds);
}

SPACELENS_TEST(Treemap_zero_size_ignored)
{
    auto items = weights({50, 0, 50, -1});
    TreemapBounds b{0, 0, 100, 100};
    auto prepared = prepareTreemapWeights(items, 48);
    SPACELENS_REQUIRE(prepared.size() == 2);
    auto nodes = layoutSquarified(prepared, b);
    SPACELENS_REQUIRE(nodes.size() == 2);
}

SPACELENS_TEST(Treemap_deterministic_output)
{
    auto items = weights({10, 20, 30, 40});
    // Shuffle order — layout must still be deterministic after internal sort.
    std::swap(items[0], items[3]);
    TreemapBounds b{0, 0, 200, 200};
    auto a = layoutSquarified(items, b);
    auto bnodes = layoutSquarified(items, b);
    SPACELENS_REQUIRE(a.size() == bnodes.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        SPACELENS_REQUIRE(a[i].key == bnodes[i].key);
        SPACELENS_REQUIRE(std::abs(a[i].rect.x - bnodes[i].rect.x) < 1e-9);
        SPACELENS_REQUIRE(std::abs(a[i].rect.y - bnodes[i].rect.y) < 1e-9);
        SPACELENS_REQUIRE(std::abs(a[i].rect.w - bnodes[i].rect.w) < 1e-9);
        SPACELENS_REQUIRE(std::abs(a[i].rect.h - bnodes[i].rect.h) < 1e-9);
    }
}

SPACELENS_TEST(Treemap_stable_tie_break)
{
    std::vector<TreemapWeightItem> items;
    for (const wchar_t* k : {L"b", L"a", L"c"}) {
        TreemapWeightItem it;
        it.key = k;
        it.weight = 10.0;
        it.tieBreak = k;
        items.push_back(it);
    }
    auto prepared = prepareTreemapWeights(items, 10);
    SPACELENS_REQUIRE(prepared.size() == 3);
    // Equal weight → tieBreak ASC: a, b, c
    SPACELENS_REQUIRE(prepared[0].key == L"a");
    SPACELENS_REQUIRE(prepared[1].key == L"b");
    SPACELENS_REQUIRE(prepared[2].key == L"c");
}

SPACELENS_TEST(Treemap_narrow_and_wide_viewport)
{
    auto items = weights({8, 7, 6, 5, 4, 3, 2, 1});
    for (TreemapBounds b : {TreemapBounds{0, 0, 50, 400},
                            TreemapBounds{0, 0, 400, 50},
                            TreemapBounds{0, 0, 300, 300}}) {
        auto nodes = layoutSquarified(items, b);
        SPACELENS_REQUIRE(nodes.size() == 8);
        auto report = validateTreemapGeometry(nodes, b, 0.05);
        SPACELENS_REQUIRE(report.ok);
    }
}

SPACELENS_TEST(Treemap_skewed_weights)
{
    // Large but finite dynamic range (1e6:1) — still stresses strip placement.
    auto items = weights({1e6, 1.0, 1.0, 1.0});
    TreemapBounds b{0, 0, 1000, 1000};
    auto nodes = layoutSquarified(items, b);
    SPACELENS_REQUIRE(nodes.size() == 4);
    auto report = validateTreemapGeometry(nodes, b, 0.05, 1e-4);
    SPACELENS_REQUIRE(report.ok);
    // Largest node should dominate area.
    double maxArea = 0;
    for (const auto& n : nodes) {
        maxArea = std::max(maxArea, n.rect.w * n.rect.h);
    }
    SPACELENS_REQUIRE(maxArea / b.area() > 0.99);
}

SPACELENS_TEST(Treemap_other_aggregation)
{
    std::vector<TreemapWeightItem> items;
    for (int i = 0; i < 20; ++i) {
        TreemapWeightItem it;
        const std::string ascii = "item" + std::to_string(i);
        it.key.assign(ascii.begin(), ascii.end());
        it.weight = static_cast<double>(100 - i);
        it.tieBreak = it.key;
        items.push_back(it);
    }
    std::uint64_t otherCount = 0;
    auto prepared = prepareTreemapWeights(items, 5, &otherCount);
    // 4 kept + 1 Other
    SPACELENS_REQUIRE(prepared.size() == 5);
    SPACELENS_REQUIRE(otherCount == 16);
    bool foundOther = false;
    for (const auto& it : prepared) {
        if (it.key == L"__spacelens_treemap_other__") {
            foundOther = true;
            SPACELENS_REQUIRE(it.weight > 0);
        }
    }
    SPACELENS_REQUIRE(foundOther);

    TreemapBounds b{0, 0, 200, 200};
    auto nodes = layoutSquarified(prepared, b);
    SPACELENS_REQUIRE(nodes.size() == 5);
    bool otherNode = false;
    for (const auto& n : nodes) {
        if (n.isOther) {
            otherNode = true;
            SPACELENS_REQUIRE(n.otherItemCount == 16);
            SPACELENS_REQUIRE(n.key == L"__spacelens_treemap_other__");
        }
    }
    SPACELENS_REQUIRE(otherNode);
    auto report = validateTreemapGeometry(nodes, b, 0.05);
    SPACELENS_REQUIRE(report.ok);
}

SPACELENS_TEST(Treemap_empty_bounds_and_empty_items)
{
    auto items = weights({10, 20});
    auto nodes = layoutSquarified(items, TreemapBounds{0, 0, 0, 100});
    SPACELENS_REQUIRE(nodes.empty());
    nodes = layoutSquarified({}, TreemapBounds{0, 0, 100, 100});
    SPACELENS_REQUIRE(nodes.empty());
    nodes = layoutSquarified(weights({0, 0}), TreemapBounds{0, 0, 100, 100});
    SPACELENS_REQUIRE(nodes.empty());
}
