#include "TestRunner.hpp"
#include "core/CleanupPlan.hpp"
#include "core/Json.hpp"

#include <array>
#include <limits>
#include <string>

using namespace spacelens;

namespace {

CleanupIdentity strongId(std::uint8_t seed, std::uint64_t volume = 7)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = seed;
    bytes[15] = static_cast<std::uint8_t>(seed + 1);
    return makeFileId128Identity(volume, bytes);
}

CleanupCandidate candidate(std::wstring path, ByteSize size, ItemKind kind = ItemKind::File)
{
    CleanupCandidate out;
    out.path = std::move(path);
    out.kind = kind;
    out.sizeAtSelection = size;
    out.objectEvidence.available = true;
    out.objectEvidence.identity = strongId(static_cast<std::uint8_t>(size));
    out.objectEvidence.kind = kind;
    out.objectEvidence.sizeScope = kind == ItemKind::File
                                       ? CleanupEvidenceScope::Direct
                                       : CleanupEvidenceScope::Recursive;
    out.objectEvidence.logicalSize = size;
    out.objectEvidence.lastAccessTime = 3;
    out.historicalDirectoryAggregate.available = kind != ItemKind::File;
    out.historicalDirectoryAggregate.recursiveLogicalSize = size;
    out.capturedSafety = LocationSafety::Ordinary;
    out.capturedReclaimability = Reclaimability::LikelyRegenerable;
    out.capturedCandidateStrength = CandidateStrength::Moderate;
    out.source = "live_scan";
    return out;
}

}  // namespace

SPACELENS_TEST(CleanupPlan_parent_child_overlap_suppresses_child)
{
    CleanupReview review;
    review.add(candidate(L"D:\\root", 100, ItemKind::Directory));
    review.add(candidate(L"D:\\root\\child", 40, ItemKind::Directory));
    review.add(candidate(L"D:\\sibling", 20, ItemKind::Directory));
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 120ULL);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 1u);
    SPACELENS_REQUIRE_EQ(plan.summary.includedCount, 2u);
    SPACELENS_REQUIRE(plan.toText().find("suppressed") != std::string::npos);
}

SPACELENS_TEST(CleanupPlan_nested_directory_chain_suppresses_all_descendants)
{
    CleanupReview review;
    review.add(candidate(L"D:\\a", 100, ItemKind::Directory));
    review.add(candidate(L"D:\\a\\b", 50, ItemKind::Directory));
    review.add(candidate(L"D:\\a\\b\\c", 25, ItemKind::Directory));
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 100ULL);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 2u);
}

SPACELENS_TEST(CleanupPlan_directory_aggregate_suppresses_nested_file_but_not_reparse)
{
    CleanupReview review;
    review.add(candidate(L"D:\\root", 100, ItemKind::Directory));
    review.add(candidate(L"D:\\root\\file.bin", 10, ItemKind::File));
    review.add(candidate(L"D:\\root\\link", 20, ItemKind::ReparseDirectory));
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 120ULL);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 1u);
    SPACELENS_REQUIRE_EQ(plan.summary.includedCount, 2u);
    SPACELENS_REQUIRE(!plan.items[1].included);
    SPACELENS_REQUIRE(plan.items[2].included);
    SPACELENS_REQUIRE_EQ(plan.items[2].kind, ItemKind::ReparseDirectory);
}

SPACELENS_TEST(CleanupPlan_directory_does_not_cover_selection_under_reparse)
{
    CleanupReview review;
    review.add(candidate(L"D:\\root", 100, ItemKind::Directory));
    review.add(candidate(L"D:\\root\\link", 20, ItemKind::ReparseDirectory));
    review.add(candidate(L"D:\\root\\link\\nested.bin", 30, ItemKind::File));
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 150ULL);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 0u);
    SPACELENS_REQUIRE_EQ(plan.summary.includedCount, 3u);
}

SPACELENS_TEST(CleanupPlan_directory_aggregate_suppresses_conflicting_descendants)
{
    CleanupReview review;
    review.add(candidate(L"D:\\root", 100, ItemKind::Directory));
    auto first = candidate(L"D:\\root\\child.bin", 40);
    first.objectEvidence.identity = strongId(40);
    auto conflicting = candidate(L"D:\\root\\child.bin", 60);
    conflicting.objectEvidence.identity = strongId(60);
    review.add(first);
    const auto outcome = review.addDetailed(conflicting);
    SPACELENS_REQUIRE(outcome.conflicted());

    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.summary.conflictCount, 1u);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 2u);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 100ULL);
    SPACELENS_REQUIRE_EQ(plan.summary.includedCount, 1u);
}

