#include "TestRunner.hpp"

#include "cli/Json.hpp"

using spacelens::cli::jsonEscape;
using spacelens::cli::jsonString;

SPACELENS_TEST(CliJson_escape_quotes_and_backslash)
{
    SPACELENS_REQUIRE_EQ(jsonEscape("a\"b\\c"), std::string("a\\\"b\\\\c"));
}

SPACELENS_TEST(CliJson_escape_controls)
{
    SPACELENS_REQUIRE_EQ(jsonEscape("a\nb\tc"), std::string("a\\nb\\tc"));
}

SPACELENS_TEST(CliJson_windows_path_string)
{
    const std::string s = jsonString(std::wstring_view(L"C:\\Users\\x"));
    // JSON string includes quotes and escaped backslashes.
    SPACELENS_REQUIRE(s.front() == '\"');
    SPACELENS_REQUIRE(s.back() == '\"');
    SPACELENS_REQUIRE(s.find("\\\\") != std::string::npos);
    SPACELENS_REQUIRE(s.find("C:") != std::string::npos);
}
