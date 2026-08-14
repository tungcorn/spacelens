#include "TestRunner.hpp"

#include "core/DirectoryTree.hpp"
#include "core/FileTime.hpp"
#include "core/ScanTypes.hpp"
#include "core/StorageIntelligence.hpp"

#include <algorithm>
#include <string>

using namespace spacelens;

namespace {

constexpr FileTimeTicks kNow = 133000000000000000ULL;

FileTimeTicks daysAgo(std::uint64_t days)
{
    return kNow - daysToTicks(days);
}

DirectoryTree makeFixtureTree()
{
    DirectoryTree tree;
    const DirIndex root = tree.createRoot(L"D:\\AgentFixture");
    const DirIndex projects = tree.addDirectory(root, L"Projects");
    const DirIndex app = tree.addDirectory(projects, L"app");
    const DirIndex node = tree.addDirectory(app, L"node_modules");
    tree.addFile(node, L"left-pad.js", 12ULL << 20, daysAgo(10));
    tree.addFile(node, L"lodash.js", 8ULL << 20, daysAgo(10));
    const DirIndex build = tree.addDirectory(app, L"build");
    const DirIndex cmake = tree.addDirectory(build, L"CMakeFiles");
    tree.addFile(cmake, L"generated.bin", 11ULL << 20, daysAgo(5));
    tree.addFile(build, L"CMakeCache.txt", 4000, daysAgo(5));
    const DirIndex cache = tree.addDirectory(app, L".cache");
    tree.addFile(cache, L"tmp.dat", 5ULL << 20, daysAgo(20));

    tree.addFile(root, L"old-backup.zip", 9ULL << 20, daysAgo(400));
    tree.addFile(root, L"recent-vm.iso", 40ULL << 20, daysAgo(2));
    tree.addFile(root, L"holiday.jpg", 3ULL << 20, daysAgo(30));

    const DirIndex windows = tree.addDirectory(root, L"Windows");
    const DirIndex temp = tree.addDirectory(windows, L"Temp");
    tree.addFile(temp, L"old.bin", 20ULL << 20, daysAgo(200));

    tree.recomputeAggregates(kNow);
    return tree;
}

ScanResult scanFromTree(DirectoryTree&& tree)
{
    ScanResult result;
    result.state = ScanState::Completed;
    result.progress.filesSeen = tree.fileCount();
    result.progress.directoriesSeen = tree.directoryCount();
    result.progress.bytesSeen = tree.empty() ? 0 : tree.dir(tree.root()).recursiveSize;
    result.tree = std::move(tree);
    return result;
}

bool hasGroup(const OpportunityReport& report, const std::string& id)
{
    return std::any_of(report.groups.begin(), report.groups.end(),
                       [&](const OpportunityGroup& g) { return g.id == id; });
}

bool hasPathContaining(const std::vector<OpportunityItem>& items,
                       const std::wstring& needle)
{
    return std::any_of(items.begin(), items.end(), [&](const OpportunityItem& i) {
        return i.path.find(needle) != std::wstring::npos;
    });
}

}  // namespace

SPACELENS_TEST(StorageIntel_overview_lists_largest_not_root)
{
    auto result = scanFromTree(makeFixtureTree());
    const auto report = buildLiveOverview(result, 5, kNow);
    SPACELENS_REQUIRE(report.ok);
    SPACELENS_REQUIRE(report.source == EvidenceSource::LiveScan);
    SPACELENS_REQUIRE(report.logicalBytes > 0);
    SPACELENS_REQUIRE(!report.largestDirectories.empty());
    SPACELENS_REQUIRE(!report.largestFiles.empty());
    for (const auto& dir : report.largestDirectories) {
        SPACELENS_REQUIRE(dir.path != report.root);
        SPACELENS_REQUIRE(dir.objectType == "directory");
        SPACELENS_REQUIRE(!dir.classification.empty());
        SPACELENS_REQUIRE(!dir.locationSafety.empty());
    }
    bool sawVm = false;
    for (const auto& file : report.largestFiles) {
        if (file.path.find(L"recent-vm.iso") != std::wstring::npos) {
            sawVm = true;
            SPACELENS_REQUIRE(file.classification == "Unknown" ||
                              file.reclaimability == "Unknown");
        }
    }
    SPACELENS_REQUIRE(sawVm);

    const auto tight = buildLiveOverview(result, 1, kNow);
    SPACELENS_REQUIRE_EQ(tight.largestDirectories.size(), 1ULL);
    SPACELENS_REQUIRE(tight.truncatedDirectories);
}

