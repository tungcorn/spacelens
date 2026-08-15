#pragma once

#include "core/FileTime.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace spacelens {

/// How snapshot age was derived. Not a live-filesystem freshness claim.
enum class SnapshotAgeState {
    Known,
    Unknown,
    ClockSkew
};

enum class SnapshotPublishKind {
    Unknown,
    Full,
    Incremental
};

enum class IndexAgeGateResult {
    NotRequested,
    Satisfied,
    TooOld,
    FreshnessUnknown
};

/// Persisted publication inputs. lastRefreshAtTicks wins when non-zero.
struct IndexPublishMetadata {
    FileTimeTicks rootIndexedAtTicks = 0;
    std::string rootIndexedAtIso;
    FileTimeTicks lastRefreshAtTicks = 0;
    std::string lastRefreshMethod;
    FileTimeTicks fullIndexedAtTicks = 0;
};

/// Objective age of one published index generation. Exact != live.
struct IndexSnapshotEvidence {
    SnapshotAgeState ageState = SnapshotAgeState::Unknown;
    SnapshotPublishKind publishKind = SnapshotPublishKind::Unknown;
    FileTimeTicks publishedAtTicks = 0;
    std::string publishedAtUtc;
    std::string fullIndexedAtUtc;
    std::optional<std::uint64_t> ageSeconds;
    std::uint64_t ageMs = 0;
    std::string basis = "published_snapshot";
};

struct IndexAgeDecision {
    IndexAgeGateResult result = IndexAgeGateResult::NotRequested;
    IndexSnapshotEvidence evidence{};
    std::optional<std::uint64_t> requestedMaxAgeSeconds;
};

[[nodiscard]] const char* toString(SnapshotAgeState state) noexcept;
[[nodiscard]] const char* toString(SnapshotPublishKind kind) noexcept;
[[nodiscard]] const char* toString(IndexAgeGateResult result) noexcept;

/// Published snapshot = last successful refresh, else full-build indexed_at.
/// nowTicks == 0 or published == 0 → Unknown. published > now → ClockSkew.
/// Age is never clamped to zero to look fresh.
[[nodiscard]] IndexSnapshotEvidence evaluateIndexSnapshot(
    const IndexPublishMetadata& meta, FileTimeTicks nowTicks);

/// age <= max → Satisfied. Unknown / clock skew + max-age → FreshnessUnknown.
[[nodiscard]] IndexAgeDecision evaluateIndexAgeGate(
    const IndexSnapshotEvidence& evidence,
    std::optional<std::uint64_t> maxAgeSeconds);

/// Compact `{...}` freshness object for indexed JSON. Additive; no `fresh`.
[[nodiscard]] std::string indexFreshnessJsonObject(
    const IndexSnapshotEvidence& evidence, const IndexAgeDecision& decision);

[[nodiscard]] std::string formatSnapshotAgeHuman(
    const IndexSnapshotEvidence& evidence);

[[nodiscard]] std::string formatIndexAgeGateError(
    const IndexAgeDecision& decision);

}  // namespace spacelens
