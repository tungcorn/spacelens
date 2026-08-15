#pragma once

#include "core/ReclaimEvidence.hpp"
#include "core/index/IndexSnapshot.hpp"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

enum class ReclaimPlanSource {
    Auto,
    PersistentIndex,
    LiveScan
};

[[nodiscard]] const char* toString(ReclaimPlanSource source) noexcept;
[[nodiscard]] bool parseReclaimPlanSource(std::string_view text,
                                          ReclaimPlanSource& out) noexcept;

struct ReclaimPlanRequest {
    std::wstring root;
    ReclaimPlanSource source = ReclaimPlanSource::Auto;
    std::size_t limit = 20;
    std::optional<ByteSize> targetFreeBytes;
    std::optional<std::uint64_t> maxIndexAgeSeconds;
    FileTimeTicks nowTicks = 0;
};

struct ReclaimPlanReport {
    bool ok = false;
    std::string error;
    std::string state = "failed";
    ReclaimPlanSource sourceUsed = ReclaimPlanSource::LiveScan;
    std::wstring root;
    bool physicalAccounting = false;
    HardLinkCoverage overallCoverage = HardLinkCoverage::Unknown;
    std::optional<ByteSize> targetFreeBytes;
    bool targetMet = false;
    std::vector<ReclaimCandidateEvidence> actionable;
    std::vector<ReclaimCandidateEvidence> reviewOnly;
    std::vector<ReclaimCandidateEvidence> selected;
    std::optional<ByteSize> exactHostReclaimBytes;
    std::optional<ByteSize> actionableHostReclaimBytes;
    std::optional<ByteSize> selectedHostReclaimBytes;
    IndexSnapshotEvidence snapshot{};
    IndexAgeDecision ageDecision{};
    bool planningOnly = true;
    bool executionSupported = false;
    std::string providerProbeDetail;

    [[nodiscard]] std::string toJson() const;
};

[[nodiscard]] ReclaimPlanReport buildReclaimPlan(const ReclaimPlanRequest& request,
                                                 std::stop_token stop = {});

}  // namespace spacelens
