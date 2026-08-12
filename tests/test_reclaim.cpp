#include "TestRunner.hpp"
#include "core/DirectoryTree.hpp"
#include "core/FileTime.hpp"
#include "core/ReclaimAnalysis.hpp"

using namespace spacelens;

namespace {

constexpr FileTimeTicks kNow = 133000000000000000ULL;

FileTimeTicks daysAgo(std::uint64_t days)
{
    return kNow - daysToTicks(days);
}

}  // namespace

SPACELENS_TEST(Reclaim_old_user_data_not_strong)
{
    Classification cls;
    cls.category = StorageCategory::UserData;
    cls.confidence = Confidence::Medium;
    cls.ruleId = "user-media-doc-ext";
    cls.reason = "video";

    const auto cand = analyzeItem(
        L"D:\\Videos\\old-video.mp4", ItemKind::File,
        18ULL * 1024ULL * 1024ULL * 1024ULL, daysAgo(800), cls,
        LocationSafety::Ordinary, kNow, 0);
    SPACELENS_REQUIRE(cand.strength != CandidateStrength::Strong);
    SPACELENS_REQUIRE(cand.strength == CandidateStrength::ReviewOnly);
    SPACELENS_REQUIRE(cand.reclaimability == Reclaimability::Unknown);
}

SPACELENS_TEST(Reclaim_old_build_can_be_strong)
{
    Classification cls;
    cls.category = StorageCategory::BuildArtifact;
    cls.confidence = Confidence::High;
    cls.ruleId = "cmake-build-dir";
    cls.reason = "CMake build";

    const auto cand = analyzeItem(
        L"D:\\Users\\Tung\\Projects\\App\\build", ItemKind::Directory,
        5ULL * 1024ULL * 1024ULL * 1024ULL, daysAgo(210), cls,
        LocationSafety::Ordinary, kNow, 0);
    SPACELENS_REQUIRE(cand.strength == CandidateStrength::Strong);
    SPACELENS_REQUIRE(cand.reclaimability == Reclaimability::LikelyRegenerable);
}

SPACELENS_TEST(Reclaim_recent_build_fails_older_filter)
{
    DirectoryTree tree;
    const DirIndex root = tree.createRoot(L"D:\\Users\\Tung\\proj");
    const DirIndex build = tree.addDirectory(root, L"build");
    tree.addFile(build, L"CMakeCache.txt", 100, daysAgo(5));
    tree.addFile(build, L"generated.bin", 200ULL << 20, daysAgo(5));
    // children names for classification via tree walk
    const DirIndex cmakeFiles = tree.addDirectory(build, L"CMakeFiles");
    tree.addFile(cmakeFiles, L"x", 10, daysAgo(5));
    tree.recomputeAggregates(kNow);

    ReclaimQuery q;
    q.minSize = 1;
    q.olderThanDays = 90;
    q.nowTicks = kNow;
    q.limit = 50;
    q.includeFiles = false;
    q.includeDirectories = true;
    const auto found = findReclaimCandidates(tree, q);
    for (const auto& c : found) {
        SPACELENS_REQUIRE(c.path.find(L"build") == std::wstring::npos ||
                          c.inactiveDays >= 90);
    }
    // Recent build should not appear.
    bool hasBuild = false;
    for (const auto& c : found) {
        if (c.path.find(L"\\build") != std::wstring::npos) {
            hasBuild = true;
        }
    }
    SPACELENS_REQUIRE(!hasBuild);
}

SPACELENS_TEST(Reclaim_protected_never_candidate)
{
    Classification cls;
    cls.category = StorageCategory::BuildArtifact;
    cls.confidence = Confidence::High;
    cls.ruleId = "cmake-build-dir";

    const auto cand = analyzeItem(
        L"C:\\Windows\\Temp\\build", ItemKind::Directory, 9ULL << 30,
        daysAgo(400), cls, LocationSafety::Protected, kNow, 0);
    SPACELENS_REQUIRE(cand.strength == CandidateStrength::None);
}

SPACELENS_TEST(Reclaim_last_access_alone_not_strong)
{
    Classification cls;
    cls.category = StorageCategory::BuildArtifact;
    cls.confidence = Confidence::High;
    cls.ruleId = "cmake-build-dir";

    // write unknown (0), access very old — must not be Strong
    const auto cand = analyzeItem(
        L"D:\\Users\\Tung\\proj\\build", ItemKind::Directory, 9ULL << 30,
        /*activityWriteTime=*/0, cls, LocationSafety::Ordinary, kNow,
        daysAgo(500));
    SPACELENS_REQUIRE(cand.strength != CandidateStrength::Strong);
}

SPACELENS_TEST(Activity_newest_descendant_write)
{
    DirectoryTree tree;
    const DirIndex root = tree.createRoot(L"D:\\t");
    const DirIndex sub = tree.addDirectory(root, L"sub");
    tree.addFile(root, L"a.txt", 1, daysAgo(10));
    tree.addFile(sub, L"b.txt", 1, daysAgo(3));
    tree.recomputeAggregates(kNow);
    SPACELENS_REQUIRE_EQ(tree.dir(root).newestDescendantWrite, daysAgo(3));
    SPACELENS_REQUIRE_EQ(tree.dir(sub).newestDescendantWrite, daysAgo(3));
    SPACELENS_REQUIRE(tree.dir(root).filesModifiedLast30Days >= 2);
}