SPACELENS_TEST(StorageIntel_opportunities_rank_developer_and_skip_recent_unknown)
{
    auto tree = makeFixtureTree();
    OpportunityQuery q;
    q.minSize = 1;
    q.olderThanDays = 90;
    q.nowTicks = kNow;
    q.limit = 20;
    const auto report = buildLiveOpportunities(tree, q);
    SPACELENS_REQUIRE(report.ok);
    SPACELENS_REQUIRE(hasGroup(report, "developer_dependencies"));
    SPACELENS_REQUIRE(hasGroup(report, "generated_outputs"));
    SPACELENS_REQUIRE(hasPathContaining(report.opportunities, L"node_modules"));
    SPACELENS_REQUIRE(hasPathContaining(report.opportunities, L"build"));
    SPACELENS_REQUIRE(hasPathContaining(report.opportunities, L"old-backup.zip"));
    SPACELENS_REQUIRE(!hasPathContaining(report.opportunities, L"recent-vm.iso"));
    SPACELENS_REQUIRE(!hasPathContaining(report.opportunities, L"holiday.jpg"));

    for (const auto& item : report.opportunities) {
        SPACELENS_REQUIRE(item.opportunityRank > 0);
        SPACELENS_REQUIRE(!item.reasonCodes.empty());
        SPACELENS_REQUIRE(item.locationSafety != "Protected" ||
                          item.candidateStrength == "None");
        const std::string json = report.toJson();
        SPACELENS_REQUIRE(json.find("safe_to_delete") == std::string::npos);
    }
}

SPACELENS_TEST(StorageIntel_nested_build_not_double_counted)
{
    DirectoryTree tree;
    const DirIndex root = tree.createRoot(L"D:\\AgentFixture");
    const DirIndex build = tree.addDirectory(root, L"build");
    const DirIndex cmake = tree.addDirectory(build, L"CMakeFiles");
    tree.addFile(cmake, L"out.bin", 10ULL << 20, daysAgo(200));
    tree.addFile(build, L"CMakeCache.txt", 100, daysAgo(200));
    tree.recomputeAggregates(kNow);

    OpportunityQuery q;
    q.minSize = 1;
    q.olderThanDays = 90;
    q.nowTicks = kNow;
    q.limit = 20;
    const auto report = buildLiveOpportunities(tree, q);
    SPACELENS_REQUIRE(hasPathContaining(report.opportunities, L"build"));
    bool sawChild = false;
    bool childOverlapped = false;
    ByteSize unique = 0;
    for (const auto& item : report.opportunities) {
        if (!item.overlapped) {
            unique += item.logicalBytes;
        }
        if (item.path.find(L"CMakeFiles") != std::wstring::npos) {
            sawChild = true;
            childOverlapped = item.overlapped;
        }
    }
    SPACELENS_REQUIRE(sawChild);
    SPACELENS_REQUIRE(childOverlapped);
    SPACELENS_REQUIRE_EQ(report.uniqueReviewBytes, unique);
    SPACELENS_REQUIRE(report.uniqueReviewBytes <=
                      tree.dir(tree.root()).recursiveSize);
    // Parent covers child: unique bytes equal the build directory, not the sum.
    bool sawBuild = false;
    for (const auto& item : report.opportunities) {
        if (item.path.size() >= 5 &&
            item.path.compare(item.path.size() - 5, 5, L"build") == 0 &&
            !item.overlapped) {
            sawBuild = true;
            SPACELENS_REQUIRE_EQ(report.uniqueReviewBytes, item.logicalBytes);
        }
    }
    SPACELENS_REQUIRE(sawBuild);
}

SPACELENS_TEST(StorageIntel_protected_path_not_an_opportunity)
{
    DirectoryTree tree;
    const DirIndex root = tree.createRoot(L"C:\\Windows");
    const DirIndex temp = tree.addDirectory(root, L"Temp");
    const DirIndex build = tree.addDirectory(temp, L"build");
    tree.addFile(build, L"CMakeCache.txt", 100, daysAgo(400));
    tree.addFile(build, L"out.bin", 50ULL << 20, daysAgo(400));
    tree.recomputeAggregates(kNow);

    OpportunityQuery q;
    q.minSize = 1;
    q.olderThanDays = 90;
    q.nowTicks = kNow;
    q.limit = 20;
    const auto report = buildLiveOpportunities(tree, q);
    SPACELENS_REQUIRE(!hasPathContaining(report.opportunities, L"build"));
    SPACELENS_REQUIRE_EQ(report.uniqueReviewBytes, 0);
}

