#include "core/HashCache.hpp"

#include "core/index/IndexPaths.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <utility>

namespace spacelens {
namespace {

constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS meta (
  key TEXT PRIMARY KEY NOT NULL,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS content_hashes (
  volume_serial INTEGER NOT NULL,
  file_id_128 BLOB NOT NULL,
  identity_source INTEGER NOT NULL,
  logical_size INTEGER NOT NULL,
  change_time INTEGER NOT NULL,
  file_usn INTEGER NOT NULL,
  journal_id INTEGER NOT NULL DEFAULT 0,
  algorithm INTEGER NOT NULL,
  evidence_version INTEGER NOT NULL,
  digest BLOB NOT NULL,
  verified_at_ticks INTEGER NOT NULL,
  last_used_ticks INTEGER NOT NULL,
  PRIMARY KEY(volume_serial, file_id_128)
);
)SQL";

bool fileIdIsZero(const std::array<std::uint8_t, 16>& id) noexcept
{
    for (const auto byte : id) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::wstring parentDirectory(const std::wstring& path)
{
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return {};
    }
    return path.substr(0, pos);
}

FileTimeTicks nowTicks() noexcept
{
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

int parseSchemaVersion(std::string_view text) noexcept
{
    if (text.empty()) {
        return 0;
    }
    int value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return 0;
        }
        value = value * 10 + (ch - '0');
        if (value > 1000000) {
            return 0;
        }
    }
    return value;
}

HashCacheRow rowFromStatement(SqliteStmt& stmt)
{
    HashCacheRow row;
    row.identity.volumeSerial = static_cast<std::uint64_t>(stmt.columnInt64(0));
    const auto id = stmt.columnBlob(1);
    if (id.size() == row.identity.fileId128.size()) {
        std::copy(id.begin(), id.end(), row.identity.fileId128.begin());
    }
    row.identity.source =
        static_cast<CleanupIdentitySource>(stmt.columnInt64(2));
    row.logicalSize = static_cast<ByteSize>(stmt.columnInt64(3));
    row.changeTime = static_cast<FileTimeTicks>(stmt.columnInt64(4));
    row.fileUsn = stmt.columnInt64(5);
    row.journalId = static_cast<std::uint64_t>(stmt.columnInt64(6));
    row.algorithm = static_cast<int>(stmt.columnInt64(7));
    row.evidenceVersion = static_cast<int>(stmt.columnInt64(8));
    row.digest = stmt.columnBlob(9);
    return row;
}

}  // namespace

const char* toString(HashCacheDisposition disposition) noexcept
{
    switch (disposition) {
    case HashCacheDisposition::Disabled:
        return "disabled";
    case HashCacheDisposition::Reusable:
        return "reusable";
    case HashCacheDisposition::MustRehash:
        return "must_rehash";
    case HashCacheDisposition::Invalid:
        return "invalid";
    }
    return "must_rehash";
}

bool isHashCachePersistable(const ContentHashEvidence& evidence) noexcept
{
    if (evidence.identity.source != CleanupIdentitySource::FileId128) {
        return false;
    }
    if (evidence.identity.volumeSerial == 0) {
        return false;
    }
    if (fileIdIsZero(evidence.identity.fileId128)) {
        return false;
    }
    if (evidence.changeTime == 0 || evidence.fileUsn == 0) {
        return false;
    }
    return true;
}

HashCacheDisposition evaluateHashCacheRow(const ContentHashEvidence& live,
                                          const HashCacheRow& stored) noexcept
{
    if (stored.algorithm != kHashAlgorithmSha256 ||
        stored.evidenceVersion != kHashEvidenceVersion ||
        stored.digest.size() != 32 ||
        stored.identity.source != CleanupIdentitySource::FileId128 ||
        stored.identity.volumeSerial == 0 ||
        fileIdIsZero(stored.identity.fileId128)) {
        return HashCacheDisposition::Invalid;
    }
    if (!isHashCachePersistable(live)) {
        return HashCacheDisposition::MustRehash;
    }
    if (live.identity.volumeSerial != stored.identity.volumeSerial ||
        live.identity.fileId128 != stored.identity.fileId128 ||
        live.logicalSize != stored.logicalSize ||
        live.changeTime != stored.changeTime ||
        live.fileUsn != stored.fileUsn) {
        return HashCacheDisposition::MustRehash;
    }
    if (stored.journalId != 0 && live.journalId != 0 &&
        stored.journalId != live.journalId) {
        return HashCacheDisposition::MustRehash;
    }
    return HashCacheDisposition::Reusable;
}

HashCacheStore HashCacheStore::tryOpen(const std::wstring& path)
{
    HashCacheStore store;
    store.m_path = path;
    if (path.empty()) {
        return store;
    }
    try {
        const std::wstring parent = parentDirectory(path);
        if (!parent.empty() && !ensureDirectory(parent)) {
            return store;
        }
        store.m_db = SqliteDb(path, SqliteOpen::ReadWrite | SqliteOpen::Create);
        if (!store.openExisting()) {
            store.m_db.close();
            return store;
        }
        store.m_available = true;
    } catch (const SqliteError&) {
        store.m_db.close();
        store.m_available = false;
    }
    return store;
}