SPACELENS_TEST(CleanupPlan_file_and_reparse_never_suppress_descendants)
{
    CleanupReview review;
    review.add(candidate(L"D:\\root\\file.bin", 10, ItemKind::File));
    review.add(candidate(L"D:\\root\\link", 20, ItemKind::ReparseDirectory));
    review.add(candidate(L"D:\\root\\link\\nested.bin", 30, ItemKind::File));
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 60ULL);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 0u);
}

SPACELENS_TEST(CleanupPlan_same_identity_and_path_fallback_are_deduped)
{
    CleanupReview review;
    auto first = candidate(L"D:\\one", 10);
    first.objectEvidence.identity = strongId(9);
    auto sameIdentity = candidate(L"D:\\renamed", 20);
    sameIdentity.objectEvidence.identity = strongId(9);
    review.add(first);
    review.add(sameIdentity);
    SPACELENS_REQUIRE_EQ(review.size(), 1u);
    auto pathA = candidate(L"D:\\fallback", 30);
    pathA.objectEvidence.identity = {};
    auto pathB = candidate(L"d:/FALLBACK\\", 40);
    pathB.objectEvidence.identity = {};
    review.add(pathA);
    review.add(pathB);
    SPACELENS_REQUIRE_EQ(review.size(), 2u);
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 0u);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 60ULL);
}

SPACELENS_TEST(CleanupPlan_fallback_identity_does_not_merge_different_paths)
{
    CleanupReview review;
    auto first = candidate(L"D:\\one", 10);
    first.objectEvidence.identity = makeFileIndex64FallbackIdentity(7, 99);
    auto second = candidate(L"D:\\two", 20);
    second.objectEvidence.identity = makeFileIndex64FallbackIdentity(7, 99);
    review.add(first);
    review.add(second);
    SPACELENS_REQUIRE_EQ(review.size(), 2u);
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 0u);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 30ULL);
}

SPACELENS_TEST(CleanupPlan_strong_same_path_conflict_remains_separate)
{
    CleanupReview review;
    auto first = candidate(L"D:\\conflict", 10);
    first.objectEvidence.identity = strongId(1);
    auto second = candidate(L"D:\\conflict", 20);
    second.objectEvidence.identity = strongId(2);
    review.add(first);
    review.add(second);
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.summary.conflictCount, 1u);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 0u);
    SPACELENS_REQUIRE_EQ(plan.summary.includedCount, 2u);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 30ULL);
}

SPACELENS_TEST(CleanupPlan_same_identity_different_kind_conflict_remains_separate)
{
    CleanupReview review;
    auto file = candidate(L"D:\\same-file", 10, ItemKind::File);
    file.objectEvidence.identity = strongId(12);
    auto directory = candidate(L"D:\\renamed-directory", 20,
                               ItemKind::Directory);
    directory.objectEvidence.identity = strongId(12);
    review.add(file);
    const auto outcome = review.addDetailed(directory);
    SPACELENS_REQUIRE(outcome.conflicted());

    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.summary.conflictCount, 1u);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 0u);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 30ULL);
}

SPACELENS_TEST(CleanupPlan_same_identity_same_kind_dedupes_after_kind_conflict)
{
    CleanupReview review;
    auto file = candidate(L"D:\\same-file", 10, ItemKind::File);
    file.objectEvidence.identity = strongId(12);
    auto directory = candidate(L"D:\\renamed-directory", 20, ItemKind::Directory);
    directory.objectEvidence.identity = strongId(12);
    auto updatedDirectory =
        candidate(L"D:\\renamed-again", 99, ItemKind::Directory);
    updatedDirectory.objectEvidence.identity = strongId(12);
    review.add(file);
    review.add(directory);
    review.add(updatedDirectory);

    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(review.size(), 2u);
    SPACELENS_REQUIRE_EQ(plan.summary.conflictCount, 1u);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 0u);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 109ULL);
}

SPACELENS_TEST(CleanupPlan_same_path_identity_form_conflict_remains_separate)
{
    CleanupReview review;
    auto file128 = candidate(L"D:\\same-path", 10);
    file128.objectEvidence.identity = strongId(13);
    auto fallback = candidate(L"d:/SAME-PATH\\", 20);
    fallback.objectEvidence.identity = makeFileIndex64FallbackIdentity(7, 13);
    review.add(file128);
    const auto outcome = review.addDetailed(fallback);
    SPACELENS_REQUIRE(outcome.conflicted());

    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.summary.conflictCount, 1u);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 0u);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 30ULL);
}

