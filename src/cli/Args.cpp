#include "cli/Args.hpp"

#include <cstdlib>
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
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (end == text || (end && *end != L'\0')) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

}  // namespace

std::string helpText()
{
    return
        "SpaceLens — native Windows storage analyzer\n"
        "\n"
        "Usage:\n"
        "  spacelens scan <path> [--json]\n"
        "  spacelens top  <path> (--files|--dirs) [--limit N] [--json]\n"
        "  spacelens help\n"
        "  spacelens version\n"
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

    if (out.command == Command::Scan || out.command == Command::Top) {
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
