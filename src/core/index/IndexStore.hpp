#pragma once

#include "core/Types.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/Sqlite.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spacelens {

enum class IndexStatus {
    Ready,
    Building,
    Failed,
    Unknown
};

[[nodiscard]] const char* toString(IndexStatus status) noexcept;

struct IndexRootInfo {
    std::int64_t rootId = 0;
    std::wstring rootPath;
    std::wstring rootKey;
    int schemaVersion = 0;
    std::uint64_t indexedAtTicks = 0;
    std::string indexedAtIso;
    std::uint64_t fileCount = 0;
    std::uint64_t dirCount = 0;
    ByteSize logicalBytes = 0;
    IndexStatus status = IndexStatus::Unknown;
};

/// Read/write access to one root's index database.
/// Caller owns the SqliteDb lifetime; IndexStore does not share across threads.
class IndexStore {
public:
    /// Open an existing ready index for read queries.
    [[nodiscard]] static IndexStore openRead(const IndexLocation& loc);

    /// Create a new staging database for a full rebuild.
    [[nodiscard]] static IndexStore createStaging(const IndexLocation& loc);

    IndexStore(IndexStore&&) noexcept = default;
    IndexStore& operator=(IndexStore&&) noexcept = default;

    [[nodiscard]] SqliteDb& db() noexcept { return m_db; }
    [[nodiscard]] const IndexLocation& location() const noexcept { return m_loc; }

    void applySchema();
    void writeRootMeta(const IndexRootInfo& info);
    [[nodiscard]] std::optional<IndexRootInfo> readRootMeta() const;

    /// Validate schema version; throws SqliteError / returns false if unsupported.
    [[nodiscard]] bool schemaSupported() const;

private:
    IndexStore(IndexLocation loc, SqliteDb db);

    IndexLocation m_loc;
    SqliteDb m_db;
};

[[nodiscard]] std::string fileTimeTicksToIsoUtc(std::uint64_t ticks);

}  // namespace spacelens
