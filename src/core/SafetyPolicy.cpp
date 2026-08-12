#include "core/SafetyPolicy.hpp"

#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {
namespace {

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

bool isAsciiDriveLetter(wchar_t ch)
{
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

bool isDriveRoot(std::wstring_view path)
{
    return path.size() == 3 && isAsciiDriveLetter(path[0]) &&
           path[1] == L':' && path[2] == L'\\';
}

std::vector<std::wstring_view> componentsOf(std::wstring_view path)
{
    std::vector<std::wstring_view> components;
    std::size_t pos = 0;

    if (path.size() >= 2 && isAsciiDriveLetter(path[0]) && path[1] == L':') {
        pos = 2;
    }

    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == L'\\') {
            ++pos;
        }
        if (pos == path.size()) {
            break;
        }
        const std::size_t begin = pos;
        while (pos < path.size() && path[pos] != L'\\') {
            ++pos;
        }
        components.push_back(path.substr(begin, pos - begin));
    }
    return components;
}

bool isProtectedRootComponent(std::wstring_view component)
{
    return equalsIgnoreCase(component, L"Windows") ||
           equalsIgnoreCase(component, L"Program Files") ||
           equalsIgnoreCase(component, L"Program Files (x86)") ||
           equalsIgnoreCase(component, L"ProgramData") ||
           equalsIgnoreCase(component, L"Recovery") ||
           equalsIgnoreCase(component, L"System Volume Information") ||
           equalsIgnoreCase(component, L"$Recycle.Bin");
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
    std::wstring normalized;
    normalized.reserve(path.size());
    for (const wchar_t ch : path) {
        normalized.push_back(ch == L'/' ? L'\\' : ch);
    }

    // Keep exactly one separator for a drive root, while removing separators
    // from all other path tails. Internal separators are left intact.
    const bool driveRoot = normalized.size() >= 2 &&
                           isAsciiDriveLetter(normalized[0]) &&
                           normalized[1] == L':' &&
                           normalized.find_first_not_of(L'\\', 2) ==
                               std::wstring::npos;
    if (driveRoot) {
        normalized.resize(3);
        normalized[2] = L'\\';
        return normalized;
    }

    while (!normalized.empty() && normalized.back() == L'\\') {
        normalized.pop_back();
    }
    return normalized;
}

LocationSafety classifyLocation(std::wstring_view path)
{
    const std::wstring normalized = normalizePathForPolicy(path);
    if (normalized.empty()) {
        return LocationSafety::Unknown;
    }

    if (isDriveRoot(normalized)) {
        return LocationSafety::Protected;
    }

    const auto components = componentsOf(normalized);
    if (components.empty()) {
        return LocationSafety::Unknown;
    }

    // These names are protected only when they are direct children of a drive
    // root (or the first component of a rooted path).
    if (isProtectedRootComponent(components.front())) {
        return LocationSafety::Protected;
    }

    // System Volume Information and the recycle bin remain protected when a
    // path representation includes a server/share prefix.
    for (const auto component : components) {
        if (equalsIgnoreCase(component, L"System Volume Information") ||
            equalsIgnoreCase(component, L"$Recycle.Bin")) {
            return LocationSafety::Protected;
        }
    }

    // C:\Users\<name> is the profile root. A path below it is ordinary unless
    // it enters AppData, which is sensitive regardless of its depth.
    if (components.size() >= 2 &&
        equalsIgnoreCase(components[0], L"Users")) {
        if (components.size() == 2) {
            return LocationSafety::Sensitive;
        }
        for (std::size_t i = 2; i < components.size(); ++i) {
            if (equalsIgnoreCase(components[i], L"AppData")) {
                return LocationSafety::Sensitive;
            }
        }
        return LocationSafety::Ordinary;
    }

    for (const auto component : components) {
        if (equalsIgnoreCase(component, L"AppData")) {
            return LocationSafety::Sensitive;
        }
    }

    // Relative, UNC, and non-user layouts are intentionally conservative.
    return LocationSafety::Unknown;
}

bool isMutationDisallowed(LocationSafety safety) noexcept
{
    return safety == LocationSafety::Protected;
}

}  // namespace spacelens
