#pragma once

#include "core/Types.hpp"

#include <string>
#include <string_view>

namespace spacelens {

class DirectoryTree;

enum class StorageCategory {
    BuildArtifact,
    DependencyDirectory,
    PackageCache,
    IdeCache,
    LogData,
    TemporaryData,
    DownloadedAiModel,
    Archive,
    ApplicationData,
    SystemData,
    UserData,
    Unknown
};

enum class Confidence {
    High,
    Medium,
    Low
};

/// Deterministic, explainable classification. Never includes "safe_to_delete".
struct Classification {
    StorageCategory category = StorageCategory::Unknown;
    Confidence confidence = Confidence::Low;
    std::string ruleId;
    std::string reason;
    /// Compact ecosystem hint for agents (node, cmake, rust, dotnet, python, …).
    std::string ecosystem;
    /// Leaf/marker that fired the rule (node_modules, Cargo.toml, .msi, …).
    std::string marker;
};

[[nodiscard]] const char* toString(StorageCategory category) noexcept;
[[nodiscard]] const char* toString(Confidence confidence) noexcept;

/// Parse category name (case-insensitive ASCII). Returns Unknown if not matched.
[[nodiscard]] StorageCategory parseStorageCategory(std::string_view text) noexcept;

/// Classify a directory from leaf name, full path, direct children, and optional
/// siblings of the same parent (files and directories). `fullPath` is used for
/// Known Folder / path-component evidence. Sibling lookup is bounded to the
/// parent; ancestors are never walked.
[[nodiscard]] Classification classifyDirectory(
    std::wstring_view directoryName,
    std::wstring_view fullPath,
    const std::wstring* childNames,
    std::size_t childCount,
    const std::wstring* siblingNames = nullptr,
    std::size_t siblingCount = 0);

/// Classify a file from leaf name / extension / path.
[[nodiscard]] Classification classifyFile(std::wstring_view fileName,
                                          std::wstring_view fullPath);

/// Sibling-aware classification from an in-memory tree node.
[[nodiscard]] Classification classifyDirectoryFromTree(const DirectoryTree& tree,
                                                       DirIndex idx);

/// Cached Windows Known Folder / temp locations. Empty if the API fails.
[[nodiscard]] const std::wstring& knownDownloadsFolder();
[[nodiscard]] const std::wstring& knownTempFolder();
[[nodiscard]] const std::wstring& knownLocalAppDataFolder();

/// Case-insensitive, component-boundary ancestry (or equality).
[[nodiscard]] bool pathIsUnderFolder(std::wstring_view path,
                                     std::wstring_view folder);

/// Case-insensitive exact path-component match (not a substring of a component).
[[nodiscard]] bool pathHasComponent(std::wstring_view path,
                                    std::wstring_view component);

}  // namespace spacelens
