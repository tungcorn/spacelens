#include "TestRunner.hpp"
#include "cli/Args.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace spacelens::cli;

namespace {

std::vector<std::wstring> makeArgvStorage(std::initializer_list<const wchar_t*> args)
{
    std::vector<std::wstring> storage;
    for (const wchar_t* a : args) {
        storage.emplace_back(a);
    }
    return storage;
}

ParsedArgs parse(std::initializer_list<const wchar_t*> args)
{
    auto storage = makeArgvStorage(args);
    std::vector<wchar_t*> argv;
    argv.reserve(storage.size());
    for (auto& s : storage) {
        argv.push_back(s.data());
    }
    return parseArgs(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

SPACELENS_TEST(CliReadonly_registered_commands_only_safe)
{
    // Explicit allow-list — destructive verbs must never appear here.
    constexpr std::array allowed{
        Command::Help,         Command::Version,      Command::Scan,
        Command::Top,          Command::Find,         Command::Capabilities,
        Command::Index,        Command::IndexStatus,  Command::IndexList,
        Command::IndexRefresh, Command::Query,        Command::Duplicates,
    };
    SPACELENS_REQUIRE_EQ(kRegisteredCommands.size(), allowed.size());
    for (const Command c : kRegisteredCommands) {
        const bool ok =
            std::find(allowed.begin(), allowed.end(), c) != allowed.end();
        SPACELENS_REQUIRE(ok);
    }
}

SPACELENS_TEST(CliReadonly_destructive_verbs_rejected)
{
    for (const wchar_t* verb : {L"delete", L"remove", L"rm", L"move", L"purge",
                                L"wipe", L"cleanup", L"dedupe", L"keep-one",
                                L"recycle", L"maintenance"}) {
        const auto args = parse({L"spacelens", verb, L"C:\\temp"});
        SPACELENS_REQUIRE(!args.error.empty());
    }
}

SPACELENS_TEST(CliArgs_capabilities)
{
    const auto args = parse({L"spacelens", L"capabilities", L"--json"});
    SPACELENS_REQUIRE(args.error.empty());
    SPACELENS_REQUIRE(args.command == Command::Capabilities);
    SPACELENS_REQUIRE(args.json);
}

SPACELENS_TEST(CliArgs_find_filters)
{
    const auto args = parse({L"spacelens", L"find", L"D:\\Models", L"--min-size",
                             L"500MB", L"--ext", L"gguf", L"--json"});
    SPACELENS_REQUIRE(args.error.empty());
    SPACELENS_REQUIRE(args.command == Command::Find);
    SPACELENS_REQUIRE(args.minSize.has_value());
    SPACELENS_REQUIRE_EQ(*args.minSize, 500ULL * 1024ULL * 1024ULL);
    SPACELENS_REQUIRE(args.extension == L"gguf");
}

SPACELENS_TEST(CliArgs_invalid_min_size)
{
    const auto args =
        parse({L"spacelens", L"top", L"D:\\", L"--files", L"--min-size", L"nope"});
    SPACELENS_REQUIRE(!args.error.empty());
}
