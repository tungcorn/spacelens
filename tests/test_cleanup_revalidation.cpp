#include "TestRunner.hpp"

#include "core/CleanupRevalidation.hpp"
#include "core/CleanupReview.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <utility>

using namespace spacelens;
namespace fs = std::filesystem;

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

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

CleanupObjectEvidence dirEvidence(std::uint8_t seed,
                                  std::uint32_t attributes = 16,
                                  ItemKind kind = ItemKind::Directory)
{
    CleanupObjectEvidence ev;
    ev.available = true;
    ev.identity = strongId(seed);
    ev.kind = kind;
    ev.sizeScope = CleanupEvidenceScope::Direct;
    ev.logicalSize = 0;
    ev.lastWriteTime = 8;
    ev.attributes = attributes;
    return ev;
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

CleanupCandidate dirCandidate(std::wstring path,
                              std::uint8_t seed,
                              ByteSize recursiveSize)
{
    CleanupCandidate out;
    out.path = std::move(path);
    out.kind = ItemKind::Directory;
    out.sizeAtSelection = recursiveSize;
    out.objectEvidence = dirEvidence(seed);
    out.historicalDirectoryAggregate.available = true;
    out.historicalDirectoryAggregate.recursiveLogicalSize = recursiveSize;
    out.historicalDirectoryAggregate.newestDescendantWrite = 20;
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

CleanupMetadataProbe outcomeProbe(CleanupMetadataProbeOutcome outcome,
                                  std::uint32_t nativeError,
                                  std::string detail)
{
    CleanupMetadataProbe probe;
    probe.outcome = outcome;
    probe.nativeError = nativeError;
    probe.detail = std::move(detail);
    return probe;
}

class FakeCleanupMetadataReader final : public ICleanupMetadataReader {
public:
    void set(std::wstring path, CleanupMetadataProbe probe)
    {
        m_probes[normalizeCleanupPath(path)] = std::move(probe);
    }

    [[nodiscard]] CleanupMetadataProbe read(const std::wstring& path) override
    {
        const auto it = m_probes.find(normalizeCleanupPath(path));
        if (it == m_probes.end()) {
            return outcomeProbe(CleanupMetadataProbeOutcome::Missing, 2,
                                "fake: path not registered");
        }
        return it->second;
    }

private:
    std::map<std::wstring, CleanupMetadataProbe> m_probes;
};

struct TempRoot {
    fs::path path;
    bool created = false;

    TempRoot()
    {
        path = fs::temp_directory_path() / "spacelens_cleanup_revalidation" /
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
        std::error_code ec;
        fs::create_directories(path, ec);
        created = !ec && fs::is_directory(path);
    }

    ~TempRoot()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool writeBytes(const fs::path& file, std::size_t size)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    if (size != 0) {
        const std::string data(size, 'x');
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

CleanupCandidate capturedFromProbe(const std::wstring& path,
                                   const CleanupMetadataProbe& probe,
                                   ByteSize historicalRecursive = 0)
{
    CleanupCandidate out;
    out.path = path;
    out.kind = probe.objectEvidence.kind;
    out.sizeAtSelection = probe.objectEvidence.logicalSize;
    out.lastWriteTime = probe.objectEvidence.lastWriteTime;
    out.attributes = probe.objectEvidence.attributes;
    out.objectEvidence = probe.objectEvidence;
    out.capturedSafety = classifyLocation(path);
    if (out.kind != ItemKind::File) {
        out.historicalDirectoryAggregate.available = historicalRecursive != 0;
        out.historicalDirectoryAggregate.recursiveLogicalSize =
            historicalRecursive;
    }
    return out;
}

}  // namespace

SPACELENS_TEST(CleanupRevalidation_unchanged_file)
{
    auto captured = fileCandidate(L"D:\\proj\\out.bin", 1, 100);
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(captured.objectEvidence));

    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE_EQ(result.probe.outcome, CleanupMetadataProbeOutcome::Present);
    SPACELENS_REQUIRE(result.current.available);
    SPACELENS_REQUIRE(result.current.exists);
    SPACELENS_REQUIRE(!result.current.directoryAggregate.available);
    SPACELENS_REQUIRE(!result.current.directoryAggregate.revalidated);
    SPACELENS_REQUIRE(result.validation.objectIdentityMatched);
    SPACELENS_REQUIRE(result.validation.directMetadataUnchanged);
    SPACELENS_REQUIRE_EQ(result.validation.state, CleanupValidationState::Unchanged);
}

SPACELENS_TEST(CleanupRevalidation_directory_direct_unchanged_recursive_not_revalidated)
{
    auto captured = dirCandidate(L"D:\\proj\\build", 2, 5000);
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(captured.objectEvidence));

    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE(result.validation.objectIdentityMatched);
    SPACELENS_REQUIRE(result.validation.directMetadataUnchanged);
    SPACELENS_REQUIRE(!result.validation.recursiveEvidenceRevalidated);
    SPACELENS_REQUIRE(!result.current.directoryAggregate.available);
    SPACELENS_REQUIRE_EQ(
        result.validation.state,
        CleanupValidationState::DirectUnchangedRecursiveNotRevalidated);
    SPACELENS_REQUIRE(hasValidationReason(
        result.validation.reasons,
        CleanupValidationReason::RecursiveNotRevalidated));
    SPACELENS_REQUIRE(result.validation.state != CleanupValidationState::Unchanged);
}

SPACELENS_TEST(CleanupRevalidation_size_write_attrs_changes)
{
    auto captured = fileCandidate(L"D:\\proj\\out.bin", 3, 100);
    auto changed = captured.objectEvidence;
    changed.logicalSize = 250;
    changed.lastWriteTime = 99;
    changed.attributes = 33;
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(changed));

    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE_EQ(result.validation.state, CleanupValidationState::Changed);
    SPACELENS_REQUIRE(hasValidationReason(
        result.validation.reasons, CleanupValidationReason::LogicalSizeChanged));
    SPACELENS_REQUIRE(hasValidationReason(
        result.validation.reasons, CleanupValidationReason::LastWriteChanged));
    SPACELENS_REQUIRE(hasValidationReason(
        result.validation.reasons, CleanupValidationReason::AttributesChanged));
}

