#include "TestRunner.hpp"
#include "core/CleanupReview.hpp"

#include <array>
#include <limits>

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
    out.objectEvidence.kind = kind;
    out.objectEvidence.sizeScope = kind == ItemKind::File
                                       ? CleanupEvidenceScope::Direct
                                       : CleanupEvidenceScope::Recursive;
    out.objectEvidence.logicalSize = size;
    if (kind != ItemKind::File) {
        out.historicalDirectoryAggregate.available = true;
        out.historicalDirectoryAggregate.recursiveLogicalSize = size;
    }
    return out;
}

}  // namespace

SPACELENS_TEST(CleanupReview_add_remove_total)
{
    CleanupReview review;
    auto a = candidate(L"D:\\proj\\build", 1000, ItemKind::Directory);
    a.reasonAdded = "manual";
    const auto id1 = review.add(a);
    SPACELENS_REQUIRE(id1 != 0);
    SPACELENS_REQUIRE_EQ(review.size(), 1u);
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 1000ULL);

    review.add(candidate(L"D:\\proj\\out.bin", 500));
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 1500ULL);
    SPACELENS_REQUIRE(review.removeById(id1));
    SPACELENS_REQUIRE_EQ(review.size(), 1u);
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 500ULL);
}

SPACELENS_TEST(CleanupReview_duplicate_path_updates)
{
    CleanupReview review;
    auto a = candidate(L"D:\\Same\\Path", 10);
    const auto id1 = review.add(a);
    auto b = candidate(L"d:/same/path\\", 99);
    const auto id2 = review.add(b);
    SPACELENS_REQUIRE_EQ(id1, id2);
    SPACELENS_REQUIRE_EQ(review.size(), 1u);
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 99ULL);
}

SPACELENS_TEST(CleanupReview_identity_forms_are_not_equivalent)
{
    const auto file128 = strongId(1);
    const auto fallback = makeFileIndex64FallbackIdentity(7, 1);
    SPACELENS_REQUIRE(isStrongIdentity(file128));
    SPACELENS_REQUIRE(isIdentityAvailable(fallback));
    SPACELENS_REQUIRE(!identitiesEqual(file128, fallback));
    SPACELENS_REQUIRE(!identitiesEqual(file128, CleanupIdentity{}));

    CleanupReview review;
    auto a = candidate(L"D:\\same", 10);
    a.objectEvidence.identity = file128;
    auto b = candidate(L"D:\\same", 10);
    b.objectEvidence.identity = fallback;
    const auto first = review.addDetailed(a);
    const auto second = review.addDetailed(b);
    SPACELENS_REQUIRE(first.accepted());
    SPACELENS_REQUIRE(second.accepted());
    SPACELENS_REQUIRE(second.conflicted());
    SPACELENS_REQUIRE_EQ(review.size(), 2u);
}

SPACELENS_TEST(CleanupReview_same_strong_identity_merges)
{
    CleanupReview review;
    auto a = candidate(L"D:\\old", 10);
    a.objectEvidence.identity = strongId(3);
    auto b = candidate(L"D:\\renamed", 99);
    b.objectEvidence.identity = strongId(3);
    const auto first = review.addDetailed(a);
    const auto second = review.addDetailed(b);
    SPACELENS_REQUIRE_EQ(first.id, second.id);
    SPACELENS_REQUIRE(second.merged());
    SPACELENS_REQUIRE_EQ(review.size(), 1u);
    SPACELENS_REQUIRE_EQ(review.items().front().sizeAtSelection, 99ULL);
}

SPACELENS_TEST(CleanupReview_same_identity_different_kind_reports_conflict)
{
    CleanupReview review;
    auto first = candidate(L"D:\\same-file", 10, ItemKind::File);
    first.objectEvidence.identity = strongId(6);
    auto differentKind = candidate(L"D:\\renamed-directory", 20,
                                   ItemKind::Directory);
    differentKind.objectEvidence.identity = strongId(6);

    const auto firstOutcome = review.addDetailed(first);
    const auto conflict = review.addDetailed(differentKind);

    SPACELENS_REQUIRE(firstOutcome.accepted());
    SPACELENS_REQUIRE(conflict.accepted());
    SPACELENS_REQUIRE(conflict.conflicted());
    SPACELENS_REQUIRE_EQ(conflict.conflictingId, firstOutcome.id);
    SPACELENS_REQUIRE_EQ(review.size(), 2u);
}

