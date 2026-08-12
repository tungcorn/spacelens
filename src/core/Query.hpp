#pragma once

#include "core/DirectoryTree.hpp"
#include "core/ScanTypes.hpp"
#include "core/TopKCollector.hpp"

#include <string>
#include <vector>

namespace spacelens {

struct PathSizeItem {
    std::wstring path;
    ByteSize size_bytes = 0;
};

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
