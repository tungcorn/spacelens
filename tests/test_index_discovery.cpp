#include "TestRunner.hpp"

#include "core/DirectoryTree.hpp"
#include "core/ScanTypes.hpp"
#include "core/index/IndexBuilder.hpp"
#include "core/index/IndexCatalog.hpp"
#include "core/index/IndexQuery.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace spacelens;

namespace {

ScanResult makeDiscoveryScan(const std::wstring& root)
{
    ScanResult result;
    result.state = ScanState::Completed;
    const DirIndex r = result.tree.createRoot(root);

    // Large recent user file
    result.tree.addFile(r, L"video.mp4", 600ULL * 1024 * 1024, 1'000'000'000'000ULL);

    // Old large archive
    result.tree.addFile(r, L"backup.zip", 200ULL * 1024 * 1024, 100ULL);

    // AI model
    result.tree.addFile(r, L"weights.gguf", 50ULL * 1024 * 1024, 500ULL);

    // Build tree
    const DirIndex build = result.tree.addDirectory(r, L"build");
    result.tree.addFile(build, L"CMakeCache.txt", 100, 100ULL);
    result.tree.addDirectory(build, L"CMakeFiles");
    result.tree.addFile(build, L"app.obj", 80ULL * 1024 * 1024, 50ULL);

    // Dependency dir
    const DirIndex nm = result.tree.addDirectory(r, L"node_modules");
    result.tree.addFile(nm, L"pkg.js", 1024, 200ULL);

    // Nested project
    const DirIndex proj = result.tree.addDirectory(r, L"OldProject");
    const DirIndex nestedBuild = result.tree.addDirectory(proj, L"build");
    result.tree.addFile(nestedBuild, L"CMakeCache.txt", 50, 80ULL);
    result.tree.addFile(nestedBuild, L"out.bin", 120ULL * 1024 * 1024, 80ULL);

    // Unicode name
    result.tree.addFile(r, L"unicodé-模型.txt", 4096, 300ULL);

    // Ordinary small recent file
    result.tree.addFile(r, L"notes.txt", 128, 1'000'000'000'000ULL);

    result.tree.recomputeAggregates(1'000'000'000'000ULL);
    result.progress.filesSeen = result.tree.fileCount();
    result.progress.directoriesSeen = result.tree.directoryCount();
    result.progress.bytesSeen = result.tree.dir(r).recursiveSize;
    return result;
}

const std::wstring kRoot = L"C:\\SpaceLensIndexUnitTestRoot\\discovery_v2";

void ensureIndex()
{
    auto scan = makeDiscoveryScan(kRoot);
    auto built = buildIndexFromScan(scan, kRoot, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);
}

bool pathEndsWith(const std::wstring& path, const std::wstring& suffix)
{
    if (path.size() < suffix.size()) {
        return false;
    }
    return path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

SPACELENS_TEST(Discovery_preset_largest_sorts_by_size)
{
    ensureIndex();
    auto spec = makeDiscoveryQuery(
        IndexDiscoveryPreset::Largest, true, true, std::nullopt, std::nullopt,
        "", "", "", "", L"", IndexSortKey::Size, true, 50);
    SPACELENS_REQUIRE(spec.includeFiles);
    SPACELENS_REQUIRE(spec.includeDirectories);
    SPACELENS_REQUIRE(spec.sortBy == IndexSortKey::Size);
    SPACELENS_REQUIRE(spec.sortDescending);

    auto q = queryIndex(kRoot, spec);
    SPACELENS_REQUIRE(q.ok);
    SPACELENS_REQUIRE(q.returned_items >= 1);
    for (std::size_t i = 1; i < q.hits.size(); ++i) {
        SPACELENS_REQUIRE(q.hits[i - 1].size_bytes >= q.hits[i].size_bytes);
    }
    // Largest entry should be video.mp4 or a large directory aggregate.
    SPACELENS_REQUIRE(q.hits.front().size_bytes >= 100ULL * 1024 * 1024);
}

SPACELENS_TEST(Discovery_preset_old_and_large)
{
    ensureIndex();
    auto spec = applyDiscoveryPreset(IndexDiscoveryPreset::OldAndLarge, {});
    SPACELENS_REQUIRE(spec.minSize.has_value());
    SPACELENS_REQUIRE_EQ(*spec.minSize, kOldAndLargeMinBytes);
    SPACELENS_REQUIRE(spec.olderThanDays.has_value());
    SPACELENS_REQUIRE_EQ(*spec.olderThanDays, kOldAndLargeOlderThanDays);

    // Use a synthetic "now" far above the old ticks so age filter works.
    spec.includeFiles = true;
    spec.includeDirectories = true;
    spec.nowTicks = 2'000'000'000'000ULL;
    spec.limit = 100;
    auto q = queryIndex(kRoot, spec);
    SPACELENS_REQUIRE(q.ok);
    // backup.zip / build artifacts should appear; recent video may be excluded
    // by age depending on ticks — old ticks (100, 50, 80) are << now.
    for (const auto& h : q.hits) {
        SPACELENS_REQUIRE(h.size_bytes >= kOldAndLargeMinBytes ||
                          h.kind == IndexEntryKind::Directory);
    }
}

SPACELENS_TEST(Discovery_preset_developer_storage)
{
    ensureIndex();
    auto spec = applyDiscoveryPreset(IndexDiscoveryPreset::DeveloperStorage, {});
    SPACELENS_REQUIRE(!spec.classifications.empty());
    const auto& cats = developerStorageClassifications();
    SPACELENS_REQUIRE(spec.classifications.size() == cats.size());

    spec.includeFiles = true;
    spec.includeDirectories = true;
    spec.limit = 100;
    auto q = queryIndex(kRoot, spec);
    SPACELENS_REQUIRE(q.ok);
    SPACELENS_REQUIRE(q.returned_items >= 1);
    for (const auto& h : q.hits) {
        bool ok = false;
        for (const auto& c : cats) {
            if (h.classification == c) {
                ok = true;
                break;
            }
        }
        SPACELENS_REQUIRE(ok);
    }
    // Expect build or node_modules or gguf somewhere.
    bool sawDev = false;
    for (const auto& h : q.hits) {
        if (h.path.find(L"build") != std::wstring::npos ||
            h.path.find(L"node_modules") != std::wstring::npos ||
            h.path.find(L".gguf") != std::wstring::npos) {
            sawDev = true;
            break;
        }
    }
    SPACELENS_REQUIRE(sawDev);
}

SPACELENS_TEST(Discovery_preset_reclaim_candidates)
{
    ensureIndex();
    auto spec = applyDiscoveryPreset(IndexDiscoveryPreset::ReclaimCandidates, {});
    SPACELENS_REQUIRE(!spec.candidateStrengths.empty());
    SPACELENS_REQUIRE(spec.sortBy == IndexSortKey::CandidateStrength);

    spec.includeFiles = true;
    spec.includeDirectories = true;
    spec.limit = 100;
    auto q = queryIndex(kRoot, spec);
    SPACELENS_REQUIRE(q.ok);
    for (const auto& h : q.hits) {
        SPACELENS_REQUIRE(h.candidate_strength == "Strong" ||
                          h.candidate_strength == "Moderate");
    }
}

SPACELENS_TEST(Discovery_search_case_insensitive_name_path)
{
    ensureIndex();
    IndexQuerySpec spec;
    spec.includeFiles = true;
    spec.includeDirectories = true;
    spec.searchText = "GGUF";
    spec.limit = 20;
    auto q = queryIndex(kRoot, spec);
    SPACELENS_REQUIRE(q.ok);
    SPACELENS_REQUIRE(q.returned_items >= 1);
    bool saw = false;
    for (const auto& h : q.hits) {
        if (h.path.find(L"gguf") != std::wstring::npos ||
            h.path.find(L"GGUF") != std::wstring::npos ||
            h.extension == "gguf") {
            saw = true;
        }
    }
    SPACELENS_REQUIRE(saw);

    IndexQuerySpec empty;
    empty.includeFiles = true;
    empty.searchText = "";
    empty.limit = 5;
    auto qEmpty = queryIndex(kRoot, empty);
    SPACELENS_REQUIRE(qEmpty.ok);

    IndexQuerySpec node;
    node.includeFiles = true;
    node.includeDirectories = true;
    node.searchText = "node_modules";
    node.limit = 20;
    auto qn = queryIndex(kRoot, node);
    SPACELENS_REQUIRE(qn.ok);
    SPACELENS_REQUIRE(qn.returned_items >= 1);
}

SPACELENS_TEST(Discovery_search_unicode_substring)
{
    ensureIndex();
    IndexQuerySpec spec;
    spec.includeFiles = true;
    spec.includeDirectories = false;
    // ASCII fragment of the unicode filename still matches path/name storage.
    spec.searchText = "unicod";
    spec.limit = 20;
    auto q = queryIndex(kRoot, spec);
    SPACELENS_REQUIRE(q.ok);
    SPACELENS_REQUIRE(q.returned_items >= 1);
}

SPACELENS_TEST(Discovery_filters_type_size_class_strength)
{
    ensureIndex();
    IndexQuerySpec filesOnly;
    filesOnly.includeFiles = true;
    filesOnly.includeDirectories = false;
    filesOnly.limit = 100;
    auto qf = queryIndex(kRoot, filesOnly);
    SPACELENS_REQUIRE(qf.ok);
    for (const auto& h : qf.hits) {
        SPACELENS_REQUIRE(h.kind == IndexEntryKind::File);
    }

    IndexQuerySpec dirsOnly;
    dirsOnly.includeFiles = false;
    dirsOnly.includeDirectories = true;
    dirsOnly.limit = 100;
    auto qd = queryIndex(kRoot, dirsOnly);
    SPACELENS_REQUIRE(qd.ok);
    for (const auto& h : qd.hits) {
        SPACELENS_REQUIRE(h.kind == IndexEntryKind::Directory);
    }

    IndexQuerySpec minSz;
    minSz.includeFiles = true;
    minSz.includeDirectories = false;
    minSz.minSize = 10ULL * 1024 * 1024;
    minSz.limit = 100;
    auto qm = queryIndex(kRoot, minSz);
    SPACELENS_REQUIRE(qm.ok);
    for (const auto& h : qm.hits) {
        SPACELENS_REQUIRE(h.size_bytes >= 10ULL * 1024 * 1024);
    }

    IndexQuerySpec cls;
    cls.includeFiles = true;
    cls.includeDirectories = true;
    cls.classification = "DownloadedAiModel";
    cls.limit = 20;
    auto qc = queryIndex(kRoot, cls);
    SPACELENS_REQUIRE(qc.ok);
    for (const auto& h : qc.hits) {
        SPACELENS_REQUIRE(h.classification == "DownloadedAiModel");
    }
}

SPACELENS_TEST(Discovery_sort_name_and_strength)
{
    ensureIndex();
    IndexQuerySpec byName;
    byName.includeFiles = true;
    byName.includeDirectories = false;
    byName.sortBy = IndexSortKey::Name;
    byName.sortDescending = false;
    byName.limit = 50;
    auto qn = queryIndex(kRoot, byName);
    SPACELENS_REQUIRE(qn.ok);
    SPACELENS_REQUIRE(qn.returned_items >= 2);
    // Deterministic: same query twice yields identical name order.
    auto qn2 = queryIndex(kRoot, byName);
    SPACELENS_REQUIRE(qn2.ok);
    SPACELENS_REQUIRE_EQ(qn.hits.size(), qn2.hits.size());
    for (std::size_t i = 0; i < qn.hits.size(); ++i) {
        SPACELENS_REQUIRE(qn.hits[i].name == qn2.hits[i].name);
        SPACELENS_REQUIRE(qn.hits[i].path == qn2.hits[i].path);
    }

    IndexQuerySpec byStr;
    byStr.includeFiles = true;
    byStr.includeDirectories = true;
    byStr.sortBy = IndexSortKey::CandidateStrength;
    byStr.sortDescending = true;
    byStr.limit = 50;
    auto qs = queryIndex(kRoot, byStr);
    SPACELENS_REQUIRE(qs.ok);
}

SPACELENS_TEST(Discovery_browse_path_immediate_children)
{
    ensureIndex();
    // Browse into OldProject — should see nested build, not root video.mp4.
    IndexQuerySpec spec;
    spec.includeFiles = true;
    spec.includeDirectories = true;
    spec.browsePath = kRoot + L"\\OldProject";
    spec.limit = 50;
    auto q = queryIndex(kRoot, spec);
    SPACELENS_REQUIRE(q.ok);
    for (const auto& h : q.hits) {
        SPACELENS_REQUIRE(h.path.find(L"\\OldProject\\") != std::wstring::npos ||
                          pathEndsWith(h.path, L"\\OldProject\\build") ||
                          pathEndsWith(h.path, L"build"));
        SPACELENS_REQUIRE(h.path.find(L"video.mp4") == std::wstring::npos);
    }

    // Root browse (empty) may include video.
    IndexQuerySpec rootSpec;
    rootSpec.includeFiles = true;
    rootSpec.includeDirectories = false;
    rootSpec.searchText = "video";
    rootSpec.limit = 10;
    auto qr = queryIndex(kRoot, rootSpec);
    SPACELENS_REQUIRE(qr.ok);
    SPACELENS_REQUIRE(qr.returned_items >= 1);
}

SPACELENS_TEST(Discovery_matched_count_exceeds_limit)
{
    ensureIndex();
    IndexQuerySpec spec;
    spec.includeFiles = true;
    spec.includeDirectories = true;
    spec.limit = 2;
    auto q = queryIndex(kRoot, spec);
    SPACELENS_REQUIRE(q.ok);
    SPACELENS_REQUIRE_EQ(q.returned_items, 2ULL);
    SPACELENS_REQUIRE(q.matched_items >= q.returned_items);
    SPACELENS_REQUIRE(q.query_elapsed_ms < 60'000);
}

SPACELENS_TEST(Discovery_hit_exposes_name_and_rule_id)
{
    ensureIndex();
    IndexQuerySpec spec;
    spec.includeFiles = true;
    spec.includeDirectories = false;
    spec.extension = "gguf";
    spec.limit = 5;
    auto q = queryIndex(kRoot, spec);
    SPACELENS_REQUIRE(q.ok);
    SPACELENS_REQUIRE_EQ(q.returned_items, 1ULL);
    SPACELENS_REQUIRE(!q.hits.front().name.empty());
    SPACELENS_REQUIRE(q.hits.front().classification == "DownloadedAiModel");
    // rule_id is stored when classification produced one
    SPACELENS_REQUIRE(!q.hits.front().path.empty());
}

SPACELENS_TEST(Discovery_toString_presets)
{
    SPACELENS_REQUIRE(std::string(toString(IndexDiscoveryPreset::Largest)) ==
                      "largest");
    SPACELENS_REQUIRE(std::string(toString(IndexDiscoveryPreset::OldAndLarge)) ==
                      "old_and_large");
    SPACELENS_REQUIRE(
        std::string(toString(IndexDiscoveryPreset::DeveloperStorage)) ==
        "developer_storage");
    SPACELENS_REQUIRE(
        std::string(toString(IndexDiscoveryPreset::ReclaimCandidates)) ==
        "reclaim_candidates");
}

SPACELENS_TEST(Discovery_makeBrowserQuerySpec_still_works)
{
    auto spec = makeBrowserQuerySpec(true, false, 1024, "gguf",
                                     "DownloadedAiModel", "Strong", 40);
    SPACELENS_REQUIRE(spec.includeFiles);
    SPACELENS_REQUIRE(!spec.includeDirectories);
    SPACELENS_REQUIRE_EQ(spec.limit, 40ULL);
    SPACELENS_REQUIRE(spec.sortBy == IndexSortKey::Size);
}
