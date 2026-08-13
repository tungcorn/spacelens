#include "core/OrdinaryLocation.hpp"

#include "core/CleanupReview.hpp"

#include <algorithm>
#include <cwctype>

namespace spacelens {
namespace {

bool isDriveLetter(wchar_t ch)
{
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
}

bool isDotOrDotDot(std::wstring_view path)
{
    return path == L"." || path == L".." || path == L".\\" || path == L"..\\" ||
           path == L"./" || path == L"../";
}

bool isDeviceNamespacePath(std::wstring_view path)
{
    return path.rfind(L"\\\\.\\", 0) == 0 || path.rfind(L"//./", 0) == 0;
}

bool hasDotOrDotDotComponent(std::wstring_view path)
{
    std::size_t pos = 0;
    if (path.size() >= 2 && isDriveLetter(path[0]) && path[1] == L':') {
        pos = 2;
    }
    while (pos < path.size()) {
        while (pos < path.size() && (path[pos] == L'\\' || path[pos] == L'/')) {
            ++pos;
        }
        if (pos == path.size()) {
            break;
        }
        const std::size_t begin = pos;
        while (pos < path.size() && path[pos] != L'\\' && path[pos] != L'/') {
            ++pos;
        }
        const auto component = path.substr(begin, pos - begin);
        if (component == L"." || component == L"..") {
            return true;
        }
    }
    return false;
}

bool pathUsesTraversalOrDeviceNamespace(std::wstring_view path)
{
    if (path.empty()) {
        return false;
    }
    if (isDeviceNamespacePath(path) || hasDotOrDotDotComponent(path)) {
        return true;
    }
    const auto normalized = normalizeOrdinaryLocationPath(path);
    return isDeviceNamespacePath(normalized) ||
           hasDotOrDotDotComponent(normalized);
}

bool isAbsolutePath(std::wstring_view normalized)
{
    if (normalized.size() >= 2 && isDriveLetter(normalized[0]) &&
        normalized[1] == L':') {
        return true;
    }
    return normalized.size() >= 2 && normalized[0] == L'\\' &&
           normalized[1] == L'\\';
}

std::wstring stripLongPathPrefix(std::wstring path)
{
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0 ||
        path.rfind(L"//?/UNC/", 0) == 0) {
        path.replace(0, 8, L"\\\\");
        return path;
    }
    if (path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"//?/", 0) == 0) {
        path.erase(0, 4);
    }
    return path;
}

bool isReparse(const CleanupMetadataProbe& probe)
{
    constexpr std::uint32_t kReparse = 0x400;
    return probe.isReparse ||
           (probe.objectEvidence.attributes & kReparse) != 0;
}

OrdinaryLocationStatus statusAfterRefresh(
    const OrdinaryLocationDeclaration& declaration,
    const CleanupMetadataProbe& probe,
    const LocationVolumeEvidence& liveVolume,
    std::string& detail)
{
    if (probe.outcome == CleanupMetadataProbeOutcome::Missing) {
        detail = "Declared path is not currently available";
        return OrdinaryLocationStatus::PathUnavailable;
    }
    if (probe.outcome == CleanupMetadataProbeOutcome::AccessDenied) {
        detail = "Declared path cannot be read";
        return OrdinaryLocationStatus::PathUnavailable;
    }
    if (probe.outcome != CleanupMetadataProbeOutcome::Present ||
        !probe.objectEvidence.available) {
        detail = "Declared path could not be inspected";
        return OrdinaryLocationStatus::Invalid;
    }
    if (isReparse(probe)) {
        detail = "Declaration root is a reparse point";
        return OrdinaryLocationStatus::Invalid;
    }
    if (probe.objectEvidence.kind != ItemKind::Directory) {
        detail = "Declaration root is not a directory";
        return OrdinaryLocationStatus::Invalid;
    }

    if (!declaration.volume.available || declaration.volume.serial == 0) {
        detail = "Volume identity was not available when this location was "
                 "declared; it cannot authorize maintenance";
        return OrdinaryLocationStatus::VolumeUnavailable;
    }
    if (!liveVolume.available || liveVolume.serial == 0) {
        detail = "Current volume identity is unavailable";
        return OrdinaryLocationStatus::VolumeUnavailable;
    }
    if (liveVolume.serial != declaration.volume.serial) {
        detail = "Volume serial does not match the declared volume";
        return OrdinaryLocationStatus::VolumeMismatch;
    }
    if (!declaration.volume.guid.empty() && !liveVolume.guid.empty() &&
        declaration.volume.guid != liveVolume.guid) {
        detail = "Volume GUID does not match the declared volume";
        return OrdinaryLocationStatus::VolumeMismatch;
    }
    detail = "Volume matched";
    return OrdinaryLocationStatus::Active;
}

}  // namespace