SPACELENS_TEST(CleanupRevalidation_deleted_file_record_remains)
{
    CleanupReview review;
    auto captured = fileCandidate(L"D:\\proj\\gone.bin", 4, 40);
    const auto id = review.add(captured);
    FakeCleanupMetadataReader reader;
    reader.set(captured.path,
               outcomeProbe(CleanupMetadataProbeOutcome::Missing, 2,
                            "deleted"));

    SPACELENS_REQUIRE(applyCleanupRevalidation(review, id, reader, 50));
    const auto stored = review.findById(id);
    SPACELENS_REQUIRE(stored.has_value());
    SPACELENS_REQUIRE_EQ(review.size(), 1u);
    SPACELENS_REQUIRE_EQ(stored->validation.state, CleanupValidationState::Missing);
    SPACELENS_REQUIRE(hasValidationReason(stored->validation.reasons,
                                          CleanupValidationReason::Missing));
    SPACELENS_REQUIRE(stored->currentEvidence.available);
    SPACELENS_REQUIRE(!stored->currentEvidence.exists);
}

SPACELENS_TEST(CleanupRevalidation_deleted_directory_record_remains)
{
    CleanupReview review;
    auto captured = dirCandidate(L"D:\\proj\\oldbuild", 5, 900);
    const auto id = review.add(captured);
    FakeCleanupMetadataReader reader;
    reader.set(captured.path,
               outcomeProbe(CleanupMetadataProbeOutcome::Missing, 3,
                            "deleted dir"));

    SPACELENS_REQUIRE(applyCleanupRevalidation(review, id, reader, 51));
    const auto stored = review.findById(id);
    SPACELENS_REQUIRE(stored.has_value());
    SPACELENS_REQUIRE_EQ(stored->validation.state, CleanupValidationState::Missing);
    SPACELENS_REQUIRE_EQ(
        stored->historicalDirectoryAggregate.recursiveLogicalSize, 900ULL);
}

SPACELENS_TEST(CleanupRevalidation_same_path_different_strong_identity)
{
    auto captured = fileCandidate(L"D:\\proj\\same.bin", 6, 10);
    auto other = captured.objectEvidence;
    other.identity = strongId(9);
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(other));

    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE_EQ(result.validation.state,
                         CleanupValidationState::IdentityChanged);
    SPACELENS_REQUIRE(!result.validation.objectIdentityMatched);
}

SPACELENS_TEST(CleanupRevalidation_file_directory_type_change)
{
    auto captured = fileCandidate(L"D:\\proj\\morph", 7, 10);
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(dirEvidence(7)));

    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE_EQ(result.validation.state, CleanupValidationState::TypeChanged);
    SPACELENS_REQUIRE(hasValidationReason(result.validation.reasons,
                                          CleanupValidationReason::TypeChanged));
}

