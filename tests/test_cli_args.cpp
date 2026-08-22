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

    auto bd = parse({L"spacelens", L"breakdown", L"D:\\data", L"--under",
                     L"D:\\data\\Projects", L"--limit", L"5", L"--json"});
    SPACELENS_REQUIRE(bd.error.empty());
    SPACELENS_REQUIRE(bd.command == Command::Breakdown);
    SPACELENS_REQUIRE(bd.under == L"D:\\data\\Projects");
    SPACELENS_REQUIRE_EQ(bd.limit, 5ULL);
    SPACELENS_REQUIRE(bd.json);

    auto bdFromIndex = parse({L"spacelens", L"breakdown", L"D:\\data",
                              L"--from-index"});
    SPACELENS_REQUIRE(!bdFromIndex.error.empty());
}

SPACELENS_TEST(CliArgs_max_index_age_seconds)
{
    auto q = parse({L"spacelens", L"query", L"D:\\data", L"--files",
                    L"--max-index-age-seconds", L"3600"});
    SPACELENS_REQUIRE(q.error.empty());
    SPACELENS_REQUIRE(q.maxIndexAgeSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*q.maxIndexAgeSeconds, 3600ULL);

    auto o = parse({L"spacelens", L"overview", L"D:\\data", L"--from-index",
                    L"--max-index-age-seconds", L"10"});
    SPACELENS_REQUIRE(o.error.empty());
    SPACELENS_REQUIRE(o.fromIndex);
    SPACELENS_REQUIRE(o.maxIndexAgeSeconds.has_value());

    auto live = parse({L"spacelens", L"overview", L"D:\\data",
                       L"--max-index-age-seconds", L"10"});
    SPACELENS_REQUIRE(!live.error.empty());

    auto dups = parse({L"spacelens", L"duplicates", L"D:\\data",
                       L"--max-index-age-seconds", L"10"});
    SPACELENS_REQUIRE(!dups.error.empty());

    auto bd = parse({L"spacelens", L"breakdown", L"D:\\data",
                     L"--max-index-age-seconds", L"3600"});
    SPACELENS_REQUIRE(bd.error.empty());
    SPACELENS_REQUIRE(bd.command == Command::Breakdown);
    SPACELENS_REQUIRE(bd.maxIndexAgeSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*bd.maxIndexAgeSeconds, 3600ULL);

    auto fresh = parse({L"spacelens", L"query", L"D:\\data", L"--files",
                        L"--fresh"});
    SPACELENS_REQUIRE(!fresh.error.empty());
}

