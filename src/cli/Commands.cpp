#include "cli/Commands.hpp"

#include "cli/Json.hpp"
#include "core/Classification.hpp"
#include "core/FileTime.hpp"
#include "core/Query.hpp"
#include "core/ScanEngine.hpp"
#include "core/SizeFormatter.hpp"
#include "core/index/IndexBuilder.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexRefresh.hpp"
#include "core/index/IndexStore.hpp"
#include "platform/windows/WindowsFileEnumerator.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef SPACELENS_VERSION_STRING
#define SPACELENS_VERSION_STRING "0.1.0"
#endif

namespace spacelens::cli {
namespace {

constexpr int kSchemaVersion = 1;

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

FileTimeTicks nowFileTime()
{
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

std::wstring fileExtensionLower(std::wstring_view name)
{
    const auto pos = name.find_last_of(L'.');
    if (pos == std::wstring_view::npos || pos == 0 || pos + 1 >= name.size()) {
        return {};
    }
    std::wstring ext(name.substr(pos + 1));
    for (wchar_t& ch : ext) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return ext;
}

std::string narrowClassification(const std::wstring& wide)
{
    std::string out;
    out.reserve(wide.size());
    for (wchar_t ch : wide) {
        if (ch < 128) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

bool classificationMatches(const Classification& cls, const std::wstring& filter)
{
    if (filter.empty()) {
        return true;
    }
    const StorageCategory wanted =
        parseStorageCategory(narrowClassification(filter));
    // If parse falls back to Unknown, also allow exact name match on toString.
    if (wanted != StorageCategory::Unknown ||
        narrowClassification(filter) == "unknown") {
        return cls.category == wanted;
    }
    return false;
}

void writeErrorJson(const char* command,
                    const std::wstring& root,
                    const char* error)
{
    std::cout << "{"
              << "\"schema_version\":" << kSchemaVersion << ","
              << "\"ok\":false,"
              << "\"command\":" << jsonString(command) << ","
              << "\"root\":" << jsonString(root) << ","
              << "\"error\":" << jsonString(error) << "}\n";
}

void writeCommonJsonFields(std::ostream& os,
                           const char* command,
                           const std::wstring& root,
                           const spacelens::ScanResult& result,
                           bool ok)
{
    const auto& pr = result.progress;
    const auto ms = elapsedMs(pr);
    os << "{"
       << "\"schema_version\":" << kSchemaVersion << ","
       << "\"ok\":" << jsonBool(ok) << ","
       << "\"command\":" << jsonString(command) << ","
       << "\"root\":" << jsonString(root) << ","
       << "\"files_scanned\":" << jsonUInt(pr.filesSeen) << ","
       << "\"directories_scanned\":" << jsonUInt(pr.directoriesSeen) << ","
       << "\"bytes_scanned\":" << jsonUInt(pr.bytesSeen) << ","
       << "\"elapsed_ms\":" << jsonUInt(ms) << ","
       << "\"access_denied\":" << jsonUInt(pr.accessDenied) << ","
       << "\"reparse_skipped\":" << jsonUInt(pr.reparsePointsSkipped) << ","
       << "\"other_errors\":" << jsonUInt(pr.otherErrors) << ","
       << "\"state\":" << jsonString(stateString(result.state)) << ","
       << "\"scan\":{"
       << "\"files\":" << jsonUInt(pr.filesSeen) << ","
       << "\"directories\":" << jsonUInt(pr.directoriesSeen) << ","
       << "\"bytes\":" << jsonUInt(pr.bytesSeen) << ","
       << "\"elapsed_ms\":" << jsonUInt(ms) << "}";
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

bool filePassesFilters(const DirectoryTree& tree,
                       FileIndex index,
                       const ParsedArgs& args,
                       FileTimeTicks now)
{
    const FileEntry& file = tree.file(index);
    if (args.minSize && file.size < *args.minSize) {
        return false;
    }
    if (!args.extension.empty()) {
        if (fileExtensionLower(file.name) != args.extension) {
            return false;
        }
    }
    if (args.olderThanDays) {
        if (file.lastWriteTime == 0 || now == 0 ||
            !isOlderThanDays(file.lastWriteTime, now, *args.olderThanDays)) {
            return false;
        }
    }
    if (!args.classification.empty()) {
        const auto path = tree.pathOfFile(index);
        const auto cls = classifyFile(file.name, path);
        if (!classificationMatches(cls, args.classification)) {
            return false;
        }
    }
    return true;
}

bool dirPassesFilters(const DirectoryTree& tree,
                      DirIndex index,
                      const ParsedArgs& args,
                      FileTimeTicks now)
{
    const DirectoryNode& node = tree.dir(index);
    if (args.minSize && node.recursiveSize < *args.minSize) {
        return false;
    }
    // --ext applies to files only.
    if (!args.extension.empty()) {
        return false;
    }
    if (args.olderThanDays) {
        const FileTimeTicks activity = node.newestDescendantWrite;
        if (activity == 0 || now == 0 ||
            !isOlderThanDays(activity, now, *args.olderThanDays)) {
            return false;
        }
    }
    if (!args.classification.empty()) {
        std::vector<std::wstring> children;
        children.reserve(node.children.size() + node.files.size());
        for (const DirIndex c : node.children) {
            children.push_back(tree.dir(c).name);
        }
        for (const FileIndex f : node.files) {
            children.push_back(tree.file(f).name);
        }
        const auto path = tree.pathOfDirectory(index);
        const auto cls =
            classifyDirectory(node.name, path, children.data(), children.size());
        if (!classificationMatches(cls, args.classification)) {
            return false;
        }
    }
    return true;
}

std::vector<PathSizeItem> filterTopFiles(const ScanResult& result,
                                         const ParsedArgs& args,
                                         FileTimeTicks now)
{
    std::vector<PathSizeItem> out;
    for (const auto& f : result.largestFiles) {
        if (f.fileIndex == InvalidFileIndex) {
            continue;
        }
        if (!filePassesFilters(result.tree, f.fileIndex, args, now)) {
            continue;
        }
        out.push_back(PathSizeItem{f.path, f.size});
        if (out.size() >= args.limit) {
            break;
        }
    }
    // If top-K during scan was smaller than needed due to filters, scan all files.
    if (out.size() < args.limit &&
        (args.minSize || !args.extension.empty() || args.olderThanDays ||
         !args.classification.empty())) {
        out.clear();
        std::vector<PathSizeItem> all;
        const std::size_t n = result.tree.fileCount();
        for (std::size_t i = 0; i < n; ++i) {
            const FileIndex idx = static_cast<FileIndex>(i);
            if (!filePassesFilters(result.tree, idx, args, now)) {
                continue;
            }
            all.push_back(PathSizeItem{result.tree.pathOfFile(idx),
                                       result.tree.file(idx).size});
        }
        std::sort(all.begin(), all.end(),
                  [](const PathSizeItem& a, const PathSizeItem& b) {
                      if (a.size_bytes != b.size_bytes) {
                          return a.size_bytes > b.size_bytes;
                      }
                      return a.path < b.path;
                  });
        if (all.size() > args.limit) {
            all.resize(args.limit);
        }
        return all;
    }
    return out;
}

std::vector<PathSizeItem> filterTopDirs(const DirectoryTree& tree,
                                        const ParsedArgs& args,
                                        FileTimeTicks now)
{
    std::vector<PathSizeItem> all;
    if (tree.empty()) {
        return all;
    }
    const std::size_t n = tree.directoryCount();
    for (std::size_t i = 0; i < n; ++i) {
        const DirIndex idx = static_cast<DirIndex>(i);
        if (!dirPassesFilters(tree, idx, args, now)) {
            continue;
        }
        all.push_back(
            PathSizeItem{tree.pathOfDirectory(idx), tree.dir(idx).recursiveSize});
    }
    std::sort(all.begin(), all.end(),
              [](const PathSizeItem& a, const PathSizeItem& b) {
                  if (a.size_bytes != b.size_bytes) {
                      return a.size_bytes > b.size_bytes;
                  }
                  return a.path < b.path;
              });
    if (all.size() > args.limit) {
        all.resize(args.limit);
    }
    return all;
}

std::vector<PathSizeItem> findFiles(const DirectoryTree& tree,
                                    const ParsedArgs& args,
                                    FileTimeTicks now)
{
    std::vector<PathSizeItem> all;
    if (tree.empty()) {
        return all;
    }
    const std::size_t n = tree.fileCount();
    for (std::size_t i = 0; i < n; ++i) {
        const FileIndex idx = static_cast<FileIndex>(i);
        if (!filePassesFilters(tree, idx, args, now)) {
            continue;
        }
        all.push_back(
            PathSizeItem{tree.pathOfFile(idx), tree.file(idx).size});
    }
    std::sort(all.begin(), all.end(),
              [](const PathSizeItem& a, const PathSizeItem& b) {
                  if (a.size_bytes != b.size_bytes) {
                      return a.size_bytes > b.size_bytes;
                  }
                  return a.path < b.path;
              });
    if (all.size() > args.limit) {
        all.resize(args.limit);
    }
    return all;
}

}  // namespace

ExitCode runScan(const ParsedArgs& args, std::stop_token stop)
{
    if (!pathExists(args.path) || !pathIsDirectory(args.path)) {
        std::cerr << "error: path is not an accessible directory\n";
        if (args.json) {
            writeErrorJson("scan", args.path, "inaccessible_root");
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
            writeErrorJson("top", args.path, "inaccessible_root");
        }
        return ExitCode::InaccessibleRoot;
    }

    const std::size_t topFiles =
        args.topMode == TopMode::Files ? std::max<std::size_t>(args.limit, 100) : 0;
    auto result = runEngine(args.path, topFiles, stop);
    const FileTimeTicks now = nowFileTime();

    std::vector<spacelens::PathSizeItem> items;
    if (result.state == spacelens::ScanState::Completed ||
        result.state == spacelens::ScanState::Cancelled) {
        if (args.topMode == TopMode::Files) {
            items = filterTopFiles(result, args, now);
        } else {
            items = filterTopDirs(result.tree, args, now);
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

ExitCode runFind(const ParsedArgs& args, std::stop_token stop)
{
    if (!pathExists(args.path) || !pathIsDirectory(args.path)) {
        std::cerr << "error: path is not an accessible directory\n";
        if (args.json) {
            writeErrorJson("find", args.path, "inaccessible_root");
        }
        return ExitCode::InaccessibleRoot;
    }

    auto result = runEngine(args.path, /*topFiles=*/0, stop);
    const FileTimeTicks now = nowFileTime();
    std::vector<PathSizeItem> items;
    if (result.state == ScanState::Completed ||
        result.state == ScanState::Cancelled) {
        items = findFiles(result.tree, args, now);
    }

    if (args.json) {
        printJsonResults(std::cout, "find", args.path, result, items,
                         /*includeTotal=*/true);
    } else {
        printHumanTable(items);
        std::cerr << "matches=" << items.size()
                  << " files_scanned=" << result.progress.filesSeen << "\n";
    }
    return mapState(result);
}

ExitCode runIndex(const ParsedArgs& args, std::stop_token stop)
{
    if (!pathExists(args.path) || !pathIsDirectory(args.path)) {
        std::cerr << "error: path is not an accessible directory\n";
        if (args.json) {
            writeErrorJson("index", args.path, "inaccessible_root");
        }
        return ExitCode::InaccessibleRoot;
    }

    const auto built = spacelens::buildIndexForRoot(args.path, stop);
    if (args.json) {
        const bool ok = built.state == IndexBuildState::Completed;
        std::cout << "{"
                  << "\"schema_version\":" << kSchemaVersion << ","
                  << "\"ok\":" << jsonBool(ok) << ","
                  << "\"command\":\"index\","
                  << "\"root\":" << jsonString(built.location.rootPath) << ","
                  << "\"index_schema_version\":" << kIndexSchemaVersion << ","
                  << "\"state\":"
                  << jsonString(built.state == IndexBuildState::Completed
                                    ? "completed"
                                    : built.state == IndexBuildState::Cancelled
                                          ? "cancelled"
                                          : "failed")
                  << ","
                  << "\"source\":\"persistent_index\","
                  << "\"index\":{"
                  << "\"path\":" << jsonString(built.location.dbPath) << ","
                  << "\"indexed_at\":" << jsonString(built.root.indexedAtIso)
                  << ","
                  << "\"file_count\":" << jsonUInt(built.root.fileCount) << ","
                  << "\"directory_count\":" << jsonUInt(built.root.dirCount)
                  << ","
                  << "\"logical_bytes\":" << jsonUInt(built.root.logicalBytes)
                  << "},"
                  << "\"elapsed_ms\":"
                  << jsonUInt(static_cast<std::uint64_t>(
                         built.elapsedSeconds * 1000.0 + 0.5));
        if (!built.error.empty()) {
            std::cout << ",\"error\":" << jsonString(built.error);
        }
        std::cout << "}\n";
    } else {
        if (built.state == IndexBuildState::Completed) {
            std::cout << "Index ready\n"
                      << "Root:   " << narrow(built.location.rootPath) << "\n"
                      << "DB:     " << narrow(built.location.dbPath) << "\n"
                      << "Files:  " << built.root.fileCount << "\n"
                      << "Dirs:   " << built.root.dirCount << "\n"
                      << "Bytes:  "
                      << SizeFormatter::format(built.root.logicalBytes) << "\n"
                      << "At:     " << built.root.indexedAtIso << "\n";
        } else {
            std::cerr << "index " << (built.state == IndexBuildState::Cancelled
                                          ? "cancelled"
                                          : "failed");
            if (!built.error.empty()) {
                std::cerr << ": " << built.error;
            }
            std::cerr << "\n";
        }
    }

    if (built.state == IndexBuildState::Completed) {
        return ExitCode::Success;
    }
    if (built.state == IndexBuildState::Cancelled) {
        return ExitCode::Cancelled;
    }
    return ExitCode::ScanFailed;
}

ExitCode runIndexStatus(const ParsedArgs& args)
{
    const auto status = spacelens::indexStatus(args.path);
    const auto probe = spacelens::probeIncremental(args.path);
    if (args.json) {
        std::cout << "{"
                  << "\"schema_version\":" << kSchemaVersion << ","
                  << "\"ok\":" << jsonBool(status.ok) << ","
                  << "\"command\":\"index_status\","
                  << "\"root\":" << jsonString(status.location.rootPath) << ","
                  << "\"source\":\"persistent_index\","
                  << "\"index_schema_version\":" << kIndexSchemaVersion << ","
                  << "\"index\":{"
                  << "\"path\":" << jsonString(status.location.dbPath) << ","
                  << "\"exists\":"
                  << jsonBool(indexDatabaseExists(status.location)) << ","
                  << "\"indexed_at\":" << jsonString(status.root.indexedAtIso)
                  << ","
                  << "\"full_indexed_at\":"
                  << jsonString(status.root.indexedAtIso) << ","
                  << "\"age_ms\":" << jsonUInt(status.age_ms) << ","
                  << "\"snapshot_age_ms\":" << jsonUInt(status.age_ms) << ","
                  << "\"file_count\":" << jsonUInt(status.root.fileCount) << ","
                  << "\"directory_count\":" << jsonUInt(status.root.dirCount)
                  << ","
                  << "\"logical_bytes\":" << jsonUInt(status.root.logicalBytes)
                  << ","
                  << "\"status\":"
                  << jsonString(toString(status.root.status)) << "},"
                  << "\"incremental_refresh\":{"
                  << "\"supported\":"
                  << jsonBool(probe.incrementalState ==
                              IncrementalRefreshState::Supported)
                  << ","
                  << "\"state\":"
                  << jsonString(toString(probe.incrementalState)) << ","
                  << "\"reason\":" << jsonString(probe.reason) << ","
                  << "\"fallback\":"
                  << jsonString(probe.incrementalState ==
                                        IncrementalRefreshState::Supported
                                    ? "none"
                                    : "full_rebuild")
                  << ","
                  << "\"refresh_method\":"
                  << jsonString(probe.checkpoint.lastRefreshMethod) << ","
                  << "\"last_incremental_refresh\":"
                  << jsonString(fileTimeTicksToIsoUtc(
                         probe.checkpoint.lastRefreshAtTicks))
                  << ","
                  << "\"checkpoint\":{"
                  << "\"usn_journal_id\":"
                  << jsonUInt(probe.checkpoint.usnJournalId) << ","
                  << "\"next_usn\":" << jsonUInt(probe.checkpoint.nextUsn) << ","
                  << "\"volume_serial\":"
                  << jsonUInt(probe.checkpoint.volumeSerial) << ","
                  << "\"status\":" << jsonString(probe.checkpoint.status)
                  << "}"
                  << "}";
        if (!status.error.empty()) {
            std::cout << ",\"error\":" << jsonString(status.error);
        }
        std::cout << "}\n";
    } else {
        if (!status.ok) {
            std::cerr << "index status: " << status.error << "\n";
        } else {
            std::cout << "Root:      " << narrow(status.location.rootPath) << "\n"
                      << "DB:        " << narrow(status.location.dbPath) << "\n"
                      << "Indexed:   " << status.root.indexedAtIso << "\n"
                      << "Age:       " << status.age_ms << " ms\n"
                      << "Files:     " << status.root.fileCount << "\n"
                      << "Dirs:      " << status.root.dirCount << "\n"
                      << "Bytes:     "
                      << SizeFormatter::format(status.root.logicalBytes)
                      << "\n"
                      << "Incremental: " << toString(probe.incrementalState)
                      << " (" << probe.reason << ")\n"
                      << "Last method: "
                      << (probe.checkpoint.lastRefreshMethod.empty()
                              ? "none"
                              : probe.checkpoint.lastRefreshMethod)
                      << "\n";
        }
    }
    if (!status.ok && status.error == "index_not_found") {
        return ExitCode::IndexNotFound;
    }
    return status.ok ? ExitCode::Success : ExitCode::ScanFailed;
}

ExitCode runIndexRefresh(const ParsedArgs& args, std::stop_token stop)
{
    if (!pathExists(args.path) || !pathIsDirectory(args.path)) {
        // Refresh still needs the root to exist for USN/path resolution.
        std::cerr << "error: path is not an accessible directory\n";
        if (args.json) {
            writeErrorJson("index_refresh", args.path, "inaccessible_root");
        }
        return ExitCode::InaccessibleRoot;
    }

    const auto result = spacelens::refreshIndex(args.path, stop);

    if (args.json) {
        const bool ok =
            result.outcome == IndexRefreshOutcome::Refreshed ||
            result.outcome == IndexRefreshOutcome::AlreadyCurrent;
        std::cout << "{"
                  << "\"schema_version\":" << kSchemaVersion << ","
                  << "\"ok\":" << jsonBool(ok) << ","
                  << "\"command\":\"index_refresh\","
                  << "\"root\":" << jsonString(result.location.rootPath) << ","
                  << "\"source\":\"persistent_index\","
                  << "\"index_schema_version\":" << kIndexSchemaVersion << ","
                  << "\"outcome\":" << jsonString(toString(result.outcome))
                  << ","
                  << "\"reason\":" << jsonString(result.reason) << ","
                  << "\"incremental_refresh\":{"
                  << "\"supported\":"
                  << jsonBool(result.incrementalState ==
                              IncrementalRefreshState::Supported)
                  << ","
                  << "\"state\":"
                  << jsonString(toString(result.incrementalState)) << ","
                  << "\"fallback\":"
                  << jsonString(
                         result.outcome ==
                                 IndexRefreshOutcome::FullRebuildRequired
                             ? "full_rebuild"
                             : "none")
                  << "},"
                  << "\"journal_records_seen\":"
                  << jsonUInt(result.journalRecordsSeen) << ","
                  << "\"records_in_root\":" << jsonUInt(result.recordsInRoot)
                  << ","
                  << "\"added\":" << jsonUInt(result.added) << ","
                  << "\"modified\":" << jsonUInt(result.modified) << ","
                  << "\"removed\":" << jsonUInt(result.removed) << ","
                  << "\"renamed\":" << jsonUInt(result.renamed) << ","
                  << "\"dirs_recomputed\":" << jsonUInt(result.dirsRecomputed)
                  << ","
                  << "\"rows_changed\":" << jsonUInt(result.rowsChanged) << ","
                  << "\"elapsed_ms\":"
                  << jsonUInt(static_cast<std::uint64_t>(
                         result.elapsedSeconds * 1000.0 + 0.5))
                  << ","
                  << "\"checkpoint\":{"
                  << "\"next_usn\":" << jsonUInt(result.checkpoint.nextUsn)
                  << ","
                  << "\"usn_journal_id\":"
                  << jsonUInt(result.checkpoint.usnJournalId) << ","
                  << "\"last_refresh_method\":"
                  << jsonString(result.checkpoint.lastRefreshMethod) << "}";
        if (!result.error.empty()) {
            std::cout << ",\"error\":" << jsonString(result.error);
        }
        std::cout << "}\n";
    } else {
        std::cout << "Refreshing " << narrow(result.location.rootPath) << "\n\n";
        if (result.outcome == IndexRefreshOutcome::AlreadyCurrent) {
            std::cout << "Index already current (no USN changes).\n";
        } else if (result.outcome == IndexRefreshOutcome::Refreshed) {
            std::cout << "Journal records seen: " << result.journalRecordsSeen
                      << "\n"
                      << "In-root records:      " << result.recordsInRoot << "\n"
                      << "Added:                " << result.added << "\n"
                      << "Modified:             " << result.modified << "\n"
                      << "Removed:              " << result.removed << "\n"
                      << "Renamed/Moved:        " << result.renamed << "\n"
                      << "Dirs recomputed:      " << result.dirsRecomputed
                      << "\n"
                      << "Elapsed:              "
                      << static_cast<std::uint64_t>(result.elapsedSeconds *
                                                        1000.0 +
                                                    0.5)
                      << " ms\n\n"
                      << "Index refreshed.\n";
        } else if (result.outcome ==
                   IndexRefreshOutcome::FullRebuildRequired) {
            std::cout << "Full rebuild required (" << result.reason << ").\n"
                      << "Run: spacelens index <path>\n";
        } else if (result.outcome == IndexRefreshOutcome::Cancelled) {
            std::cerr << "index refresh cancelled\n";
        } else if (result.outcome == IndexRefreshOutcome::IndexNotFound) {
            std::cerr << "index not found — run: spacelens index <path>\n";
        } else {
            std::cerr << "index refresh failed: "
                      << (result.error.empty() ? result.reason : result.error)
                      << "\n";
        }
    }

    switch (result.outcome) {
    case IndexRefreshOutcome::Refreshed:
    case IndexRefreshOutcome::AlreadyCurrent:
        return ExitCode::Success;
    case IndexRefreshOutcome::Cancelled:
        return ExitCode::Cancelled;
    case IndexRefreshOutcome::IndexNotFound:
        return ExitCode::IndexNotFound;
    case IndexRefreshOutcome::FullRebuildRequired:
        // Not a hard failure — agent should full rebuild. Exit 0 with outcome
        // in JSON; use ScanFailed only when failed.
        return ExitCode::Success;
    case IndexRefreshOutcome::Failed:
    default:
        return ExitCode::ScanFailed;
    }
}

ExitCode runIndexList(const ParsedArgs& args)
{
    const auto listed = spacelens::listIndexedRoots();
    if (args.json) {
        std::cout << "{"
                  << "\"schema_version\":" << kSchemaVersion << ","
                  << "\"ok\":true,"
                  << "\"command\":\"index_list\","
                  << "\"source\":\"persistent_index\","
                  << "\"indexes\":[";
        bool first = true;
        for (const auto& item : listed) {
            IndexLocation loc;
            loc.rootKey = item.rootKey;
            loc.dbPath = item.dbPath;
            loc.indexDir = item.dbPath;  // unused for open by path
            // Open by db path via locate is awkward; open file directly.
            try {
                // Reconstruct location from key only for openRead needs full loc.
                IndexLocation openLoc;
                openLoc.rootKey = item.rootKey;
                openLoc.dbPath = item.dbPath;
                openLoc.indexDir = item.dbPath.substr(
                    0, item.dbPath.find_last_of(L"\\/"));
                auto store = IndexStore::openRead(openLoc);
                auto meta = store.readRootMeta();
                if (!first) {
                    std::cout << ",";
                }
                first = false;
                std::cout << "{"
                          << "\"root\":"
                          << jsonString(meta ? meta->rootPath : L"") << ","
                          << "\"path\":" << jsonString(item.dbPath) << ","
                          << "\"root_key\":" << jsonString(item.rootKey);
                if (meta) {
                    std::cout << ",\"indexed_at\":"
                              << jsonString(meta->indexedAtIso)
                              << ",\"logical_bytes\":"
                              << jsonUInt(meta->logicalBytes);
                }
                std::cout << "}";
            } catch (...) {
                if (!first) {
                    std::cout << ",";
                }
                first = false;
                std::cout << "{\"path\":" << jsonString(item.dbPath)
                          << ",\"root_key\":" << jsonString(item.rootKey)
                          << ",\"error\":\"open_failed\"}";
            }
        }
        std::cout << "]}\n";
    } else {
        if (listed.empty()) {
            std::cout << "No indexes found under "
                      << narrow(spaceLensIndexesRoot()) << "\n";
        }
        for (const auto& item : listed) {
            std::cout << narrow(item.rootKey) << "  " << narrow(item.dbPath)
                      << "\n";
        }
    }
    return ExitCode::Success;
}

ExitCode runQuery(const ParsedArgs& args)
{
    IndexQuerySpec spec;
    spec.includeFiles = args.topMode == TopMode::Files;
    spec.includeDirectories = args.topMode == TopMode::Dirs;
    spec.minSize = args.minSize;
    if (!args.extension.empty()) {
        // extension already lowercase wide; convert to UTF-8 ascii
        std::string ext;
        for (wchar_t ch : args.extension) {
            if (ch < 128) {
                ext.push_back(static_cast<char>(ch));
            }
        }
        spec.extension = ext;
    }
    spec.olderThanDays = args.olderThanDays;
    if (!args.classification.empty()) {
        // Canonicalize via parseStorageCategory when possible.
        std::string raw;
        for (wchar_t ch : args.classification) {
            if (ch < 128) {
                raw.push_back(static_cast<char>(ch));
            }
        }
        const auto cat = parseStorageCategory(raw);
        spec.classification = toString(cat);
        // If user passed exact Unknown intentionally, keep Unknown.
        if (cat == StorageCategory::Unknown &&
            narrowClassification(args.classification) != "unknown" &&
            !raw.empty()) {
            // Keep original casing from toString of parse fallback — already Unknown.
            // Prefer exact string if it matches a known toString.
            spec.classification = toString(cat);
        }
    }
    if (!args.strength.empty()) {
        std::string s;
        for (wchar_t ch : args.strength) {
            if (ch < 128) {
                s.push_back(static_cast<char>(ch));
            }
        }
        // Normalize common aliases
        if (s == "strong" || s == "Strong") {
            spec.candidateStrength = "Strong";
        } else if (s == "moderate" || s == "Moderate") {
            spec.candidateStrength = "Moderate";
        } else if (s == "reviewonly" || s == "ReviewOnly" || s == "review") {
            spec.candidateStrength = "ReviewOnly";
        } else if (s == "none" || s == "None") {
            spec.candidateStrength = "None";
        } else {
            spec.candidateStrength = s;
        }
    }
    spec.limit = args.limit;

    const auto result = queryIndex(args.path, spec);

    if (args.json) {
        std::cout << "{"
                  << "\"schema_version\":" << kSchemaVersion << ","
                  << "\"ok\":" << jsonBool(result.ok) << ","
                  << "\"command\":\"query\","
                  << "\"source\":\"persistent_index\","
                  << "\"root\":" << jsonString(result.location.rootPath) << ","
                  << "\"index\":{"
                  << "\"path\":" << jsonString(result.location.dbPath) << ","
                  << "\"indexed_at\":" << jsonString(result.root.indexedAtIso)
                  << ","
                  << "\"age_ms\":" << jsonUInt(result.age_ms) << ","
                  << "\"index_schema_version\":" << kIndexSchemaVersion << "},"
                  << "\"matched_items\":" << jsonUInt(result.matched_items)
                  << ","
                  << "\"returned_items\":" << jsonUInt(result.returned_items)
                  << ","
                  << "\"matched_logical_bytes\":"
                  << jsonUInt(result.matched_logical_bytes) << ","
                  << "\"results\":[";
        for (std::size_t i = 0; i < result.hits.size(); ++i) {
            if (i > 0) {
                std::cout << ",";
            }
            const auto& h = result.hits[i];
            std::cout << "{\"path\":" << jsonString(h.path)
                      << ",\"kind\":"
                      << jsonString(h.kind == IndexEntryKind::Directory
                                        ? "directory"
                                        : "file")
                      << ",\"size_bytes\":" << jsonUInt(h.size_bytes)
                      << ",\"classification\":" << jsonString(h.classification)
                      << ",\"confidence\":" << jsonString(h.confidence)
                      << ",\"location_safety\":"
                      << jsonString(h.location_safety)
                      << ",\"reclaimability\":" << jsonString(h.reclaimability)
                      << ",\"candidate_strength\":"
                      << jsonString(h.candidate_strength) << "}";
        }
        std::cout << "]";
        if (!result.error.empty()) {
            std::cout << ",\"error\":" << jsonString(result.error);
        }
        std::cout << "}\n";
    } else {
        if (!result.ok) {
            std::cerr << "query failed: " << result.error << "\n";
        } else {
            std::cout << "SIZE           PATH\n";
            for (const auto& h : result.hits) {
                const std::string size = SizeFormatter::format(h.size_bytes);
                std::cout << size;
                if (size.size() < 14) {
                    std::cout << std::string(14 - size.size(), ' ');
                } else {
                    std::cout << ' ';
                }
                std::cout << narrow(h.path) << "\n";
            }
            std::cerr << "source=persistent_index matched="
                      << result.matched_items
                      << " returned=" << result.returned_items
                      << " age_ms=" << result.age_ms << "\n";
        }
    }

    if (!result.ok && result.error == "index_not_found") {
        return ExitCode::IndexNotFound;
    }
    return result.ok ? ExitCode::Success : ExitCode::ScanFailed;
}

ExitCode runCapabilities(const ParsedArgs& args)
{
    if (args.json) {
        std::cout << "{"
                  << "\"schema_version\":" << kSchemaVersion << ","
                  << "\"version\":" << jsonString(SPACELENS_VERSION_STRING) << ","
                  << "\"commands\":[\"scan\",\"top\",\"find\",\"index\","
                     "\"index status\",\"index list\",\"index refresh\",\"query\","
                     "\"capabilities\",\"help\",\"version\"],"
                  << "\"features\":{"
                  << "\"json\":true,"
                  << "\"cancellation\":true,"
                  << "\"persistent_index\":true,"
                  << "\"indexed_query\":true,"
                  << "\"incremental_index\":true,"
                  << "\"filesystem_mutation\":false,"
                  << "\"classification\":true,"
                  << "\"filters\":true"
                  << "},"
                  << "\"read_only\":true,"
                  << "\"filesystem_mutation\":false,"
                  << "\"index_schema_version\":" << kIndexSchemaVersion
                  << "}\n";
    } else {
        std::cout << "spacelens " << SPACELENS_VERSION_STRING << "\n"
                  << "commands: scan top find index index-refresh query "
                     "capabilities help version\n"
                  << "read_only: true\n"
                  << "filesystem_mutation: false\n"
                  << "features: json, cancellation, classification, filters, "
                     "persistent_index, indexed_query, incremental_index\n"
                  << "not available: incremental_index, filesystem_mutation\n"
                  << "index_schema_version: " << kIndexSchemaVersion << "\n";
    }
    return ExitCode::Success;
}

}  // namespace spacelens::cli
