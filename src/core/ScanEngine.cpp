#include "core/ScanEngine.hpp"

#include "core/TopKCollector.hpp"

#include <utility>

namespace spacelens {
namespace {

struct TopFileRecord {
    FileIndex index = InvalidFileIndex;
    ByteSize size = 0;
    std::wstring path;
};

struct TopFileCompare {
    bool operator()(const TopFileRecord& a, const TopFileRecord& b) const
    {
        // "Worse than": smaller size ranks worse. Equal size → larger path worse.
        if (a.size != b.size) {
            return a.size < b.size;
        }
        return a.path > b.path;
    }
};

constexpr auto kProgressInterval = std::chrono::milliseconds(100);

}  // namespace

// Out-of-line type referenced from ScanEngine.hpp (implementation detail).
struct ScanTopFiles {
    explicit ScanTopFiles(std::size_t k)
        : collector(k)
    {
    }

    void consider(FileIndex index, ByteSize size, std::wstring path)
    {
        TopFileRecord rec{index, size, std::move(path)};
        collector.consider(std::move(rec));
    }

    // Uses types from this translation unit; compare is defined above.
    TopKCollector<TopFileRecord, TopFileCompare> collector;
};

ScanEngine::ScanEngine(IFileEnumerator& enumerator)
    : m_enumerator(enumerator)
{
}

ScanResult ScanEngine::scan(const std::wstring& rootPath,
                            const ScanOptions& options,
                            std::stop_token stopToken,
                            ProgressCallback onProgress)
{
    ScanResult result;
    result.state = ScanState::Running;
    result.progress = {};
    m_start = std::chrono::steady_clock::now();
    m_lastProgress = m_start;

    if (rootPath.empty()) {
        result.state = ScanState::Failed;
        result.errorMessage = L"Root path is empty";
        return result;
    }

    if (stopToken.stop_requested()) {
        result.state = ScanState::Cancelled;
        return result;
    }

    const DirIndex root = result.tree.createRoot(rootPath);
    result.progress.directoriesSeen = 1;
    result.progress.currentPath = rootPath;
    maybeReportProgress(result.progress, onProgress, true);

    ScanTopFiles topFiles(options.topFileCount);

    scanDirectory(result.tree, root, rootPath, options, stopToken,
                  result.progress, onProgress, topFiles);

    if (stopToken.stop_requested()) {
        result.state = ScanState::Cancelled;
    } else {
        result.state = ScanState::Completed;
    }

    result.tree.recomputeAggregates();

    for (auto& item : topFiles.collector.sortedDescending()) {
        LargestFileItem out;
        out.fileIndex = item.index;
        out.size = item.size;
        out.path = std::move(item.path);
        result.largestFiles.push_back(std::move(out));
    }

    const auto elapsed = std::chrono::steady_clock::now() - m_start;
    result.progress.elapsedSeconds =
        std::chrono::duration<double>(elapsed).count();
    maybeReportProgress(result.progress, onProgress, true);
    return result;
}

void ScanEngine::scanDirectory(DirectoryTree& tree,
                               DirIndex dirIndex,
                               const std::wstring& dirPath,
                               const ScanOptions& options,
                               std::stop_token stopToken,
                               ScanProgress& progress,
                               ProgressCallback& onProgress,
                               ScanTopFiles& topFiles)
{
    if (stopToken.stop_requested()) {
        return;
    }

    progress.currentPath = dirPath;
    maybeReportProgress(progress, onProgress, false);

    EnumerateResult listing = m_enumerator.enumerate(dirPath);
    switch (listing.status) {
    case EnumerateStatus::Cancelled:
        return;
    case EnumerateStatus::AccessDenied:
        progress.accessDenied += 1;
        maybeReportProgress(progress, onProgress, false);
        return;
    case EnumerateStatus::NotFound:
        progress.otherErrors += 1;
        maybeReportProgress(progress, onProgress, false);
        return;
    case EnumerateStatus::Error:
        progress.otherErrors += 1;
        maybeReportProgress(progress, onProgress, false);
        return;
    case EnumerateStatus::Ok:
        break;
    }

    // First pass: files and record child directories for recursion.
    std::vector<std::pair<DirIndex, std::wstring>> childDirs;
    childDirs.reserve(listing.entries.size());

    for (auto& entry : listing.entries) {
        if (stopToken.stop_requested()) {
            return;
        }

        switch (entry.kind) {
        case EntryKind::File: {
            const FileIndex fi = tree.addFile(dirIndex,
                                              std::move(entry.name),
                                              entry.size,
                                              entry.lastWriteTime,
                                              entry.attributes);
            const std::wstring filePath = tree.pathOfFile(fi);
            topFiles.consider(fi, entry.size, filePath);
            progress.filesSeen += 1;
            progress.bytesSeen += entry.size;
            break;
        }
        case EntryKind::Directory: {
            const DirIndex child = tree.addDirectory(dirIndex, entry.name);
            std::wstring childPath = dirPath;
            if (!childPath.empty() && childPath.back() != L'\\' &&
                childPath.back() != L'/') {
                childPath.push_back(L'\\');
            }
            childPath.append(entry.name);
            childDirs.emplace_back(child, std::move(childPath));
            progress.directoriesSeen += 1;
            break;
        }
        case EntryKind::ReparseDirectory: {
            if (options.followDirectoryReparsePoints) {
                const DirIndex child = tree.addDirectory(dirIndex, entry.name);
                std::wstring childPath = dirPath;
                if (!childPath.empty() && childPath.back() != L'\\' &&
                    childPath.back() != L'/') {
                    childPath.push_back(L'\\');
                }
                childPath.append(entry.name);
                childDirs.emplace_back(child, std::move(childPath));
                progress.directoriesSeen += 1;
            } else {
                // Still record the directory node so the UI can show it existed,
                // but do not recurse into it.
                tree.addDirectory(dirIndex, std::move(entry.name));
                progress.directoriesSeen += 1;
                progress.reparsePointsSkipped += 1;
            }
            break;
        }
        case EntryKind::Other:
            progress.otherErrors += 1;
            break;
        }

        maybeReportProgress(progress, onProgress, false);
    }

    for (auto& [childIndex, childPath] : childDirs) {
        if (stopToken.stop_requested()) {
            return;
        }
        scanDirectory(tree, childIndex, childPath, options, stopToken,
                      progress, onProgress, topFiles);
    }
}

void ScanEngine::maybeReportProgress(ScanProgress& progress,
                                     ProgressCallback& onProgress,
                                     bool force)
{
    if (!onProgress) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && (now - m_lastProgress) < kProgressInterval) {
        return;
    }
    m_lastProgress = now;
    progress.elapsedSeconds =
        std::chrono::duration<double>(now - m_start).count();
    onProgress(progress);
}

}  // namespace spacelens
