#include "TestRunner.hpp"

#include "core/Maintenance.hpp"
#include "core/OrdinaryLocation.hpp"

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace spacelens;

namespace {

CleanupIdentity strongId(std::uint8_t seed, std::uint64_t volume = 7)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = seed;
    bytes[15] = static_cast<std::uint8_t>(seed + 1);
    return makeFileId128Identity(volume, bytes);
}

CleanupObjectEvidence fileEvidence(std::uint8_t seed,
                                   ByteSize size,
                                   FileTimeTicks writeTime = 10,
                                   std::uint32_t attributes = 32)
{
    CleanupObjectEvidence ev;
    ev.available = true;
    ev.identity = strongId(seed);
    ev.kind = ItemKind::File;
    ev.sizeScope = CleanupEvidenceScope::Direct;
    ev.logicalSize = size;
    ev.lastWriteTime = writeTime;
    ev.lastAccessTime = 4;
    ev.attributes = attributes;
    return ev;
}

std::wstring ordinaryPath(std::wstring_view name)
{
    return std::wstring(L"C:\\Users\\TestUser\\Projects\\") + std::wstring(name);
}

CleanupCandidate fileCandidate(std::wstring path,
                               std::uint8_t seed,
                               ByteSize size)
{
    CleanupCandidate out;
    out.path = std::move(path);
    out.kind = ItemKind::File;
    out.sizeAtSelection = size;
    out.lastWriteTime = 10;
    out.attributes = 32;
    out.objectEvidence = fileEvidence(seed, size);
    out.capturedSafety = LocationSafety::Ordinary;
    return out;
}

CleanupMetadataProbe presentProbe(CleanupObjectEvidence evidence,
                                  bool isReparse = false)
{
    CleanupMetadataProbe probe;
    probe.outcome = CleanupMetadataProbeOutcome::Present;
    probe.objectEvidence = std::move(evidence);
    probe.isReparse = isReparse;
    return probe;
}

class MapReader final : public ICleanupMetadataReader {
public:
    std::map<std::wstring, CleanupMetadataProbe> probes;

    CleanupMetadataProbe read(const std::wstring& path) override
    {
        const auto it = probes.find(path);
        if (it == probes.end()) {
            CleanupMetadataProbe missing;
            missing.outcome = CleanupMetadataProbeOutcome::Missing;
            missing.detail = "not in map";
            return missing;
        }
        return it->second;
    }
};

class ScriptedRecycle final : public IRecycleOperation {
public:
    std::vector<MaintenanceItemResult> script;
    std::size_t calls = 0;
    std::vector<std::wstring> paths;

    MaintenanceItemReceipt recycle(const MaintenancePlanItem& item) override
    {
        paths.push_back(item.path);
        MaintenanceItemReceipt receipt;
        receipt.reviewId = item.reviewId;
        receipt.path = item.path;
        receipt.expectedIdentity = item.expectedIdentity;
        if (calls < script.size()) {
            receipt.result = script[calls];
        } else {
            receipt.result = MaintenanceItemResult::Recycled;
        }
        if (receipt.result == MaintenanceItemResult::Recycled) {
            receipt.recycleParsingName = "recycle://item";
        }
        ++calls;
        return receipt;
    }
};

OrdinaryLocationDeclaration activeRoot(std::wstring path, std::uint64_t id = 1)
{
    OrdinaryLocationDeclaration row;
    row.id = id;
    row.configuredPath = path;
    row.normalizedPathKey = normalizeOrdinaryLocationPath(path);
    row.volume.available = true;
    row.volume.serial = 0xAABBCCDD;
    row.volume.guid = L"\\\\?\\Volume{test}\\";
    row.status = OrdinaryLocationStatus::Active;
    return row;
}

}  // namespace

SPACELENS_TEST(Maintenance_ordinary_file_with_strong_identity_is_eligible)
{
    const auto candidate = fileCandidate(ordinaryPath(L"out.bin"), 1, 4096);
    const auto item = evaluateMaintenanceEligibility(
        candidate, presentProbe(candidate.objectEvidence), LocationSafety::Ordinary);
    SPACELENS_REQUIRE(item.eligible);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::None);
    SPACELENS_REQUIRE_EQ(item.logicalSize, 4096ULL);
}

