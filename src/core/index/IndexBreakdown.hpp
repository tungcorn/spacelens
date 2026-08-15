#pragma once

#include "core/FileTime.hpp"
#include "core/Types.hpp"
#include "core/index/IndexQuery.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace spacelens {

inline constexpr std::size_t kDefaultBreakdownLimit = 20;
inline constexpr std::size_t kMaxBreakdownLimit = 200;

/// Last-write age bucket. JSON names are stable agent contract.
enum class WriteAgeBucket {
    Lt30d,      // lt_30d
    D30_90,     // d30_90
    D90_365,    // d90_365
    Ge365d,     // ge_365d
    Unknown,    // unknown
    Future      // future
};

[[nodiscard]] const char* toString(WriteAgeBucket bucket) noexcept;
[[nodiscard]] const char* writeAgeBucketHuman(WriteAgeBucket bucket) noexcept;

/// Classify one last_write_ticks against a captured now. `then==0` is unknown;
/// `then > now` is future. Exact day boundaries use whole-day ageDays.
[[nodiscard]] WriteAgeBucket classifyWriteAgeBucket(FileTimeTicks then,
                                                    FileTimeTicks now) noexcept;

struct BreakdownGroup {
    std::string key;
    std::uint64_t file_count = 0;
    ByteSize logical_bytes = 0;
};

struct BreakdownExtensionOther {
    std::uint64_t extension_groups = 0;
    std::uint64_t file_count = 0;
    ByteSize logical_bytes = 0;
};

struct IndexedBreakdownSpec {
    std::wstring pathPrefix;
    std::size_t limit = kDefaultBreakdownLimit;
    FileTimeTicks nowTicks = 0;
    std::optional<std::uint64_t> maxIndexAgeSeconds;
};

struct IndexedBreakdown {
    bool ok = false;
    std::string error;
    IndexRootInfo root{};
    IndexLocation location{};
    std::wstring under;
    std::size_t limit = kDefaultBreakdownLimit;
    std::uint64_t total_file_count = 0;
    ByteSize total_logical_bytes = 0;
    bool logical_bytes_estimated = false;
    std::vector<BreakdownGroup> by_classification;
    std::vector<BreakdownGroup> by_extension;
    BreakdownExtensionOther extension_other{};
    std::vector<BreakdownGroup> by_last_write_age;
    std::uint64_t query_elapsed_ms = 0;
    IndexSnapshotEvidence snapshot{};
    IndexAgeDecision ageDecision{};
};

/// Index-only file-logical-byte breakdown. Never scans, refreshes, or
/// materializes per-file IndexHit rows. Directories contribute no bytes.
[[nodiscard]] IndexedBreakdown queryIndexedBreakdown(
    const std::wstring& rootPath, const IndexedBreakdownSpec& spec,
    std::stop_token stop = {});

}  // namespace spacelens