SPACELENS_TEST(CleanupReview_path_type_fallback_is_conservative)
{
    CleanupReview review;
    const auto first = review.addDetailed(candidate(L"D:\\fallback", 10));
    const auto duplicate = review.addDetailed(candidate(L"d:/FALLBACK\\", 20));
    SPACELENS_REQUIRE(duplicate.merged());
    SPACELENS_REQUIRE_EQ(first.id, duplicate.id);

    const auto differentKind =
        review.addDetailed(candidate(L"D:\\fallback", 30, ItemKind::Directory));
    SPACELENS_REQUIRE(differentKind.accepted());
    SPACELENS_REQUIRE_EQ(review.size(), 2u);
}

SPACELENS_TEST(CleanupReview_fallback_identity_does_not_merge_different_paths)
{
    CleanupReview review;
    auto first = candidate(L"D:\\one", 10);
    first.objectEvidence.identity = makeFileIndex64FallbackIdentity(7, 99);
    auto second = candidate(L"D:\\two", 20);
    second.objectEvidence.identity = makeFileIndex64FallbackIdentity(7, 99);
    review.add(first);
    review.add(second);
    SPACELENS_REQUIRE_EQ(review.size(), 2u);
}

SPACELENS_TEST(CleanupReview_path_normalization_component_ancestry)
{
    SPACELENS_REQUIRE_EQ(normalizeCleanupPath(L"D:/Foo/Bar\\"),
                         std::wstring(L"d:\\foo\\bar"));
    SPACELENS_REQUIRE(isPathAncestorOrEqual(L"D:\\Foo", L"d:/foo/bar"));
    SPACELENS_REQUIRE(isPathAncestorOrEqual(L"D:\\Foo", L"D:\\Foo"));
    SPACELENS_REQUIRE(!isPathAncestorOrEqual(L"D:\\Foo", L"D:\\Foobar"));
    SPACELENS_REQUIRE(!isStrictPathAncestor(L"D:\\Foo", L"D:\\Foo"));
    SPACELENS_REQUIRE(isStrictPathAncestor(L"D:\\Foo", L"D:\\Foo\\Bar"));
}

SPACELENS_TEST(CleanupReview_validation_separates_directory_recursive_state)
{
    auto captured = candidate(L"D:\\project", 100, ItemKind::Directory);
    captured.objectEvidence.identity = strongId(5);
    captured.objectEvidence.lastWriteTime = 10;
    captured.historicalDirectoryAggregate.newestDescendantWrite = 20;

    CleanupCurrentEvidence current;
    current.available = true;
    current.objectEvidence = captured.objectEvidence;
    current.directoryAggregate.available = false;
    const auto validation = validateCleanupCandidate(captured, current);
    SPACELENS_REQUIRE(validation.objectIdentityMatched);
    SPACELENS_REQUIRE(validation.directMetadataUnchanged);
    SPACELENS_REQUIRE(!validation.recursiveEvidenceRevalidated);
    SPACELENS_REQUIRE_EQ(
        validation.state,
        CleanupValidationState::DirectUnchangedRecursiveNotRevalidated);
    SPACELENS_REQUIRE(hasValidationReason(
        validation.reasons,
        CleanupValidationReason::RecursiveNotRevalidated));
}