SPACELENS_TEST(Maintenance_directory_is_blocked)
{
    CleanupCandidate dir;
    dir.path = ordinaryPath(L"build");
    dir.kind = ItemKind::Directory;
    dir.objectEvidence = fileEvidence(2, 0);
    dir.objectEvidence.kind = ItemKind::Directory;
    const auto item = evaluateMaintenanceEligibility(
        dir, presentProbe(dir.objectEvidence), LocationSafety::Ordinary);
    SPACELENS_REQUIRE(!item.eligible);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::UnsupportedType);
}

SPACELENS_TEST(Maintenance_reparse_file_is_blocked)
{
    const auto candidate = fileCandidate(ordinaryPath(L"link.bin"), 3, 100);
    auto probe = presentProbe(candidate.objectEvidence, true);
    const auto item = evaluateMaintenanceEligibility(
        candidate, probe, LocationSafety::Ordinary);
    SPACELENS_REQUIRE(!item.eligible);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::ReparsePoint);
}

SPACELENS_TEST(Maintenance_protected_sensitive_unknown_are_blocked)
{
    const auto candidate = fileCandidate(L"C:\\Windows\\notepad.exe", 4, 10);
    SPACELENS_REQUIRE_EQ(
        evaluateMaintenanceEligibility(
            candidate, presentProbe(candidate.objectEvidence),
            LocationSafety::Protected)
            .blockReason,
        MaintenanceBlockReason::Protected);
    SPACELENS_REQUIRE_EQ(
        evaluateMaintenanceEligibility(
            candidate, presentProbe(candidate.objectEvidence),
            LocationSafety::Sensitive)
            .blockReason,
        MaintenanceBlockReason::Sensitive);
    SPACELENS_REQUIRE_EQ(
        evaluateMaintenanceEligibility(
            candidate, presentProbe(candidate.objectEvidence),
            LocationSafety::Unknown)
            .blockReason,
        MaintenanceBlockReason::UnknownLocation);
}

SPACELENS_TEST(Maintenance_weak_identity_is_blocked)
{
    auto candidate = fileCandidate(ordinaryPath(L"weak.bin"), 5, 20);
    candidate.objectEvidence.identity = makeFileIndex64FallbackIdentity(7, 99);
    const auto item = evaluateMaintenanceEligibility(
        candidate, presentProbe(candidate.objectEvidence), LocationSafety::Ordinary);
    SPACELENS_REQUIRE(!item.eligible);
    SPACELENS_REQUIRE_EQ(item.blockReason,
                         MaintenanceBlockReason::IdentityUnavailable);
}

SPACELENS_TEST(Maintenance_identity_mismatch_is_blocked)
{
    const auto candidate = fileCandidate(ordinaryPath(L"swap.bin"), 6, 30);
    auto live = candidate.objectEvidence;
    live.identity = strongId(99);
    const auto item = evaluateMaintenanceEligibility(
        candidate, presentProbe(live), LocationSafety::Ordinary);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::IdentityMismatch);
}

SPACELENS_TEST(Maintenance_changed_size_is_blocked)
{
    const auto candidate = fileCandidate(ordinaryPath(L"grow.bin"), 7, 40);
    auto live = candidate.objectEvidence;
    live.logicalSize = 80;
    const auto item = evaluateMaintenanceEligibility(
        candidate, presentProbe(live), LocationSafety::Ordinary);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::ChangedSinceReview);
}

SPACELENS_TEST(Maintenance_missing_and_access_denied)
{
    const auto candidate = fileCandidate(ordinaryPath(L"gone.bin"), 8, 10);
    CleanupMetadataProbe missing;
    missing.outcome = CleanupMetadataProbeOutcome::Missing;
    SPACELENS_REQUIRE_EQ(
        evaluateMaintenanceEligibility(candidate, missing, LocationSafety::Ordinary)
            .blockReason,
        MaintenanceBlockReason::Missing);

    CleanupMetadataProbe denied;
    denied.outcome = CleanupMetadataProbeOutcome::AccessDenied;
    SPACELENS_REQUIRE_EQ(
        evaluateMaintenanceEligibility(candidate, denied, LocationSafety::Ordinary)
            .blockReason,
        MaintenanceBlockReason::AccessDenied);
}

