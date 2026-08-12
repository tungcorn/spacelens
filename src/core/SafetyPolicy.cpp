#include "core/SafetyPolicy.hpp"

#include <cwctype>
#include <vector>

namespace spacelens {
namespace {

void toLowerInPlace(std::wstring& s)
{
    for (wchar_t& ch : s) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
}

bool startsWithIgnoreCase(std::wstring_view path, std::wstring_view prefix)
{
    if (path.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(path[i]) != std::towlower(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }
    return true;
}

/// True for "C:\" style drive roots after normalization.
bool isDriveRoot(std::wstring_view path)
{
    return path.size() == 3 &&
           ((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) &&
           path[1] == L':' && path[2] == L'\\';
}

std::vector<std::wstring_view> splitComponents(std::wstring_view path)
{
    std::vector<std::wstring_view> parts;
    std::size_t i = 0;
    // Skip drive prefix "C:"
    if (path.size() >= 2 && path[1] == L':') {
        i = 2;
        if (i < path.size() && path[i] == L'\\') {
            ++i;
        }
    }
    while (i < path.size()) {
        while (i < path.size() && path[i] == L'\\') {
            ++i;
        }
        if (i >= path.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < path.size() && path[i] != L'\\') {
            ++i;
        }
        parts.push_back(path.substr(start, i - start));
    }
    return parts;
}

}  // namespace

const char* toString(LocationSafety safety) noexcept
{
    switch (safety) {
    case LocationSafety::Protected:
        return "Protected";
    case LocationSafety::Sensitive:
        return "Sensitive";
    case LocationSafety::Ordinary:
        return "Ordinary";
    case LocationSafety::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

std::wstring normalizePathForPolicy(std::wstring_view path)
{
    std::wstring out;
    out.reserve(path.size());
    for (wchar_t ch : path) {
        if (ch == L'/') {
            ch = L'\\';
        }
        out.push_back(ch);
    }
    // Collapse duplicate separators (keep leading drive form).
    std::wstring collapsed;
    collapsed.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == L'\\' && !collapsed.empty() && collapsed.back() == L'\\') {
            continue;
        }
        collapsed.push_back(out[i]);
    }
    // Strip trailing separators except drive roots ("C:\").
    while (collapsed.size() > 3 && collapsed.back() == L'\\') {
        collapsed.pop_back();
    }
    if (collapsed.size() == 2 && collapsed[1] == L':') {
        collapsed.push_back(L'\\');
    }
    return collapsed;
}

LocationSafety classifyLocation(std::wstring_view path)
{
    if (path.empty()) {
        return LocationSafety::Unknown;
    }

    const std::wstring normalized = normalizePathForPolicy(path);
    if (normalized.empty()) {
        return LocationSafety::Unknown;
    }

    if (isDriveRoot(normalized)) {
        return LocationSafety::Protected;
    }

    const auto parts = splitComponents(normalized);
    if (parts.empty()) {
        return LocationSafety::Unknown;
    }

    auto matchPart = [](std::wstring_view part, std::wstring_view expected) {
        return equalsIgnoreCase(part, expected);
    };

    // First component checks (under drive).
    const auto& p0 = parts[0];
    if (matchPart(p0, L"Windows") || matchPart(p0, L"Program Files") ||
        matchPart(p0, L"Program Files (x86)") || matchPart(p0, L"ProgramData") ||
        matchPart(p0, L"System Volume Information") ||
        matchPart(p0, L"$Recycle.Bin") || matchPart(p0, L"Recovery") ||
        matchPart(p0, L"Boot") || matchPart(p0, L"PerfLogs") ||
        matchPart(p0, L"Documents and Settings")) {
        return LocationSafety::Protected;
    }

    // Nested protected names anywhere.
    for (const auto& part : parts) {
        if (matchPart(part, L"System Volume Information") ||
            matchPart(part, L"$Recycle.Bin")) {
            return LocationSafety::Protected;
        }
    }

    // User profile root: C:\Users\<name>
    if (parts.size() >= 2 && matchPart(parts[0], L"Users")) {
        if (parts.size() == 2) {
            // C:\Users\<name>
            return LocationSafety::Sensitive;
        }
        // C:\Users\<name>\AppData\...
        if (parts.size() >= 3 && matchPart(parts[2], L"AppData")) {
            return LocationSafety::Sensitive;
        }
        // Deeper under profile — ordinary project/data areas by default.
        return LocationSafety::Ordinary;
    }

    // AppData outside Users (unusual) still sensitive if present.
    for (const auto& part : parts) {
        if (matchPart(part, L"AppData")) {
            return LocationSafety::Sensitive;
        }
    }

    // UNC or non-drive paths.
    if (startsWithIgnoreCase(normalized, L"\\\\")) {
        return LocationSafety::Unknown;
    }

    return LocationSafety::Ordinary;
}

bool isMutationDisallowed(LocationSafety safety) noexcept
{
    return safety == LocationSafety::Protected;
}

}  // namespace spacelens
