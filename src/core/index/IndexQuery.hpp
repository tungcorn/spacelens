#pragma once

#include "core/Duplicates.hpp"
#include "core/FileTime.hpp"
#include "core/Types.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexStore.hpp"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace spacelens {

enum class IndexEntryKind {
    File = 0,
    Directory = 1
};

/// Deterministic result ordering for indexed discovery queries.
enum class IndexSortKey {
    Size = 0,               // effective size (file size / dir recursive_size)
    Name,                   // leaf name, case-insensitive
    LastWrite,              // activity write ticks
    Classification,         // classification string
    CandidateStrength,      // Strong > Moderate > ReviewOnly > None, then size
    OpportunityRank         // opportunity_rank_v2: strength, confidence, size, path
};

struct IndexQuerySpec {
    bool includeFiles = true;
    bool includeDirectories = false;
    std::optional<ByteSize> minSize;
    std::string extension;  // lowercase, no leading dot
    std::optional<std::uint64_t> olderThanDays;
    /// Single classification equality filter (empty = any). Ignored when
    /// `classifications` is non-empty.
    std::string classification;
    /// Multi-value classification IN filter (e.g. Developer Storage preset).
    std::vector<std::string> classifications;
    /// Single candidate-strength equality (empty = any). Ignored when
    /// `candidateStrengths` is non-empty.
    std::string candidateStrength;
    /// Multi-value strength IN filter (e.g. Reclaim Candidates preset).
    std::vector<std::string> candidateStrengths;
    /// Optional reclaimability equality (empty = any).
    std::string reclaimability;
    /// Case-insensitive substring match against name, path, and extension.
    /// Empty disables text search. Matching is deterministic LIKE %needle%.
    std::string searchText;
    /// When non-empty, restrict to immediate children of this absolute path
    /// (indexed browse / breadcrumb navigation). Empty = whole index.
    std::wstring browsePath;
    /// When non-empty and browsePath is empty, restrict to the path itself or
    /// any descendant (path = prefix OR path LIKE prefix\...). Used for
    /// "under this folder" filters without requiring parent_id.
    std::wstring pathPrefix;
    IndexSortKey sortBy = IndexSortKey::Size;
    bool sortDescending = true;
    std::size_t limit = 20;
    FileTimeTicks nowTicks = 0;  // for age filters
};

struct IndexHit {
    std::wstring path;
    std::wstring name;
    IndexEntryKind kind = IndexEntryKind::File;
    ByteSize size_bytes = 0;
    std::string extension;
    std::string classification;
    std::string confidence;
    std::string rule_id;
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
    /// Wall-clock query time inside queryIndex (open + SQL), milliseconds.
    std::uint64_t query_elapsed_ms = 0;
};

/// Query a published persistent index. Never falls back to a live scan.
[[nodiscard]] IndexQueryResult queryIndex(const std::wstring& rootPath,
                                          const IndexQuerySpec& spec);

/// Indexed opportunity inclusion + opportunity_rank_v2. Filters and ranking
/// run in SQL; `limit` is the public top-N (implementation fetches limit+1).
struct IndexedOpportunitySpec {
    ByteSize minSize = 1024ULL * 1024ULL;
    std::uint64_t olderThanDays = 90;
    FileTimeTicks nowTicks = 0;
    std::size_t limit = 20;
    std::optional<std::string> classification;
    bool matchNone = false;
    std::wstring pathPrefix;
    std::wstring excludePath;
    std::size_t aggregateLimit = 50000;
};

struct IndexedOpportunityFetch {
    bool ok = false;
    std::string error;
    IndexRootInfo root{};
    IndexLocation location{};
    std::uint64_t age_ms = 0;
    std::uint64_t query_elapsed_ms = 0;
    std::vector<IndexHit> topHits;
    std::uint64_t matchedItems = 0;
    std::vector<IndexHit> aggregateHits;
    bool aggregatesCapped = false;
    std::size_t sqlTopRows = 0;
    std::size_t sqlAggregateRows = 0;
};

/// Exact top-N opportunity retrieval across the whole published index.
/// Does not refresh, scan, or write the index.
[[nodiscard]] IndexedOpportunityFetch queryIndexedOpportunities(
    const std::wstring& rootPath, const IndexedOpportunitySpec& spec,
    std::stop_token stop = {});

/// Read status for a root without running a query.
[[nodiscard]] IndexQueryResult indexStatus(const std::wstring& rootPath);

/// Same-size regular-file candidate buckets. This is not duplicate proof.
/// Directories, zero-length files, and indexed reparse files are excluded.
/// Live verification still has to reject stale/missing/reparse paths.
[[nodiscard]] DuplicateCandidateQueryResult queryDuplicateSizeCandidates(
    const std::wstring& rootPath,
    ByteSize minimumSize);

/// Query an already-open store. Used by tests and the root-path wrapper.
[[nodiscard]] DuplicateCandidateQueryResult queryDuplicateSizeCandidates(
    IndexStore& store,
    ByteSize minimumSize);

}  // namespace spacelens
