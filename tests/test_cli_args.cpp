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

SPACELENS_TEST(CliArgs_duplicates_min_size)
{
    auto a = parse({L"spacelens", L"duplicates", L"D:\\Models", L"--min-size",
                    L"2MB", L"--json"});
    SPACELENS_REQUIRE(a.error.empty());
    SPACELENS_REQUIRE(a.command == Command::Duplicates);
    SPACELENS_REQUIRE(a.json);
    SPACELENS_REQUIRE(a.minSize.has_value());
    SPACELENS_REQUIRE_EQ(*a.minSize, 2ULL * 1024ULL * 1024ULL);
}

SPACELENS_TEST(CliArgs_duplicates_rejects_delete)
{
    auto a = parse({L"spacelens", L"duplicates", L"D:\\", L"--delete"});
    SPACELENS_REQUIRE(!a.error.empty());
    auto b = parse({L"spacelens", L"duplicates", L"D:\\", L"--dedupe"});
    SPACELENS_REQUIRE(!b.error.empty());
    auto c = parse({L"spacelens", L"duplicates", L"D:\\", L"--keep-one"});
    SPACELENS_REQUIRE(!c.error.empty());
}

SPACELENS_TEST(CliArgs_overview_and_opportunities)
{
    auto o = parse({L"spacelens", L"overview", L"D:\\data", L"--json"});
    SPACELENS_REQUIRE(o.error.empty());
    SPACELENS_REQUIRE(o.command == Command::Overview);
    SPACELENS_REQUIRE(o.limit == 10);
    SPACELENS_REQUIRE(o.json);

    auto p = parse({L"spacelens", L"opportunities", L"D:\\data", L"--from-index",
                    L"--min-size", L"1MB", L"--limit", L"5", L"--json"});
    SPACELENS_REQUIRE(p.error.empty());
    SPACELENS_REQUIRE(p.command == Command::Opportunities);
    SPACELENS_REQUIRE(p.fromIndex);
    SPACELENS_REQUIRE(p.limit == 5);
    SPACELENS_REQUIRE(p.minSize.has_value());

    auto q = parse({L"spacelens", L"query", L"D:\\data", L"--dirs", L"--under",
                    L"D:\\data\\Projects", L"--json"});
    SPACELENS_REQUIRE(q.error.empty());
    SPACELENS_REQUIRE(q.under == L"D:\\data\\Projects");

    auto bad = parse({L"spacelens", L"scan", L"D:\\", L"--from-index"});
    SPACELENS_REQUIRE(!bad.error.empty());

    auto cls = parse({L"spacelens", L"opportunities", L"D:\\data",
                      L"--classification", L"BuildArtifact", L"--json"});
    SPACELENS_REQUIRE(cls.error.empty());
    SPACELENS_REQUIRE(cls.command == Command::Opportunities);
    SPACELENS_REQUIRE(cls.classification == L"BuildArtifact");
    SPACELENS_REQUIRE(cls.json);

    auto underOpp = parse({L"spacelens", L"opportunities", L"D:\\data",
                           L"--from-index", L"--under", L"D:\\data\\Projects",
                           L"--json"});
    SPACELENS_REQUIRE(underOpp.error.empty());
    SPACELENS_REQUIRE(underOpp.under == L"D:\\data\\Projects");

    auto underScan = parse({L"spacelens", L"scan", L"D:\\", L"--under", L"D:\\x"});
    SPACELENS_REQUIRE(!underScan.error.empty());
}
