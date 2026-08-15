#pragma once

#include "core/PhysicalStorage.hpp"
#include "core/index/Sqlite.hpp"
#include "platform/windows/FileIdentity.hpp"

#include <stop_token>

namespace spacelens {

inline constexpr const char* kPhysicalAccountingMetaKey = "physical_accounting";

struct PhysicalRow {
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    std::uint32_t hardLinkCount = 0;
    std::uint32_t observedLinkCount = 0;
    bool sparse = false;
    bool compressed = false;
    std::uint64_t volumeSerial = 0;
    HardLinkCoverage coverage = HardLinkCoverage::Unknown;
};

[[nodiscard]] PhysicalRow physicalRowFromIdentity(const FileIdentity& id,
                                                  bool directoryHandle) noexcept;

[[nodiscard]] bool indexHasPhysicalAccounting(SqliteDb& db);
void writePhysicalAccountingFlag(SqliteDb& db, bool enabled);

/// Recompute observed_link_count, per-file coverage, and directory unique
/// allocation. When markComplete, set meta physical_accounting=1.
/// Returns false if cancelled; caller should not mark the index ready.
[[nodiscard]] bool finalizePhysicalAccounting(SqliteDb& db, bool markComplete,
                                              std::stop_token stop = {});

}  // namespace spacelens
