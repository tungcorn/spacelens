#include "core/SizeParse.hpp"

#include <cctype>
#include <cmath>
#include <string>

namespace spacelens {
namespace {

void trimInPlace(std::string& s)
{
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

std::string narrow(std::wstring_view wide)
{
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t ch : wide) {
        if (ch > 127) {
            return {};
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

bool startsWithIgnoreCase(std::string_view s, std::string_view prefix)
{
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto a = static_cast<unsigned char>(s[i]);
        const auto b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

}  // namespace

SizeParseResult parseSize(std::string_view text)
{
    SizeParseResult result;
    std::string s(text);
    trimInPlace(s);
    if (s.empty()) {
        result.error = "Empty size value.";
        return result;
    }

    // Number part: digits with optional single '.' fraction.
    std::size_t i = 0;
    if (s[i] == '+' ) {
        ++i;
    }
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
        result.error = "Size must start with a number.";
        return result;
    }
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
            result.error = "Invalid fractional size.";
            return result;
        }
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
    }

    const std::string numberPart = s.substr(0, i);
    std::string unitPart = s.substr(i);
    trimInPlace(unitPart);

    char* end = nullptr;
    const double value = std::strtod(numberPart.c_str(), &end);
    if (end == numberPart.c_str() || value < 0.0 || !std::isfinite(value)) {
        result.error = "Invalid size number.";
        return result;
    }

    std::uint64_t multiplier = 1;
    if (unitPart.empty() ||
        (startsWithIgnoreCase(unitPart, "B") && unitPart.size() == 1)) {
        multiplier = 1;
    } else if (startsWithIgnoreCase(unitPart, "KiB") ||
               (startsWithIgnoreCase(unitPart, "KB") && unitPart.size() == 2)) {
        multiplier = 1024ULL;
    } else if (startsWithIgnoreCase(unitPart, "MiB") ||
               (startsWithIgnoreCase(unitPart, "MB") && unitPart.size() == 2)) {
        multiplier = 1024ULL * 1024ULL;
    } else if (startsWithIgnoreCase(unitPart, "GiB") ||
               (startsWithIgnoreCase(unitPart, "GB") && unitPart.size() == 2)) {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (startsWithIgnoreCase(unitPart, "TiB") ||
               (startsWithIgnoreCase(unitPart, "TB") && unitPart.size() == 2)) {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    } else if (startsWithIgnoreCase(unitPart, "K") && unitPart.size() == 1) {
        multiplier = 1024ULL;
    } else if (startsWithIgnoreCase(unitPart, "M") && unitPart.size() == 1) {
        multiplier = 1024ULL * 1024ULL;
    } else if (startsWithIgnoreCase(unitPart, "G") && unitPart.size() == 1) {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (startsWithIgnoreCase(unitPart, "T") && unitPart.size() == 1) {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    } else {
        result.error =
            "Unknown size unit. Use B, KB, MB, GB, TB (binary, 1024-based).";
        return result;
    }

    // Validate unit fully consumed (allow trailing spaces already trimmed).
    // For multi-char units we already matched exact sizes above.

    const double bytes = value * static_cast<double>(multiplier);
    if (bytes > static_cast<double>(UINT64_MAX)) {
        result.error = "Size value overflows 64-bit byte count.";
        return result;
    }
    result.bytes = static_cast<ByteSize>(bytes);
    return result;
}

SizeParseResult parseSize(std::wstring_view text)
{
    const std::string narrowText = narrow(text);
    if (narrowText.empty() && !text.empty()) {
        SizeParseResult r;
        r.error = "Size text must be ASCII.";
        return r;
    }
    return parseSize(std::string_view{narrowText});
}

}  // namespace spacelens
