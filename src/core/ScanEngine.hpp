#pragma once

#include "core/IFileEnumerator.hpp"
#include "core/ScanTypes.hpp"

#include <functional>
#include <stop_token>
#include <string>

namespace spacelens {

// Defined only in ScanEngine.cpp (keeps Top-K implementation details private).
struct ScanTopFiles;

/// Synchronous recursive scanner. Builds a DirectoryTree and Top-K file list.
/// Safe to run on a worker thread; does not touch Qt.
class ScanEngine {
public:
    using ProgressCallback = std::function<void(const ScanProgress&)>;

    explicit ScanEngine(IFileEnumerator& enumerator);

    /// Scan `rootPath` into a new ScanResult. Honors stop_token between directories.
    [[nodiscard]] ScanResult scan(const std::wstring& rootPath,
                                  const ScanOptions& options,
                                  std::stop_token stopToken = {},
                                  ProgressCallback onProgress = {});

private:
    void scanDirectory(DirectoryTree& tree,
                       DirIndex dirIndex,
                       const std::wstring& dirPath,
                       const ScanOptions& options,
                       std::stop_token stopToken,
                       ScanProgress& progress,
                       ProgressCallback& onProgress,
                       ScanTopFiles& topFiles);

    void maybeReportProgress(ScanProgress& progress,
                             ProgressCallback& onProgress,
                             bool force);

    IFileEnumerator& m_enumerator;
    std::chrono::steady_clock::time_point m_start{};
    std::chrono::steady_clock::time_point m_lastProgress{};
};

}  // namespace spacelens