const char* toString(LocationSafetySource source) noexcept
{
    switch (source) {
    case LocationSafetySource::BuiltInProtected:
        return "BuiltInProtected";
    case LocationSafetySource::BuiltInSensitive:
        return "BuiltInSensitive";
    case LocationSafetySource::BuiltInOrdinary:
        return "BuiltInOrdinary";
    case LocationSafetySource::UserDeclaredOrdinary:
        return "UserDeclaredOrdinary";
    case LocationSafetySource::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

const char* toString(OrdinaryLocationStatus status) noexcept
{
    switch (status) {
    case OrdinaryLocationStatus::Active:
        return "Active";
    case OrdinaryLocationStatus::VolumeMismatch:
        return "VolumeMismatch";
    case OrdinaryLocationStatus::VolumeUnavailable:
        return "VolumeUnavailable";
    case OrdinaryLocationStatus::PathUnavailable:
        return "PathUnavailable";
    case OrdinaryLocationStatus::Invalid:
        return "Invalid";
    }
    return "Invalid";
}

const char* toString(OrdinaryLocationAddResult result) noexcept
{
    switch (result) {
    case OrdinaryLocationAddResult::Added:
        return "Added";
    case OrdinaryLocationAddResult::AlreadyExists:
        return "AlreadyExists";
    case OrdinaryLocationAddResult::RejectedProtected:
        return "RejectedProtected";
    case OrdinaryLocationAddResult::RejectedSensitive:
        return "RejectedSensitive";
    case OrdinaryLocationAddResult::RejectedReparse:
        return "RejectedReparse";
    case OrdinaryLocationAddResult::RejectedTooBroad:
        return "RejectedTooBroad";
    case OrdinaryLocationAddResult::InvalidPath:
        return "InvalidPath";
    case OrdinaryLocationAddResult::PathUnavailable:
        return "PathUnavailable";
    case OrdinaryLocationAddResult::VolumeUnavailable:
        return "VolumeUnavailable";
    case OrdinaryLocationAddResult::Error:
        return "Error";
    }
    return "Error";
}

std::wstring normalizeOrdinaryLocationPath(std::wstring_view path)
{
    return normalizeCleanupPath(stripLongPathPrefix(std::wstring(path)));
}

bool declarationContainsPath(std::wstring_view root, std::wstring_view path)
{
    if (pathUsesTraversalOrDeviceNamespace(root) ||
        pathUsesTraversalOrDeviceNamespace(path)) {
        return false;
    }
    const auto a = normalizeOrdinaryLocationPath(root);
    const auto p = normalizeOrdinaryLocationPath(path);
    if (a.empty() || p.empty()) {
        return false;
    }
    return isPathAncestorOrEqual(a, p);
}

bool isDriveRootPathKey(std::wstring_view normalized)
{
    return normalized.size() == 3 && isDriveLetter(normalized[0]) &&
           normalized[1] == L':' && normalized[2] == L'\\';
}

LocationSafetyAssessment assessLocationSafety(
    std::wstring_view path,
    const OrdinaryLocationPolicy& policy)
{
    LocationSafetyAssessment out;
    const auto builtIn = classifyLocation(path);
    switch (builtIn) {
    case LocationSafety::Protected:
        out.safety = LocationSafety::Protected;
        out.source = LocationSafetySource::BuiltInProtected;
        return out;
    case LocationSafety::Sensitive:
        out.safety = LocationSafety::Sensitive;
        out.source = LocationSafetySource::BuiltInSensitive;
        return out;
    case LocationSafety::Ordinary:
        out.safety = LocationSafety::Ordinary;
        out.source = LocationSafetySource::BuiltInOrdinary;
        return out;
    case LocationSafety::Unknown:
        break;
    }

    const auto* match = policy.matchingActiveDeclaration(path);
    if (match != nullptr) {
        out.safety = LocationSafety::Ordinary;
        out.source = LocationSafetySource::UserDeclaredOrdinary;
        out.declarationId = match->id;
        out.declarationPath = match->configuredPath;
        return out;
    }
    out.safety = LocationSafety::Unknown;
    out.source = LocationSafetySource::Unknown;
    return out;
}

LocationSafety effectiveLocationSafety(std::wstring_view path,
                                       const OrdinaryLocationPolicy& policy)
{
    return assessLocationSafety(path, policy).safety;
}

const OrdinaryLocationDeclaration*
OrdinaryLocationPolicy::matchingActiveDeclaration(std::wstring_view path) const
{
    const OrdinaryLocationDeclaration* best = nullptr;
    std::size_t bestLen = 0;
    for (const auto& declaration : declarations) {
        if (declaration.status != OrdinaryLocationStatus::Active) {
            continue;
        }
        if (!declarationContainsPath(declaration.normalizedPathKey, path)) {
            continue;
        }
        if (declaration.normalizedPathKey.size() > bestLen) {
            best = &declaration;
            bestLen = declaration.normalizedPathKey.size();
        }
    }
    return best;
}

LocationSafetyAssessment OrdinaryLocationPolicy::classify(
    std::wstring_view path) const
{
    return assessLocationSafety(path, *this);
}

OrdinaryLocationAddOutcome evaluateOrdinaryLocationDeclaration(
    std::wstring_view configuredPath,
    ICleanupMetadataReader& rootProbe,
    IVolumeIdentityReader& volumes,
    FileTimeTicks createdAt)
{
    OrdinaryLocationAddOutcome out;
    std::wstring raw(configuredPath);
    while (!raw.empty() && (raw.back() == L' ' || raw.back() == L'\t')) {
        raw.pop_back();
    }
    if (raw.empty() || isDotOrDotDot(raw)) {
        out.result = OrdinaryLocationAddResult::InvalidPath;
        out.message = "Path is empty or not absolute";
        return out;
    }

    const auto key = normalizeOrdinaryLocationPath(raw);
    if (key.empty() || isDotOrDotDot(key) || !isAbsolutePath(key)) {
        out.result = OrdinaryLocationAddResult::InvalidPath;
        out.message = "Path must be an absolute directory";
        return out;
    }
    if (pathUsesTraversalOrDeviceNamespace(raw) ||
        pathUsesTraversalOrDeviceNamespace(key)) {
        out.result = OrdinaryLocationAddResult::InvalidPath;
        out.message = "Path must not contain '.' or '..' components or use "
                      "the Win32 device namespace";
        return out;
    }
    if (isDriveRootPathKey(key)) {
        out.result = OrdinaryLocationAddResult::RejectedTooBroad;
        out.message = "Whole-volume roots cannot be declared ordinary";
        return out;
    }

    const auto builtIn = classifyLocation(key);
    if (builtIn == LocationSafety::Protected) {
        out.result = OrdinaryLocationAddResult::RejectedProtected;
        out.message = "Built-in protection cannot be overridden";
        return out;
    }
    if (builtIn == LocationSafety::Sensitive) {
        out.result = OrdinaryLocationAddResult::RejectedSensitive;
        out.message = "Sensitive locations cannot be declared ordinary";
        return out;
    }

    const auto probe = rootProbe.read(raw);
    if (probe.outcome == CleanupMetadataProbeOutcome::Missing) {
        out.result = OrdinaryLocationAddResult::PathUnavailable;
        out.message = "Directory does not exist";
        return out;
    }
    if (probe.outcome != CleanupMetadataProbeOutcome::Present ||
        !probe.objectEvidence.available) {
        out.result = OrdinaryLocationAddResult::PathUnavailable;
        out.message = probe.detail.empty() ? "Directory could not be inspected"
                                           : probe.detail;
        return out;
    }
    if (isReparse(probe)) {
        out.result = OrdinaryLocationAddResult::RejectedReparse;
        out.message = "Declaration root must not be a reparse point";
        return out;
    }
    if (probe.objectEvidence.kind != ItemKind::Directory) {
        out.result = OrdinaryLocationAddResult::InvalidPath;
        out.message = "Declaration must be a directory";
        return out;
    }

    auto volume = volumes.read(raw);
    OrdinaryLocationDeclaration declaration;
    declaration.configuredPath = raw;
    declaration.normalizedPathKey = key;
    declaration.createdAt = createdAt;
    declaration.volume = volume;
    if (!volume.available || volume.serial == 0) {
        declaration.status = OrdinaryLocationStatus::VolumeUnavailable;
        declaration.detail =
            "Volume identity is unavailable; this declaration cannot "
            "authorize maintenance";
        out.result = OrdinaryLocationAddResult::VolumeUnavailable;
        out.message = declaration.detail;
        out.declaration = std::move(declaration);
        return out;
    }

    declaration.status = OrdinaryLocationStatus::Active;
    declaration.detail = "Volume matched";
    out.result = OrdinaryLocationAddResult::Added;
    out.declaration = std::move(declaration);
    return out;
}

void refreshOrdinaryLocationDeclaration(
    OrdinaryLocationDeclaration& declaration,
    ICleanupMetadataReader& rootProbe,
    IVolumeIdentityReader& volumes)
{
    if (pathUsesTraversalOrDeviceNamespace(declaration.configuredPath) ||
        pathUsesTraversalOrDeviceNamespace(declaration.normalizedPathKey)) {
        declaration.status = OrdinaryLocationStatus::Invalid;
        declaration.detail =
            "Declaration path contains a traversal or device-namespace "
            "component";
        return;
    }
    const auto probe = rootProbe.read(declaration.configuredPath);
    const auto live = volumes.read(declaration.configuredPath);
    declaration.status =
        statusAfterRefresh(declaration, probe, live, declaration.detail);
}

void refreshOrdinaryLocationPolicy(OrdinaryLocationPolicy& policy,
                                   ICleanupMetadataReader& rootProbe,
                                   IVolumeIdentityReader& volumes)
{
    for (auto& declaration : policy.declarations) {
        refreshOrdinaryLocationDeclaration(declaration, rootProbe, volumes);
    }
}

}  // namespace spacelens