SPACELENS_TEST(CleanupRevalidation_ordinary_to_reparse)
{
    auto captured = dirCandidate(L"D:\\proj\\linked", 8, 100);
    auto reparse = dirEvidence(8, 1040, ItemKind::ReparseDirectory);
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(reparse, true));

    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE(result.probe.isReparse);
    SPACELENS_REQUIRE_EQ(result.current.objectEvidence.kind,
                         ItemKind::ReparseDirectory);
    SPACELENS_REQUIRE_EQ(result.validation.state, CleanupValidationState::TypeChanged);
}

SPACELENS_TEST(CleanupRevalidation_access_denied)
{
    auto captured = fileCandidate(L"D:\\proj\\secret.bin", 9, 12);
    FakeCleanupMetadataReader reader;
    reader.set(captured.path,
               outcomeProbe(CleanupMetadataProbeOutcome::AccessDenied, 5,
                            "denied"));

    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE_EQ(result.probe.outcome,
                         CleanupMetadataProbeOutcome::AccessDenied);
    SPACELENS_REQUIRE(!result.current.available);
    SPACELENS_REQUIRE(result.current.exists);
    SPACELENS_REQUIRE_EQ(result.validation.state,
                         CleanupValidationState::AccessDenied);
    SPACELENS_REQUIRE(hasValidationReason(
        result.validation.reasons, CleanupValidationReason::AccessDenied));
    SPACELENS_REQUIRE(hasValidationReason(
        result.validation.reasons, CleanupValidationReason::NoCurrentEvidence));
    SPACELENS_REQUIRE(result.validation.state != CleanupValidationState::Missing);
    SPACELENS_REQUIRE(result.validation.state != CleanupValidationState::Unchanged);
    SPACELENS_REQUIRE(result.validation.state != CleanupValidationState::NotValidated);
}

SPACELENS_TEST(CleanupRevalidation_identity_unavailable_path_fallback)
{
    auto captured = fileCandidate(L"D:\\proj\\weak.bin", 10, 16);
    captured.objectEvidence.identity = {};
    auto current = captured.objectEvidence;
    current.identity = {};
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(current));

    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE_EQ(result.validation.state,
                         CleanupValidationState::IdentityUnavailable);
    SPACELENS_REQUIRE(hasValidationReason(
        result.validation.reasons, CleanupValidationReason::IdentityUnavailable));
    SPACELENS_REQUIRE(result.validation.state != CleanupValidationState::Unchanged);
}

SPACELENS_TEST(CleanupRevalidation_protected_and_sensitive_classifyLocation)
{
    auto protectedItem =
        fileCandidate(L"C:\\Windows\\Temp\\artifact.bin", 11, 8);
    FakeCleanupMetadataReader reader;
    reader.set(protectedItem.path, presentProbe(protectedItem.objectEvidence));
    const auto protectedResult =
        revalidateCleanupCandidate(protectedItem, reader);
    SPACELENS_REQUIRE_EQ(protectedResult.safety, LocationSafety::Protected);
    SPACELENS_REQUIRE_EQ(protectedResult.current.safety, LocationSafety::Protected);
    SPACELENS_REQUIRE_EQ(protectedResult.validation.state,
                         CleanupValidationState::Protected);
    SPACELENS_REQUIRE(hasValidationReason(
        protectedResult.validation.reasons,
        CleanupValidationReason::ProtectedLocation));

    auto sensitiveItem =
        fileCandidate(L"C:\\Users\\Example\\AppData\\Local\\cache.bin", 12, 8);
    sensitiveItem.capturedSafety = LocationSafety::Sensitive;
    reader.set(sensitiveItem.path, presentProbe(sensitiveItem.objectEvidence));
    const auto sensitiveResult =
        revalidateCleanupCandidate(sensitiveItem, reader);
    SPACELENS_REQUIRE_EQ(sensitiveResult.safety, LocationSafety::Sensitive);
    SPACELENS_REQUIRE_EQ(sensitiveResult.current.safety, LocationSafety::Sensitive);
    SPACELENS_REQUIRE_EQ(sensitiveResult.validation.state,
                         CleanupValidationState::Unchanged);
}

SPACELENS_TEST(CleanupRevalidation_unicode_path)
{
    auto captured = fileCandidate(L"D:\\测试\\файл-文件.bin", 13, 21);
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(captured.objectEvidence));
    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE_EQ(result.validation.state, CleanupValidationState::Unchanged);
}

