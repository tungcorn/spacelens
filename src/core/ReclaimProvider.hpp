#pragma once

#include "core/ReclaimEvidence.hpp"

#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace spacelens {

struct ReclaimProbeInput {
    std::wstring path;
    std::wstring name;
    ItemKind kind = ItemKind::Directory;
    Classification classification{};
    LocationSafety safety = LocationSafety::Unknown;
    const std::wstring* childNames = nullptr;
    std::size_t childCount = 0;
    const std::wstring* siblingNames = nullptr;
    std::size_t siblingCount = 0;
};

struct ReclaimProviderHit {
    ReclaimOwnership ownership;
    ReclaimAction action;
    ReclaimActionability actionability =
        ReclaimActionability::InformationalOnly;
    ReclaimDisruption disruption = ReclaimDisruption::Review;
    ReclaimConfidence confidence = ReclaimConfidence::Heuristic;
    std::vector<std::string> reasonCodes;
};

class IReclaimProvider {
public:
    virtual ~IReclaimProvider() = default;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual std::optional<ReclaimProviderHit> classify(
        const ReclaimProbeInput& input) const = 0;
};

/// Optional read-only cache-location hints. Missing tools degrade to
/// well-known filesystem locations; they never fail the plan.
struct ReclaimProviderContext {
    std::wstring nugetGlobalPackages;
    std::wstring nugetHttpCache;
    std::wstring npmCache;
    std::wstring pipCache;
    bool nugetProbed = false;
    bool probeFailed = false;
    std::string probeDetail;
};

[[nodiscard]] ReclaimProviderContext probeProviderLocations(
    std::stop_token stop = {});

[[nodiscard]] std::optional<ReclaimProviderHit> classifyReclaimOwnership(
    const ReclaimProbeInput& input, const ReclaimProviderContext& context);

[[nodiscard]] bool isProtectedFromReclaim(LocationSafety safety) noexcept;

[[nodiscard]] bool isReviewOnlyClassification(StorageCategory category,
                                              std::wstring_view path,
                                              std::wstring_view name);

}  // namespace spacelens
