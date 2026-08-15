#include "cli/Args.hpp"

#include "core/SizeParse.hpp"

#include <cerrno>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <string>
#include <string_view>

namespace spacelens::cli {
namespace {

bool eq(const wchar_t* a, const wchar_t* b)
{
    return std::wstring_view(a) == std::wstring_view(b);
}

std::optional<std::size_t> parseLimit(const wchar_t* text)
{
    if (text == nullptr || text[0] == L'\0') {
        return std::nullopt;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (end == text || (end && *end != L'\0') || errno == ERANGE ||
        value > static_cast<unsigned long long>(
                    std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

std::optional<std::uint64_t> parseDays(const wchar_t* text)
{
    if (text == nullptr || text[0] == L'\0') {
        return std::nullopt;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (end == text || (end && *end != L'\0') || errno == ERANGE) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value);
}

std::wstring normalizeExtension(std::wstring value)
{
    if (!value.empty() && value.front() == L'.') {
        value.erase(value.begin());
    }
    for (wchar_t& ch : value) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

bool isFilterCommand(Command c)
{
    return c == Command::Top || c == Command::Find || c == Command::Query ||
           c == Command::Duplicates || c == Command::Opportunities ||
           c == Command::Overview;
}

}  // namespace

std::string helpText()
{
    return
        "SpaceLens — native Windows storage analyzer\n"
        "\n"
        "Usage:\n"
        "  spacelens scan <path> [--json]\n"
        "  spacelens top  <path> (--files|--dirs) [filters] [--limit N] [--json]\n"
        "  spacelens find <path> [filters] [--limit N] [--json]\n"
        "  spacelens index <path> [--json]\n"
        "  spacelens index status <path> [--json]\n"
        "  spacelens index list [--json]\n"
        "  spacelens index refresh <path> [--json]\n"
        "  spacelens query <path> (--files|--dirs) [filters] [--under P] [--limit N] [--max-index-age-seconds N] [--json]\n"
        "  spacelens overview <path> [--from-index] [--limit N] [--max-index-age-seconds N] [--json]\n"
        "  spacelens opportunities <path> [--from-index] [--min-size S] [--older-than D] [--classification C] [--under P] [--limit N] [--max-index-age-seconds N] [--json]\n"
        "  spacelens breakdown <path> [--under P] [--limit N] [--max-index-age-seconds N] [--json]\n"
        "  spacelens duplicates <path> [--min-size S] [--json]\n"
        "  spacelens capabilities [--json]\n"
        "  spacelens help\n"
        "  spacelens version\n"
        "\n"
        "Filters (top/find/query/opportunities):\n"
        "  --min-size S     Minimum logical size (binary units, e.g. 10MB)\n"
        "  --ext EXT        File extension, with or without a leading dot\n"
        "  --older-than D   Last-write age in whole days\n"
        "  --classification C  Classification name\n"
        "  --strength S     Reclaim candidate strength (query; e.g. Strong)\n"
        "  --under P        Restrict query/opportunities/breakdown to P and descendants\n"
        "\n"
        "Options:\n"
        "  --json          Machine-readable JSON on stdout\n"
        "  --files         Files mode (top/query)\n"
        "  --dirs          Directories mode (top/query)\n"
        "  --from-index    Use a published index (overview/opportunities)\n"
        "  --max-index-age-seconds N  Fail if the published snapshot is older than N seconds\n"
        "                  (query / overview --from-index / opportunities --from-index / breakdown)\n"
        "  --limit N       Number of results (default 20; overview default 10;\n"
        "                  breakdown: top extensions, default 20)\n"
        "  -h, --help      Show this help\n"
        "\n"
        "Index notes:\n"
        "  index builds a snapshot under %LOCALAPPDATA%\\SpaceLens\\indexes\n"
        "  index refresh applies USN deltas when safe; else full_rebuild_required\n"
        "  query / breakdown use the persistent index only (no live rescan fallback)\n"
        "  duplicates finds exact file-content copies from an index, then live-verifies\n"
        "  Source filesystem remains read-only; SpaceLens never mutates the USN journal\n"
        "\n"
        "Exit codes: 0 success, 2 usage, 3 inaccessible root, 4 scan/index failed,\n"
        "            5 cancelled, 6 index not found\n"
        "stdout = results; stderr = diagnostics/errors\n";
}

ParsedArgs parseArgs(int argc, wchar_t** argv)
{
    ParsedArgs out;

    if (argc <= 1) {
        out.command = Command::Help;
        return out;
    }

    const std::wstring_view cmd = argv[1];

    if (cmd == L"help" || cmd == L"-h" || cmd == L"--help") {
        out.command = Command::Help;
        return out;
    }
    if (cmd == L"version" || cmd == L"--version") {
        out.command = Command::Version;
        return out;
    }

    if (cmd == L"scan") {
        out.command = Command::Scan;
    } else if (cmd == L"top") {
        out.command = Command::Top;
    } else if (cmd == L"find") {
        out.command = Command::Find;
    } else if (cmd == L"capabilities") {
        out.command = Command::Capabilities;
    } else if (cmd == L"query") {
        out.command = Command::Query;
    } else if (cmd == L"duplicates") {
        out.command = Command::Duplicates;
    } else if (cmd == L"overview") {
        out.command = Command::Overview;
        out.limit = 10;
    } else if (cmd == L"opportunities") {
        out.command = Command::Opportunities;
    } else if (cmd == L"breakdown") {
        out.command = Command::Breakdown;
    } else if (cmd == L"index") {
        // index | index status | index list | index refresh
        if (argc >= 3 && std::wstring_view(argv[2]) == L"status") {
            out.command = Command::IndexStatus;
        } else if (argc >= 3 && std::wstring_view(argv[2]) == L"list") {
            out.command = Command::IndexList;
        } else if (argc >= 3 && std::wstring_view(argv[2]) == L"refresh") {
            out.command = Command::IndexRefresh;
        } else {
            out.command = Command::Index;
        }
    } else {
        out.error = "Unknown command. Run 'spacelens help'.";
        return out;
    }

    const int start =
        (out.command == Command::IndexStatus || out.command == Command::IndexList ||
         out.command == Command::IndexRefresh)
            ? 3
            : 2;

    for (int i = start; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--json") {
            out.json = true;
            continue;
        }
        if (arg == L"--files") {
            if (out.command != Command::Top && out.command != Command::Query) {
                out.error = "--files is only valid with 'top' or 'query'.";
                return out;
            }
            out.topMode = TopMode::Files;
            continue;
        }
        if (arg == L"--dirs") {
            if (out.command != Command::Top && out.command != Command::Query) {
                out.error = "--dirs is only valid with 'top' or 'query'.";
                return out;
            }
            out.topMode = TopMode::Dirs;
            continue;
        }
        if (arg == L"--limit") {
            if (i + 1 >= argc) {
                out.error = "--limit requires a value.";
                return out;
            }
            const auto limit = parseLimit(argv[++i]);
            if (!limit) {
                out.error = "Invalid --limit value.";
                return out;
            }
            out.limit = *limit;
            continue;
        }
        if (arg == L"--min-size") {
            if (!isFilterCommand(out.command)) {
                out.error =
                "--min-size is only valid with top/find/query/duplicates/opportunities.";
                return out;
            }
            if (i + 1 >= argc) {
                out.error = "--min-size requires a value.";
                return out;
            }
            const auto parsed = spacelens::parseSize(std::wstring_view(argv[++i]));
            if (!parsed.error.empty()) {
                out.error = "Invalid --min-size value: " + parsed.error;
                return out;
            }
            out.minSize = parsed.bytes;
            continue;
        }
        if (arg == L"--ext") {
            if (out.command != Command::Top && out.command != Command::Find &&
                out.command != Command::Query) {
                out.error = "--ext is only valid with top/find/query.";
                return out;
            }
            if (i + 1 >= argc || argv[i + 1][0] == L'\0') {
                out.error = "--ext requires a value.";
                return out;
            }
            out.extension = normalizeExtension(argv[++i]);
            if (out.extension.empty()) {
                out.error = "--ext requires a non-empty extension.";
                return out;
            }
            continue;
        }
        if (arg == L"--older-than") {
            if (out.command != Command::Top && out.command != Command::Find &&
                out.command != Command::Query &&
                out.command != Command::Opportunities) {
                out.error = "--older-than is only valid with top/find/query/opportunities.";
                return out;
            }
            if (i + 1 >= argc) {
                out.error = "--older-than requires a value.";
                return out;
            }
            const auto days = parseDays(argv[++i]);
            if (!days) {
                out.error = "Invalid --older-than value.";
                return out;
            }
            out.olderThanDays = *days;
            continue;
        }
        if (arg == L"--classification") {
            if (out.command != Command::Top && out.command != Command::Find &&
                out.command != Command::Query &&
                out.command != Command::Opportunities) {
                out.error =
                    "--classification is only valid with top/find/query/opportunities.";
                return out;
            }
            if (i + 1 >= argc || argv[i + 1][0] == L'\0') {
                out.error = "--classification requires a value.";
                return out;
            }
            out.classification = argv[++i];
            continue;
        }
        if (arg == L"--from-index") {
            if (out.command != Command::Overview &&
                out.command != Command::Opportunities) {
                out.error = "--from-index is only valid with overview/opportunities.";
                return out;
            }
            out.fromIndex = true;
            continue;
        }
        if (arg == L"--max-index-age-seconds") {
            if (out.command != Command::Query &&
                out.command != Command::Overview &&
                out.command != Command::Opportunities &&
                out.command != Command::Breakdown) {
                out.error =
                    "--max-index-age-seconds is only valid with query, "
                    "overview --from-index, opportunities --from-index, "
                    "or breakdown.";
                return out;
            }
            if (i + 1 >= argc) {
                out.error = "--max-index-age-seconds requires a value.";
                return out;
            }
            const auto seconds = parseDays(argv[++i]);
            if (!seconds) {
                out.error = "Invalid --max-index-age-seconds value.";
                return out;
            }
            out.maxIndexAgeSeconds = *seconds;
            continue;
        }
        if (arg == L"--under") {
            if (out.command != Command::Query &&
                out.command != Command::Opportunities &&
                out.command != Command::Breakdown) {
                out.error = "--under is only valid with query/opportunities/breakdown.";
                return out;
            }
            if (i + 1 >= argc || argv[i + 1][0] == L'\0') {
                out.error = "--under requires a path.";
                return out;
            }
            out.under = argv[++i];
            continue;
        }
        if (arg == L"--strength") {
            if (out.command != Command::Query) {
                out.error = "--strength is only valid with 'query'.";
                return out;
            }
            if (i + 1 >= argc || argv[i + 1][0] == L'\0') {
                out.error = "--strength requires a value.";
                return out;
            }
            out.strength = argv[++i];
            continue;
        }
        if (arg == L"-h" || arg == L"--help") {
            out.command = Command::Help;
            return out;
        }
        if (!arg.empty() && arg.front() == L'-') {
            out.error = "Unknown option.";
            return out;
        }
        if (out.command == Command::IndexList) {
            out.error = "'index list' does not take a path.";
            return out;
        }
        if (!out.path.empty()) {
            out.error = "Unexpected extra argument.";
            return out;
        }
        out.path.assign(arg.begin(), arg.end());
    }

    if (out.command == Command::Scan || out.command == Command::Top ||
        out.command == Command::Find || out.command == Command::Index ||
        out.command == Command::IndexStatus ||
        out.command == Command::IndexRefresh || out.command == Command::Query ||
        out.command == Command::Duplicates || out.command == Command::Overview ||
        out.command == Command::Opportunities ||
        out.command == Command::Breakdown) {
        if (out.path.empty()) {
            out.error = "Missing path argument.";
            return out;
        }
    }
    if (out.command == Command::Top || out.command == Command::Query) {
        if (out.topMode == TopMode::None) {
            out.error = "Specify --files or --dirs.";
            return out;
        }
    }
    if (out.maxIndexAgeSeconds.has_value() &&
        (out.command == Command::Overview ||
         out.command == Command::Opportunities) &&
        !out.fromIndex) {
        out.error =
            "--max-index-age-seconds requires --from-index on overview/"
            "opportunities.";
        return out;
    }

    return out;
}

}  // namespace spacelens::cli