SPACELENS_TEST(CleanupRevalidation_error_precedence_not_hidden)
{
    auto captured = fileCandidate(L"D:\\proj\\x.bin", 14, 5);

    const auto missing = revalidateCleanupCandidate(
        captured, outcomeProbe(CleanupMetadataProbeOutcome::Missing, 2, "gone"));
    SPACELENS_REQUIRE_EQ(missing.probe.outcome, CleanupMetadataProbeOutcome::Missing);
    SPACELENS_REQUIRE_EQ(missing.validation.state, CleanupValidationState::Missing);
    SPACELENS_REQUIRE(missing.validation.state != CleanupValidationState::Unchanged);

    const auto denied = revalidateCleanupCandidate(
        captured,
        outcomeProbe(CleanupMetadataProbeOutcome::AccessDenied, 5, "denied"));
    SPACELENS_REQUIRE_EQ(denied.probe.outcome,
                         CleanupMetadataProbeOutcome::AccessDenied);
    SPACELENS_REQUIRE_EQ(denied.validation.state,
                         CleanupValidationState::AccessDenied);
    SPACELENS_REQUIRE(denied.validation.state != CleanupValidationState::Missing);
    SPACELENS_REQUIRE(denied.validation.state != CleanupValidationState::Unchanged);
    SPACELENS_REQUIRE(denied.validation.state != CleanupValidationState::NotValidated);

    const auto error = revalidateCleanupCandidate(
        captured, outcomeProbe(CleanupMetadataProbeOutcome::ProbeError, 21, "fail"));
    SPACELENS_REQUIRE_EQ(error.probe.outcome, CleanupMetadataProbeOutcome::ProbeError);
    SPACELENS_REQUIRE_EQ(error.probe.nativeError, 21U);
    SPACELENS_REQUIRE_EQ(error.validation.state,
                         CleanupValidationState::ProbeError);
    SPACELENS_REQUIRE(hasValidationReason(error.validation.reasons,
                                          CleanupValidationReason::ProbeError));
    SPACELENS_REQUIRE(error.validation.state != CleanupValidationState::Missing);
    SPACELENS_REQUIRE(error.validation.state != CleanupValidationState::Unchanged);
    SPACELENS_REQUIRE(error.validation.state != CleanupValidationState::NotValidated);
}

SPACELENS_TEST(CleanupRevalidation_present_without_object_evidence_is_probe_error)
{
    auto captured = fileCandidate(L"D:\\proj\\opaque.bin", 16, 9);
    CleanupMetadataProbe probe;
    probe.outcome = CleanupMetadataProbeOutcome::Present;
    probe.objectEvidence.available = false;
    probe.detail = "opened but metadata unavailable";

    const auto result = revalidateCleanupCandidate(captured, probe);
    SPACELENS_REQUIRE_EQ(result.current.observation, CleanupObservation::ProbeError);
    SPACELENS_REQUIRE(!result.current.available);
    SPACELENS_REQUIRE_EQ(result.validation.state, CleanupValidationState::ProbeError);
    SPACELENS_REQUIRE(result.validation.state != CleanupValidationState::Changed);
    SPACELENS_REQUIRE(result.validation.state != CleanupValidationState::Missing);
    SPACELENS_REQUIRE(result.validation.state != CleanupValidationState::Unchanged);
}

SPACELENS_TEST(CleanupRevalidation_refresh_file_size_updates)
{
    CleanupReview review;
    auto captured = fileCandidate(L"D:\\proj\\grow.bin", 15, 10);
    const auto id = review.add(captured);
    auto grown = captured.objectEvidence;
    grown.logicalSize = 44;
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(grown));

    SPACELENS_REQUIRE(applyCleanupRevalidation(review, id, reader, 70));
    SPACELENS_REQUIRE(review.refreshEvidence(id));
    const auto stored = *review.findById(id);
    SPACELENS_REQUIRE_EQ(stored.sizeAtSelection, 44ULL);
    SPACELENS_REQUIRE_EQ(stored.objectEvidence.logicalSize, 44ULL);
}

SPACELENS_TEST(CleanupRevalidation_refresh_directory_retains_recursive_aggregate)
{
    CleanupReview review;
    auto captured = dirCandidate(L"D:\\proj\\keep", 16, 7777);
    const auto id = review.add(captured);
    auto currentObject = captured.objectEvidence;
    currentObject.logicalSize = 4096;
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(currentObject));

    SPACELENS_REQUIRE(applyCleanupRevalidation(review, id, reader, 71));
    SPACELENS_REQUIRE(review.refreshEvidence(id));
    const auto stored = *review.findById(id);
    SPACELENS_REQUIRE_EQ(stored.objectEvidence.logicalSize, 4096ULL);
    SPACELENS_REQUIRE_EQ(stored.objectEvidence.sizeScope,
                         CleanupEvidenceScope::Direct);
    SPACELENS_REQUIRE(stored.historicalDirectoryAggregate.available);
    SPACELENS_REQUIRE_EQ(stored.historicalDirectoryAggregate.recursiveLogicalSize,
                         7777ULL);
    SPACELENS_REQUIRE(!stored.historicalDirectoryAggregate.revalidated);
    SPACELENS_REQUIRE_EQ(
        stored.validation.state,
        CleanupValidationState::DirectUnchangedRecursiveNotRevalidated);
}

