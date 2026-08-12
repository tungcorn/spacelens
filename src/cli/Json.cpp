#include "cli/Json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace spacelens::cli {
namespace {

std::string narrowUtf8(std::wstring_view wide)
{
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
        nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

}  // namespace

std::string jsonEscape(std::string_view input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char ch : input) {
        switch (ch) {
        case '\"':
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
            if (ch < 0x20) {
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
    return jsonString(std::string_view{narrowUtf8(wide)});
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

}  // namespace spacelens::cli
