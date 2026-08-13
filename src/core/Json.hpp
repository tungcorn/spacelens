#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace spacelens {

/// Convert UTF-16/UTF-32 wide text to UTF-8 without a platform dependency.
[[nodiscard]] std::string utf8FromWide(std::wstring_view wide);

/// Escape an UTF-8/narrow string for use inside a JSON string literal.
[[nodiscard]] std::string jsonEscape(std::string_view input);

/// Encode a wide or UTF-8 string as a quoted JSON string value.
[[nodiscard]] std::string jsonString(std::wstring_view wide);
[[nodiscard]] std::string jsonString(std::string_view utf8);

[[nodiscard]] std::string jsonBool(bool value);
[[nodiscard]] std::string jsonUInt(std::uint64_t value);
[[nodiscard]] std::string jsonInt(std::int64_t value);

namespace json {
using ::spacelens::jsonBool;
using ::spacelens::jsonEscape;
using ::spacelens::jsonInt;
using ::spacelens::jsonString;
using ::spacelens::jsonUInt;
using ::spacelens::utf8FromWide;
}  // namespace json

}  // namespace spacelens
