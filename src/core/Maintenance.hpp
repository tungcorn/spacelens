#pragma once

#include "core/CleanupRevalidation.hpp"
#include "core/CleanupReview.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

inline constexpr int kMaintenanceSchemaVersion = 1;

enum class MaintenanceBlockReason {
    None,
    UnsupportedType,
    ReparsePoint,
    IdentityUnavailable,
    IdentityMismatch,
    ChangedSinceReview,
    Missing,
    AccessDenied,
    Protected,
    Sensitive,
    UnknownLocation,
    EmptyPath,
    RootPath,
    RecycleUnavailable,
    SameIdentityAlreadySelected,
    AlreadyRecycled,
    ProbeError,
    RequiresElevation
};

[[nodiscard]] const char* toString(MaintenanceBlockReason reason) noexcept;

enum class MaintenanceItemResult {
    Recycled,
    BlockedPreflight,
    BlockedFinalGuard,
    Cancelled,
    NotAttempted,
    AccessDenied,
    ShellError,
    OperationAborted,
    UnexpectedPermanentRemoval,
    UnknownResult
};

[[nodiscard]] const char* toString(MaintenanceItemResult result) noexcept;

struct MaintenancePlanItem {
    std::uint64_t reviewId = 0;
    std::wstring path;
    CleanupIdentity expectedIdentity{};
    ByteSize logicalSize = 0;
    FileTimeTicks lastWrite = 0;
    std::uint32_t attributes = 0;
    LocationSafety safety = LocationSafety::Unknown;
    bool eligible = false;
    MaintenanceBlockReason blockReason = MaintenanceBlockReason::None;
    std::string detail;
};

struct MaintenancePlan {
    std::string generatedAt;
    std::uint64_t selectedCount = 0;
    std::uint64_t eligibleCount = 0;
    std::uint64_t blockedCount = 0;
    ByteSize selectedLogicalBytes = 0;
    ByteSize eligibleLogicalBytes = 0;
    bool selectedBytesSaturated = false;
    bool eligibleBytesSaturated = false;
    std::vector<MaintenancePlanItem> items;

    [[nodiscard]] std::vector<const MaintenancePlanItem*> eligibleItems() const;
    [[nodiscard]] std::vector<const MaintenancePlanItem*> blockedItems() const;
};

struct MaintenanceItemReceipt {
    std::uint64_t reviewId = 0;
    std::wstring path;
    CleanupIdentity expectedIdentity{};
    MaintenanceItemResult result = MaintenanceItemResult::UnknownResult;
    MaintenanceBlockReason blockReason = MaintenanceBlockReason::None;
    std::int32_t hresult = 0;
    std::uint32_t nativeError = 0;
    std::string recycleParsingName;
    std::string detail;
};

struct MaintenanceReceipt {
    std::uint64_t operationId = 0;
    FileTimeTicks requestedAt = 0;
    FileTimeTicks confirmedAt = 0;
    FileTimeTicks completedAt = 0;
    std::uint64_t attempted = 0;
    std::uint64_t recycled = 0;
    std::uint64_t blocked = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;
    ByteSize recycledLogicalBytes = 0;
    bool unexpectedPermanentRemoval = false;
    std::vector<MaintenanceItemReceipt> items;
};

/// Pure eligibility against a live probe. Classification/reclaimability are
/// ignored. Strong FileId128 identity is required; path equality is not enough.
[[nodiscard]] MaintenancePlanItem evaluateMaintenanceEligibility(
    const CleanupCandidate& candidate,
    const CleanupMetadataProbe& probe,
    LocationSafety currentSafety);

[[nodiscard]] MaintenancePlanItem evaluateMaintenanceEligibility(
    const CleanupCandidate& candidate,
    ICleanupMetadataReader& reader);

/// Fresh live preflight for the selected review rows. Deduplicates executable
/// work by strong identity (first stable review id wins). Directories never
/// become eligible.
[[nodiscard]] MaintenancePlan prepareMaintenancePlan(
    const CleanupReview& review,
    const std::vector<std::uint64_t>& selectedIds,
    ICleanupMetadataReader& reader,
    const std::string& generatedAt = {});

/// Downgrade eligible items when the Recycle Bin is unavailable for that path.
/// `canRecycle` is supplied by the GUI adapter so core stays Shell-free.
void applyRecycleAvailability(
    MaintenancePlan& plan,
    const std::function<bool(const MaintenancePlanItem&, std::string*)>&
        canRecycle);

/// Final identity/safety guard immediately before a recycle attempt.
[[nodiscard]] MaintenanceBlockReason evaluateMaintenanceFinalGuard(
    const MaintenancePlanItem& planned,
    const CleanupMetadataProbe& probe,
    LocationSafety currentSafety);

struct IRecycleOperation {
    virtual ~IRecycleOperation() = default;
    [[nodiscard]] virtual MaintenanceItemReceipt recycle(
        const MaintenancePlanItem& item) = 0;
};

/// Sequential execute. Re-probes each eligible item, then calls `recycle` only
/// when the final guard passes. Cancellation applies before the next item.
[[nodiscard]] MaintenanceReceipt executeMaintenancePlan(
    const MaintenancePlan& plan,
    ICleanupMetadataReader& reader,
    IRecycleOperation& recycle,
    FileTimeTicks confirmedAt,
    const std::function<bool()>& cancelled = {});

}  // namespace spacelens