SPACELENS_TEST(CleanupRevalidation_probe_all_applies_only_when_complete)
{
    CleanupReview review;
    auto first = fileCandidate(L"D:\\proj\\a.bin", 17, 10);
    auto second = fileCandidate(L"D:\\proj\\b.bin", 18, 20);
    const auto firstId = review.add(first);
    const auto secondId = review.add(second);
    FakeCleanupMetadataReader reader;
    reader.set(first.path, presentProbe(first.objectEvidence));
    auto grown = second.objectEvidence;
    grown.logicalSize = 99;
    reader.set(second.path, presentProbe(grown));

    const auto pass = probeCleanupReview(review, reader, 80);
    SPACELENS_REQUIRE(pass.completed);
    SPACELENS_REQUIRE_EQ(pass.probedCount, 2u);
    SPACELENS_REQUIRE_EQ(pass.updates.size(), 2u);
    SPACELENS_REQUIRE(review.replaceValidationBatch(pass.updates));
    SPACELENS_REQUIRE_EQ(review.findById(firstId)->validation.state,
                         CleanupValidationState::Unchanged);
    SPACELENS_REQUIRE_EQ(review.findById(secondId)->validation.state,
                         CleanupValidationState::Changed);
}

SPACELENS_TEST(CleanupRevalidation_probe_cancel_discards_partial_updates)
{
    CleanupReview review;
    auto first = fileCandidate(L"D:\\proj\\a.bin", 19, 10);
    auto second = fileCandidate(L"D:\\proj\\b.bin", 20, 20);
    review.add(first);
    review.add(second);
    FakeCleanupMetadataReader reader;
    reader.set(first.path, presentProbe(first.objectEvidence));
    reader.set(second.path, presentProbe(second.objectEvidence));

    int checks = 0;
    const auto pass = probeCleanupReview(review, reader, 81, [&]() {
        ++checks;
        return checks > 1;
    });
    SPACELENS_REQUIRE(!pass.completed);
    SPACELENS_REQUIRE_EQ(pass.probedCount, 1u);
    SPACELENS_REQUIRE(pass.updates.empty());
    SPACELENS_REQUIRE_EQ(review.items()[0].validation.state,
                         CleanupValidationState::NotValidated);
    SPACELENS_REQUIRE_EQ(review.items()[1].validation.state,
                         CleanupValidationState::NotValidated);
}

SPACELENS_TEST(CleanupRevalidation_attach_live_evidence_keeps_missing_candidate)
{
    auto captured = fileCandidate(L"D:\\proj\\missing.bin", 21, 10);
    captured.objectEvidence = {};
    FakeCleanupMetadataReader reader;
    reader.set(captured.path,
               outcomeProbe(CleanupMetadataProbeOutcome::Missing, 2, "gone"));
    attachLiveObjectEvidence(captured, reader);
    SPACELENS_REQUIRE(!captured.objectEvidence.available);
    SPACELENS_REQUIRE_EQ(captured.path, std::wstring(L"D:\\proj\\missing.bin"));
    SPACELENS_REQUIRE_EQ(captured.sizeAtSelection, 10ULL);
}

SPACELENS_TEST(CleanupRevalidation_attach_live_evidence_fills_file_identity)
{
    auto captured = fileCandidate(L"D:\\proj\\live.bin", 22, 0);
    captured.objectEvidence = {};
    FakeCleanupMetadataReader reader;
    reader.set(captured.path, presentProbe(fileEvidence(22, 33, 12, 32)));
    attachLiveObjectEvidence(captured, reader);
    SPACELENS_REQUIRE(captured.objectEvidence.available);
    SPACELENS_REQUIRE(isStrongIdentity(captured.objectEvidence.identity));
    SPACELENS_REQUIRE_EQ(captured.objectEvidence.logicalSize, 33ULL);
    SPACELENS_REQUIRE_EQ(captured.sizeAtSelection, 33ULL);
}

