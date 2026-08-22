#pragma once

#include "core/ReclaimPlan.hpp"
#include "core/Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spacelens::cli {

enum class Command {
    Help,
    Version,
    Scan,
    Top,
    Find,
    Capabilities,
    Index,
    IndexStatus,
    IndexList,
    IndexRefresh,
    Query,
    Duplicates,
    Overview,
    Opportunities,
    Breakdown,
    ReclaimPlan,
    AppStorageZalo,
    AppStorageZaloItems,
};

/// Commands intentionally exposed by the read-only CLI. Keep this list explicit
/// so destructive verbs cannot be registered accidentally.
inline constexpr std::array<Command, 18> kRegisteredCommands{
    Command::Scan,
    Command::Top,
    Command::Find,
    Command::Capabilities,
    Command::Index,
    Command::IndexStatus,
    Command::IndexList,
    Command::IndexRefresh,
    Command::Query,
    Command::Duplicates,
    Command::Overview,
    Command::Opportunities,
    Command::Breakdown,
    Command::ReclaimPlan,
    Command::AppStorageZalo,
    Command::AppStorageZaloItems,
    Command::Help,
    Command::Version,
};

enum class TopMode {
    None,
    Files,
    Dirs,
};

struct ParsedArgs {
    Command command = Command::Help;
    std::wstring path;
    std::vector<std::wstring> rootPaths;  // repeatable app-storage roots
    std::vector<std::wstring> comparePaths;  // repeatable Zalo item scopes
    bool json = false;
    TopMode topMode = TopMode::None;
    std::size_t limit = 20;
    std::optional<ByteSize> minSize;
    std::string storageType;  // normalized app-storage content type/category
    bool unknown = false;     // app-storage unknown-content filter
    std::wstring extension;  // normalized without a leading dot
    std::optional<std::uint64_t> olderThanDays;
    std::wstring classification;
    std::wstring strength;  // CandidateStrength name for query
    std::wstring under;     // query path prefix (descendants of this path)
    bool fromIndex = false;
    /// Optional published-snapshot max age. Empty = no policy.
    std::optional<std::uint64_t> maxIndexAgeSeconds;
    ReclaimPlanSource reclaimSource = ReclaimPlanSource::Auto;
    std::optional<ByteSize> targetFree;
    std::string error;      // non-empty => usage error
};

/// Parse wide argv (Windows wmain). argv[0] is program name.
[[nodiscard]] ParsedArgs parseArgs(int argc, wchar_t** argv);

[[nodiscard]] std::string helpText();

}  // namespace spacelens::cli
