#include "TestRunner.hpp"

#include "cli/Args.hpp"

#include <vector>

using spacelens::cli::Command;
using spacelens::cli::ParsedArgs;
using spacelens::cli::TopMode;
using spacelens::cli::parseArgs;

namespace {

ParsedArgs parse(std::vector<std::wstring> tokens)
{
    std::vector<wchar_t*> argv;
    argv.reserve(tokens.size());
    for (auto& t : tokens) {
        argv.push_back(t.data());
    }
    return parseArgs(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

SPACELENS_TEST(CliArgs_help_default)
{
    auto a = parse({L"spacelens"});
    SPACELENS_REQUIRE(a.command == Command::Help);
    SPACELENS_REQUIRE(a.error.empty());
}

SPACELENS_TEST(CliArgs_top_files_json)
{
    auto a = parse({L"spacelens", L"top", L"C:\\Users", L"--files", L"--limit",
                    L"5", L"--json"});
    SPACELENS_REQUIRE(a.error.empty());
    SPACELENS_REQUIRE(a.command == Command::Top);
    SPACELENS_REQUIRE(a.path == L"C:\\Users");
    SPACELENS_REQUIRE(a.topMode == TopMode::Files);
    SPACELENS_REQUIRE(a.limit == 5);
    SPACELENS_REQUIRE(a.json);
}

SPACELENS_TEST(CliArgs_top_requires_mode)
{
    auto a = parse({L"spacelens", L"top", L"C:\\Users"});
    SPACELENS_REQUIRE(!a.error.empty());
}

SPACELENS_TEST(CliArgs_unknown_flag)
{
    auto a = parse({L"spacelens", L"scan", L"C:\\", L"--nope"});
    SPACELENS_REQUIRE(!a.error.empty());
}

SPACELENS_TEST(CliArgs_default_limit)
{
    auto a = parse({L"spacelens", L"top", L"D:\\data", L"--dirs"});
    SPACELENS_REQUIRE(a.error.empty());
    SPACELENS_REQUIRE(a.limit == 20);
    SPACELENS_REQUIRE(a.topMode == TopMode::Dirs);
}

SPACELENS_TEST(CliArgs_scan_missing_path)
{
    auto a = parse({L"spacelens", L"scan", L"--json"});
    SPACELENS_REQUIRE(!a.error.empty());
}
