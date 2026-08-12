#include "TestRunner.hpp"

#include "core/DirectoryTree.hpp"
#include "core/ScanTypes.hpp"
#include "core/index/IndexBuilder.hpp"
#include "core/index/IndexCatalog.hpp"
#include "core/index/IndexOverview.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/treemap/TreemapLayout.hpp"

#include <string>
#include <vector>

using namespace spacelens;

namespace {

const std::wstring kRoot = L"C:\\SpaceLensIndexUnitTestRoot\\overview_v1";

ScanResult makeOverviewScan(const std::wstring& root)
{
    ScanResult result;
    result.state = ScanState::Completed;
    const DirIndex r = result.tree.createRoot(root);

    result.tree.addFile(r, L"big.dat", 400ULL * 1024 * 1024, 1'000'000'000'000ULL);
    result.tree.addFile(r, L"small.txt", 100, 1'000'000'000'000ULL);

    const DirIndex proj = result.tree.addDirectory(r, L"Projects");
    result.tree.addFile(proj, L"a.bin", 100ULL * 1024 * 1024, 500ULL);

    const DirIndex build = result.tree.addDirectory(r, L"build");
    result.tree.addFile(build, L"CMakeCache.txt", 100, 50ULL);
    result.tree.addFile(build, L"out.obj", 50ULL * 1024 * 1024, 50ULL);

    // Many tiny files for Other aggregation
    for (int i = 0; i < 30; ++i) {
        result.tree.addFile(r, L"tiny" + std::to_wstring(i) + L".tmp", 1024,
                            100ULL);
    }

    result.tree.recomputeAggregates(1'000'000'000'000ULL);
    result.progress.filesSeen = result.tree.fileCount();
    result.progress.directoriesSeen = result.tree.directoryCount();
    result.progress.bytesSeen = result.tree.dir(r).recursiveSize;
    return result;
}

void ensureIndex()
{
    auto scan = makeOverviewScan(kRoot);
    auto built = buildIndexFromScan(scan, kRoot, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);
}

}  // namespace

SPACELENS_TEST(Overview_hierarchy_children_partition)
{
    ensureIndex();
    auto h = queryHierarchyChildren(kRoot, kRoot, 10000);
    SPACELENS_REQUIRE(h.ok);
    SPACELENS_REQUIRE(!h.children.empty());

    // Direct children only — no nested Projects\a.bin at root level.
    for (const auto& c : h.children) {
        SPACELENS_REQUIRE(c.path.find(L"\\a.bin") == std::wstring::npos ||
                          c.path.find(L"Projects") == std::wstring::npos);
    }

    // Sum of child effective sizes should equal location logical (or be close).
    ByteSize sum = 0;
    for (const auto& c : h.children) {
        sum += c.size_bytes;
    }
    SPACELENS_REQUIRE(h.locationLogicalBytes > 0);
    SPACELENS_REQUIRE(sum == h.locationLogicalBytes ||
                      sum == h.overview.directChildrenLogicalBytes);
    SPACELENS_REQUIRE(h.overview.directChildrenLogicalBytes == sum);
}

SPACELENS_TEST(Overview_no_double_count_totals)
{
    ensureIndex();
    auto h = queryHierarchyChildren(kRoot, kRoot, 10000);
    SPACELENS_REQUIRE(h.ok);

    // locationLogicalBytes is root meta once — not sum of nested recursive sizes
    // of Projects + build + files double-counting.
    ByteSize recursiveDirs = 0;
    ByteSize filesOnly = 0;
    for (const auto& c : h.children) {
        if (c.kind == IndexEntryKind::Directory) {
            recursiveDirs += c.size_bytes;
        } else {
            filesOnly += c.size_bytes;
        }
    }
    // Non-overlapping partition: dirs recursive + direct files == location total.
    SPACELENS_REQUIRE(recursiveDirs + filesOnly ==
                      h.overview.directChildrenLogicalBytes);
    SPACELENS_REQUIRE(h.overview.locationLogicalBytes ==
                      h.overview.directChildrenLogicalBytes);

    // Intelligence metrics are counts, not byte sums of recursive dirs.
    SPACELENS_REQUIRE(h.overview.developerCandidateCount <= h.children.size());
    SPACELENS_REQUIRE(h.overview.strongReclaimCount <= h.children.size());
}

