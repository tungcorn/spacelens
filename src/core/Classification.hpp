#pragma once

#include <string>
#include <string_view>

namespace spacelens {

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
};

[[nodiscard]] const char* toString(StorageCategory category) noexcept;
[[nodiscard]] const char* toString(Confidence confidence) noexcept;

/// Parse category name (case-insensitive ASCII). Returns Unknown if not matched.
[[nodiscard]] StorageCategory parseStorageCategory(std::string_view text) noexcept;

/// Classify a directory from path leaf name + direct child leaf names.
/// `childNames` are immediate children only (files and dirs).
[[nodiscard]] Classification classifyDirectory(
    std::wstring_view directoryName,
    std::wstring_view fullPath,
    const std::wstring* childNames,
    std::size_t childCount);

/// Classify a file from leaf name / extension / path.
[[nodiscard]] Classification classifyFile(std::wstring_view fileName,
                                          std::wstring_view fullPath);

}  // namespace spacelens
