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

std::wstring wideFromUtf8(std::string_view utf8)
{
    std::wstring out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const unsigned char lead = static_cast<unsigned char>(utf8[i]);
        std::uint32_t codePoint = 0;
        std::size_t need = 0;
        if (lead < 0x80U) {
            codePoint = lead;
            need = 1;
        } else if ((lead & 0xe0U) == 0xc0U) {
            codePoint = lead & 0x1fU;
            need = 2;
        } else if ((lead & 0xf0U) == 0xe0U) {
            codePoint = lead & 0x0fU;
            need = 3;
        } else if ((lead & 0xf8U) == 0xf0U) {
            codePoint = lead & 0x07U;
            need = 4;
        } else {
            out.push_back(static_cast<wchar_t>(0xfffd));
            ++i;
            continue;
        }
        if (i + need > utf8.size()) {
            out.push_back(static_cast<wchar_t>(0xfffd));
            break;
        }
        bool invalid = need == 2 && lead < 0xc2U;
        for (std::size_t n = 1; n < need; ++n) {
            const unsigned char cont = static_cast<unsigned char>(utf8[i + n]);
            if ((cont & 0xc0U) != 0x80U) {
                invalid = true;
                break;
            }
            codePoint = (codePoint << 6U) | (cont & 0x3fU);
        }
        if (invalid || codePoint > 0x10ffffU ||
            (codePoint >= 0xd800U && codePoint <= 0xdfffU) ||
            (need == 3 && codePoint < 0x800U) ||
            (need == 4 && codePoint < 0x10000U)) {
            out.push_back(static_cast<wchar_t>(0xfffd));
            ++i;
            continue;
        }
        i += need;
#if WCHAR_MAX <= 0xffff
        if (codePoint >= 0x10000U) {
            const std::uint32_t payload = codePoint - 0x10000U;
            out.push_back(static_cast<wchar_t>(0xd800U + (payload >> 10U)));
            out.push_back(static_cast<wchar_t>(0xdc00U + (payload & 0x3ffU)));
        } else {
            out.push_back(static_cast<wchar_t>(codePoint));
        }
#else
        out.push_back(static_cast<wchar_t>(codePoint));
#endif
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
