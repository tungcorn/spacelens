#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

/// One directory in the scan result. Relationships are indices into DirectoryTree.
struct DirectoryNode {
    std::wstring name;  // leaf name only (root may be a full path or drive)
    DirIndex parent = InvalidDirIndex;
    ByteSize directFileSize = 0;   // sum of files stored directly under this dir
    ByteSize recursiveSize = 0;    // directFileSize + sum(child.recursiveSize)
    std::uint64_t fileCount = 0;   // files stored directly under this dir
    std::uint64_t totalFileCount = 0; // recursive file count
    std::uint64_t childDirCount = 0;  // direct child directories
    std::vector<DirIndex> children;
    std::vector<FileIndex> files;
};

/// One file in the scan result. Path is reconstructed via parent directory.
struct FileEntry {
    std::wstring name;
    DirIndex parent = InvalidDirIndex;
    ByteSize size = 0;
    std::uint64_t lastWriteTime = 0;  // FILETIME as 64-bit, 0 if unknown
    std::uint32_t attributes = 0;
};

/// Owns all directory and file records for one scan.
/// Paths are reconstructed from parent indices; full paths are not stored per entry.
class DirectoryTree {
public:
    DirectoryTree() = default;

    DirectoryTree(const DirectoryTree&) = delete;
    DirectoryTree& operator=(const DirectoryTree&) = delete;
    DirectoryTree(DirectoryTree&&) noexcept = default;
    DirectoryTree& operator=(DirectoryTree&&) noexcept = default;

    [[nodiscard]] bool empty() const noexcept { return m_dirs.empty(); }
    [[nodiscard]] DirIndex root() const noexcept { return m_root; }
    [[nodiscard]] std::size_t directoryCount() const noexcept { return m_dirs.size(); }
    [[nodiscard]] std::size_t fileCount() const noexcept { return m_files.size(); }

    [[nodiscard]] const DirectoryNode& dir(DirIndex index) const;
    [[nodiscard]] DirectoryNode& dir(DirIndex index);
    [[nodiscard]] const FileEntry& file(FileIndex index) const;
    [[nodiscard]] FileEntry& file(FileIndex index);

    /// Create the root node. Name may be a full path (e.g. L"C:\\Users").
    DirIndex createRoot(std::wstring name);

    /// Add a child directory under parent. Returns its index.
    DirIndex addDirectory(DirIndex parent, std::wstring name);

    /// Add a file under parent. Updates parent's direct size/count.
    FileIndex addFile(DirIndex parent,
                      std::wstring name,
                      ByteSize size,
                      std::uint64_t lastWriteTime = 0,
                      std::uint32_t attributes = 0);

    /// Bottom-up aggregation: set recursiveSize / totalFileCount for all nodes.
    /// Call after the full tree is built (or after a subtree is complete).
    void recomputeAggregates();

    /// Reconstruct absolute path for a directory or file.
    [[nodiscard]] std::wstring pathOfDirectory(DirIndex index) const;
    [[nodiscard]] std::wstring pathOfFile(FileIndex index) const;

    /// Direct children sorted by recursiveSize descending (stable by name).
    [[nodiscard]] std::vector<DirIndex> largestChildDirectories(DirIndex parent,
                                                                std::size_t limit) const;

private:
    void recomputeNode(DirIndex index);

    std::vector<DirectoryNode> m_dirs;
    std::vector<FileEntry> m_files;
    DirIndex m_root = InvalidDirIndex;
};

}  // namespace spacelens