SPACELENS_TEST(CleanupRevalidation_attach_live_evidence_keeps_directory_aggregate)
{
    auto captured = dirCandidate(L"D:\\proj\\keep-agg", 23, 8888);
    FakeCleanupMetadataReader reader;
    auto live = captured.objectEvidence;
    live.logicalSize = 4096;
    reader.set(captured.path, presentProbe(live));
    attachLiveObjectEvidence(captured, reader);
    SPACELENS_REQUIRE(captured.historicalDirectoryAggregate.available);
    SPACELENS_REQUIRE_EQ(captured.historicalDirectoryAggregate.recursiveLogicalSize,
                         8888ULL);
    SPACELENS_REQUIRE_EQ(captured.objectEvidence.sizeScope,
                         CleanupEvidenceScope::Direct);
}

SPACELENS_TEST(CleanupRevalidation_prepare_add_promotes_directory_reparse)
{
    FakeCleanupMetadataReader reader;
    auto captured = dirCandidate(L"D:\\proj\\junction", 24, 0);
    captured.kind = ItemKind::Directory;
    auto live = dirEvidence(24, 1040, ItemKind::ReparseDirectory);
    reader.set(captured.path, presentProbe(live, true));

    prepareCleanupCandidateForAdd(captured, reader, 7);
    CleanupReview review;
    const auto id = review.add(captured);
    const auto stored = review.findById(id);
    SPACELENS_REQUIRE(stored.has_value());
    SPACELENS_REQUIRE_EQ(stored->kind, ItemKind::ReparseDirectory);
    SPACELENS_REQUIRE_EQ(stored->objectEvidence.kind, ItemKind::ReparseDirectory);

    const auto result = revalidateCleanupCandidate(*stored, reader);
    SPACELENS_REQUIRE(result.validation.state !=
                      CleanupValidationState::TypeChanged);
    SPACELENS_REQUIRE_EQ(
        result.validation.state,
        CleanupValidationState::DirectUnchangedRecursiveNotRevalidated);
}

SPACELENS_TEST(CleanupRevalidation_replace_batch_rejects_path_mismatch)
{
    CleanupReview review;
    auto first = fileCandidate(L"D:\\proj\\a.bin", 25, 10);
    auto second = fileCandidate(L"D:\\proj\\b.bin", 26, 20);
    const auto firstId = review.add(first);
    const auto secondId = review.add(second);

    CleanupCurrentEvidence missing;
    missing.available = true;
    missing.exists = false;
    missing.observation = CleanupObservation::Missing;
    missing.safety = LocationSafety::Ordinary;

    std::vector<CleanupValidationReplacement> updates(2);
    updates[0].id = firstId;
    updates[0].expectedPath = first.path;
    updates[0].current = missing;
    updates[0].checkedAt = 90;
    updates[1].id = secondId;
    updates[1].expectedPath = L"D:\\proj\\renamed.bin";
    updates[1].current = missing;
    updates[1].checkedAt = 90;

    SPACELENS_REQUIRE(!review.replaceValidationBatch(updates));
    SPACELENS_REQUIRE_EQ(review.findById(firstId)->validation.state,
                         CleanupValidationState::NotValidated);
    SPACELENS_REQUIRE_EQ(review.findById(secondId)->validation.state,
                         CleanupValidationState::NotValidated);

    updates[1].expectedPath = second.path;
    SPACELENS_REQUIRE(review.replaceValidationBatch(updates));
    SPACELENS_REQUIRE_EQ(review.findById(firstId)->validation.state,
                         CleanupValidationState::Missing);
    SPACELENS_REQUIRE_EQ(review.findById(secondId)->validation.state,
                         CleanupValidationState::Missing);
}

SPACELENS_TEST(CleanupRevalidation_prepare_add_keeps_directory_aggregate)
{
    FakeCleanupMetadataReader reader;
    auto captured = dirCandidate(L"D:\\proj\\keep", 23, 4096);
    auto live = dirEvidence(23);
    live.lastWriteTime = 99;
    reader.set(captured.path, presentProbe(live));

    prepareCleanupCandidateForAdd(captured, reader, 123);
    SPACELENS_REQUIRE_EQ(captured.addedAt, 123ULL);
    SPACELENS_REQUIRE(captured.historicalDirectoryAggregate.available);
    SPACELENS_REQUIRE_EQ(captured.historicalDirectoryAggregate.recursiveLogicalSize,
                         4096ULL);
    SPACELENS_REQUIRE_EQ(captured.objectEvidence.identity.source,
                         CleanupIdentitySource::FileId128);
    SPACELENS_REQUIRE_EQ(captured.objectEvidence.sizeScope,
                         CleanupEvidenceScope::Direct);
}

