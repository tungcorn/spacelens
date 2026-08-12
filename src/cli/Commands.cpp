#include "cli/Commands.hpp"

#include "cli/Json.hpp"
#include "core/Query.hpp"
#include "core/ScanEngine.hpp"
#include "core/SizeFormatter.hpp"
#include "platform/windows/WindowsFileEnumerator.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace spacelens::cli {
namespace {

std::string narrow(const std::wstring& wide)
{
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
        nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

const char* stateString(spacelens::ScanState state)
{
    using spacelens::ScanState;
    switch (state) {
    case ScanState::Completed:
        return "completed";
    case ScanState::Cancelled:
        return "cancelled";
    case ScanState::Failed:
        return "failed";
    case ScanState::Running:
        return "running";
    case ScanState::Idle:
        return "idle";
    }
    return "unknown";
}

ExitCode mapState(const spacelens::ScanResult& result)
{
    using spacelens::ScanState;
    switch (result.state) {
    case ScanState::Completed:
        return ExitCode::Success;
    case ScanState::Cancelled:
        return ExitCode::Cancelled;
    case ScanState::Failed:
        // Heuristic: empty path / not found style messages → inaccessible.
        if (result.errorMessage.find(L"empty") != std::wstring::npos) {
            return ExitCode::InaccessibleRoot;
        }
        return ExitCode::ScanFailed;
    default:
        return ExitCode::InternalError;
    }
}

bool pathExists(const std::wstring& path)
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool pathIsDirectory(const std::wstring& path)
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::uint64_t elapsedMs(const spacelens::ScanProgress& p)
{
    if (p.elapsedSeconds <= 0.0) {
        return 0;
    }
    return static_cast<std::uint64_t>(p.elapsedSeconds * 1000.0 + 0.5);
}

void writeCommonJsonFields(std::ostream& os,
                           const char* command,
                           const std::wstring& root,
                           const spacelens::ScanResult& result,
                           bool ok)
{
    const auto& pr = result.progress;
    os << "{"
       << "\"ok\":" << jsonBool(ok) << ","
       << "\"command\":" << jsonString(command) << ","
       << "\"root\":" << jsonString(root) << ","
       << "\"files_scanned\":" << jsonUInt(pr.filesSeen) << ","
       << "\"directories_scanned\":" << jsonUInt(pr.directoriesSeen) << ","
       << "\"bytes_scanned\":" << jsonUInt(pr.bytesSeen) << ","
       << "\"elapsed_ms\":" << jsonUInt(elapsedMs(pr)) << ","
       << "\"access_denied\":" << jsonUInt(pr.accessDenied) << ","
       << "\"reparse_skipped\":" << jsonUInt(pr.reparsePointsSkipped) << ","
       << "\"other_errors\":" << jsonUInt(pr.otherErrors) << ","
       << "\"state\":" << jsonString(stateString(result.state));
}

spacelens::ScanResult runEngine(const std::wstring& path,
                                std::size_t topFiles,
                                std::stop_token stop)
{
    spacelens::WindowsFileEnumerator enumerator;
    spacelens::ScanEngine engine(enumerator);
    spacelens::ScanOptions options;
    options.topFileCount = topFiles;
    return engine.scan(path, options, stop);
}

void printHumanSummary(const spacelens::ScanResult& result)
{
    const auto& pr = result.progress;
    ByteSize total = 0;
    if (!result.tree.empty()) {
        total = result.tree.dir(result.tree.root()).recursiveSize;
    }
    std::cout << "Root:        " << narrow(result.tree.empty()
                                               ? L""
                                               : result.tree.pathOfDirectory(
                                                     result.tree.root()))
              << "\n"
              << "State:       " << stateString(result.state) << "\n"
              << "Files:       " << pr.filesSeen << "\n"
              << "Directories: " << pr.directoriesSeen << "\n"
              << "Total size:  " << SizeFormatter::format(total) << "\n"
              << "Elapsed:     " << pr.elapsedSeconds << " s\n"
              << "Access denied: " << pr.accessDenied
              << "  reparse skipped: " << pr.reparsePointsSkipped
              << "  other errors: " << pr.otherErrors << "\n";
}

void printHumanTable(const std::vector<spacelens::PathSizeItem>& items)
{
    std::cout << "SIZE           PATH\n";
    for (const auto& item : items) {
        const std::string size = SizeFormatter::format(item.size_bytes);
        std::cout << size;
        // pad to ~14
        if (size.size() < 14) {
            std::cout << std::string(14 - size.size(), ' ');
        } else {
            std::cout << ' ';
        }
        std::cout << narrow(item.path) << "\n";
    }
}

void printJsonResults(std::ostream& os,
                      const char* command,
                      const std::wstring& root,
                      const spacelens::ScanResult& result,
                      const std::vector<spacelens::PathSizeItem>& items,
                      bool includeTotal)
{
    const bool ok = result.state == spacelens::ScanState::Completed;
    writeCommonJsonFields(os, command, root, result, ok);
    if (includeTotal && !result.tree.empty()) {
        os << ",\"total_size_bytes\":"
           << jsonUInt(result.tree.dir(result.tree.root()).recursiveSize);
    }
    os << ",\"results\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        os << "{\"path\":" << jsonString(items[i].path)
           << ",\"size_bytes\":" << jsonUInt(items[i].size_bytes) << "}";
    }
    os << "]}\n";
}

}  // namespace

ExitCode runScan(const ParsedArgs& args, std::stop_token stop)
{
    if (!pathExists(args.path) || !pathIsDirectory(args.path)) {
        std::cerr << "error: path is not an accessible directory\n";
        if (args.json) {
            std::cout << "{\"ok\":false,\"command\":\"scan\",\"root\":"
                      << jsonString(args.path)
                      << ",\"error\":\"inaccessible_root\"}\n";
        }
        return ExitCode::InaccessibleRoot;
    }

    auto result = runEngine(args.path, /*topFiles=*/0, stop);
    const ExitCode code = mapState(result);

    if (args.json) {
        printJsonResults(std::cout, "scan", args.path, result, /*items=*/{},
                         /*includeTotal=*/true);
    } else {
        printHumanSummary(result);
    }

    if (code == ExitCode::Cancelled) {
        std::cerr << "scan cancelled\n";
    } else if (code == ExitCode::ScanFailed) {
        std::cerr << "scan failed\n";
    }
    return code;
}

ExitCode runTop(const ParsedArgs& args, std::stop_token stop)
{
    if (!pathExists(args.path) || !pathIsDirectory(args.path)) {
        std::cerr << "error: path is not an accessible directory\n";
        if (args.json) {
            std::cout << "{\"ok\":false,\"command\":\"top\",\"root\":"
                      << jsonString(args.path)
                      << ",\"error\":\"inaccessible_root\"}\n";
        }
        return ExitCode::InaccessibleRoot;
    }

    const std::size_t topFiles =
        args.topMode == TopMode::Files ? args.limit : 0;
    auto result = runEngine(args.path, topFiles, stop);

    std::vector<spacelens::PathSizeItem> items;
    if (result.state == spacelens::ScanState::Completed ||
        result.state == spacelens::ScanState::Cancelled) {
        if (args.topMode == TopMode::Files) {
            items = spacelens::topFilesFromResult(result);
        } else {
            items = spacelens::topDirectories(result.tree, args.limit);
        }
    }

    if (args.json) {
        printJsonResults(std::cout, "top", args.path, result, items,
                         /*includeTotal=*/true);
    } else {
        printHumanTable(items);
        std::cerr << "files=" << result.progress.filesSeen
                  << " dirs=" << result.progress.directoriesSeen
                  << " elapsed_s=" << result.progress.elapsedSeconds << "\n";
    }

    return mapState(result);
}

}  // namespace spacelens::cli
