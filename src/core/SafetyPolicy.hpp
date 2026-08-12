#pragma once

#include <string>
#include <string_view>

namespace spacelens {

/// Deterministic location policy for warnings and future mutation gates.
/// Independent of AI and of classification/reclaimability.
enum class LocationSafety {
    Protected,  // OS / install roots — do not manage deletion
    Sensitive,  // profile root, AppData, etc. — extra caution
    Ordinary,   // typical project / data folders
    Unknown
};

[[nodiscard]] const char* toString(LocationSafety safety) noexcept;

/// Normalize separators to '\\' and strip trailing slashes (except drive roots).
[[nodiscard]] std::wstring normalizePathForPolicy(std::wstring_view path);

/// Classify a filesystem path. Does not touch the filesystem.
[[nodiscard]] LocationSafety classifyLocation(std::wstring_view path);

/// True when policy forbids future mutation workflows for this location.
[[nodiscard]] bool isMutationDisallowed(LocationSafety safety) noexcept;

}  // namespace spacelens