SPACELENS_TEST(Windows_CleanupRevalidation_temp_file_fileid)
{
    TempRoot root;
    if (!root.created) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_temp_file_fileid — "
                     "fixture cannot be created\n";
        return;
    }
    const auto file = root.path / "sample.bin";
    if (!writeBytes(file, 128)) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_temp_file_fileid — "
                     "fixture cannot be created\n";
        return;
    }

    WindowsCleanupMetadataReader reader;
    const auto probe = reader.read(file.wstring());
    SPACELENS_REQUIRE_EQ(probe.outcome, CleanupMetadataProbeOutcome::Present);
    SPACELENS_REQUIRE(probe.objectEvidence.available);
    SPACELENS_REQUIRE_EQ(probe.objectEvidence.kind, ItemKind::File);
    SPACELENS_REQUIRE_EQ(probe.objectEvidence.sizeScope,
                         CleanupEvidenceScope::Direct);
    SPACELENS_REQUIRE_EQ(probe.objectEvidence.logicalSize, 128ULL);
    SPACELENS_REQUIRE_EQ(probe.objectEvidence.identity.source,
                         CleanupIdentitySource::FileId128);
    SPACELENS_REQUIRE(isStrongIdentity(probe.objectEvidence.identity));
    SPACELENS_REQUIRE(!probe.isReparse);
}

SPACELENS_TEST(Windows_CleanupRevalidation_missing_path_is_not_present)
{
    TempRoot root;
    if (!root.created) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_missing_path_is_not_present "
                     "— fixture cannot be created\n";
        return;
    }
    const auto missing = root.path / "does-not-exist.bin";
    WindowsCleanupMetadataReader reader;
    const auto probe = reader.read(missing.wstring());
    SPACELENS_REQUIRE_EQ(probe.outcome, CleanupMetadataProbeOutcome::Missing);
    SPACELENS_REQUIRE(!probe.objectEvidence.available);
    SPACELENS_REQUIRE(probe.outcome != CleanupMetadataProbeOutcome::Present);
    SPACELENS_REQUIRE(probe.outcome != CleanupMetadataProbeOutcome::AccessDenied);
}

SPACELENS_TEST(Windows_CleanupRevalidation_temp_directory_fileid)
{
    TempRoot root;
    if (!root.created) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_temp_directory_fileid "
                     "— fixture cannot be created\n";
        return;
    }
    const auto dir = root.path / "folder";
    std::error_code ec;
    fs::create_directory(dir, ec);
    if (ec) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_temp_directory_fileid "
                     "— fixture cannot be created\n";
        return;
    }
    if (!writeBytes(dir / "child.bin", 64)) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_temp_directory_fileid "
                     "— fixture cannot be created\n";
        return;
    }

    WindowsCleanupMetadataReader reader;
    const auto probe = reader.read(dir.wstring());
    SPACELENS_REQUIRE_EQ(probe.outcome, CleanupMetadataProbeOutcome::Present);
    SPACELENS_REQUIRE(probe.objectEvidence.available);
    SPACELENS_REQUIRE_EQ(probe.objectEvidence.kind, ItemKind::Directory);
    SPACELENS_REQUIRE_EQ(probe.objectEvidence.sizeScope,
                         CleanupEvidenceScope::Direct);
    SPACELENS_REQUIRE_EQ(probe.objectEvidence.identity.source,
                         CleanupIdentitySource::FileId128);
    SPACELENS_REQUIRE(isStrongIdentity(probe.objectEvidence.identity));
    // Direct handle size is not the recursive child total.
    SPACELENS_REQUIRE(probe.objectEvidence.logicalSize != 64ULL);

    const auto current =
        currentEvidenceFromProbe(probe, classifyLocation(dir.wstring()));
    SPACELENS_REQUIRE(!current.directoryAggregate.available);
    SPACELENS_REQUIRE(!current.directoryAggregate.revalidated);
}