SPACELENS_TEST(StorageIntel_json_is_bounded_and_deterministic)
{
    auto tree = makeFixtureTree();
    OpportunityQuery q;
    q.minSize = 1;
    q.olderThanDays = 90;
    q.nowTicks = kNow;
    q.limit = 3;
    const auto a = buildLiveOpportunities(tree, q);
    const auto b = buildLiveOpportunities(tree, q);
    SPACELENS_REQUIRE_EQ(a.toJson(), b.toJson());
    SPACELENS_REQUIRE(a.returnedCount <= 3);
    SPACELENS_REQUIRE(a.truncated || a.opportunities.size() < 3);
    SPACELENS_REQUIRE(a.toJson().find("\"filesystem_mutation\":false") !=
                      std::string::npos);
    SPACELENS_REQUIRE(a.toJson().find("potential_reclaim_bytes") ==
                      std::string::npos);

    if (a.opportunities.size() >= 2) {
        SPACELENS_REQUIRE(a.opportunities[0].opportunityRank == 1);
        SPACELENS_REQUIRE(a.opportunities[1].opportunityRank == 2);
        const auto rank = [](const std::string& s) {
            if (s == "Strong") {
                return 3;
            }
            if (s == "Moderate") {
                return 2;
            }
            if (s == "ReviewOnly") {
                return 1;
            }
            return 0;
        };
        SPACELENS_REQUIRE(rank(a.opportunities[0].candidateStrength) >=
                          rank(a.opportunities[1].candidateStrength));
    }
}

SPACELENS_TEST(StorageIntel_reason_codes_are_stable)
{
    Classification cls;
    cls.category = StorageCategory::DependencyDirectory;
    cls.confidence = Confidence::High;
    cls.ruleId = "node-modules";
    const auto cand =
        analyzeItem(L"D:\\AgentFixture\\node_modules", ItemKind::Directory,
                    20ULL << 20, daysAgo(10), cls, LocationSafety::Ordinary, kNow,
                    0);
    const auto codes = reasonCodesFor(cand, false);
    bool hasDep = false;
    bool hasRegen = false;
    for (const auto& c : codes) {
        if (c == reason::kDeveloperDependency) {
            hasDep = true;
        }
        if (c == reason::kLikelyRegenerable) {
            hasRegen = true;
        }
        SPACELENS_REQUIRE(c.find(' ') == std::string::npos);
    }
    SPACELENS_REQUIRE(hasDep);
    SPACELENS_REQUIRE(hasRegen);

    const auto& regen = regenerableOpportunityClassifications();
    SPACELENS_REQUIRE(std::find(regen.begin(), regen.end(), "TemporaryData") !=
                      regen.end());
    SPACELENS_REQUIRE(std::find(regen.begin(), regen.end(),
                                "DependencyDirectory") != regen.end());
    SPACELENS_REQUIRE(std::find(regen.begin(), regen.end(),
                                "DownloadedAiModel") == regen.end());
}

