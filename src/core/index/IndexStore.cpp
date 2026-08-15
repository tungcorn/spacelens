#include "core/index/IndexStore.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <ctime>
#include <string>

namespace spacelens {
namespace {

constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS meta (
  key TEXT PRIMARY KEY NOT NULL,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS roots (
  id INTEGER PRIMARY KEY NOT NULL,
  root_path TEXT NOT NULL UNIQUE,
  root_key TEXT NOT NULL UNIQUE,
  schema_version INTEGER NOT NULL,
  indexed_at_ticks INTEGER NOT NULL,
  indexed_at_iso TEXT NOT NULL,
  file_count INTEGER NOT NULL,
  dir_count INTEGER NOT NULL,
  logical_bytes INTEGER NOT NULL,
  status TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS entries (
  id INTEGER PRIMARY KEY NOT NULL,
  root_id INTEGER NOT NULL,
  parent_id INTEGER,
  kind INTEGER NOT NULL,
  name TEXT NOT NULL,
  path TEXT NOT NULL,
  size_bytes INTEGER NOT NULL,
  recursive_size INTEGER NOT NULL DEFAULT 0,
  extension TEXT NOT NULL DEFAULT '',
  last_write_ticks INTEGER NOT NULL DEFAULT 0,
  last_access_ticks INTEGER NOT NULL DEFAULT 0,
  attributes INTEGER NOT NULL DEFAULT 0,
  is_reparse INTEGER NOT NULL DEFAULT 0,
  classification TEXT NOT NULL DEFAULT 'Unknown',
  confidence TEXT NOT NULL DEFAULT 'Low',
  rule_id TEXT NOT NULL DEFAULT '',
  location_safety TEXT NOT NULL DEFAULT 'Unknown',
  reclaimability TEXT NOT NULL DEFAULT 'Unknown',
  candidate_strength TEXT NOT NULL DEFAULT 'None',
  newest_descendant_write INTEGER NOT NULL DEFAULT 0,
  oldest_descendant_write INTEGER NOT NULL DEFAULT 0,
  file_id INTEGER NOT NULL DEFAULT 0,
  parent_file_id INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS refresh_checkpoint (
  root_id INTEGER PRIMARY KEY NOT NULL,
  volume_device_path TEXT NOT NULL DEFAULT '',
  volume_root_path TEXT NOT NULL DEFAULT '',
  volume_serial INTEGER NOT NULL DEFAULT 0,
  filesystem TEXT NOT NULL DEFAULT '',
  usn_journal_id INTEGER NOT NULL DEFAULT 0,
  next_usn INTEGER NOT NULL DEFAULT 0,
  lowest_valid_usn INTEGER NOT NULL DEFAULT 0,
  full_indexed_at_ticks INTEGER NOT NULL DEFAULT 0,
  last_refresh_at_ticks INTEGER NOT NULL DEFAULT 0,
  last_refresh_method TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'unavailable',
  FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_entries_root_kind_size
  ON entries(root_id, kind, size_bytes DESC);
CREATE INDEX IF NOT EXISTS idx_entries_root_kind_rsize
  ON entries(root_id, kind, recursive_size DESC);
CREATE INDEX IF NOT EXISTS idx_entries_root_ext
  ON entries(root_id, extension);
CREATE INDEX IF NOT EXISTS idx_entries_root_write
  ON entries(root_id, last_write_ticks);
CREATE INDEX IF NOT EXISTS idx_entries_root_class
  ON entries(root_id, classification);
CREATE INDEX IF NOT EXISTS idx_entries_root_strength
  ON entries(root_id, candidate_strength);
CREATE INDEX IF NOT EXISTS idx_entries_root_path
  ON entries(root_id, path);
CREATE INDEX IF NOT EXISTS idx_entries_root_file_id
  ON entries(root_id, file_id);
)SQL";

int readSchemaVersion(SqliteDb& db)
{
    try {
        SqliteStmt stmt(db, "SELECT value FROM meta WHERE key = ?1;");
        stmt.bindText(1, "index_schema_version");
        if (!stmt.step()) {
            return 0;
        }
        return std::stoi(stmt.columnText(0));
    } catch (...) {
        return 0;
    }
}

void writeSchemaVersion(SqliteDb& db, int version)
{
    SqliteStmt del(db, "DELETE FROM meta WHERE key = ?1;");
    del.bindText(1, "index_schema_version");
    del.stepDone();
    SqliteStmt ins(db, "INSERT INTO meta(key, value) VALUES(?1, ?2);");
    ins.bindText(1, "index_schema_version");
    ins.bindText(2, std::to_string(version));
    ins.stepDone();
}

bool columnExists(SqliteDb& db, const char* table, const char* column)
{
    // PRAGMA table_info returns: cid, name, type, notnull, dflt_value, pk
    const std::string sql = std::string("PRAGMA table_info(") + table + ");";
    SqliteStmt stmt(db, sql);
    while (stmt.step()) {
        if (stmt.columnText(1) == column) {
            return true;
        }
    }
    return false;
}

void migrateToV2(SqliteDb& db)
{
    if (!columnExists(db, "entries", "file_id")) {
        db.exec("ALTER TABLE entries ADD COLUMN file_id INTEGER NOT NULL DEFAULT 0;");
    }
    if (!columnExists(db, "entries", "parent_file_id")) {
        db.exec(
            "ALTER TABLE entries ADD COLUMN parent_file_id INTEGER NOT NULL DEFAULT 0;");
    }
    db.exec(R"SQL(
CREATE TABLE IF NOT EXISTS refresh_checkpoint (
  root_id INTEGER PRIMARY KEY NOT NULL,
  volume_device_path TEXT NOT NULL DEFAULT '',
  volume_root_path TEXT NOT NULL DEFAULT '',
  volume_serial INTEGER NOT NULL DEFAULT 0,
  filesystem TEXT NOT NULL DEFAULT '',
  usn_journal_id INTEGER NOT NULL DEFAULT 0,
  next_usn INTEGER NOT NULL DEFAULT 0,
  lowest_valid_usn INTEGER NOT NULL DEFAULT 0,
  full_indexed_at_ticks INTEGER NOT NULL DEFAULT 0,
  last_refresh_at_ticks INTEGER NOT NULL DEFAULT 0,
  last_refresh_method TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'unavailable',
  FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_entries_root_file_id
  ON entries(root_id, file_id);
)SQL");
}

}  // namespace

const char* toString(IndexStatus status) noexcept
{
    switch (status) {
    case IndexStatus::Ready:
        return "ready";
    case IndexStatus::Building:
        return "building";
    case IndexStatus::Failed:
        return "failed";
    case IndexStatus::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string fileTimeTicksToIsoUtc(std::uint64_t ticks)
{
    if (ticks == 0) {
        return {};
    }
    constexpr std::uint64_t kEpochDiff = 11644473600ULL;
    const std::uint64_t seconds = ticks / 10'000'000ULL;
    if (seconds < kEpochDiff) {
        return {};
    }
    const std::time_t unix = static_cast<std::time_t>(seconds - kEpochDiff);
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &unix) != 0) {
        return {};
    }
#else
    if (gmtime_r(&unix, &tm) == nullptr) {
        return {};
    }
#endif
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buf;
}

IndexStore::IndexStore(IndexLocation loc, SqliteDb db)
    : m_loc(std::move(loc))
    , m_db(std::move(db))
{
}

IndexStore IndexStore::openRead(const IndexLocation& loc)
{
    if (!indexDatabaseExists(loc)) {
        throw SqliteError("index database not found");
    }
    try {
        SqliteDb readonly(loc.dbPath, SqliteOpen::ReadOnly);
        IndexStore store(loc, std::move(readonly));
        if (store.schemaSupported()) {
            return store;
        }
    } catch (const SqliteError&) {
        // Fall through and migrate with a write connection.
    }
    SqliteDb db(loc.dbPath, SqliteOpen::ReadWrite);
    IndexStore store(loc, std::move(db));
    store.migrateSchemaIfNeeded();
    if (!store.schemaSupported()) {
        throw SqliteError("unsupported index_schema_version");
    }
    return store;
}

IndexStore IndexStore::openInspect(const IndexLocation& loc)
{
    if (!indexDatabaseExists(loc)) {
        throw SqliteError("index database not found");
    }
    SqliteDb readonly(loc.dbPath, SqliteOpen::ReadOnly);
    return IndexStore(loc, std::move(readonly));
}

IndexStore IndexStore::openReadWrite(const IndexLocation& loc)
{
    if (!indexDatabaseExists(loc)) {
        throw SqliteError("index database not found");
    }
    SqliteDb db(loc.dbPath, SqliteOpen::ReadWrite);
    IndexStore store(loc, std::move(db));
    store.migrateSchemaIfNeeded();
    if (!store.schemaSupported()) {
        throw SqliteError("unsupported index_schema_version");
    }
    return store;
}

IndexStore IndexStore::createStaging(const IndexLocation& loc)
{
    if (!ensureDirectory(loc.indexDir)) {
        throw SqliteError("failed to create index directory");
    }
    discardStagingDatabase(loc);
    ::DeleteFileW(loc.stagingDbPath.c_str());
    SqliteDb db(loc.stagingDbPath, SqliteOpen::ReadWrite | SqliteOpen::Create);
    IndexStore store(loc, std::move(db));
    store.applySchema();
    return store;
}

void IndexStore::applySchema()
{
    m_db.exec(kSchemaSql);
    writeSchemaVersion(m_db, kIndexSchemaVersion);
}

void IndexStore::migrateSchemaIfNeeded()
{
    // Ensure base tables exist for empty/corrupt edge cases.
    m_db.exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY NOT NULL, "
              "value TEXT NOT NULL);");

    int version = readSchemaVersion(m_db);
    if (version == 0) {
        // Might be a fresh partial DB — apply full schema.
        applySchema();
        return;
    }
    if (version > kIndexSchemaVersion) {
        throw SqliteError("unsupported index_schema_version");
    }
    if (version < 2) {
        migrateToV2(m_db);
        writeSchemaVersion(m_db, 2);
        version = 2;
    }
    if (version < kIndexSchemaVersion) {
        // Future migrations chain here.
        writeSchemaVersion(m_db, kIndexSchemaVersion);
    }
}

bool IndexStore::schemaSupported() const
{
    try {
        const int version = readSchemaVersion(const_cast<SqliteDb&>(m_db));
        return version == kIndexSchemaVersion;
    } catch (...) {
        return false;
    }
}

int IndexStore::schemaVersion() const
{
    return readSchemaVersion(const_cast<SqliteDb&>(m_db));
}

void IndexStore::writeRootMeta(const IndexRootInfo& info)
{
    // Upsert only the roots row. Never DELETE roots while entries exist —
    // ON DELETE CASCADE would wipe the index body.
    SqliteStmt stmt(
        m_db,
        "INSERT INTO roots(id, root_path, root_key, schema_version, "
        "indexed_at_ticks, indexed_at_iso, file_count, dir_count, "
        "logical_bytes, status) VALUES(1, ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) "
        "ON CONFLICT(id) DO UPDATE SET "
        "root_path=excluded.root_path, "
        "root_key=excluded.root_key, "
        "schema_version=excluded.schema_version, "
        "indexed_at_ticks=excluded.indexed_at_ticks, "
        "indexed_at_iso=excluded.indexed_at_iso, "
        "file_count=excluded.file_count, "
        "dir_count=excluded.dir_count, "
        "logical_bytes=excluded.logical_bytes, "
        "status=excluded.status;");
    stmt.bindText16(1, info.rootPath);
    stmt.bindText16(2, info.rootKey);
    stmt.bindInt64(3, info.schemaVersion);
    stmt.bindInt64(4, static_cast<std::int64_t>(info.indexedAtTicks));
    stmt.bindText(5, info.indexedAtIso);
    stmt.bindInt64(6, static_cast<std::int64_t>(info.fileCount));
    stmt.bindInt64(7, static_cast<std::int64_t>(info.dirCount));
    stmt.bindInt64(8, static_cast<std::int64_t>(info.logicalBytes));
    stmt.bindText(9, toString(info.status));
    stmt.stepDone();
}

std::optional<IndexRootInfo> IndexStore::readRootMeta() const
{
    try {
        SqliteStmt stmt(
            const_cast<SqliteDb&>(m_db),
            "SELECT id, root_path, root_key, schema_version, indexed_at_ticks, "
            "indexed_at_iso, file_count, dir_count, logical_bytes, status "
            "FROM roots LIMIT 1;");
        if (!stmt.step()) {
            return std::nullopt;
        }
        IndexRootInfo info;
        info.rootId = stmt.columnInt64(0);
        info.rootPath = stmt.columnText16(1);
        info.rootKey = stmt.columnText16(2);
        info.schemaVersion = static_cast<int>(stmt.columnInt64(3));
        info.indexedAtTicks = static_cast<std::uint64_t>(stmt.columnInt64(4));
        info.indexedAtIso = stmt.columnText(5);
        info.fileCount = static_cast<std::uint64_t>(stmt.columnInt64(6));
        info.dirCount = static_cast<std::uint64_t>(stmt.columnInt64(7));
        info.logicalBytes = static_cast<ByteSize>(stmt.columnInt64(8));
        const std::string status = stmt.columnText(9);
        if (status == "ready") {
            info.status = IndexStatus::Ready;
        } else if (status == "building") {
            info.status = IndexStatus::Building;
        } else if (status == "failed") {
            info.status = IndexStatus::Failed;
        } else {
            info.status = IndexStatus::Unknown;
        }
        return info;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace spacelens