bool HashCacheStore::openExisting()
{
    m_db.exec(kSchemaSql);

    int version = 0;
    {
        SqliteStmt stmt(m_db,
                        "SELECT value FROM meta WHERE key = "
                        "'hash_cache_schema_version'");
        if (stmt.step()) {
            version = parseSchemaVersion(stmt.columnText(0));
        }
    }

    if (version == 0) {
        SqliteTxn txn(m_db);
        SqliteStmt stmt(m_db,
                        "INSERT OR REPLACE INTO meta(key, value) VALUES(?1, ?2)");
        stmt.bindText(1, "hash_cache_schema_version");
        stmt.bindText(2, "1");
        stmt.stepDone();
        txn.commit();
        return true;
    }
    if (version != kHashCacheSchemaVersion) {
        // Newer or malformed schema: do not migrate, do not reuse.
        return false;
    }
    return true;
}

HashCacheLookup HashCacheStore::lookup(const ContentHashEvidence& live)
{
    HashCacheLookup result;
    if (!m_available) {
        result.disposition = HashCacheDisposition::Disabled;
        result.detail = "hash cache unavailable";
        return result;
    }
    if (!isHashCachePersistable(live)) {
        result.disposition = HashCacheDisposition::MustRehash;
        result.detail = "live evidence is not persistable";
        return result;
    }

    try {
        SqliteStmt stmt(
            m_db,
            "SELECT volume_serial, file_id_128, identity_source, logical_size, "
            "change_time, file_usn, journal_id, algorithm, evidence_version, "
            "digest FROM content_hashes WHERE volume_serial = ?1 AND "
            "file_id_128 = ?2");
        stmt.bindInt64(1, static_cast<std::int64_t>(live.identity.volumeSerial));
        stmt.bindBlob(2, live.identity.fileId128.data(),
                      live.identity.fileId128.size());
        if (!stmt.step()) {
            result.disposition = HashCacheDisposition::MustRehash;
            result.detail = "cache miss";
            return result;
        }
        const HashCacheRow row = rowFromStatement(stmt);
        result.disposition = evaluateHashCacheRow(live, row);
        if (result.disposition == HashCacheDisposition::Reusable) {
            std::copy(row.digest.begin(), row.digest.end(), result.digest.begin());
            touchLastUsed(live.identity.volumeSerial, live.identity.fileId128);
            result.detail = "reusable";
            return result;
        }
        if (result.disposition == HashCacheDisposition::Invalid) {
            dropCorruptRow(live.identity.volumeSerial, live.identity.fileId128);
            result.detail = "invalid cache row";
            return result;
        }
        result.detail = "evidence mismatch";
        return result;
    } catch (const SqliteError& ex) {
        result.disposition = HashCacheDisposition::MustRehash;
        result.detail = ex.what();
        return result;
    }
}

bool HashCacheStore::store(const ContentHashEvidence& live,
                           const std::array<std::uint8_t, 32>& digest)
{
    if (!m_available || !isHashCachePersistable(live)) {
        return false;
    }
    try {
        const auto now = static_cast<std::int64_t>(nowTicks());
        SqliteStmt stmt(
            m_db,
            "INSERT OR REPLACE INTO content_hashes("
            "volume_serial, file_id_128, identity_source, logical_size, "
            "change_time, file_usn, journal_id, algorithm, evidence_version, "
            "digest, verified_at_ticks, last_used_ticks) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)");
        stmt.bindInt64(1, static_cast<std::int64_t>(live.identity.volumeSerial));
        stmt.bindBlob(2, live.identity.fileId128.data(),
                      live.identity.fileId128.size());
        stmt.bindInt64(3, static_cast<std::int64_t>(live.identity.source));
        stmt.bindInt64(4, static_cast<std::int64_t>(live.logicalSize));
        stmt.bindInt64(5, static_cast<std::int64_t>(live.changeTime));
        stmt.bindInt64(6, live.fileUsn);
        stmt.bindInt64(7, static_cast<std::int64_t>(live.journalId));
        stmt.bindInt64(8, kHashAlgorithmSha256);
        stmt.bindInt64(9, kHashEvidenceVersion);
        stmt.bindBlob(10, digest.data(), digest.size());
        stmt.bindInt64(11, now);
        stmt.bindInt64(12, now);
        stmt.stepDone();
        return true;
    } catch (const SqliteError&) {
        return false;
    }
}

void HashCacheStore::dropCorruptRow(
    std::uint64_t volumeSerial, const std::array<std::uint8_t, 16>& fileId) noexcept
{
    try {
        SqliteStmt stmt(m_db,
                        "DELETE FROM content_hashes WHERE volume_serial = ?1 "
                        "AND file_id_128 = ?2");
        stmt.bindInt64(1, static_cast<std::int64_t>(volumeSerial));
        stmt.bindBlob(2, fileId.data(), fileId.size());
        stmt.stepDone();
    } catch (...) {
    }
}

void HashCacheStore::touchLastUsed(
    std::uint64_t volumeSerial, const std::array<std::uint8_t, 16>& fileId) noexcept
{
    try {
        SqliteStmt stmt(m_db,
                        "UPDATE content_hashes SET last_used_ticks = ?1 "
                        "WHERE volume_serial = ?2 AND file_id_128 = ?3");
        stmt.bindInt64(1, static_cast<std::int64_t>(nowTicks()));
        stmt.bindInt64(2, static_cast<std::int64_t>(volumeSerial));
        stmt.bindBlob(3, fileId.data(), fileId.size());
        stmt.stepDone();
    } catch (...) {
    }
}

}  // namespace spacelens