SPACELENS_TEST(CleanupPlan_saturating_raw_and_unique_sums)
{
    CleanupReview review;
    review.add(candidate(L"D:\\one", std::numeric_limits<ByteSize>::max() - 1));
    review.add(candidate(L"D:\\two", 10));
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.rawLogicalBytes(),
                         std::numeric_limits<ByteSize>::max());
    SPACELENS_REQUIRE(plan.summary.rawSumSaturated);
    SPACELENS_REQUIRE(plan.summary.uniqueSumSaturated);
    SPACELENS_REQUIRE(plan.summary.estimated);
    SPACELENS_REQUIRE(plan.toJson().find("saturating_sum") != std::string::npos);
}

SPACELENS_TEST(CleanupPlan_directory_state_is_not_full_unchanged)
{
    auto item = candidate(L"D:\\folder", 100, ItemKind::Directory);
    item.objectEvidence.identity = strongId(4);
    item.currentEvidence.available = true;
    item.currentEvidence.objectEvidence = item.objectEvidence;
    item.currentEvidence.directoryAggregate.available = false;
    CleanupReview review;
    review.add(item);
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(
        plan.items.front().validation.state,
        CleanupValidationState::DirectUnchangedRecursiveNotRevalidated);
    SPACELENS_REQUIRE(plan.toJson().find("DirectUnchangedRecursiveNotRevalidated") !=
                      std::string::npos);
}

SPACELENS_TEST(CleanupPlan_direct_directory_without_aggregate_does_not_cover_child)
{
    auto directory = candidate(L"D:\\direct", 100, ItemKind::Directory);
    directory.historicalDirectoryAggregate = {};
    directory.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    directory.objectEvidence.logicalSize = 7;
    auto child = candidate(L"D:\\direct\\child.bin", 40);

    CleanupReview review;
    review.add(directory);
    review.add(child);
    const auto plan = buildCleanupPlan(review);
    SPACELENS_REQUIRE_EQ(plan.summary.suppressedCount, 0u);
    SPACELENS_REQUIRE_EQ(plan.uniqueLogicalBytes(), 47ULL);
}

SPACELENS_TEST(CleanupPlan_directory_comparable_size_requires_available_revalidated_aggregate)
{
    auto item = candidate(L"D:\\folder", 100, ItemKind::Directory);
    item.currentEvidence.available = true;
    item.currentEvidence.objectEvidence = item.objectEvidence;
    item.currentEvidence.directoryAggregate.available = false;
    item.currentEvidence.directoryAggregate.revalidated = true;
    item.currentEvidence.objectEvidence.logicalSize = 7;

    CleanupReview review;
    review.add(item);
    const auto plan = buildCleanupPlan(review);
    const auto& planned = plan.items.front();
    SPACELENS_REQUIRE(planned.currentLogicalBytes.has_value());
    SPACELENS_REQUIRE(!planned.comparableCurrentLogicalBytes.has_value());
}

SPACELENS_TEST(CleanupPlan_directory_recursive_and_direct_sizes_are_not_compared)
{
    auto item = candidate(L"D:\\folder", 100, ItemKind::Directory);
    item.objectEvidence.identity = strongId(8);
    item.objectEvidence.sizeScope = CleanupEvidenceScope::Recursive;
    item.objectEvidence.logicalSize = 100;
    item.currentEvidence.available = true;
    item.currentEvidence.objectEvidence = item.objectEvidence;
    item.currentEvidence.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    item.currentEvidence.objectEvidence.logicalSize = 0;
    item.currentEvidence.directoryAggregate.available = false;

    CleanupReview review;
    review.add(item);
    const auto plan = buildCleanupPlan(review);
    const auto& planned = plan.items.front();
    SPACELENS_REQUIRE_EQ(planned.capturedLogicalBytes, 100ULL);
    SPACELENS_REQUIRE_EQ(planned.capturedEvidence.sizeScope,
                         CleanupEvidenceScope::Direct);
    SPACELENS_REQUIRE_EQ(planned.capturedEvidence.logicalSize, 0ULL);
    SPACELENS_REQUIRE(!hasValidationReason(
        planned.validation.reasons, CleanupValidationReason::LogicalSizeChanged));
    SPACELENS_REQUIRE(hasValidationReason(
        planned.validation.reasons,
        CleanupValidationReason::RecursiveNotRevalidated));
}

