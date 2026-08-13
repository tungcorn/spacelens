#pragma once

#include "core/CleanupRevalidation.hpp"
#include "core/FileTime.hpp"
#include "core/SafetyPolicy.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

inline constexpr int kLocationSchemaVersion = 1;

/// Why a path received its effective LocationSafety.
/// Ordinary from a user declaration is never "safe to delete".
enum class LocationSafetySource {
    BuiltInProtected,
    BuiltInSensitive,
    BuiltInOrdinary,
    UserDeclaredOrdinary,
    Unknown
};

[[nodiscard]] const char* toString(LocationSafetySource source) noexcept;

enum class OrdinaryLocationStatus {
    Active,
    VolumeMismatch,
    VolumeUnavailable,
    PathUnavailable,
    Invalid
};

[[nodiscard]] const char* toString(OrdinaryLocationStatus status) noexcept;

enum class OrdinaryLocationAddResult {
    Added,
    AlreadyExists,
    RejectedProtected,
    RejectedSensitive,
    RejectedReparse,
    RejectedTooBroad,
    InvalidPath,
    PathUnavailable,
    VolumeUnavailable,
    Error
};

[[nodiscard]] const char* toString(OrdinaryLocationAddResult result) noexcept;

/// Volume evidence captured with a declaration. Drive letters are not identity.
struct LocationVolumeEvidence {
    bool available = false;
    std::uint32_t serial = 0;
    std::wstring guid;
    std::wstring rootPath;
};

class IVolumeIdentityReader {
public:
    virtual ~IVolumeIdentityReader() = default;
    [[nodiscard]] virtual LocationVolumeEvidence read(
        const std::wstring& path) const = 0;
};

/// Qt-free declaration value. Status is refreshed from live volume/path evidence
/// and is never silently discarded.
struct OrdinaryLocationDeclaration {
    std::uint64_t id = 0;
    std::wstring configuredPath;
    std::wstring normalizedPathKey;
    FileTimeTicks createdAt = 0;
    LocationVolumeEvidence volume{};
    OrdinaryLocationStatus status = OrdinaryLocationStatus::Invalid;
    std::string detail;
};

struct LocationSafetyAssessment {
    LocationSafety safety = LocationSafety::Unknown;
    LocationSafetySource source = LocationSafetySource::Unknown;
    std::uint64_t declarationId = 0;
    std::wstring declarationPath;
};

/// Snapshot of user-declared ordinary roots plus a monotonic generation.
/// Empty declarations preserve built-in-only classification.
struct OrdinaryLocationPolicy {
    std::uint64_t generation = 0;
    std::vector<OrdinaryLocationDeclaration> declarations;

    [[nodiscard]] LocationSafetyAssessment classify(std::wstring_view path) const;
    [[nodiscard]] const OrdinaryLocationDeclaration* matchingActiveDeclaration(
        std::wstring_view path) const;
};

struct OrdinaryLocationAddOutcome {
    OrdinaryLocationAddResult result = OrdinaryLocationAddResult::Error;
    OrdinaryLocationDeclaration declaration{};
    std::string message;

    [[nodiscard]] bool added() const noexcept
    {
        return result == OrdinaryLocationAddResult::Added;
    }
};

/// Normalize separators, strip \\?\ long-path prefixes, then apply the shared
/// cleanup path key (case-insensitive, trailing-separator, drive-root safe).
[[nodiscard]] std::wstring normalizeOrdinaryLocationPath(std::wstring_view path);

/// Component-aware ancestry. `D:\proj` matches `D:\proj\app` and does not
/// match `D:\project`.
[[nodiscard]] bool declarationContainsPath(std::wstring_view root,
                                           std::wstring_view path);

[[nodiscard]] bool isDriveRootPathKey(std::wstring_view normalized);

/// Validate and build a declaration. Does not persist. Does not follow reparse
/// points. Built-in Protected and Sensitive roots are rejected.
[[nodiscard]] OrdinaryLocationAddOutcome evaluateOrdinaryLocationDeclaration(
    std::wstring_view configuredPath,
    ICleanupMetadataReader& rootProbe,
    IVolumeIdentityReader& volumes,
    FileTimeTicks createdAt = 0);

/// Recompute status from live path/volume evidence. The configured path and
/// captured volume identity are preserved.
void refreshOrdinaryLocationDeclaration(OrdinaryLocationDeclaration& declaration,
                                        ICleanupMetadataReader& rootProbe,
                                        IVolumeIdentityReader& volumes);

void refreshOrdinaryLocationPolicy(OrdinaryLocationPolicy& policy,
                                   ICleanupMetadataReader& rootProbe,
                                   IVolumeIdentityReader& volumes);

/// Built-in Protected/Sensitive always win. Built-in Ordinary wins next.
/// Only then may an Active matching declaration yield UserDeclaredOrdinary.
[[nodiscard]] LocationSafetyAssessment assessLocationSafety(
    std::wstring_view path,
    const OrdinaryLocationPolicy& policy);

[[nodiscard]] LocationSafety effectiveLocationSafety(
    std::wstring_view path,
    const OrdinaryLocationPolicy& policy);

}  // namespace spacelens