SPACELENS_TEST(Maintenance_already_recycled_is_blocked)
{
    auto candidate = fileCandidate(ordinaryPath(L"old.bin"), 9, 10);
    candidate.lifecycle = CleanupItemLifecycle::Recycled;
    const auto item = evaluateMaintenanceEligibility(
        candidate, presentProbe(candidate.objectEvidence), LocationSafety::Ordinary);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::AlreadyRecycled);
}

SPACELENS_TEST(Maintenance_drive_root_and_empty_path_are_blocked)
{
    auto root = fileCandidate(L"D:\\", 10, 0);
    SPACELENS_REQUIRE_EQ(
        evaluateMaintenanceEligibility(
            root, presentProbe(root.objectEvidence), LocationSafety::Ordinary)
            .blockReason,
        MaintenanceBlockReason::RootPath);

    auto empty = fileCandidate(L"", 11, 0);
    SPACELENS_REQUIRE_EQ(
        evaluateMaintenanceEligibility(
            empty, presentProbe(empty.objectEvidence), LocationSafety::Ordinary)
            .blockReason,
        MaintenanceBlockReason::EmptyPath);
}

SPACELENS_TEST(Maintenance_plan_dedupes_hard_link_aliases)
{
    CleanupReview review;
    auto a = fileCandidate(ordinaryPath(L"a.bin"), 12, 1000);
    auto b = fileCandidate(ordinaryPath(L"b.bin"), 12, 1000);
    a.id = 1;
    b.id = 2;
    review.resetTo({a, b}, 3);

    MapReader reader;
    reader.probes[a.path] = presentProbe(a.objectEvidence);
    reader.probes[b.path] = presentProbe(b.objectEvidence);

    const auto plan = prepareMaintenancePlan(review, {2, 1}, reader, "t");
    SPACELENS_REQUIRE_EQ(plan.selectedCount, 2ULL);
    SPACELENS_REQUIRE_EQ(plan.eligibleCount, 1ULL);
    SPACELENS_REQUIRE_EQ(plan.blockedCount, 1ULL);
    SPACELENS_REQUIRE_EQ(plan.eligibleLogicalBytes, 1000ULL);
    bool sawAlias = false;
    for (const auto& item : plan.items) {
        if (item.blockReason == MaintenanceBlockReason::SameIdentityAlreadySelected) {
            sawAlias = true;
        }
    }
    SPACELENS_REQUIRE(sawAlias);
}

SPACELENS_TEST(Maintenance_final_guard_catches_identity_race)
{
    auto planned = evaluateMaintenanceEligibility(
        fileCandidate(ordinaryPath(L"race.bin"), 13, 50),
        presentProbe(fileEvidence(13, 50)),
        LocationSafety::Ordinary);
    SPACELENS_REQUIRE(planned.eligible);

    auto live = fileEvidence(13, 50);
    live.identity = strongId(77);
    SPACELENS_REQUIRE_EQ(
        evaluateMaintenanceFinalGuard(
            planned, presentProbe(live), LocationSafety::Ordinary),
        MaintenanceBlockReason::IdentityMismatch);
}

SPACELENS_TEST(Maintenance_execute_stops_on_unexpected_permanent_removal)
{
    CleanupReview review;
    auto first = fileCandidate(ordinaryPath(L"one.bin"), 14, 10);
    auto second = fileCandidate(ordinaryPath(L"two.bin"), 15, 20);
    const auto id1 = review.add(first);
    const auto id2 = review.add(second);
    MapReader reader;
    reader.probes[first.path] = presentProbe(first.objectEvidence);
    reader.probes[second.path] = presentProbe(second.objectEvidence);
    const auto plan = prepareMaintenancePlan(review, {id1, id2}, reader, "t");
    SPACELENS_REQUIRE_EQ(plan.eligibleCount, 2ULL);

    ScriptedRecycle recycle;
    recycle.script = {MaintenanceItemResult::UnexpectedPermanentRemoval,
                      MaintenanceItemResult::Recycled};
    const auto receipt = executeMaintenancePlan(plan, reader, recycle, 1, {});
    SPACELENS_REQUIRE(receipt.unexpectedPermanentRemoval);
    SPACELENS_REQUIRE_EQ(receipt.failed, 1ULL);
    SPACELENS_REQUIRE_EQ(recycle.calls, 1ULL);
    SPACELENS_REQUIRE_EQ(receipt.items.size(), 2ULL);
    SPACELENS_REQUIRE_EQ(receipt.items[1].result, MaintenanceItemResult::NotAttempted);
}