SPACELENS_TEST(Overview_drilldown_projects)
{
    ensureIndex();
    const std::wstring proj = kRoot + L"\\Projects";
    auto h = queryHierarchyChildren(kRoot, proj, 1000);
    SPACELENS_REQUIRE(h.ok);
    SPACELENS_REQUIRE(h.locationPath == proj);
    SPACELENS_REQUIRE(!h.overview.isRoot);
    bool found = false;
    for (const auto& c : h.children) {
        if (c.name == L"a.bin" ||
            c.path.size() >= 5 && c.path.substr(c.path.size() - 5) == L"a.bin") {
            found = true;
            SPACELENS_REQUIRE(c.size_bytes == 100ULL * 1024 * 1024);
        }
    }
    SPACELENS_REQUIRE(found);
}

SPACELENS_TEST(Overview_treemap_weights_and_other)
{
    ensureIndex();
    auto h = queryHierarchyChildren(kRoot, kRoot, 10000);
    SPACELENS_REQUIRE(h.ok);
    auto weights = hierarchyToTreemapWeights(h.children);
    SPACELENS_REQUIRE(!weights.empty());
    std::uint64_t otherCount = 0;
    auto prepared = prepareTreemapWeights(weights, 8, &otherCount);
    SPACELENS_REQUIRE(prepared.size() <= 8);
    if (weights.size() > 8) {
        SPACELENS_REQUIRE(otherCount > 0);
        bool hasOther = false;
        for (const auto& w : prepared) {
            if (isTreemapOtherKey(w.key)) {
                hasOther = true;
            }
        }
        SPACELENS_REQUIRE(hasOther);
    }
    auto nodes = layoutSquarified(prepared, TreemapBounds{0, 0, 400, 300});
    SPACELENS_REQUIRE(!nodes.empty());
    auto report = validateTreemapGeometry(nodes, TreemapBounds{0, 0, 400, 300}, 0.05);
    SPACELENS_REQUIRE(report.ok);

    // Other is never a real IndexHit path.
    for (const auto& n : nodes) {
        if (n.isOther) {
            SPACELENS_REQUIRE(isTreemapOtherKey(n.key));
            for (const auto& c : h.children) {
                SPACELENS_REQUIRE(c.path != n.key);
            }
        }
    }
}

SPACELENS_TEST(Overview_build_pure_without_sql)
{
    IndexRootSummary root;
    root.rootPath = L"D:\\data";
    root.exists = true;
    root.logicalBytes = 1000;
    root.ageMs = 42;
    root.freshnessLabel = "Fresh snapshot";

    std::vector<IndexHit> children;
    IndexHit a;
    a.path = L"D:\\data\\A";
    a.name = L"A";
    a.kind = IndexEntryKind::Directory;
    a.size_bytes = 600;
    a.classification = "BuildArtifact";
    a.candidate_strength = "Strong";
    children.push_back(a);

    IndexHit b;
    b.path = L"D:\\data\\b.bin";
    b.name = L"b.bin";
    b.kind = IndexEntryKind::File;
    b.size_bytes = 400;
    b.classification = "UserData";
    b.candidate_strength = "ReviewOnly";
    children.push_back(b);

    auto o = buildStorageOverview(root, L"D:\\data", 1000, children, 0);
    SPACELENS_REQUIRE(o.isRoot);
    SPACELENS_REQUIRE(o.locationLogicalBytes == 1000);
    SPACELENS_REQUIRE(o.directDirCount == 1);
    SPACELENS_REQUIRE(o.directFileCount == 1);
    SPACELENS_REQUIRE(o.directChildrenLogicalBytes == 1000);
    SPACELENS_REQUIRE(o.hasLargestChild);
    SPACELENS_REQUIRE(o.largestChildName == L"A");
    SPACELENS_REQUIRE(o.strongReclaimCount == 1);
    SPACELENS_REQUIRE(o.developerCandidateCount == 1);
}

SPACELENS_TEST(Overview_other_key_not_cleanup_item)
{
    SPACELENS_REQUIRE(isTreemapOtherKey(treemapOtherKey()));
    SPACELENS_REQUIRE(!isTreemapOtherKey(L"C:\\foo"));
}
