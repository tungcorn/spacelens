#include "core/Json.hpp"

#include <climits>
#include <cstdio>

namespace spacelens {
namespace {

void appendUtf8(std::string& out, std::uint32_t codePoint)
{
    if (codePoint <= 0x7fU) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else {
        out.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    }
}

}  // namespace

std::string utf8FromWide(std::wstring_view wide)
{
    std::string out;
    out.reserve(wide.size());
    for (std::size_t i = 0; i < wide.size(); ++i) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(wide[i]);
#if WCHAR_MAX <= 0xffff
        if (codePoint >= 0xd800U && codePoint <= 0xdbffU && i + 1 < wide.size()) {
            const std::uint32_t low = static_cast<std::uint32_t>(wide[i + 1]);
            if (low >= 0xdc00U && low <= 0xdfffU) {
                codePoint = 0x10000U + ((codePoint - 0xd800U) << 10U) +
                            (low - 0xdc00U);
                ++i;
            }
        }
#endif
        if ((codePoint >= 0xd800U && codePoint <= 0xdfffU) ||
            codePoint > 0x10ffffU) {
            codePoint = 0xfffdU;
        }
        appendUtf8(out, codePoint);
    }
    return out;
}

std::string jsonEscape(std::string_view input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (const unsigned char ch : input) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20U) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

std::string jsonString(std::string_view utf8)
{
    return std::string("\"") + jsonEscape(utf8) + "\"";
}

std::string jsonString(std::wstring_view wide)
{
    return jsonString(utf8FromWide(wide));
}

std::string jsonBool(bool value)
{
    return value ? "true" : "false";
}

std::string jsonUInt(std::uint64_t value)
{
    return std::to_string(value);
}

std::string jsonInt(std::int64_t value)
{
    return std::to_string(value);
}

}  // namespace spacelens