SPACELENS_TEST(Windows_CleanupRevalidation_delete_recreate_identity_mismatch)
{
    TempRoot root;
    if (!root.created) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_delete_recreate_"
                     "identity_mismatch — fixture cannot be created\n";
        return;
    }
    const auto file = root.path / "recycle.bin";
    if (!writeBytes(file, 32)) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_delete_recreate_"
                     "identity_mismatch — fixture cannot be created\n";
        return;
    }

    WindowsCleanupMetadataReader reader;
    const auto first = reader.read(file.wstring());
    SPACELENS_REQUIRE_EQ(first.outcome, CleanupMetadataProbeOutcome::Present);
    SPACELENS_REQUIRE(isIdentityAvailable(first.objectEvidence.identity));

    std::error_code ec;
    fs::remove(file, ec);
    if (ec || !writeBytes(file, 32)) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_delete_recreate_"
                     "identity_mismatch — fixture cannot be created\n";
        return;
    }

    auto captured = capturedFromProbe(file.wstring(), first);
    const auto result = revalidateCleanupCandidate(captured, reader);
    SPACELENS_REQUIRE_EQ(result.probe.outcome, CleanupMetadataProbeOutcome::Present);
    SPACELENS_REQUIRE(!identitiesEqual(first.objectEvidence.identity,
                                       result.current.objectEvidence.identity));
    SPACELENS_REQUIRE_EQ(result.validation.state,
                         CleanupValidationState::IdentityChanged);
}

SPACELENS_TEST(Windows_CleanupRevalidation_unicode_path)
{
    TempRoot root;
    if (!root.created) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_unicode_path — "
                     "fixture cannot be created\n";
        return;
    }
    const auto file = root.path / L"测试-файл.bin";
    if (!writeBytes(file, 17)) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_unicode_path — "
                     "fixture cannot be created\n";
        return;
    }

    WindowsCleanupMetadataReader reader;
    const auto probe = reader.read(file.wstring());
    SPACELENS_REQUIRE_EQ(probe.outcome, CleanupMetadataProbeOutcome::Present);
    SPACELENS_REQUIRE_EQ(probe.objectEvidence.logicalSize, 17ULL);
    SPACELENS_REQUIRE(isIdentityAvailable(probe.objectEvidence.identity));
}

SPACELENS_TEST(Windows_CleanupRevalidation_reparse_no_follow)
{
    TempRoot root;
    if (!root.created) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_reparse_no_follow — "
                     "fixture cannot be created\n";
        return;
    }

    const auto targetFile = root.path / "target.bin";
    const auto fileLink = root.path / "file.link";
    const auto targetDir = root.path / "target-dir";
    const auto dirLink = root.path / "dir.link";
    if (!writeBytes(targetFile, 48)) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_reparse_no_follow — "
                     "fixture cannot be created\n";
        return;
    }
    std::error_code ec;
    fs::create_directory(targetDir, ec);
    if (ec) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_reparse_no_follow — "
                     "fixture cannot be created\n";
        return;
    }

    const BOOL fileOk = ::CreateSymbolicLinkW(
        fileLink.c_str(), targetFile.c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    const BOOL dirOk = ::CreateSymbolicLinkW(
        dirLink.c_str(), targetDir.c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (!fileOk && !dirOk) {
        std::cout << "[ SKIP ] Windows_CleanupRevalidation_reparse_no_follow — "
                     "reparse fixture creation denied\n";
        return;
    }

    WindowsCleanupMetadataReader reader;
    if (fileOk) {
        const auto target = reader.read(targetFile.wstring());
        const auto link = reader.read(fileLink.wstring());
        SPACELENS_REQUIRE_EQ(link.outcome, CleanupMetadataProbeOutcome::Present);
        SPACELENS_REQUIRE(link.isReparse);
        SPACELENS_REQUIRE_EQ(link.objectEvidence.kind, ItemKind::File);
        SPACELENS_REQUIRE((link.objectEvidence.attributes &
                           FILE_ATTRIBUTE_REPARSE_POINT) != 0U);
        SPACELENS_REQUIRE(isIdentityAvailable(target.objectEvidence.identity));
        SPACELENS_REQUIRE(isIdentityAvailable(link.objectEvidence.identity));
        SPACELENS_REQUIRE(!identitiesEqual(target.objectEvidence.identity,
                                           link.objectEvidence.identity));
    }
    if (dirOk) {
        const auto target = reader.read(targetDir.wstring());
        const auto link = reader.read(dirLink.wstring());
        SPACELENS_REQUIRE_EQ(link.outcome, CleanupMetadataProbeOutcome::Present);
        SPACELENS_REQUIRE(link.isReparse);
        SPACELENS_REQUIRE_EQ(link.objectEvidence.kind, ItemKind::ReparseDirectory);
        SPACELENS_REQUIRE((link.objectEvidence.attributes &
                           FILE_ATTRIBUTE_REPARSE_POINT) != 0U);
        SPACELENS_REQUIRE(isIdentityAvailable(target.objectEvidence.identity));
        SPACELENS_REQUIRE(isIdentityAvailable(link.objectEvidence.identity));
        SPACELENS_REQUIRE(!identitiesEqual(target.objectEvidence.identity,
                                           link.objectEvidence.identity));
    }
}
