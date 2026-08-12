#pragma once

#include "core/DirectoryTree.hpp"
#include "core/Types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace spacelens {

enum class ScanState {
    Idle,
    Running,
    Completed,
    Cancelled,
    Failed
};

struct ScanProgress {
    std::wstring currentPath;
    std::uint64_t filesSeen = 0;
    std::uint64_t directoriesSeen = 0;
    ByteSize bytesSeen = 0;
    std::uint64_t accessDenied = 0;
    std::uint64_t reparsePointsSkipped = 0;
    std::uint64_t otherErrors = 0;
    double elapsedSeconds = 0.0;
};

struct LargestFileItem {
    FileIndex fileIndex = InvalidFileIndex;
    std::wstring path;
    ByteSize size = 0;
};

struct ScanOptions {
    std::size_t topFileCount = 100;
    /// If true, directory reparse points are entered (not recommended).
    bool followDirectoryReparsePoints = false;
};

struct ScanResult {
    ScanState state = ScanState::Idle;
    DirectoryTree tree;
    std::vector<LargestFileItem> largestFiles;
    ScanProgress progress;
    std::wstring errorMessage;
};

/// Thread-safe progress snapshot published by the scanner.
class ScanProgressAtomics {
public:
    void reset()
    {
        filesSeen.store(0, std::memory_order_relaxed);
        directoriesSeen.store(0, std::memory_order_relaxed);
        bytesSeen.store(0, std::memory_order_relaxed);
        accessDenied.store(0, std::memory_order_relaxed);
        reparsePointsSkipped.store(0, std::memory_order_relaxed);
        otherErrors.store(0, std::memory_order_relaxed);
    }

    std::atomic<std::uint64_t> filesSeen{0};
    std::atomic<std::uint64_t> directoriesSeen{0};
    std::atomic<std::uint64_t> bytesSeen{0};
    std::atomic<std::uint64_t> accessDenied{0};
    std::atomic<std::uint64_t> reparsePointsSkipped{0};
    std::atomic<std::uint64_t> otherErrors{0};
};

}  // namespace spacelens