SPACELENS_TEST(CleanupPlan_deterministic_json_contains_safety_and_schema)
{
    CleanupReview review;
    auto item = candidate(L"C:\\Users\\Tung\\Projects\\δοκιμή", 12);
    item.sourceRoot = L"C:\\Users\\Tung";
    item.addedAt = 11644473600ULL * kFileTimeTicksPerSecond;
    review.add(item);
    CleanupPlanOptions options;
    options.generatedAt = "2026-01-02T03:04:05Z";
    options.userProfilePath = L"c:/users/tung";
    const auto first = buildCleanupPlan(review, options).toJson(options);
    const auto second = buildCleanupPlan(review, options).toJson(options);
    SPACELENS_REQUIRE_EQ(first, second);
    SPACELENS_REQUIRE(first.find("\"plan_schema_version\":1") !=
                      std::string::npos);
    SPACELENS_REQUIRE(first.find("\"planning_only\":true") !=
                      std::string::npos);
    SPACELENS_REQUIRE(first.find("\"read_only\":true") != std::string::npos);
    SPACELENS_REQUIRE(first.find("\"filesystem_mutation\":false") !=
                      std::string::npos);
    SPACELENS_REQUIRE(first.find("%USERPROFILE%\\\\Projects\\\\") !=
                      std::string::npos);
    SPACELENS_REQUIRE(first.find("δοκιμή") != std::string::npos);
}

SPACELENS_TEST(CleanupPlan_profile_redaction_is_component_bounded)
{
    SPACELENS_REQUIRE_EQ(
        redactUserProfilePath(L"C:\\Users\\Tung\\file.txt", L"c:/users/tung"),
        std::wstring(L"%USERPROFILE%\\file.txt"));
    SPACELENS_REQUIRE_EQ(
        redactUserProfilePath(L"C:\\Users\\Tungsten\\file.txt", L"c:/users/tung"),
        std::wstring(L"C:\\Users\\Tungsten\\file.txt"));
    SPACELENS_REQUIRE_EQ(
        redactUserProfilePath(L"D:\\Users\\Tung\\file.txt", L"c:/users/tung"),
        std::wstring(L"D:\\Users\\Tung\\file.txt"));
}

SPACELENS_TEST(CleanupPlan_text_redaction_honors_options_and_preserves_candidate)
{
    CleanupReview review;
    auto item = candidate(L"C:\\Users\\Tung\\δοκιμή 😀.bin", 12);
    item.sourceRoot = L"C:\\Users\\Tung";
    review.add(item);

    CleanupPlanOptions options;
    options.userProfilePath = L"c:/users/tung";
    const auto text = cleanupPlanText(review, options);
    SPACELENS_REQUIRE(text.find("%USERPROFILE%\\δοκιμή 😀.bin") !=
                      std::string::npos);
    SPACELENS_REQUIRE(text.find("source: live_scan (%USERPROFILE%)") !=
                      std::string::npos);
    SPACELENS_REQUIRE(text.find("C:\\Users\\Tung\\δοκιμή") ==
                      std::string::npos);
    SPACELENS_REQUIRE_EQ(review.items().front().path,
                         std::wstring(L"C:\\Users\\Tung\\δοκιμή 😀.bin"));
}

SPACELENS_TEST(CleanupPlan_shared_json_utf8_and_escape_helpers)
{
    const auto utf8 = utf8FromWide(L"δοκιμή 😀");
    SPACELENS_REQUIRE(utf8.find("δοκιμή") != std::string::npos);
    const auto escaped = jsonString(std::string("a\"b\\c\n"));
    SPACELENS_REQUIRE_EQ(escaped, std::string("\"a\\\"b\\\\c\\n\""));
}

SPACELENS_TEST(CleanupPlan_copy_report_is_utf8_and_planning_only)
{
    CleanupReview review;
    review.add(candidate(L"D:\\δοκιμή", 42));
    const auto report = review.copyReport();
    SPACELENS_REQUIRE(report.find("δοκιμή") != std::string::npos);
    SPACELENS_REQUIRE(report.find("planning-only") != std::string::npos);
    SPACELENS_REQUIRE(report.find("authorization") != std::string::npos);
}
