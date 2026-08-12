#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

/// Index schema version stored inside the database (not CLI JSON schema_version).
inline constexpr int kIndexSchemaVersion = 1;

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

/// Normalize a root path for indexing (native separators, strip trailing slash
/// except drive roots). Does not touch the filesystem.
[[nodiscard]] std::wstring normalizeIndexRoot(std::wstring_view path);

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
