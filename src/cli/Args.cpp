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
        "  spacelens capabilities [--json]\n"
        "  spacelens help\n"
        "  spacelens version\n"
        "\n"
        "Filters (top/find):\n"
        "  --min-size S     Minimum logical size (binary units, e.g. 10MB)\n"
        "  --ext EXT        File extension, with or without a leading dot\n"
        "  --older-than D   Last-write age in whole days\n"
        "  --classification C  Classification name\n"
        "\n"
        "Options:\n"
        "  --json          Machine-readable JSON on stdout\n"
        "  --files         Top largest files (with top)\n"
        "  --dirs          Top largest directories (with top)\n"
        "  --limit N       Number of results (default 20)\n"
        "  -h, --help      Show this help\n"
        "\n"
        "Exit codes: 0 success, 2 usage, 3 inaccessible root, 4 scan failed, 5 cancelled\n"
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
    } else {
        out.error = "Unknown command. Run 'spacelens help'.";
        return out;
    }

    // Remaining tokens after command.
    for (int i = 2; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--json") {
            out.json = true;
            continue;
        }
        if (arg == L"--files") {
            if (out.command != Command::Top) {
                out.error = "--files is only valid with 'top'.";
                return out;
            }
            out.topMode = TopMode::Files;
            continue;
        }
        if (arg == L"--dirs") {
            if (out.command != Command::Top) {
                out.error = "--dirs is only valid with 'top'.";
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
            if (out.command != Command::Top && out.command != Command::Find) {
                out.error = "--min-size is only valid with 'top' or 'find'.";
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
            if (out.command != Command::Top && out.command != Command::Find) {
                out.error = "--ext is only valid with 'top' or 'find'.";
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
            if (out.command != Command::Top && out.command != Command::Find) {
                out.error = "--older-than is only valid with 'top' or 'find'.";
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
            if (out.command != Command::Top && out.command != Command::Find) {
                out.error =
                    "--classification is only valid with 'top' or 'find'.";
                return out;
            }
            if (i + 1 >= argc || argv[i + 1][0] == L'\0') {
                out.error = "--classification requires a value.";
                return out;
            }
            out.classification.assign(argv[++i]);
            continue;
        }
        if (arg == L"-h" || arg == L"--help") {
            out.command = Command::Help;
            out.error.clear();
            return out;
        }
        if (!arg.empty() && arg[0] == L'-') {
            out.error = "Unknown option.";
            return out;
        }
        // Positional path (first non-option).
        if (out.path.empty()) {
            out.path.assign(arg);
        } else {
            out.error = "Unexpected extra argument.";
            return out;
        }
    }

    if (out.command == Command::Scan || out.command == Command::Top ||
        out.command == Command::Find) {
        if (out.path.empty()) {
            out.error = "Missing path argument.";
            return out;
        }
    }
    if (out.command == Command::Top && out.topMode == TopMode::None) {
        out.error = "'top' requires --files or --dirs.";
        return out;
    }

    return out;
}

}  // namespace spacelens::cli
