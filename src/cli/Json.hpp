#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace spacelens::cli {

/// Escape a UTF-8/narrow string for JSON string literals.
[[nodiscard]] std::string jsonEscape(std::string_view input);

/// Escape a wide path/string as a JSON UTF-8 string value (including quotes).
[[nodiscard]] std::string jsonString(std::wstring_view wide);

[[nodiscard]] std::string jsonString(std::string_view utf8);

[[nodiscard]] std::string jsonBool(bool value);
[[nodiscard]] std::string jsonUInt(std::uint64_t value);
[[nodiscard]] std::string jsonInt(std::int64_t value);

}  // namespace spacelens::cli