SPACELENS_TEST(Maintenance_cancel_before_next_item)
{
    CleanupReview review;
    auto first = fileCandidate(ordinaryPath(L"keep.bin"), 16, 10);
    auto second = fileCandidate(ordinaryPath(L"later.bin"), 17, 20);
    const auto id1 = review.add(first);
    const auto id2 = review.add(second);
    MapReader reader;
    reader.probes[first.path] = presentProbe(first.objectEvidence);
    reader.probes[second.path] = presentProbe(second.objectEvidence);
    const auto plan = prepareMaintenancePlan(review, {id1, id2}, reader, "t");

    ScriptedRecycle recycle;
    const auto receipt = executeMaintenancePlan(
        plan, reader, recycle, 1, [&]() { return recycle.calls >= 1; });
    SPACELENS_REQUIRE_EQ(receipt.recycled, 1ULL);
    SPACELENS_REQUIRE_EQ(receipt.cancelled, 1ULL);
    SPACELENS_REQUIRE_EQ(recycle.calls, 1ULL);
    SPACELENS_REQUIRE_EQ(receipt.items.back().result,
                         MaintenanceItemResult::Cancelled);
}

SPACELENS_TEST(Maintenance_blocked_preflight_is_not_attempted)
{
    CleanupReview review;
    auto dir = fileCandidate(ordinaryPath(L"folder"), 18, 0);
    dir.kind = ItemKind::Directory;
    dir.objectEvidence.kind = ItemKind::Directory;
    const auto id = review.add(dir);
    MapReader reader;
    reader.probes[dir.path] = presentProbe(dir.objectEvidence);
    const auto plan = prepareMaintenancePlan(review, {id}, reader, "t");
    ScriptedRecycle recycle;
    const auto receipt = executeMaintenancePlan(plan, reader, recycle, 1, {});
    SPACELENS_REQUIRE_EQ(receipt.attempted, 0ULL);
    SPACELENS_REQUIRE_EQ(receipt.blocked, 1ULL);
    SPACELENS_REQUIRE_EQ(recycle.calls, 0ULL);
    SPACELENS_REQUIRE_EQ(receipt.items.front().result,
                         MaintenanceItemResult::BlockedPreflight);
}

SPACELENS_TEST(Maintenance_recycle_unavailable_is_blocked_at_preflight)
{
    CleanupReview review;
    auto file = fileCandidate(ordinaryPath(L"gone.bin"), 20, 64);
    const auto id = review.add(file);
    MapReader reader;
    reader.probes[file.path] = presentProbe(file.objectEvidence);
    auto plan = prepareMaintenancePlan(review, {id}, reader, "t");
    SPACELENS_REQUIRE_EQ(plan.eligibleCount, 1ULL);

    applyRecycleAvailability(
        plan, [](const MaintenancePlanItem&, std::string* detail) {
            if (detail != nullptr) {
                *detail = "test recycle unavailable";
            }
            return false;
        });
    SPACELENS_REQUIRE_EQ(plan.eligibleCount, 0ULL);
    SPACELENS_REQUIRE_EQ(plan.blockedCount, 1ULL);
    SPACELENS_REQUIRE_EQ(plan.eligibleLogicalBytes, 0ULL);
    SPACELENS_REQUIRE_EQ(plan.items.front().blockReason,
                         MaintenanceBlockReason::RecycleUnavailable);
    SPACELENS_REQUIRE(plan.items.front().detail == "test recycle unavailable");
}

