#pragma once

#include "core/CleanupRevalidation.hpp"
#include "core/CleanupReview.hpp"
#include "core/OrdinaryLocation.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

/// Additive maintenance tables in state.db. review_schema_version stays 1.
inline constexpr int kMaintenanceSchemaVersion = 2;

/// Extra stale-plan guard. Time never replaces identity/metadata/final guard.
inline constexpr FileTimeTicks kMaintenancePlanMaxAgeTicks =
    2ULL * 60ULL * kFileTimeTicksPerSecond;

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
    RequiresElevation,
    UncertainPriorOutcome
};

[[nodiscard]] const char* toString(MaintenanceBlockReason reason) noexcept;
[[nodiscard]] MaintenanceBlockReason parseMaintenanceBlockReason(
    std::string_view text) noexcept;

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
    Attempting,
    Uncertain,
    UnknownResult
};

[[nodiscard]] const char* toString(MaintenanceItemResult result) noexcept;
[[nodiscard]] MaintenanceItemResult parseMaintenanceItemResult(
    std::string_view text) noexcept;

enum class MaintenanceOperationStatus {
    Executing,
    Completed,
    Cancelled,
    HardStopped,
    Uncertain
};

[[nodiscard]] const char* toString(MaintenanceOperationStatus status) noexcept;
[[nodiscard]] MaintenanceOperationStatus parseMaintenanceOperationStatus(
    std::string_view text) noexcept;

enum class MaintenanceConfirmGate {
    Ok,
    NotPrepared,
    AlreadyExecuting,
    StalePlan
};

[[nodiscard]] MaintenanceConfirmGate evaluateMaintenanceConfirmGate(
    bool running,
    bool executing,
    bool awaitingConfirm,
    FileTimeTicks preparedAt,
    FileTimeTicks now,
    FileTimeTicks maxAge = kMaintenancePlanMaxAgeTicks) noexcept;

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
    FileTimeTicks preparedAt = 0;
    std::uint64_t locationPolicyGeneration = 0;
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
    MaintenanceOperationStatus status = MaintenanceOperationStatus::Completed;
    FileTimeTicks requestedAt = 0;
    FileTimeTicks confirmedAt = 0;
    FileTimeTicks completedAt = 0;
    std::uint64_t selectedCount = 0;
    std::uint64_t eligibleCount = 0;
    std::uint64_t attempted = 0;
    std::uint64_t recycled = 0;
    std::uint64_t blocked = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;
    std::uint64_t uncertain = 0;
    ByteSize selectedLogicalBytes = 0;
    ByteSize eligibleLogicalBytes = 0;
    ByteSize recycledLogicalBytes = 0;
    bool unexpectedPermanentRemoval = false;
    std::vector<MaintenanceItemReceipt> items;
};

[[nodiscard]] bool isMaintenancePlanStale(
    FileTimeTicks preparedAt,
    FileTimeTicks now,
    FileTimeTicks maxAge = kMaintenancePlanMaxAgeTicks) noexcept;

[[nodiscard]] bool identityHasUncertainHistory(
    const CleanupIdentity& identity,
    const std::vector<MaintenanceReceipt>& history) noexcept;

/// Pure eligibility against a live probe. Classification/reclaimability are
/// ignored. Strong FileId128 identity is required; path equality is not enough.
[[nodiscard]] MaintenancePlanItem evaluateMaintenanceEligibility(
    const CleanupCandidate& candidate,
    const CleanupMetadataProbe& probe,
    LocationSafety currentSafety);

[[nodiscard]] MaintenancePlanItem evaluateMaintenanceEligibility(
    const CleanupCandidate& candidate,
    ICleanupMetadataReader& reader,
    const OrdinaryLocationPolicy& locationPolicy = {});

/// Fresh live preflight for the selected review rows. Deduplicates executable
/// work by strong identity (first stable review id wins). Directories never
/// become eligible. History with Attempting/Uncertain for the same strong
/// identity blocks as UncertainPriorOutcome.
[[nodiscard]] MaintenancePlan prepareMaintenancePlan(
    const CleanupReview& review,
    const std::vector<std::uint64_t>& selectedIds,
    ICleanupMetadataReader& reader,
    const std::string& generatedAt = {},
    const OrdinaryLocationPolicy& locationPolicy = {},
    const std::vector<MaintenanceReceipt>& history = {});

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

/// Small durable checkpoints. Never hold a write txn across Shell or dialogs.
struct IMaintenanceJournal {
    virtual ~IMaintenanceJournal() = default;
    [[nodiscard]] virtual bool checkpointItem(
        std::uint64_t operationId,
        const MaintenanceItemReceipt& item,
        ByteSize recycledLogicalBytes = 0) = 0;
};

/// Sequential execute. Re-probes each eligible item, persists Attempting before
/// recycle when a journal is provided, then calls `recycle` only when the final
/// guard passes. Cancellation applies before the next item.
[[nodiscard]] MaintenanceReceipt executeMaintenancePlan(
    const MaintenancePlan& plan,
    ICleanupMetadataReader& reader,
    IRecycleOperation& recycle,
    FileTimeTicks confirmedAt,
    const std::function<bool()>& cancelled = {},
    const OrdinaryLocationPolicy& locationPolicy = {},
    IMaintenanceJournal* journal = nullptr,
    std::uint64_t operationId = 0,
    const std::function<bool(const MaintenancePlanItem&, std::string*)>&
        canRecycle = {});

}  // namespace spacelens
