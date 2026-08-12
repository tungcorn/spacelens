#pragma once

#include "core/DirectoryTree.hpp"
#include "core/FileTime.hpp"
#include "core/ScanTypes.hpp"
#include "core/TopKCollector.hpp"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

struct PathSizeItem {
    std::wstring path;
    ByteSize size_bytes = 0;
};

struct FileQuery {
    ByteSize minSize = 0;
    std::wstring extension;  // normalized without a leading dot
    std::uint64_t olderThanDays = 0;
    FileTimeTicks nowTicks = 0;
    std::size_t limit = 20;
    std::function<bool(std::wstring_view, const FileEntry&)> predicate;
};

/// Find files matching the supplied filters, sorted largest first.
[[nodiscard]] inline std::vector<PathSizeItem> findFiles(
    const DirectoryTree& tree,
    const FileQuery& query)
{
    if (tree.empty() || query.limit == 0) {
        return {};
    }

    auto extensionMatches = [&query](std::wstring_view name) {
        if (query.extension.empty()) {
            return true;
        }
        const std::size_t dot = name.rfind(L'.');
        if (dot == std::wstring_view::npos || dot + 1 >= name.size()) {
            return false;
        }
        const std::wstring_view actual = name.substr(dot + 1);
        if (actual.size() != query.extension.size()) {
            return false;
        }
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (std::towlower(actual[i]) != std::towlower(query.extension[i])) {
                return false;
            }
        }
        return true;
    };

    std::vector<PathSizeItem> out;
    out.reserve(tree.fileCount());
    for (std::size_t i = 0; i < tree.fileCount(); ++i) {
        const auto fileIndex = static_cast<FileIndex>(i);
        const FileEntry& file = tree.file(fileIndex);
        if (file.size < query.minSize || !extensionMatches(file.name)) {
            continue;
        }
        if (query.olderThanDays > 0 &&
            !isOlderThanDays(file.lastWriteTime, query.nowTicks,
                             query.olderThanDays)) {
            continue;
        }
        const std::wstring path = tree.pathOfFile(fileIndex);
        if (query.predicate && !query.predicate(path, file)) {
            continue;
        }
        out.push_back(PathSizeItem{path, file.size});
    }

    std::sort(out.begin(), out.end(), [](const PathSizeItem& a,
                                         const PathSizeItem& b) {
        if (a.size_bytes != b.size_bytes) {
            return a.size_bytes > b.size_bytes;
        }
        return a.path < b.path;
    });
    if (out.size() > query.limit) {
        out.resize(query.limit);
    }
    return out;
}

/// Largest directories by recursiveSize (includes root). O(D log K).
[[nodiscard]] inline std::vector<PathSizeItem> topDirectories(
    const DirectoryTree& tree,
    std::size_t limit)
{
    struct Rec {
        std::wstring path;
        ByteSize size = 0;
    };
    struct Cmp {
        bool operator()(const Rec& a, const Rec& b) const
        {
            if (a.size != b.size) {
                return a.size < b.size;
            }
            return a.path > b.path;
        }
    };

    TopKCollector<Rec, Cmp> top(limit);
    if (tree.empty() || limit == 0) {
        return {};
    }

    const std::size_t n = tree.directoryCount();
    for (std::size_t i = 0; i < n; ++i) {
        const DirIndex idx = static_cast<DirIndex>(i);
        Rec rec;
        rec.path = tree.pathOfDirectory(idx);
        rec.size = tree.dir(idx).recursiveSize;
        top.consider(std::move(rec));
    }

    std::vector<PathSizeItem> out;
    out.reserve(top.size());
    for (auto& r : top.sortedDescending()) {
        out.push_back(PathSizeItem{std::move(r.path), r.size});
    }
    return out;
}

/// Convert ScanResult largest files into PathSizeItem list.
[[nodiscard]] inline std::vector<PathSizeItem> topFilesFromResult(
    const ScanResult& result)
{
    std::vector<PathSizeItem> out;
    out.reserve(result.largestFiles.size());
    for (const auto& f : result.largestFiles) {
        out.push_back(PathSizeItem{f.path, f.size});
    }
    return out;
}

}  // namespace spacelens
