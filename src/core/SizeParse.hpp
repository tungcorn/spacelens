#pragma once

#include "core/Types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace spacelens {

/// Strict size parser for CLI/GUI filters.
///
/// Units are **binary** (powers of 1024), matching SizeFormatter:
///   B, KB/KiB, MB/MiB, GB/GiB, TB/TiB
/// Examples: "500MB", "1.5 GB", "1024", "2TiB"
///
/// Decimal SI (1000-based) is intentionally not accepted to avoid mixed conventions.
struct SizeParseResult {
    ByteSize bytes = 0;
    std::string error;  // non-empty on failure
};

[[nodiscard]] SizeParseResult parseSize(std::string_view text);
[[nodiscard]] SizeParseResult parseSize(std::wstring_view text);

}  // namespace spacelens
