#pragma once

#include "core/CleanupReview.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

inline constexpr int kCleanupPlanSchemaVersion = 1;

struct CleanupPlanOptions {
    /// If omitted, the builder uses a deterministic epoch value so exports are
    /// reproducible in tests and for copied reports.
    std::string generatedAt;

    /// Explicit profile root used only while rendering text/JSON exports.
    /// Stored candidate paths are never modified.
    std::wstring userProfilePath;
};

struct CleanupPlanSummary {
    std::size_t selectedCount = 0;
    std::size_t itemCount = 0;
    std::size_t includedCount = 0;
    std::size_t suppressedCount = 0;
    std::size_t conflictCount = 0;
    ByteSize rawLogicalBytes = 0;
    ByteSize uniqueLogicalBytes = 0;
    bool rawSumSaturated = false;
    bool uniqueSumSaturated = false;
    bool estimated = false;
    std::vector<std::string> estimatedReasons;
};

struct CleanupPlanItem {
    std::uint64_t id = 0;
    std::wstring path;
    ItemKind kind = ItemKind::File;

    CleanupCandidate candidate;
    CleanupObjectEvidence capturedEvidence;
    CleanupDirectoryAggregateEvidence capturedDirectoryAggregate;
    CleanupCurrentEvidence currentEvidence;
    CleanupValidation validation;

    ByteSize capturedLogicalBytes = 0;
    std::optional<ByteSize> currentLogicalBytes;
    std::optional<ByteSize> comparableCurrentLogicalBytes;

    Classification classification{};
    LocationSafety safety = LocationSafety::Unknown;
    Reclaimability reclaimability = Reclaimability::Unknown;
    CandidateStrength candidateStrength = CandidateStrength::None;
    std::string source;

    bool included = true;
    bool suppressed = false;
    bool conflict = false;
    std::uint64_t suppressedById = 0;
    std::vector<std::string> planningReasons;
};

struct CleanupPlan {
    int planSchemaVersion = kCleanupPlanSchemaVersion;
    std::string generatedAt = "1970-01-01T00:00:00Z";
    CleanupPlanSummary summary;
    std::vector<std::uint64_t> suppressedIds;
    std::vector<std::uint64_t> conflictIds;
    std::vector<CleanupPlanItem> items;

    [[nodiscard]] ByteSize uniqueSelectedLogicalSize() const noexcept
    {
        return summary.uniqueLogicalBytes;
    }
    [[nodiscard]] ByteSize uniqueLogicalBytes() const noexcept
    {
        return summary.uniqueLogicalBytes;
    }
    [[nodiscard]] ByteSize rawSelectedLogicalSize() const noexcept
    {
        return summary.rawLogicalBytes;
    }
    [[nodiscard]] ByteSize rawLogicalBytes() const noexcept
    {
        return summary.rawLogicalBytes;
    }

    /// UTF-8 human report. This describes a plan only and grants no authority.
    [[nodiscard]] std::string toText() const;
    [[nodiscard]] std::string toText(const CleanupPlanOptions& options) const;
    [[nodiscard]] std::string text() const { return toText(); }
    [[nodiscard]] std::string humanReport() const { return toText(); }

    /// Deterministic UTF-8 JSON export. Options are useful when the same plan
    /// is exported for different injected profile roots.
    [[nodiscard]] std::string toJson(const CleanupPlanOptions& options = {}) const;
    [[nodiscard]] std::string json(const CleanupPlanOptions& options = {}) const
    {
        return toJson(options);
    }
};

[[nodiscard]] CleanupPlan buildCleanupPlan(
    const CleanupReview& review,
    const CleanupPlanOptions& options = {});

[[nodiscard]] inline CleanupPlan makeCleanupPlan(
    const CleanupReview& review,
    const CleanupPlanOptions& options = {})
{
    return buildCleanupPlan(review, options);
}

[[nodiscard]] std::wstring redactUserProfilePath(
    std::wstring_view path,
    std::wstring_view userProfilePath);

[[nodiscard]] std::string cleanupPlanGeneratedAt(
    const CleanupPlanOptions& options);

[[nodiscard]] std::string cleanupPlanText(
    const CleanupReview& review,
    const CleanupPlanOptions& options = {});

[[nodiscard]] std::string cleanupPlanJson(
    const CleanupReview& review,
    const CleanupPlanOptions& options = {});

}  // namespace spacelens