SPACELENS_TEST(CleanupReview_safety_change_is_not_direct_unchanged)
{
    auto captured = candidate(L"D:\\project", 100, ItemKind::Directory);
    captured.objectEvidence.identity = strongId(22);
    captured.capturedSafety = LocationSafety::Ordinary;
    CleanupCurrentEvidence current;
    current.available = true;
    current.objectEvidence = captured.objectEvidence;
    current.directoryAggregate.available = false;
    current.safety = LocationSafety::Sensitive;
    const auto validation = validateCleanupCandidate(captured, current);
    SPACELENS_REQUIRE(hasValidationReason(
        validation.reasons, CleanupValidationReason::SafetyChanged));
    SPACELENS_REQUIRE_EQ(validation.state, CleanupValidationState::Changed);
}

SPACELENS_TEST(CleanupReview_identity_unavailable_is_not_unchanged)
{
    for (const ItemKind kind : {ItemKind::File, ItemKind::Directory}) {
        auto captured = candidate(L"D:\\identity", 100, kind);
        captured.objectEvidence.identity = {};
        CleanupCurrentEvidence current;
        current.available = true;
        current.objectEvidence = objectEvidenceOf(captured);
        current.objectEvidence.identity = {};
        const auto validation = validateCleanupCandidate(captured, current);
        SPACELENS_REQUIRE_EQ(validation.state,
                             CleanupValidationState::IdentityUnavailable);
        SPACELENS_REQUIRE(hasValidationReason(
            validation.reasons, CleanupValidationReason::IdentityUnavailable));
    }
}

SPACELENS_TEST(CleanupReview_legacy_directory_scalar_synthesizes_aggregate)
{
    CleanupCandidate legacy;
    legacy.path = L"D:\\legacy";
    legacy.kind = ItemKind::Directory;
    legacy.sizeAtSelection = 100;

    CleanupReview review;
    review.add(legacy);
    const auto& stored = review.items().front();
    SPACELENS_REQUIRE(stored.objectEvidence.available);
    SPACELENS_REQUIRE(stored.historicalDirectoryAggregate.available);
    SPACELENS_REQUIRE_EQ(
        stored.historicalDirectoryAggregate.recursiveLogicalSize, 100ULL);
    SPACELENS_REQUIRE_EQ(stored.objectEvidence.lastWriteTime, 0ULL);
}

SPACELENS_TEST(CleanupReview_legacy_directory_mtime_is_not_object_last_write)
{
    CleanupCandidate legacy;
    legacy.path = L"D:\\legacy";
    legacy.kind = ItemKind::Directory;
    legacy.sizeAtSelection = 100;
    legacy.lastWriteTime = 42;

    CleanupReview review;
    review.add(legacy);
    const auto stored = review.items().front();
    SPACELENS_REQUIRE_EQ(stored.objectEvidence.lastWriteTime, 0ULL);
    SPACELENS_REQUIRE_EQ(
        stored.historicalDirectoryAggregate.newestDescendantWrite, 42ULL);

    CleanupCurrentEvidence current;
    current.available = true;
    current.objectEvidence = stored.objectEvidence;
    current.objectEvidence.identity = strongId(23);
    current.objectEvidence.lastWriteTime = 7;
    current.directoryAggregate.available = true;
    current.directoryAggregate.revalidated = true;
    current.directoryAggregate.recursiveLogicalSize = 100;
    current.directoryAggregate.newestDescendantWrite = 42;
    const auto validation = validateCleanupCandidate(stored, current);
    SPACELENS_REQUIRE(!hasValidationReason(
        validation.reasons, CleanupValidationReason::LastWriteChanged));
}

SPACELENS_TEST(CleanupReview_unknown_recursive_newest_write_is_not_a_change)
{
    auto captured = candidate(L"D:\\project", 100, ItemKind::Directory);
    captured.objectEvidence.identity = strongId(24);
    captured.historicalDirectoryAggregate.newestDescendantWrite = 0;
    CleanupCurrentEvidence current;
    current.available = true;
    current.objectEvidence = objectEvidenceOf(captured);
    current.directoryAggregate.available = true;
    current.directoryAggregate.revalidated = true;
    current.directoryAggregate.recursiveLogicalSize = 100;
    current.directoryAggregate.newestDescendantWrite = 99;
    const auto validation = validateCleanupCandidate(captured, current);
    SPACELENS_REQUIRE(!hasValidationReason(
        validation.reasons, CleanupValidationReason::RecursiveChanged));
    SPACELENS_REQUIRE(validation.recursiveEvidenceRevalidated);
}

