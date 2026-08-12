#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace spacelens::cli {

enum class Command {
    Help,
    Version,
    Scan,
    Top,
};

enum class TopMode {
    None,
    Files,
    Dirs,
};

struct ParsedArgs {
    Command command = Command::Help;
    std::wstring path;
    bool json = false;
    TopMode topMode = TopMode::None;
    std::size_t limit = 20;
    std::string error;  // non-empty => usage error
};

/// Parse wide argv (Windows wmain). argv[0] is program name.
[[nodiscard]] ParsedArgs parseArgs(int argc, wchar_t** argv);

[[nodiscard]] std::string helpText();

}  // namespace spacelens::cli
