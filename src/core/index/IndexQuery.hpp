#pragma once

#include "core/FileTime.hpp"
#include "core/Types.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexStore.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spacelens {

enum class IndexEntryKind {
    File = 0,
    Directory = 1
};

struct IndexQuerySpec {
    bool includeFiles = true;
    bool includeDirectories = false;
    std::optional<ByteSize> minSize;
    std::string extension;  // lowercase, no leading dot
    std::optional<std::uint64_t> olderThanDays;
    std::string classification;       // empty = any
    std::string candidateStrength;    // empty = any
    std::size_t limit = 20;
    FileTimeTicks nowTicks = 0;  // for age filters
};

struct IndexHit {
    std::wstring path;
    IndexEntryKind kind = IndexEntryKind::File;
    ByteSize size_bytes = 0;
    std::string classification;
    std::string confidence;
    std::string location_safety;
    std::string reclaimability;
    std::string candidate_strength;
    std::uint64_t last_write_ticks = 0;
};

struct IndexQueryResult {
    bool ok = false;
    std::string error;  // e.g. index_not_found, unsupported_schema
    IndexRootInfo root{};
    IndexLocation location{};
    std::vector<IndexHit> hits;
    std::uint64_t matched_items = 0;
    std::uint64_t returned_items = 0;
    ByteSize matched_logical_bytes = 0;
    std::uint64_t age_ms = 0;
};

/// Query a published persistent index. Never falls back to a live scan.
[[nodiscard]] IndexQueryResult queryIndex(const std::wstring& rootPath,
                                          const IndexQuerySpec& spec);

/// Read status for a root without running a query.
[[nodiscard]] IndexQueryResult indexStatus(const std::wstring& rootPath);

}  // namespace spacelens