SPACELENS_TEST(CleanupReview_explicit_direct_directory_preserves_unavailable_aggregate)
{
    CleanupCandidate direct;
    direct.path = L"D:\\direct";
    direct.kind = ItemKind::Directory;
    direct.sizeAtSelection = 100;
    direct.objectEvidence.available = true;
    direct.objectEvidence.kind = ItemKind::Directory;
    direct.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    direct.objectEvidence.logicalSize = 7;
    direct.historicalDirectoryAggregate.available = false;

    CleanupReview review;
    review.add(direct);
    const auto& stored = review.items().front();
    SPACELENS_REQUIRE(stored.objectEvidence.available);
    SPACELENS_REQUIRE_EQ(stored.objectEvidence.logicalSize, 7ULL);
    SPACELENS_REQUIRE(!stored.historicalDirectoryAggregate.available);
}

SPACELENS_TEST(CleanupReview_recursive_object_evidence_migrates_into_aggregate)
{
    CleanupCandidate recursive;
    recursive.path = L"D:\\recursive";
    recursive.kind = ItemKind::Directory;
    recursive.sizeAtSelection = 0;
    recursive.objectEvidence.available = true;
    recursive.objectEvidence.kind = ItemKind::Directory;
    recursive.objectEvidence.sizeScope = CleanupEvidenceScope::Recursive;
    recursive.objectEvidence.logicalSize = 5000;
    recursive.objectEvidence.lastWriteTime = 42;
    recursive.historicalDirectoryAggregate.available = false;

    CleanupReview review;
    review.add(recursive);
    const auto& stored = review.items().front();
    SPACELENS_REQUIRE(stored.historicalDirectoryAggregate.available);
    SPACELENS_REQUIRE_EQ(
        stored.historicalDirectoryAggregate.recursiveLogicalSize, 5000ULL);
    SPACELENS_REQUIRE_EQ(
        stored.historicalDirectoryAggregate.newestDescendantWrite, 42ULL);
    SPACELENS_REQUIRE_EQ(stored.objectEvidence.sizeScope,
                         CleanupEvidenceScope::Direct);
    SPACELENS_REQUIRE_EQ(stored.objectEvidence.logicalSize, 0ULL);
    SPACELENS_REQUIRE_EQ(stored.objectEvidence.lastWriteTime, 0ULL);
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 5000ULL);
}

SPACELENS_TEST(CleanupReview_same_identity_prefers_same_kind_merge_over_conflict)
{
    CleanupReview review;
    auto file = candidate(L"D:\\same-file", 10, ItemKind::File);
    file.objectEvidence.identity = strongId(6);
    auto directory = candidate(L"D:\\renamed-directory", 20, ItemKind::Directory);
    directory.objectEvidence.identity = strongId(6);
    auto updatedDirectory =
        candidate(L"D:\\renamed-again", 99, ItemKind::Directory);
    updatedDirectory.objectEvidence.identity = strongId(6);

    review.add(file);
    const auto conflict = review.addDetailed(directory);
    const auto merged = review.addDetailed(updatedDirectory);

    SPACELENS_REQUIRE(conflict.conflicted());
    SPACELENS_REQUIRE(merged.merged());
    SPACELENS_REQUIRE_EQ(merged.id, conflict.id);
    SPACELENS_REQUIRE_EQ(review.size(), 2u);
    SPACELENS_REQUIRE_EQ(review.findById(conflict.id)->sizeAtSelection, 99ULL);
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 109ULL);
}