SPACELENS_TEST(Maintenance_classification_is_unused)
{
    auto candidate = fileCandidate(ordinaryPath(L"user.bin"), 19, 8);
    candidate.classification.category = StorageCategory::UserData;
    candidate.capturedReclaimability = Reclaimability::NotApplicable;
    candidate.capturedCandidateStrength = CandidateStrength::ReviewOnly;
    const auto item = evaluateMaintenanceEligibility(
        candidate, presentProbe(candidate.objectEvidence), LocationSafety::Ordinary);
    SPACELENS_REQUIRE(item.eligible);
}

SPACELENS_TEST(Maintenance_unknown_layout_is_blocked_without_declaration)
{
    auto candidate = fileCandidate(L"D:\\Projects\\scratch.bin", 21, 64);
    MapReader reader;
    reader.probes[candidate.path] = presentProbe(candidate.objectEvidence);
    const auto item =
        evaluateMaintenanceEligibility(candidate, reader, OrdinaryLocationPolicy{});
    SPACELENS_REQUIRE(!item.eligible);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::UnknownLocation);
}

SPACELENS_TEST(Maintenance_user_declared_ordinary_root_can_satisfy_location_gate)
{
    auto candidate = fileCandidate(L"D:\\Projects\\scratch.bin", 22, 64);
    MapReader reader;
    reader.probes[candidate.path] = presentProbe(candidate.objectEvidence);
    OrdinaryLocationPolicy policy;
    policy.generation = 4;
    policy.declarations.push_back(activeRoot(L"D:\\Projects"));
    const auto item = evaluateMaintenanceEligibility(candidate, reader, policy);
    SPACELENS_REQUIRE(item.eligible);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::None);
}

SPACELENS_TEST(Maintenance_inactive_declaration_does_not_satisfy_location_gate)
{
    auto candidate = fileCandidate(L"D:\\Projects\\scratch.bin", 23, 64);
    MapReader reader;
    reader.probes[candidate.path] = presentProbe(candidate.objectEvidence);
    auto row = activeRoot(L"D:\\Projects");
    row.status = OrdinaryLocationStatus::VolumeMismatch;
    OrdinaryLocationPolicy policy;
    policy.declarations.push_back(row);
    const auto item = evaluateMaintenanceEligibility(candidate, reader, policy);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::UnknownLocation);
}

SPACELENS_TEST(Maintenance_protected_still_wins_over_declaration)
{
    auto candidate = fileCandidate(L"C:\\Windows\\notepad.exe", 24, 10);
    MapReader reader;
    reader.probes[candidate.path] = presentProbe(candidate.objectEvidence);
    OrdinaryLocationPolicy policy;
    policy.declarations.push_back(activeRoot(L"C:\\Windows"));
    const auto item = evaluateMaintenanceEligibility(candidate, reader, policy);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::Protected);
}

SPACELENS_TEST(Maintenance_declaration_removed_between_preflight_and_final_guard)
{
    CleanupReview review;
    auto candidate = fileCandidate(L"D:\\Projects\\scratch.bin", 25, 32);
    const auto id = review.add(candidate);
    MapReader reader;
    reader.probes[candidate.path] = presentProbe(candidate.objectEvidence);

    OrdinaryLocationPolicy preparePolicy;
    preparePolicy.generation = 2;
    preparePolicy.declarations.push_back(activeRoot(L"D:\\Projects"));
    const auto plan =
        prepareMaintenancePlan(review, {id}, reader, "t", preparePolicy);
    SPACELENS_REQUIRE_EQ(plan.eligibleCount, 1ULL);
    SPACELENS_REQUIRE_EQ(plan.locationPolicyGeneration, 2ULL);

    OrdinaryLocationPolicy executePolicy;
    executePolicy.generation = 3;
    ScriptedRecycle recycle;
    const auto receipt =
        executeMaintenancePlan(plan, reader, recycle, 1, {}, executePolicy);
    SPACELENS_REQUIRE_EQ(recycle.calls, 0ULL);
    SPACELENS_REQUIRE_EQ(receipt.recycled, 0ULL);
    SPACELENS_REQUIRE_EQ(receipt.blocked, 1ULL);
    SPACELENS_REQUIRE_EQ(receipt.items.front().result,
                         MaintenanceItemResult::BlockedFinalGuard);
    SPACELENS_REQUIRE_EQ(receipt.items.front().blockReason,
                         MaintenanceBlockReason::UnknownLocation);
}
