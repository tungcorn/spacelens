#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

/// Index schema version stored inside the database (not CLI JSON schema_version).
/// V2 adds file_id/parent_file_id on entries and refresh_checkpoint for USN.
/// V3 adds physical allocation / hard-link evidence. Reclaim-plan requires
/// meta physical_accounting=1 (set only after a full v3 build finalize).
inline constexpr int kIndexSchemaVersion = 3;

/// Paths and keys for SpaceLens-owned index files under LocalAppData.
/// Never writes into the scanned source tree.
struct IndexLocation {
    std::wstring rootPath;       // normalized source root
    std::wstring rootKey;        // stable directory key
    std::wstring indexDir;       // .../SpaceLens/indexes/<key>/
    std::wstring dbPath;         // .../index.db
    std::wstring stagingDbPath;  // .../index.db.building
};

/// %LOCALAPPDATA%\SpaceLens (created on demand by callers if needed).
[[nodiscard]] std::wstring spaceLensDataRoot();

/// %LOCALAPPDATA%\SpaceLens\indexes
[[nodiscard]] std::wstring spaceLensIndexesRoot();

/// %LOCALAPPDATA%\SpaceLens\state.db — durable Cleanup Review database.
/// Independent of replaceable per-root indexes/*/index.db files.
[[nodiscard]] std::wstring spaceLensReviewStatePath();

/// %LOCALAPPDATA%\SpaceLens\hash-cache.db — derived SHA-256 cache.
/// Independent of state.db and of per-root indexes. Disposable.
[[nodiscard]] std::wstring spaceLensHashCachePath();

/// Normalize a root path for indexing (native separators, strip trailing slash
/// except drive roots). Does not touch the filesystem.
[[nodiscard]] std::wstring normalizeIndexRoot(std::wstring_view path);

/// Case-insensitive compare of normalized index roots. Negative / 0 / positive.
/// Does not touch the filesystem.
[[nodiscard]] int compareIndexRootPath(std::wstring_view a, std::wstring_view b);

/// Stable filesystem-safe key for a normalized root (hex hash).
[[nodiscard]] std::wstring rootKeyFor(std::wstring_view normalizedRoot);

[[nodiscard]] IndexLocation locateIndex(std::wstring_view rootPath);

/// Ensure a directory exists (creates parents). Returns false on failure.
[[nodiscard]] bool ensureDirectory(const std::wstring& path);

/// Atomic-ish publish: staging -> db. Preserves old db on failure.
/// Returns false if rename/replace fails (old db left intact when possible).
[[nodiscard]] bool publishIndexDatabase(const IndexLocation& loc);

/// Delete staging file if present.
void discardStagingDatabase(const IndexLocation& loc);

[[nodiscard]] bool indexDatabaseExists(const IndexLocation& loc);

struct ListedIndex {
    std::wstring rootPath;
    std::wstring dbPath;
    std::wstring rootKey;
};

/// Enumerate ready indexes under the SpaceLens indexes root (best-effort).
[[nodiscard]] std::vector<ListedIndex> listIndexedRoots();

}  // namespace spacelens