SPACELENS_TEST(StorageIntel_v2_coverage_ranking_and_filters)
{
    DirectoryTree tree;
    const DirIndex root = tree.createRoot(L"D:\\V2Fixture");
    const DirIndex photos = tree.addDirectory(root, L"Photos");
    const DirIndex photosBuild = tree.addDirectory(photos, L"build");
    tree.addFile(photosBuild, L"IMG_001.jpg", 12ULL << 20, daysAgo(400));

    const DirIndex rust = tree.addDirectory(root, L"rust-app");
    tree.addFile(rust, L"Cargo.toml", 200, daysAgo(20));
    const DirIndex target = tree.addDirectory(rust, L"target");
    tree.addFile(target, L"app.exe", 30ULL << 20, daysAgo(8));

    const DirIndex dotnet = tree.addDirectory(root, L"dotnet-app");
    tree.addFile(dotnet, L"App.csproj", 300, daysAgo(20));
    const DirIndex bin = tree.addDirectory(dotnet, L"bin");
    tree.addFile(bin, L"App.dll", 16ULL << 20, daysAgo(6));

    const DirIndex py = tree.addDirectory(root, L"py-app");
    tree.addFile(py, L"pyproject.toml", 120, daysAgo(20));
    const DirIndex venv = tree.addDirectory(py, L".venv");
    tree.addFile(venv, L"pyvenv.cfg", 80, daysAgo(12));
    tree.addFile(venv, L"lib.bin", 22ULL << 20, daysAgo(12));

    tree.addFile(root, L"old-setup.msi", 14ULL << 20, daysAgo(400));
    tree.addFile(root, L"recent-vm.iso", 40ULL << 20, daysAgo(2));
    tree.recomputeAggregates(kNow);

    OpportunityQuery q;
    q.minSize = 1;
    q.olderThanDays = 90;
    q.nowTicks = kNow;
    q.limit = 20;
    const auto report = buildLiveOpportunities(tree, q);
    SPACELENS_REQUIRE(report.ok);
    SPACELENS_REQUIRE(report.rankingPolicy == kOpportunityRankPolicy);
    SPACELENS_REQUIRE(hasPathContaining(report.opportunities, L"target"));
    SPACELENS_REQUIRE(hasPathContaining(report.opportunities, L"\\bin"));
    SPACELENS_REQUIRE(hasPathContaining(report.opportunities, L".venv"));
    SPACELENS_REQUIRE(hasPathContaining(report.opportunities, L"old-setup.msi"));
    SPACELENS_REQUIRE(!hasPathContaining(report.opportunities, L"recent-vm.iso"));
    for (const auto& item : report.opportunities) {
        const bool underPhotosBuild =
            item.path.find(L"Photos") != std::wstring::npos &&
            item.path.find(L"build") != std::wstring::npos;
        if (underPhotosBuild) {
            SPACELENS_REQUIRE(item.classification != "BuildArtifact");
            SPACELENS_REQUIRE(item.objectType != "directory");
        }
    }

    bool sawRustEvidence = false;
    bool sawInstaller = false;
    for (const auto& item : report.opportunities) {
        if (item.path.find(L"target") != std::wstring::npos) {
            sawRustEvidence = item.ecosystem == "rust" && item.marker == "target";
            SPACELENS_REQUIRE(item.candidateStrength == "Moderate");
        }
        if (item.path.find(L"old-setup.msi") != std::wstring::npos) {
            sawInstaller = true;
            SPACELENS_REQUIRE(item.classification == "Archive");
            SPACELENS_REQUIRE(std::find(item.reasonCodes.begin(), item.reasonCodes.end(),
                                        reason::kOldLargeInstaller) !=
                              item.reasonCodes.end());
        }
    }
    SPACELENS_REQUIRE(sawRustEvidence);
    SPACELENS_REQUIRE(sawInstaller);

    const std::string json = report.toJson();
    SPACELENS_REQUIRE(json.find("\"ranking_policy\":\"opportunity_rank_v2\"") !=
                      std::string::npos);
    SPACELENS_REQUIRE(json.find("\"evidence\":") != std::string::npos);
    SPACELENS_REQUIRE(json.find("safe_to_delete") == std::string::npos);
    SPACELENS_REQUIRE(json.find("\"strongest_candidate_strength\"") !=
                      std::string::npos);

    OpportunityQuery filtered = q;
    filtered.categoryOnly = StorageCategory::BuildArtifact;
    const auto onlyBuild = buildLiveOpportunities(tree, filtered);
    SPACELENS_REQUIRE(hasPathContaining(onlyBuild.opportunities, L"target"));
    SPACELENS_REQUIRE(!hasPathContaining(onlyBuild.opportunities, L".venv"));
    SPACELENS_REQUIRE(!hasPathContaining(onlyBuild.opportunities, L"old-setup.msi"));

    OpportunityQuery none = q;
    none.matchNone = true;
    const auto empty = buildLiveOpportunities(tree, none);
    SPACELENS_REQUIRE(empty.opportunities.empty());
}

SPACELENS_TEST(StorageIntel_confidence_breaks_ranking_ties)
{
    DirectoryTree tree;
    const DirIndex root = tree.createRoot(L"D:\\RankFixture");
    const DirIndex high = tree.addDirectory(root, L"node_modules");
    tree.addFile(high, L"a.js", 20ULL << 20, daysAgo(10));
    const DirIndex med = tree.addDirectory(root, L".cache");
    tree.addFile(med, L"b.dat", 20ULL << 20, daysAgo(10));
    tree.recomputeAggregates(kNow);

    OpportunityQuery q;
    q.minSize = 1;
    q.olderThanDays = 90;
    q.nowTicks = kNow;
    q.limit = 10;
    const auto report = buildLiveOpportunities(tree, q);
    SPACELENS_REQUIRE(report.opportunities.size() >= 2);
    SPACELENS_REQUIRE(report.opportunities[0].path.find(L"node_modules") !=
                      std::wstring::npos);
    SPACELENS_REQUIRE(report.opportunities[0].confidence == "High");
}

SPACELENS_TEST(StorageIntel_overview_has_live_opportunity_summary)
{
    auto result = scanFromTree(makeFixtureTree());
    const auto report = buildLiveOverview(result, 5, kNow);
    SPACELENS_REQUIRE(!report.opportunitySummary.empty());
    SPACELENS_REQUIRE(hasGroup(
        [&] {
            OpportunityReport wrap;
            wrap.groups = report.opportunitySummary;
            return wrap;
        }(),
        "developer_dependencies"));
    const std::string json = report.toJson();
    SPACELENS_REQUIRE(json.find("\"opportunity_summary\"") != std::string::npos);

    StorageOverviewReport indexed;
    indexed.source = EvidenceSource::PersistentIndex;
    SPACELENS_REQUIRE(indexed.opportunitySummary.empty());
}