SPACELENS_TEST(CleanupReview_remove_by_path_clears_same_path_conflicts)
{
    CleanupReview review;
    auto first = candidate(L"D:\\same", 10);
    first.objectEvidence.identity = strongId(1);
    auto second = candidate(L"D:\\same", 20);
    second.objectEvidence.identity = strongId(2);
    review.add(first);
    const auto conflict = review.addDetailed(second);
    SPACELENS_REQUIRE(conflict.conflicted());
    SPACELENS_REQUIRE_EQ(review.size(), 2u);
    SPACELENS_REQUIRE(review.removeByPath(L"d:/SAME\\"));
    SPACELENS_REQUIRE(review.empty());
    SPACELENS_REQUIRE(!review.containsPath(L"D:\\same"));
}

SPACELENS_TEST(CleanupReview_directory_recursive_size_is_not_direct_size_diff)
{
    auto captured = candidate(L"D:\\project", 100, ItemKind::Directory);
    captured.objectEvidence.identity = strongId(10);
    captured.objectEvidence.sizeScope = CleanupEvidenceScope::Recursive;
    captured.objectEvidence.logicalSize = 100;
    CleanupCurrentEvidence current;
    current.available = true;
    current.objectEvidence = captured.objectEvidence;
    current.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    current.objectEvidence.logicalSize = 0;
    const auto validation = validateCleanupCandidate(captured, current);
    SPACELENS_REQUIRE(!hasValidationReason(
        validation.reasons, CleanupValidationReason::LogicalSizeChanged));
    SPACELENS_REQUIRE(hasValidationReason(
        validation.reasons, CleanupValidationReason::RecursiveNotRevalidated));
}

SPACELENS_TEST(CleanupReview_recursive_mtime_is_not_object_last_write)
{
    CleanupCandidate captured;
    captured.path = L"D:\\project";
    captured.kind = ItemKind::Directory;
    captured.objectEvidence.available = true;
    captured.objectEvidence.identity = strongId(21);
    captured.objectEvidence.kind = ItemKind::Directory;
    captured.objectEvidence.sizeScope = CleanupEvidenceScope::Recursive;
    captured.objectEvidence.logicalSize = 100;
    captured.objectEvidence.lastWriteTime = 42;

    CleanupReview review;
    review.add(captured);
    auto stored = review.items().front();

    CleanupCurrentEvidence current;
    current.available = true;
    current.objectEvidence = stored.objectEvidence;
    current.objectEvidence.lastWriteTime = 7;
    current.directoryAggregate.available = true;
    current.directoryAggregate.revalidated = true;
    current.directoryAggregate.recursiveLogicalSize = 100;
    current.directoryAggregate.newestDescendantWrite = 42;
    const auto validation = validateCleanupCandidate(stored, current);
    SPACELENS_REQUIRE(!hasValidationReason(
        validation.reasons, CleanupValidationReason::LastWriteChanged));
    SPACELENS_REQUIRE(validation.directMetadataUnchanged);
}

SPACELENS_TEST(CleanupReview_validation_captures_last_access_diff)
{
    auto captured = candidate(L"D:\\file.bin", 100);
    captured.objectEvidence.available = true;
    captured.objectEvidence.identity = strongId(11);
    captured.objectEvidence.lastAccessTime = 10;
    CleanupCurrentEvidence current;
    current.available = true;
    current.objectEvidence = captured.objectEvidence;
    current.objectEvidence.lastAccessTime = 11;
    const auto validation = validateCleanupCandidate(captured, current);
    SPACELENS_REQUIRE(hasValidationReason(
        validation.reasons, CleanupValidationReason::LastAccessChanged));
    SPACELENS_REQUIRE_EQ(validation.state, CleanupValidationState::Changed);
}

SPACELENS_TEST(CleanupReview_clear_and_report)
{
    CleanupReview review;
    auto a = candidate(L"D:\\x", 1);
    a.classification.category = StorageCategory::BuildArtifact;
    a.classification.reason = "test";
    review.add(std::move(a));
    const auto report = review.copyReport();
    SPACELENS_REQUIRE(report.find("Cleanup Review") != std::string::npos);
    SPACELENS_REQUIRE(report.find("not authorization") != std::string::npos);
    SPACELENS_REQUIRE(report.find("Unique selected logical size") !=
                      std::string::npos);
    review.clear();
    SPACELENS_REQUIRE(review.empty());
}