SPACELENS_TEST(CliArgs_app_storage_zalo_grammar)
{
    auto overview = parse({L"spacelens", L"app-storage", L"zalo", L"--json"});
    SPACELENS_REQUIRE(overview.error.empty());
    SPACELENS_REQUIRE(overview.command == Command::AppStorageZalo);
    SPACELENS_REQUIRE(overview.rootPaths.empty());
    SPACELENS_REQUIRE(overview.json);

    auto items = parse({L"spacelens", L"app-storage", L"zalo", L"items",
                        L"--root", L"C:\\ZaloDownloads", L"--largest", L"7",
                        L"--type", L"Document", L"--min-size", L"10MB",
                        L"--json"});
    SPACELENS_REQUIRE(items.error.empty());
    SPACELENS_REQUIRE(items.command == Command::AppStorageZaloItems);
    SPACELENS_REQUIRE_EQ(items.rootPaths.size(), 1U);
    SPACELENS_REQUIRE(items.rootPaths.front() == L"C:\\ZaloDownloads");
    SPACELENS_REQUIRE_EQ(items.limit, 7ULL);
    SPACELENS_REQUIRE_EQ(items.storageType, "document");
    SPACELENS_REQUIRE(items.minSize.has_value());
    SPACELENS_REQUIRE_EQ(*items.minSize, 10ULL * 1024ULL * 1024ULL);

    auto comparison = parse({L"spacelens", L"app-storage", L"zalo", L"items",
                              L"--root", L"C:\\ZaloDownloads", L"--compare",
                              L"C:\\Downloads", L"--compare", L"D:\\Documents"});
    SPACELENS_REQUIRE(comparison.error.empty());
    SPACELENS_REQUIRE_EQ(comparison.comparePaths.size(), 2U);
    SPACELENS_REQUIRE(comparison.comparePaths[0] == L"C:\\Downloads");
    SPACELENS_REQUIRE(comparison.comparePaths[1] == L"D:\\Documents");

    auto comparisonBadCommand = parse({L"spacelens", L"app-storage", L"zalo",
                                       L"--compare", L"C:\\Downloads"});
    SPACELENS_REQUIRE(!comparisonBadCommand.error.empty());

    auto unknown = parse({L"spacelens", L"app-storage", L"zalo", L"items",
                          L"--unknown"});
    SPACELENS_REQUIRE(unknown.error.empty());
    SPACELENS_REQUIRE(unknown.unknown);

    auto badPath = parse({L"spacelens", L"app-storage", L"zalo",
                          L"C:\\ZaloDownloads"});
    SPACELENS_REQUIRE(!badPath.error.empty());
    auto badOption = parse({L"spacelens", L"app-storage", L"zalo", L"--delete"});
    SPACELENS_REQUIRE(!badOption.error.empty());
}

SPACELENS_TEST(CliArgs_reclaim_plan)
{
    auto a = parse({L"spacelens", L"reclaim-plan", L"D:\\data", L"--json"});
    SPACELENS_REQUIRE(a.error.empty());
    SPACELENS_REQUIRE(a.command == Command::ReclaimPlan);
    SPACELENS_REQUIRE(a.json);
    SPACELENS_REQUIRE(a.reclaimSource == spacelens::ReclaimPlanSource::Auto);
    SPACELENS_REQUIRE_EQ(a.limit, 20ULL);

    auto live = parse({L"spacelens", L"reclaim-plan", L"D:\\data", L"--source",
                       L"live_scan", L"--target-free", L"30GB", L"--limit",
                       L"5", L"--json"});
    SPACELENS_REQUIRE(live.error.empty());
    SPACELENS_REQUIRE(live.reclaimSource ==
                      spacelens::ReclaimPlanSource::LiveScan);
    SPACELENS_REQUIRE(live.targetFree.has_value());
    SPACELENS_REQUIRE_EQ(*live.targetFree, 30ULL * 1024ULL * 1024ULL * 1024ULL);
    SPACELENS_REQUIRE_EQ(live.limit, 5ULL);

    auto idx = parse({L"spacelens", L"reclaim-plan", L"D:\\data", L"--source",
                      L"persistent_index", L"--max-index-age-seconds", L"3600"});
    SPACELENS_REQUIRE(idx.error.empty());
    SPACELENS_REQUIRE(idx.reclaimSource ==
                      spacelens::ReclaimPlanSource::PersistentIndex);
    SPACELENS_REQUIRE(idx.maxIndexAgeSeconds.has_value());

    auto badSrc = parse({L"spacelens", L"reclaim-plan", L"D:\\data", L"--source",
                         L"index_refresh"});
    SPACELENS_REQUIRE(!badSrc.error.empty());

    auto fromIndex = parse({L"spacelens", L"reclaim-plan", L"D:\\data",
                            L"--from-index"});
    SPACELENS_REQUIRE(!fromIndex.error.empty());

    auto del = parse({L"spacelens", L"reclaim-plan", L"D:\\data", L"--delete"});
    SPACELENS_REQUIRE(!del.error.empty());

    auto scanSrc = parse({L"spacelens", L"scan", L"D:\\data", L"--source",
                          L"live_scan"});
    SPACELENS_REQUIRE(!scanSrc.error.empty());
}
