#include "core/Maintenance.hpp"

#include "core/SafetyPolicy.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

#ifndef FILE_ATTRIBUTE_REPARSE_POINT
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#endif

namespace spacelens {
namespace {

bool isReparseAttributes(std::uint32_t attributes) noexcept
{
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}

bool isEmptyOrDotPath(std::wstring_view path) noexcept
{
    const auto normalized = normalizeCleanupPath(path);
    return normalized.empty() || normalized == L"." || normalized == L"..";
}

bool isDriveRootPath(std::wstring_view path) noexcept
{
    const auto normalized = normalizePathForPolicy(path);
    return normalized.size() == 3 &&
           ((normalized[0] >= L'A' && normalized[0] <= L'Z') ||
            (normalized[0] >= L'a' && normalized[0] <= L'z')) &&
           normalized[1] == L':' && normalized[2] == L'\\';
}

bool metadataMatches(const CleanupObjectEvidence& expected,
                     const CleanupObjectEvidence& live) noexcept
{
    return expected.available && live.available &&
           expected.logicalSize == live.logicalSize &&
           expected.lastWriteTime == live.lastWriteTime &&
           expected.attributes == live.attributes;
}

void addSaturating(ByteSize& total, ByteSize value, bool& saturated) noexcept
{
    if (value > std::numeric_limits<ByteSize>::max() - total) {
        total = std::numeric_limits<ByteSize>::max();
        saturated = true;
        return;
    }
    total += value;
}

MaintenanceBlockReason locationBlock(LocationSafety safety) noexcept
{
    switch (safety) {
    case LocationSafety::Ordinary:
        return MaintenanceBlockReason::None;
    case LocationSafety::Protected:
        return MaintenanceBlockReason::Protected;
    case LocationSafety::Sensitive:
        return MaintenanceBlockReason::Sensitive;
    case LocationSafety::Unknown:
        return MaintenanceBlockReason::UnknownLocation;
    }
    return MaintenanceBlockReason::UnknownLocation;
}

}  // namespace

const char* toString(MaintenanceBlockReason reason) noexcept
{
    switch (reason) {
    case MaintenanceBlockReason::None:
        return "None";
    case MaintenanceBlockReason::UnsupportedType:
        return "UnsupportedType";
    case MaintenanceBlockReason::ReparsePoint:
        return "ReparsePoint";
    case MaintenanceBlockReason::IdentityUnavailable:
        return "IdentityUnavailable";
    case MaintenanceBlockReason::IdentityMismatch:
        return "IdentityMismatch";
    case MaintenanceBlockReason::ChangedSinceReview:
        return "ChangedSinceReview";
    case MaintenanceBlockReason::Missing:
        return "Missing";
    case MaintenanceBlockReason::AccessDenied:
        return "AccessDenied";
    case MaintenanceBlockReason::Protected:
        return "Protected";
    case MaintenanceBlockReason::Sensitive:
        return "Sensitive";
    case MaintenanceBlockReason::UnknownLocation:
        return "UnknownLocation";
    case MaintenanceBlockReason::EmptyPath:
        return "EmptyPath";
    case MaintenanceBlockReason::RootPath:
        return "RootPath";
    case MaintenanceBlockReason::RecycleUnavailable:
        return "RecycleUnavailable";
    case MaintenanceBlockReason::SameIdentityAlreadySelected:
        return "SameIdentityAlreadySelected";
    case MaintenanceBlockReason::AlreadyRecycled:
        return "AlreadyRecycled";
    case MaintenanceBlockReason::ProbeError:
        return "ProbeError";
    case MaintenanceBlockReason::RequiresElevation:
        return "RequiresElevation";
    case MaintenanceBlockReason::UncertainPriorOutcome:
        return "UncertainPriorOutcome";
    }
    return "ProbeError";
}

MaintenanceBlockReason parseMaintenanceBlockReason(std::string_view text) noexcept
{
    if (text == "UnsupportedType") {
        return MaintenanceBlockReason::UnsupportedType;
    }
    if (text == "ReparsePoint") {
        return MaintenanceBlockReason::ReparsePoint;
    }
    if (text == "IdentityUnavailable") {
        return MaintenanceBlockReason::IdentityUnavailable;
    }
    if (text == "IdentityMismatch") {
        return MaintenanceBlockReason::IdentityMismatch;
    }
    if (text == "ChangedSinceReview") {
        return MaintenanceBlockReason::ChangedSinceReview;
    }
    if (text == "Missing") {
        return MaintenanceBlockReason::Missing;
    }
    if (text == "AccessDenied") {
        return MaintenanceBlockReason::AccessDenied;
    }
    if (text == "Protected") {
        return MaintenanceBlockReason::Protected;
    }
    if (text == "Sensitive") {
        return MaintenanceBlockReason::Sensitive;
    }
    if (text == "UnknownLocation") {
        return MaintenanceBlockReason::UnknownLocation;
    }
    if (text == "EmptyPath") {
        return MaintenanceBlockReason::EmptyPath;
    }
    if (text == "RootPath") {
        return MaintenanceBlockReason::RootPath;
    }
    if (text == "RecycleUnavailable") {
        return MaintenanceBlockReason::RecycleUnavailable;
    }
    if (text == "SameIdentityAlreadySelected") {
        return MaintenanceBlockReason::SameIdentityAlreadySelected;
    }
    if (text == "AlreadyRecycled") {
        return MaintenanceBlockReason::AlreadyRecycled;
    }
    if (text == "RequiresElevation") {
        return MaintenanceBlockReason::RequiresElevation;
    }
    if (text == "UncertainPriorOutcome") {
        return MaintenanceBlockReason::UncertainPriorOutcome;
    }
    if (text == "ProbeError") {
        return MaintenanceBlockReason::ProbeError;
    }
    return MaintenanceBlockReason::None;
}

const char* toString(MaintenanceItemResult result) noexcept
{
    switch (result) {
    case MaintenanceItemResult::Recycled:
        return "Recycled";
    case MaintenanceItemResult::BlockedPreflight:
        return "BlockedPreflight";
    case MaintenanceItemResult::BlockedFinalGuard:
        return "BlockedFinalGuard";
    case MaintenanceItemResult::Cancelled:
        return "Cancelled";
    case MaintenanceItemResult::NotAttempted:
        return "NotAttempted";
    case MaintenanceItemResult::AccessDenied:
        return "AccessDenied";
    case MaintenanceItemResult::ShellError:
        return "ShellError";
    case MaintenanceItemResult::OperationAborted:
        return "OperationAborted";
    case MaintenanceItemResult::UnexpectedPermanentRemoval:
        return "UnexpectedPermanentRemoval";
    case MaintenanceItemResult::Attempting:
        return "Attempting";
    case MaintenanceItemResult::Uncertain:
        return "Uncertain";
    case MaintenanceItemResult::UnknownResult:
        return "UnknownResult";
    }
    return "UnknownResult";
}

MaintenanceItemResult parseMaintenanceItemResult(std::string_view text) noexcept
{
    if (text == "Recycled") {
        return MaintenanceItemResult::Recycled;
    }
    if (text == "BlockedPreflight") {
        return MaintenanceItemResult::BlockedPreflight;
    }
    if (text == "BlockedFinalGuard") {
        return MaintenanceItemResult::BlockedFinalGuard;
    }
    if (text == "Cancelled") {
        return MaintenanceItemResult::Cancelled;
    }
    if (text == "NotAttempted") {
        return MaintenanceItemResult::NotAttempted;
    }
    if (text == "AccessDenied") {
        return MaintenanceItemResult::AccessDenied;
    }
    if (text == "ShellError") {
        return MaintenanceItemResult::ShellError;
    }
    if (text == "OperationAborted") {
        return MaintenanceItemResult::OperationAborted;
    }
    if (text == "UnexpectedPermanentRemoval") {
        return MaintenanceItemResult::UnexpectedPermanentRemoval;
    }
    if (text == "Attempting") {
        return MaintenanceItemResult::Attempting;
    }
    if (text == "Uncertain") {
        return MaintenanceItemResult::Uncertain;
    }
    return MaintenanceItemResult::UnknownResult;
}

const char* toString(MaintenanceOperationStatus status) noexcept
{
    switch (status) {
    case MaintenanceOperationStatus::Executing:
        return "Executing";
    case MaintenanceOperationStatus::Completed:
        return "Completed";
    case MaintenanceOperationStatus::Cancelled:
        return "Cancelled";
    case MaintenanceOperationStatus::HardStopped:
        return "HardStopped";
    case MaintenanceOperationStatus::Uncertain:
        return "Uncertain";
    }
    return "Uncertain";
}

MaintenanceOperationStatus parseMaintenanceOperationStatus(
    std::string_view text) noexcept
{
    if (text == "Executing") {
        return MaintenanceOperationStatus::Executing;
    }
    if (text == "Completed") {
        return MaintenanceOperationStatus::Completed;
    }
    if (text == "Cancelled") {
        return MaintenanceOperationStatus::Cancelled;
    }
    if (text == "HardStopped") {
        return MaintenanceOperationStatus::HardStopped;
    }
    return MaintenanceOperationStatus::Uncertain;
}

bool isMaintenancePlanStale(FileTimeTicks preparedAt,
                            FileTimeTicks now,
                            FileTimeTicks maxAge) noexcept
{
    if (preparedAt == 0 || now == 0 || maxAge == 0) {
        return true;
    }
    if (now < preparedAt) {
        return true;
    }
    return (now - preparedAt) > maxAge;
}

MaintenanceConfirmGate evaluateMaintenanceConfirmGate(
    bool running,
    bool executing,
    bool awaitingConfirm,
    FileTimeTicks preparedAt,
    FileTimeTicks now,
    FileTimeTicks maxAge) noexcept
{
    if (executing) {
        return MaintenanceConfirmGate::AlreadyExecuting;
    }
    if (!running || !awaitingConfirm) {
        return MaintenanceConfirmGate::NotPrepared;
    }
    if (isMaintenancePlanStale(preparedAt, now, maxAge)) {
        return MaintenanceConfirmGate::StalePlan;
    }
    return MaintenanceConfirmGate::Ok;
}

bool identityHasUncertainHistory(
    const CleanupIdentity& identity,
    const std::vector<MaintenanceReceipt>& history) noexcept
{
    if (!isStrongIdentity(identity)) {
        return false;
    }
    for (const auto& receipt : history) {
        for (const auto& item : receipt.items) {
            if ((item.result == MaintenanceItemResult::Uncertain ||
                 item.result == MaintenanceItemResult::Attempting) &&
                identitiesEqual(item.expectedIdentity, identity)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<const MaintenancePlanItem*> MaintenancePlan::eligibleItems() const
{
    std::vector<const MaintenancePlanItem*> out;
    out.reserve(items.size());
    for (const auto& item : items) {
        if (item.eligible) {
            out.push_back(&item);
        }
    }
    return out;
}

std::vector<const MaintenancePlanItem*> MaintenancePlan::blockedItems() const
{
    std::vector<const MaintenancePlanItem*> out;
    out.reserve(items.size());
    for (const auto& item : items) {
        if (!item.eligible) {
            out.push_back(&item);
        }
    }
    return out;
}

MaintenancePlanItem evaluateMaintenanceEligibility(
    const CleanupCandidate& candidate,
    const CleanupMetadataProbe& probe,
    LocationSafety currentSafety)
{
    MaintenancePlanItem item;
    item.reviewId = candidate.id;
    item.path = candidate.path;
    item.expectedIdentity = identityOf(candidate);
    item.logicalSize = candidate.objectEvidence.available
                           ? candidate.objectEvidence.logicalSize
                           : candidate.sizeAtSelection;
    item.lastWrite = candidate.objectEvidence.available
                         ? candidate.objectEvidence.lastWriteTime
                         : candidate.lastWriteTime;
    item.attributes = candidate.objectEvidence.available
                          ? candidate.objectEvidence.attributes
                          : candidate.attributes;
    item.safety = currentSafety;

    if (candidate.lifecycle == CleanupItemLifecycle::Recycled) {
        item.blockReason = MaintenanceBlockReason::AlreadyRecycled;
        item.detail = "Review item was already recycled";
        return item;
    }
    if (isEmptyOrDotPath(candidate.path)) {
        item.blockReason = MaintenanceBlockReason::EmptyPath;
        item.detail = "Empty or relative path is not eligible";
        return item;
    }
    if (isDriveRootPath(candidate.path)) {
        item.blockReason = MaintenanceBlockReason::RootPath;
        item.detail = "Drive roots are not eligible";
        return item;
    }

    if (candidate.kind != ItemKind::File) {
        item.blockReason = MaintenanceBlockReason::UnsupportedType;
        item.detail = "Directory maintenance is not supported in V1. "
                      "Review its contents individually.";
        return item;
    }

    const auto location = locationBlock(currentSafety);
    if (location != MaintenanceBlockReason::None) {
        item.blockReason = location;
        item.detail = "Location safety forbids maintenance";
        return item;
    }

    if (!isStrongIdentity(item.expectedIdentity)) {
        item.blockReason = MaintenanceBlockReason::IdentityUnavailable;
        item.detail = "Strong FileId128 identity is required";
        return item;
    }

    switch (probe.outcome) {
    case CleanupMetadataProbeOutcome::Missing:
        item.blockReason = MaintenanceBlockReason::Missing;
        item.detail = probe.detail.empty() ? "Path is missing" : probe.detail;
        return item;
    case CleanupMetadataProbeOutcome::AccessDenied:
        item.blockReason = MaintenanceBlockReason::AccessDenied;
        item.detail = probe.detail.empty() ? "Access denied" : probe.detail;
        return item;
    case CleanupMetadataProbeOutcome::ProbeError:
        item.blockReason = MaintenanceBlockReason::ProbeError;
        item.detail = probe.detail.empty() ? "Metadata probe failed" : probe.detail;
        return item;
    case CleanupMetadataProbeOutcome::Present:
        break;
    }

    if (!probe.objectEvidence.available) {
        item.blockReason = MaintenanceBlockReason::ProbeError;
        item.detail = "Live object evidence is unavailable";
        return item;
    }

    if (probe.objectEvidence.kind != ItemKind::File) {
        item.blockReason = MaintenanceBlockReason::UnsupportedType;
        item.detail = "Live object is not a regular file";
        return item;
    }

    if (probe.isReparse || isReparseAttributes(probe.objectEvidence.attributes) ||
        isReparseAttributes(item.attributes)) {
        item.blockReason = MaintenanceBlockReason::ReparsePoint;
        item.detail = "Reparse points are not recycled";
        return item;
    }

    if (!isStrongIdentity(probe.objectEvidence.identity)) {
        item.blockReason = MaintenanceBlockReason::IdentityUnavailable;
        item.detail = "Live strong identity is unavailable";
        return item;
    }

    if (!identitiesEqual(item.expectedIdentity, probe.objectEvidence.identity)) {
        item.blockReason = MaintenanceBlockReason::IdentityMismatch;
        item.detail = "Live identity does not match the reviewed object";
        return item;
    }

    if (!metadataMatches(candidate.objectEvidence, probe.objectEvidence) &&
        !(candidate.objectEvidence.available == false &&
          candidate.sizeAtSelection == probe.objectEvidence.logicalSize &&
          candidate.lastWriteTime == probe.objectEvidence.lastWriteTime &&
          candidate.attributes == probe.objectEvidence.attributes)) {
        if (candidate.objectEvidence.available &&
            !metadataMatches(candidate.objectEvidence, probe.objectEvidence)) {
            item.blockReason = MaintenanceBlockReason::ChangedSinceReview;
            item.detail = "Size, write time, or attributes changed since review";
            return item;
        }
        if (!candidate.objectEvidence.available &&
            (candidate.sizeAtSelection != probe.objectEvidence.logicalSize ||
             candidate.lastWriteTime != probe.objectEvidence.lastWriteTime)) {
            item.blockReason = MaintenanceBlockReason::ChangedSinceReview;
            item.detail = "Size or write time changed since review";
            return item;
        }
    }

    item.logicalSize = probe.objectEvidence.logicalSize;
    item.lastWrite = probe.objectEvidence.lastWriteTime;
    item.attributes = probe.objectEvidence.attributes;
    item.eligible = true;
    item.blockReason = MaintenanceBlockReason::None;
    return item;
}

MaintenancePlanItem evaluateMaintenanceEligibility(
    const CleanupCandidate& candidate,
    ICleanupMetadataReader& reader,
    const OrdinaryLocationPolicy& locationPolicy)
{
    const auto probe = reader.read(candidate.path);
    return evaluateMaintenanceEligibility(
        candidate, probe, effectiveLocationSafety(candidate.path, locationPolicy));
}

MaintenancePlan prepareMaintenancePlan(
    const CleanupReview& review,
    const std::vector<std::uint64_t>& selectedIds,
    ICleanupMetadataReader& reader,
    const std::string& generatedAt,
    const OrdinaryLocationPolicy& locationPolicy,
    const std::vector<MaintenanceReceipt>& history)
{
    MaintenancePlan plan;
    plan.generatedAt = generatedAt.empty() ? "1970-01-01T00:00:00Z" : generatedAt;
    plan.locationPolicyGeneration = locationPolicy.generation;

    std::vector<std::uint64_t> ordered = selectedIds;
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

    std::set<std::string> seenIdentities;
    for (const auto id : ordered) {
        const auto found = review.findById(id);
        if (!found) {
            MaintenancePlanItem missing;
            missing.reviewId = id;
            missing.blockReason = MaintenanceBlockReason::Missing;
            missing.detail = "Review item is no longer present";
            plan.items.push_back(std::move(missing));
            ++plan.selectedCount;
            ++plan.blockedCount;
            continue;
        }

        auto item = evaluateMaintenanceEligibility(*found, reader, locationPolicy);
        ++plan.selectedCount;
        addSaturating(plan.selectedLogicalBytes, item.logicalSize,
                      plan.selectedBytesSaturated);

        if (item.eligible && isStrongIdentity(item.expectedIdentity)) {
            std::string hexKey = toString(item.expectedIdentity.source);
            hexKey += ':';
            hexKey += std::to_string(item.expectedIdentity.volumeSerial);
            hexKey += ':';
            static const char kHex[] = "0123456789abcdef";
            for (const auto byte : item.expectedIdentity.fileId128) {
                hexKey.push_back(kHex[(byte >> 4U) & 0xFU]);
                hexKey.push_back(kHex[byte & 0xFU]);
            }
            if (!seenIdentities.insert(hexKey).second) {
                item.eligible = false;
                item.blockReason = MaintenanceBlockReason::SameIdentityAlreadySelected;
                item.detail =
                    "Another selected path is a hard-link alias of this file";
            } else if (identityHasUncertainHistory(item.expectedIdentity,
                                                   history)) {
                item.eligible = false;
                item.blockReason = MaintenanceBlockReason::UncertainPriorOutcome;
                item.detail =
                    "A prior Recycle Bin attempt for this identity is Uncertain";
            }
        }

        if (item.eligible) {
            ++plan.eligibleCount;
            addSaturating(plan.eligibleLogicalBytes, item.logicalSize,
                          plan.eligibleBytesSaturated);
        } else {
            ++plan.blockedCount;
        }
        plan.items.push_back(std::move(item));
    }
    return plan;
}

void applyRecycleAvailability(
    MaintenancePlan& plan,
    const std::function<bool(const MaintenancePlanItem&, std::string*)>&
        canRecycle)
{
    if (!canRecycle) {
        return;
    }
    plan.eligibleCount = 0;
    plan.blockedCount = 0;
    plan.eligibleLogicalBytes = 0;
    plan.eligibleBytesSaturated = false;
    for (auto& item : plan.items) {
        if (item.eligible) {
            std::string detail;
            if (!canRecycle(item, &detail)) {
                item.eligible = false;
                item.blockReason = MaintenanceBlockReason::RecycleUnavailable;
                item.detail = detail.empty()
                                  ? "Recycle Bin is not available for this path"
                                  : std::move(detail);
            }
        }
        if (item.eligible) {
            ++plan.eligibleCount;
            addSaturating(plan.eligibleLogicalBytes, item.logicalSize,
                          plan.eligibleBytesSaturated);
        } else {
            ++plan.blockedCount;
        }
    }
}

MaintenanceBlockReason evaluateMaintenanceFinalGuard(
    const MaintenancePlanItem& planned,
    const CleanupMetadataProbe& probe,
    LocationSafety currentSafety)
{
    if (!planned.eligible) {
        return planned.blockReason == MaintenanceBlockReason::None
                   ? MaintenanceBlockReason::ProbeError
                   : planned.blockReason;
    }
    if (isEmptyOrDotPath(planned.path) || isDriveRootPath(planned.path)) {
        return isEmptyOrDotPath(planned.path) ? MaintenanceBlockReason::EmptyPath
                                              : MaintenanceBlockReason::RootPath;
    }
    const auto location = locationBlock(currentSafety);
    if (location != MaintenanceBlockReason::None) {
        return location;
    }
    if (probe.outcome == CleanupMetadataProbeOutcome::Missing) {
        return MaintenanceBlockReason::Missing;
    }
    if (probe.outcome == CleanupMetadataProbeOutcome::AccessDenied) {
        return MaintenanceBlockReason::AccessDenied;
    }
    if (probe.outcome != CleanupMetadataProbeOutcome::Present ||
        !probe.objectEvidence.available) {
        return MaintenanceBlockReason::ProbeError;
    }
    if (probe.objectEvidence.kind != ItemKind::File) {
        return MaintenanceBlockReason::UnsupportedType;
    }
    if (probe.isReparse || isReparseAttributes(probe.objectEvidence.attributes)) {
        return MaintenanceBlockReason::ReparsePoint;
    }
    if (!isStrongIdentity(probe.objectEvidence.identity) ||
        !isStrongIdentity(planned.expectedIdentity) ||
        !identitiesEqual(planned.expectedIdentity, probe.objectEvidence.identity)) {
        return isStrongIdentity(probe.objectEvidence.identity)
                   ? MaintenanceBlockReason::IdentityMismatch
                   : MaintenanceBlockReason::IdentityUnavailable;
    }
    if (planned.logicalSize != probe.objectEvidence.logicalSize ||
        planned.lastWrite != probe.objectEvidence.lastWriteTime ||
        planned.attributes != probe.objectEvidence.attributes) {
        return MaintenanceBlockReason::ChangedSinceReview;
    }
    return MaintenanceBlockReason::None;
}

MaintenanceReceipt executeMaintenancePlan(
    const MaintenancePlan& plan,
    ICleanupMetadataReader& reader,
    IRecycleOperation& recycle,
    FileTimeTicks confirmedAt,
    const std::function<bool()>& cancelled,
    const OrdinaryLocationPolicy& locationPolicy,
    IMaintenanceJournal* journal,
    std::uint64_t operationId,
    const std::function<bool(const MaintenancePlanItem&, std::string*)>&
        canRecycle)
{
    MaintenanceReceipt receipt;
    receipt.operationId = operationId;
    receipt.status = MaintenanceOperationStatus::Executing;
    receipt.confirmedAt = confirmedAt;
    receipt.requestedAt = confirmedAt;
    receipt.selectedCount = plan.selectedCount;
    receipt.eligibleCount = plan.eligibleCount;
    receipt.selectedLogicalBytes = plan.selectedLogicalBytes;
    receipt.eligibleLogicalBytes = plan.eligibleLogicalBytes;

    const auto persist = [&](const MaintenanceItemReceipt& row,
                             ByteSize recycledBytes = 0) -> bool {
        if (journal == nullptr) {
            return true;
        }
        return journal->checkpointItem(operationId, row, recycledBytes);
    };

    const auto markUncertain = [&](MaintenanceItemReceipt& row,
                                   std::string detail) {
        row.result = MaintenanceItemResult::Uncertain;
        row.detail = std::move(detail);
        ++receipt.uncertain;
        persist(row);
        receipt.status = MaintenanceOperationStatus::Uncertain;
    };

    bool stop = false;
    for (const auto& item : plan.items) {
        MaintenanceItemReceipt row;
        row.reviewId = item.reviewId;
        row.path = item.path;
        row.expectedIdentity = item.expectedIdentity;

        if (stop) {
            row.result = MaintenanceItemResult::NotAttempted;
            row.detail = "Not attempted after a critical stop";
            persist(row);
            receipt.items.push_back(std::move(row));
            continue;
        }
        if (!item.eligible) {
            row.result = MaintenanceItemResult::BlockedPreflight;
            row.blockReason = item.blockReason;
            row.detail = item.detail;
            ++receipt.blocked;
            persist(row);
            receipt.items.push_back(std::move(row));
            continue;
        }
        if (cancelled && cancelled()) {
            row.result = MaintenanceItemResult::Cancelled;
            row.detail = "Cancelled before this file started";
            ++receipt.cancelled;
            stop = true;
            persist(row);
            receipt.items.push_back(std::move(row));
            continue;
        }

        ++receipt.attempted;
        const auto probe = reader.read(item.path);
        const auto guard = evaluateMaintenanceFinalGuard(
            item, probe, effectiveLocationSafety(item.path, locationPolicy));
        if (guard != MaintenanceBlockReason::None) {
            row.result = MaintenanceItemResult::BlockedFinalGuard;
            row.blockReason = guard;
            row.detail = "Final guard blocked the recycle";
            ++receipt.blocked;
            persist(row);
            receipt.items.push_back(std::move(row));
            continue;
        }
        if (canRecycle) {
            std::string detail;
            if (!canRecycle(item, &detail)) {
                row.result = MaintenanceItemResult::BlockedFinalGuard;
                row.blockReason = MaintenanceBlockReason::RecycleUnavailable;
                row.detail = detail.empty()
                                 ? "Recycle Bin is not available for this path"
                                 : std::move(detail);
                ++receipt.blocked;
                persist(row);
                receipt.items.push_back(std::move(row));
                continue;
            }
        }

        row.result = MaintenanceItemResult::Attempting;
        row.detail = "Recycle in progress";
        if (!persist(row)) {
            markUncertain(row,
                          "Failed to persist Attempting before recycle");
            stop = true;
            receipt.items.push_back(std::move(row));
            continue;
        }

        auto executed = recycle.recycle(item);
        executed.reviewId = item.reviewId;
        if (executed.path.empty()) {
            executed.path = item.path;
        }
        executed.expectedIdentity = item.expectedIdentity;
        switch (executed.result) {
        case MaintenanceItemResult::Recycled:
            if (!persist(executed, item.logicalSize)) {
                markUncertain(
                    executed,
                    "Recycle evidence existed but the receipt could not be "
                    "persisted");
                stop = true;
                receipt.items.push_back(std::move(executed));
                continue;
            }
            ++receipt.recycled;
            {
                bool saturated = false;
                addSaturating(receipt.recycledLogicalBytes, item.logicalSize,
                              saturated);
            }
            break;
        case MaintenanceItemResult::Cancelled:
        case MaintenanceItemResult::NotAttempted:
            ++receipt.cancelled;
            stop = true;
            persist(executed);
            break;
        case MaintenanceItemResult::UnexpectedPermanentRemoval:
            ++receipt.failed;
            receipt.unexpectedPermanentRemoval = true;
            stop = true;
            persist(executed);
            break;
        case MaintenanceItemResult::OperationAborted:
            ++receipt.failed;
            stop = true;
            persist(executed);
            break;
        case MaintenanceItemResult::Uncertain:
            ++receipt.uncertain;
            receipt.status = MaintenanceOperationStatus::Uncertain;
            stop = true;
            persist(executed);
            break;
        default:
            ++receipt.failed;
            persist(executed);
            break;
        }
        receipt.items.push_back(std::move(executed));
    }

    if (receipt.status == MaintenanceOperationStatus::Executing) {
        if (receipt.unexpectedPermanentRemoval) {
            receipt.status = MaintenanceOperationStatus::HardStopped;
        } else if (receipt.uncertain > 0) {
            receipt.status = MaintenanceOperationStatus::Uncertain;
        } else if (receipt.cancelled > 0) {
            receipt.status = MaintenanceOperationStatus::Cancelled;
        } else {
            receipt.status = MaintenanceOperationStatus::Completed;
        }
    }
    return receipt;
}

}  // namespace spacelens
