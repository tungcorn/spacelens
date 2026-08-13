#include "core/index/IndexRefresh.hpp"

#include "core/Classification.hpp"
#include "core/FileTime.hpp"
#include "core/ReclaimAnalysis.hpp"
#include "core/SafetyPolicy.hpp"
#include "platform/windows/FileIdentity.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spacelens {
namespace {

FileTimeTicks nowFileTime()
{
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

std::string extensionLower(std::wstring_view name)
{
    const auto pos = name.find_last_of(L'.');
    if (pos == std::wstring_view::npos || pos == 0 || pos + 1 >= name.size()) {
        return {};
    }
    std::string out;
    out.reserve(name.size() - pos);
    for (std::size_t i = pos + 1; i < name.size(); ++i) {
        const wchar_t ch = name[i];
        if (ch < 128) {
            out.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

std::wstring leafName(const std::wstring& path)
{
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

std::wstring parentPathOf(const std::wstring& path)
{
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return {};
    }
    if (pos == 2 && path.size() >= 3 && path[1] == L':') {
        // Drive root parent of C:\foo is C:\ (three chars).
        return path.substr(0, 3);
    }
    return path.substr(0, pos);
}

struct CoalescedChange {
    std::uint64_t fileId = 0;
    std::uint64_t parentFileId = 0;
    std::wstring fileName;
    std::uint32_t reasonBits = 0;
    std::uint32_t attributes = 0;
    bool sawDelete = false;
    bool sawCreate = false;
    bool sawRenameOld = false;
    bool sawRenameNew = false;
    bool sawDataOrBasic = false;
};

void coalesce(std::unordered_map<std::uint64_t, CoalescedChange>& map,
              const UsnChangeRecord& rec)
{
    auto& c = map[rec.fileReferenceNumber];
    c.fileId = rec.fileReferenceNumber;
    if (rec.parentFileReferenceNumber != 0) {
        c.parentFileId = rec.parentFileReferenceNumber;
    }
    if (!rec.fileName.empty()) {
        c.fileName = rec.fileName;
    }
    c.reasonBits |= rec.reason;
    c.attributes = rec.fileAttributes;
    if (rec.reason & UsnReason::FileDelete) {
        c.sawDelete = true;
    }
    if (rec.reason & UsnReason::FileCreate) {
        c.sawCreate = true;
        c.sawDelete = false;  // recreate after delete in same window
    }
    if (rec.reason & UsnReason::RenameOldName) {
        c.sawRenameOld = true;
    }
    if (rec.reason & UsnReason::RenameNewName) {
        c.sawRenameNew = true;
        c.sawDelete = false;
    }
    if (rec.reason & (UsnReason::DataExtend | UsnReason::DataOverwrite |
                      UsnReason::DataTruncation | UsnReason::BasicInfoChange |
                      UsnReason::StreamChange)) {
        c.sawDataOrBasic = true;
    }
}

std::optional<RefreshCheckpoint> readCheckpoint(SqliteDb& db)
{
    try {
        SqliteStmt stmt(
            db,
            "SELECT root_id, volume_device_path, volume_root_path, volume_serial, "
            "filesystem, usn_journal_id, next_usn, lowest_valid_usn, "
            "full_indexed_at_ticks, last_refresh_at_ticks, last_refresh_method, "
            "status FROM refresh_checkpoint WHERE root_id = 1;");
        if (!stmt.step()) {
            return std::nullopt;
        }
        RefreshCheckpoint cp;
        cp.rootId = stmt.columnInt64(0);
        cp.volumeDevicePath = stmt.columnText16(1);
        cp.volumeRootPath = stmt.columnText16(2);
        cp.volumeSerial = static_cast<std::uint32_t>(stmt.columnInt64(3));
        cp.filesystem = stmt.columnText16(4);
        cp.usnJournalId = static_cast<std::uint64_t>(stmt.columnInt64(5));
        cp.nextUsn = static_cast<std::uint64_t>(stmt.columnInt64(6));
        cp.lowestValidUsnAtCapture =
            static_cast<std::uint64_t>(stmt.columnInt64(7));
        cp.fullIndexedAtTicks = static_cast<std::uint64_t>(stmt.columnInt64(8));
        cp.lastRefreshAtTicks = static_cast<std::uint64_t>(stmt.columnInt64(9));
        cp.lastRefreshMethod = stmt.columnText(10);
        cp.status = stmt.columnText(11);
        return cp;
    } catch (...) {
        return std::nullopt;
    }
}

void upsertCheckpoint(SqliteDb& db, const RefreshCheckpoint& cp)
{
    SqliteStmt stmt(
        db,
        "INSERT INTO refresh_checkpoint("
        "root_id, volume_device_path, volume_root_path, volume_serial, filesystem, "
        "usn_journal_id, next_usn, lowest_valid_usn, full_indexed_at_ticks, "
        "last_refresh_at_ticks, last_refresh_method, status) "
        "VALUES(1,?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) "
        "ON CONFLICT(root_id) DO UPDATE SET "
        "volume_device_path=excluded.volume_device_path, "
        "volume_root_path=excluded.volume_root_path, "
        "volume_serial=excluded.volume_serial, "
        "filesystem=excluded.filesystem, "
        "usn_journal_id=excluded.usn_journal_id, "
        "next_usn=excluded.next_usn, "
        "lowest_valid_usn=excluded.lowest_valid_usn, "
        "full_indexed_at_ticks=excluded.full_indexed_at_ticks, "
        "last_refresh_at_ticks=excluded.last_refresh_at_ticks, "
        "last_refresh_method=excluded.last_refresh_method, "
        "status=excluded.status;");
    stmt.bindText16(1, cp.volumeDevicePath);
    stmt.bindText16(2, cp.volumeRootPath);
    stmt.bindInt64(3, static_cast<std::int64_t>(cp.volumeSerial));
    stmt.bindText16(4, cp.filesystem);
    stmt.bindInt64(5, static_cast<std::int64_t>(cp.usnJournalId));
    stmt.bindInt64(6, static_cast<std::int64_t>(cp.nextUsn));
    stmt.bindInt64(7, static_cast<std::int64_t>(cp.lowestValidUsnAtCapture));
    stmt.bindInt64(8, static_cast<std::int64_t>(cp.fullIndexedAtTicks));
    stmt.bindInt64(9, static_cast<std::int64_t>(cp.lastRefreshAtTicks));
    stmt.bindText(10, cp.lastRefreshMethod);
    stmt.bindText(11, cp.status);
    stmt.stepDone();
}

std::optional<std::int64_t> findEntryIdByFileId(SqliteDb& db, std::uint64_t fileId)
{
    SqliteStmt stmt(db,
                    "SELECT id FROM entries WHERE root_id = 1 AND file_id = ?1 LIMIT 1;");
    stmt.bindInt64(1, static_cast<std::int64_t>(fileId));
    if (!stmt.step()) {
        return std::nullopt;
    }
    return stmt.columnInt64(0);
}

std::optional<std::int64_t> findEntryIdByPath(SqliteDb& db, const std::wstring& path)
{
    SqliteStmt stmt(db,
                    "SELECT id FROM entries WHERE root_id = 1 AND path = ?1 LIMIT 1;");
    stmt.bindText16(1, path);
    if (!stmt.step()) {
        return std::nullopt;
    }
    return stmt.columnInt64(0);
}

std::optional<std::int64_t> findDirIdByFileId(SqliteDb& db, std::uint64_t fileId)
{
    SqliteStmt stmt(
        db,
        "SELECT id FROM entries WHERE root_id = 1 AND kind = 1 AND file_id = ?1 LIMIT 1;");
    stmt.bindInt64(1, static_cast<std::int64_t>(fileId));
    if (!stmt.step()) {
        return std::nullopt;
    }
    return stmt.columnInt64(0);
}

std::int64_t nextEntryId(SqliteDb& db)
{
    SqliteStmt stmt(db, "SELECT COALESCE(MAX(id),0)+1 FROM entries;");
    if (stmt.step()) {
        return stmt.columnInt64(0);
    }
    return 1;
}

void deleteEntrySubtree(SqliteDb& db, std::int64_t entryId)
{
    // Delete entry and all descendants by path prefix for directories.
    SqliteStmt kindStmt(db, "SELECT kind, path FROM entries WHERE id = ?1;");
    kindStmt.bindInt64(1, entryId);
    if (!kindStmt.step()) {
        return;
    }
    const bool isDir = kindStmt.columnInt64(0) == 1;
    const std::wstring path = kindStmt.columnText16(1);
    if (isDir) {
        // path and path\...
        SqliteStmt del(
            db,
            "DELETE FROM entries WHERE root_id = 1 AND (path = ?1 OR path LIKE ?2);");
        del.bindText16(1, path);
        del.bindText16(2, path + L"\\%");
        del.stepDone();
    } else {
        SqliteStmt del(db, "DELETE FROM entries WHERE id = ?1;");
        del.bindInt64(1, entryId);
        del.stepDone();
    }
}

std::vector<std::wstring> listChildNames(SqliteDb& db, std::int64_t parentId)
{
    std::vector<std::wstring> names;
    SqliteStmt stmt(db, "SELECT name FROM entries WHERE parent_id = ?1;");
    stmt.bindInt64(1, parentId);
    while (stmt.step()) {
        names.push_back(stmt.columnText16(0));
    }
    return names;
}

/// When a directory is renamed/moved, USN typically emits only the directory
/// record — not every child. Rewrite descendant path prefixes so the index
/// stays path-consistent without requiring a full rebuild.
void rewriteDescendantPaths(SqliteDb& db, const std::wstring& oldPath,
                            const std::wstring& newPath)
{
    if (oldPath.empty() || newPath.empty() || oldPath == newPath) {
        return;
    }
    SqliteStmt sel(
        db,
        "SELECT id, path FROM entries WHERE root_id = 1 AND path LIKE ?1;");
    sel.bindText16(1, oldPath + L"\\%");
    std::vector<std::pair<std::int64_t, std::wstring>> rows;
    while (sel.step()) {
        rows.emplace_back(sel.columnInt64(0), sel.columnText16(1));
    }
    for (const auto& [id, p] : rows) {
        if (p.size() < oldPath.size()) {
            continue;
        }
        const std::wstring rewritten = newPath + p.substr(oldPath.size());
        SqliteStmt upd(db, "UPDATE entries SET path = ?1 WHERE id = ?2;");
        upd.bindText16(1, rewritten);
        upd.bindInt64(2, id);
        upd.stepDone();
    }
}

struct UpsertTouch {
    std::int64_t entryId = 0;
    std::int64_t oldParentId = 0;
    bool pathChanged = false;
};

/// Upsert one live path. Returns old parent id so callers can dirty both sides
/// of a rename/move for aggregate recompute.
UpsertTouch upsertEntryFromDisk(SqliteDb& db, const std::wstring& path,
                                const FileIdentity& id,
                                std::int64_t parentEntryId, FileTimeTicks now,
                                IndexRefreshResult& stats)
{
    UpsertTouch touch;
    const std::wstring name = leafName(path);
    const bool isDir = id.isDirectory;
    const ByteSize size = isDir ? 0 : id.sizeBytes;
    const std::string ext = isDir ? std::string{} : extensionLower(name);

    auto existing = findEntryIdByFileId(db, id.fileId);
    if (!existing) {
        existing = findEntryIdByPath(db, path);
    }

    std::wstring oldPath;
    std::int64_t oldParentId = 0;
    if (existing) {
        SqliteStmt prev(
            db, "SELECT path, parent_id FROM entries WHERE id = ?1;");
        prev.bindInt64(1, *existing);
        if (prev.step()) {
            oldPath = prev.columnText16(0);
            oldParentId = prev.columnInt64(1);
            touch.oldParentId = oldParentId;
        }
    }

    std::vector<std::wstring> children;
    if (isDir && existing) {
        children = listChildNames(db, *existing);
    }

    if (existing) {
        touch.entryId = *existing;
        touch.pathChanged = (!oldPath.empty() && oldPath != path);
        // Directory rename/move: rewrite children paths before updating parent.
        if (isDir && touch.pathChanged) {
            rewriteDescendantPaths(db, oldPath, path);
        }
        SqliteStmt upd(
            db,
            "UPDATE entries SET parent_id=?1, kind=?2, name=?3, path=?4, "
            "size_bytes=?5, extension=?6, last_write_ticks=?7, last_access_ticks=?8, "
            "attributes=?9, file_id=?10, parent_file_id=?11, "
            "classification=?12, confidence=?13, rule_id=?14, location_safety=?15, "
            "reclaimability=?16, candidate_strength=?17 "
            "WHERE id=?18;");
        upd.bindInt64(1, parentEntryId);
        upd.bindInt64(2, isDir ? 1 : 0);
        upd.bindText16(3, name);
        upd.bindText16(4, path);
        upd.bindInt64(5, static_cast<std::int64_t>(size));
        upd.bindText(6, ext);
        upd.bindInt64(7, static_cast<std::int64_t>(id.lastWriteTicks));
        upd.bindInt64(8, static_cast<std::int64_t>(id.lastAccessTicks));
        upd.bindInt64(9, static_cast<std::int64_t>(id.attributes));
        upd.bindInt64(10, static_cast<std::int64_t>(id.fileId));
        // parent_file_id filled by caller via separate update if known — 0 ok
        upd.bindInt64(11, 0);
        {
            Classification cls =
                isDir ? classifyDirectory(name, path, children.data(),
                                          children.size())
                      : classifyFile(name, path);
            const auto safety = classifyLocation(path);
            const auto reclaim = analyzeItem(
                path, isDir ? ItemKind::Directory : ItemKind::File, size,
                id.lastWriteTicks, cls, safety, now, id.lastAccessTicks);
            upd.bindText(12, toString(cls.category));
            upd.bindText(13, toString(cls.confidence));
            upd.bindText(14, cls.ruleId);
            upd.bindText(15, toString(safety));
            upd.bindText(16, toString(reclaim.reclaimability));
            upd.bindText(17, toString(reclaim.strength));
        }
        upd.bindInt64(18, *existing);
        upd.stepDone();
        ++stats.modified;
        ++stats.rowsChanged;
    } else {
        const std::int64_t newId = nextEntryId(db);
        SqliteStmt ins(
            db,
            "INSERT INTO entries("
            "id, root_id, parent_id, kind, name, path, size_bytes, recursive_size, "
            "extension, last_write_ticks, last_access_ticks, attributes, is_reparse, "
            "classification, confidence, rule_id, location_safety, reclaimability, "
            "candidate_strength, newest_descendant_write, oldest_descendant_write, "
            "file_id, parent_file_id) "
            "VALUES(?1,1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,0,?12,?13,?14,?15,?16,?17,"
            "?18,?19,?20,?21);");
        ins.bindInt64(1, newId);
        ins.bindInt64(2, parentEntryId);
        ins.bindInt64(3, isDir ? 1 : 0);
        ins.bindText16(4, name);
        ins.bindText16(5, path);
        ins.bindInt64(6, static_cast<std::int64_t>(size));
        ins.bindInt64(7, static_cast<std::int64_t>(size));  // dirs recomputed later
        ins.bindText(8, ext);
        ins.bindInt64(9, static_cast<std::int64_t>(id.lastWriteTicks));
        ins.bindInt64(10, static_cast<std::int64_t>(id.lastAccessTicks));
        ins.bindInt64(11, static_cast<std::int64_t>(id.attributes));
        {
            Classification cls = classifyFile(name, path);
            if (isDir) {
                cls = classifyDirectory(name, path, nullptr, 0);
            }
            const auto safety = classifyLocation(path);
            const auto reclaim = analyzeItem(
                path, isDir ? ItemKind::Directory : ItemKind::File, size,
                id.lastWriteTicks, cls, safety, now, id.lastAccessTicks);
            ins.bindText(12, toString(cls.category));
            ins.bindText(13, toString(cls.confidence));
            ins.bindText(14, cls.ruleId);
            ins.bindText(15, toString(safety));
            ins.bindText(16, toString(reclaim.reclaimability));
            ins.bindText(17, toString(reclaim.strength));
        }
        ins.bindInt64(18, isDir ? static_cast<std::int64_t>(id.lastWriteTicks) : 0);
        ins.bindInt64(19, isDir ? static_cast<std::int64_t>(id.lastWriteTicks) : 0);
        ins.bindInt64(20, static_cast<std::int64_t>(id.fileId));
        ins.bindInt64(21, 0);
        ins.stepDone();
        ++stats.added;
        ++stats.rowsChanged;
        touch.entryId = newId;
    }
    return touch;
}

/// Recompute recursive_size and write activity for a directory from children.
void recomputeDirectory(SqliteDb& db, std::int64_t dirId, FileTimeTicks now)
{
    ByteSize recursive = 0;
    std::uint64_t newest = 0;
    std::uint64_t oldest = 0;
    bool oldestSet = false;

    // Direct files.
    {
        SqliteStmt files(
            db,
            "SELECT size_bytes, last_write_ticks FROM entries "
            "WHERE parent_id = ?1 AND kind = 0;");
        files.bindInt64(1, dirId);
        while (files.step()) {
            recursive += static_cast<ByteSize>(files.columnInt64(0));
            const auto w = static_cast<std::uint64_t>(files.columnInt64(1));
            if (w > newest) {
                newest = w;
            }
            if (w != 0 && (!oldestSet || w < oldest)) {
                oldest = w;
                oldestSet = true;
            }
        }
    }
    // Child directories.
    {
        SqliteStmt dirs(
            db,
            "SELECT recursive_size, newest_descendant_write, oldest_descendant_write "
            "FROM entries WHERE parent_id = ?1 AND kind = 1;");
        dirs.bindInt64(1, dirId);
        while (dirs.step()) {
            recursive += static_cast<ByteSize>(dirs.columnInt64(0));
            const auto n = static_cast<std::uint64_t>(dirs.columnInt64(1));
            const auto o = static_cast<std::uint64_t>(dirs.columnInt64(2));
            if (n > newest) {
                newest = n;
            }
            if (o != 0 && (!oldestSet || o < oldest)) {
                oldest = o;
                oldestSet = true;
            }
        }
    }

    // Reclassify directory using current children names.
    std::wstring path;
    std::wstring name;
    {
        SqliteStmt s(db, "SELECT path, name FROM entries WHERE id = ?1;");
        s.bindInt64(1, dirId);
        if (!s.step()) {
            return;
        }
        path = s.columnText16(0);
        name = s.columnText16(1);
    }
    const auto children = listChildNames(db, dirId);
    const auto cls =
        classifyDirectory(name, path, children.data(), children.size());
    const auto safety = classifyLocation(path);
    const auto reclaim =
        analyzeItem(path, ItemKind::Directory, recursive, newest, cls, safety,
                    now, 0);

    SqliteStmt upd(
        db,
        "UPDATE entries SET size_bytes=?1, recursive_size=?1, "
        "last_write_ticks=?2, newest_descendant_write=?2, oldest_descendant_write=?3, "
        "classification=?4, confidence=?5, rule_id=?6, location_safety=?7, "
        "reclaimability=?8, candidate_strength=?9 WHERE id=?10;");
    upd.bindInt64(1, static_cast<std::int64_t>(recursive));
    upd.bindInt64(2, static_cast<std::int64_t>(newest));
    upd.bindInt64(3, static_cast<std::int64_t>(oldestSet ? oldest : 0));
    upd.bindText(4, toString(cls.category));
    upd.bindText(5, toString(cls.confidence));
    upd.bindText(6, cls.ruleId);
    upd.bindText(7, toString(safety));
    upd.bindText(8, toString(reclaim.reclaimability));
    upd.bindText(9, toString(reclaim.strength));
    upd.bindInt64(10, dirId);
    upd.stepDone();
}

/// Ensure a directory path under the indexed root exists as an entry, creating
/// ancestor dirs from live disk when they appear in the same USN window as
/// their children (unordered FRN map would otherwise hit missing_parent).
/// Returns entry id, or nullopt if the path cannot be materialized safely.
std::optional<std::int64_t> ensureDirUnderRoot(
    SqliteDb& db, const std::wstring& dirPath, const std::wstring& root,
    FileTimeTicks now, IndexRefreshResult& stats,
    std::unordered_set<std::int64_t>& dirtyDirs, int depth = 0)
{
    if (depth > 64 || dirPath.empty()) {
        return std::nullopt;
    }
    const std::wstring aligned = rebasePathOntoRoot(dirPath, root);
    if (auto existing = findEntryIdByPath(db, aligned)) {
        return existing;
    }

    if (win32PathsEqual(aligned, root)) {
        SqliteStmt rs(
            db,
            "SELECT id FROM entries WHERE root_id=1 AND parent_id IS NULL LIMIT 1;");
        if (rs.step()) {
            return rs.columnInt64(0);
        }
        return findEntryIdByPath(db, root);
    }
    if (!pathIsUnderRoot(aligned, root)) {
        return std::nullopt;
    }

    const std::wstring parentPath = parentPathOf(aligned);
    auto parentId =
        ensureDirUnderRoot(db, parentPath, root, now, stats, dirtyDirs, depth + 1);
    if (!parentId || *parentId <= 0) {
        return std::nullopt;
    }

    auto identity = queryFileIdentity(aligned);
    if (!identity || !identity->isDirectory) {
        return std::nullopt;
    }
    if (auto byFrn = findEntryIdByFileId(db, identity->fileId)) {
        return byFrn;
    }

    const UpsertTouch touch =
        upsertEntryFromDisk(db, aligned, *identity, *parentId, now, stats);
    if (touch.entryId <= 0) {
        return std::nullopt;
    }
    dirtyDirs.insert(*parentId);
    dirtyDirs.insert(touch.entryId);
    return touch.entryId;
}

void recomputeAncestors(SqliteDb& db, std::int64_t startId, FileTimeTicks now,
                        IndexRefreshResult& stats)
{
    // Collect start → root. start is typically a dirty directory; if a file id
    // is passed, the walk skips non-dirs when recomputing.
    //
    // Order matters: child recursive_size must be final before the parent sums
    // it. Chain is deepest-first (start, …, root) — iterate forward, never
    // reverse. Reverse order left parents at pre-delta sizes while leaves were
    // correct (multi-batch parity: file counts match, logical_bytes lag).
    std::vector<std::int64_t> chain;
    std::int64_t cur = startId;
    while (cur > 0) {
        chain.push_back(cur);
        SqliteStmt s(db, "SELECT parent_id, kind FROM entries WHERE id = ?1;");
        s.bindInt64(1, cur);
        if (!s.step()) {
            break;
        }
        const auto parent = s.columnInt64(0);
        const bool isDir = s.columnInt64(1) == 1;
        if (!isDir) {
            // Start from parent directory.
            cur = parent;
            continue;
        }
        if (parent <= 0) {
            // Root is already in chain; stop.
            break;
        }
        cur = parent;
    }

    for (const std::int64_t id : chain) {
        SqliteStmt s(db, "SELECT kind FROM entries WHERE id = ?1;");
        s.bindInt64(1, id);
        if (!s.step() || s.columnInt64(0) != 1) {
            continue;
        }
        recomputeDirectory(db, id, now);
        ++stats.dirsRecomputed;
    }
}

void refreshRootCounts(SqliteDb& db, IndexRootInfo& meta)
{
    SqliteStmt c(
        db,
        "SELECT "
        "SUM(CASE WHEN kind=0 THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN kind=1 THEN 1 ELSE 0 END) FROM entries WHERE root_id=1;");
    if (c.step()) {
        meta.fileCount = static_cast<std::uint64_t>(c.columnInt64(0));
        meta.dirCount = static_cast<std::uint64_t>(c.columnInt64(1));
    }
    SqliteStmt root(
        db,
        "SELECT recursive_size FROM entries WHERE root_id=1 AND parent_id IS NULL "
        "LIMIT 1;");
    if (root.step()) {
        meta.logicalBytes = static_cast<ByteSize>(root.columnInt64(0));
    }
}

IndexRefreshResult makeProbeBase(const std::wstring& rootPath)
{
    IndexRefreshResult r;
    r.location = locateIndex(rootPath);
    r.root.rootPath = r.location.rootPath;
    return r;
}

}  // namespace

const char* toString(IncrementalRefreshState state) noexcept
{
    switch (state) {
    case IncrementalRefreshState::Supported:
        return "supported";
    case IncrementalRefreshState::Unavailable:
        return "unavailable";
    case IncrementalRefreshState::UnsupportedFilesystem:
        return "unsupported_filesystem";
    case IncrementalRefreshState::AccessDenied:
        return "access_denied";
    case IncrementalRefreshState::JournalNotActive:
        return "journal_not_active";
    case IncrementalRefreshState::JournalChanged:
        return "journal_changed";
    case IncrementalRefreshState::HistoryLost:
        return "history_lost";
    case IncrementalRefreshState::VolumeChanged:
        return "volume_changed";
    case IncrementalRefreshState::NeedsFullRebuild:
        return "needs_full_rebuild";
    case IncrementalRefreshState::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* toString(IndexRefreshOutcome outcome) noexcept
{
    switch (outcome) {
    case IndexRefreshOutcome::Refreshed:
        return "refreshed";
    case IndexRefreshOutcome::AlreadyCurrent:
        return "already_current";
    case IndexRefreshOutcome::FullRebuildRequired:
        return "full_rebuild_required";
    case IndexRefreshOutcome::Cancelled:
        return "cancelled";
    case IndexRefreshOutcome::Failed:
        return "failed";
    case IndexRefreshOutcome::IndexNotFound:
        return "index_not_found";
    }
    return "failed";
}

void writeRefreshCheckpointAfterFullBuild(IndexStore& store,
                                          const std::wstring& rootPath,
                                          std::uint64_t fullIndexedAtTicks)
{
    RefreshCheckpoint cp;
    cp.fullIndexedAtTicks = fullIndexedAtTicks;
    cp.lastRefreshAtTicks = fullIndexedAtTicks;
    cp.lastRefreshMethod = "full";
    cp.status = "unavailable";

    UsnJournalReader reader;
    const UsnCapability cap = UsnJournalReader::tryOpen(rootPath, reader);
    if (cap == UsnCapability::Supported) {
        UsnJournalState st{};
        if (reader.query(st) == UsnCapability::Supported) {
            cp.volumeDevicePath = reader.volume().devicePath;
            cp.volumeRootPath = reader.volume().rootPath;
            cp.volumeSerial = reader.volume().serialNumber;
            cp.filesystem = reader.volume().fileSystem;
            cp.usnJournalId = st.journalId;
            cp.nextUsn = st.nextUsn;
            cp.lowestValidUsnAtCapture = st.lowestValidUsn;
            cp.status = "ready";
        } else {
            VolumeIdentity vol{};
            if (resolveVolumeIdentity(rootPath, vol)) {
                cp.volumeDevicePath = vol.devicePath;
                cp.volumeRootPath = vol.rootPath;
                cp.volumeSerial = vol.serialNumber;
                cp.filesystem = vol.fileSystem;
            }
            // Query failed after open — treat like unavailable.
            cp.status = "unavailable";
        }
    } else {
        VolumeIdentity vol{};
        if (resolveVolumeIdentity(rootPath, vol)) {
            cp.volumeDevicePath = vol.devicePath;
            cp.volumeRootPath = vol.rootPath;
            cp.volumeSerial = vol.serialNumber;
            cp.filesystem = vol.fileSystem;
        }
        // Persist a specific status so status/refresh can report the real reason
        // without requiring a live re-open (still re-checked on probe when useful).
        switch (cap) {
        case UsnCapability::AccessDenied:
            cp.status = "access_denied";
            break;
        case UsnCapability::JournalNotActive:
            cp.status = "journal_not_active";
            break;
        case UsnCapability::UnsupportedFilesystem:
            cp.status = "unsupported_filesystem";
            break;
        default:
            cp.status = "unavailable";
            break;
        }
    }
    upsertCheckpoint(store.db(), cp);
}

IndexRefreshResult probeIncremental(const std::wstring& rootPath)
{
    IndexRefreshResult r = makeProbeBase(rootPath);
    if (!indexDatabaseExists(r.location)) {
        r.outcome = IndexRefreshOutcome::IndexNotFound;
        r.reason = "index_not_found";
        r.incrementalState = IncrementalRefreshState::Unavailable;
        return r;
    }
    try {
        auto store = IndexStore::openRead(r.location);
        auto meta = store.readRootMeta();
        if (!meta) {
            r.outcome = IndexRefreshOutcome::Failed;
            r.reason = "index_corrupt";
            return r;
        }
        r.root = *meta;
        auto cp = readCheckpoint(store.db());
        if (cp) {
            r.checkpoint = *cp;
        }

        // Live capability always wins for reporting; checkpoint status is a
        // hint when the journal cannot be opened again.
        UsnJournalReader reader;
        const UsnCapability cap =
            UsnJournalReader::tryOpen(r.location.rootPath, reader);

        if (!cp || cp->status != "ready" || cp->usnJournalId == 0) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            if (cap == UsnCapability::AccessDenied ||
                (cp && cp->status == "access_denied")) {
                r.incrementalState = IncrementalRefreshState::AccessDenied;
                r.reason = "access_denied";
            } else if (cap == UsnCapability::JournalNotActive ||
                       (cp && cp->status == "journal_not_active")) {
                r.incrementalState = IncrementalRefreshState::JournalNotActive;
                r.reason = "journal_not_active";
            } else if (cap == UsnCapability::UnsupportedFilesystem ||
                       (cp && cp->status == "unsupported_filesystem")) {
                r.incrementalState =
                    IncrementalRefreshState::UnsupportedFilesystem;
                r.reason = "unsupported_filesystem";
            } else if (cap != UsnCapability::Supported &&
                       cap != UsnCapability::InvalidPath) {
                r.incrementalState = IncrementalRefreshState::NeedsFullRebuild;
                r.reason = toString(cap);
            } else {
                r.incrementalState = IncrementalRefreshState::Unavailable;
                r.reason = "checkpoint_unavailable";
            }
            return r;
        }

        if (cap == UsnCapability::UnsupportedFilesystem) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = IncrementalRefreshState::UnsupportedFilesystem;
            r.reason = "unsupported_filesystem";
            return r;
        }
        if (cap == UsnCapability::AccessDenied) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = IncrementalRefreshState::AccessDenied;
            r.reason = "access_denied";
            return r;
        }
        if (cap == UsnCapability::JournalNotActive) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = IncrementalRefreshState::JournalNotActive;
            r.reason = "journal_not_active";
            return r;
        }
        if (cap != UsnCapability::Supported) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = IncrementalRefreshState::NeedsFullRebuild;
            r.reason = toString(cap);
            return r;
        }

        if (reader.volume().serialNumber != 0 && cp->volumeSerial != 0 &&
            reader.volume().serialNumber != cp->volumeSerial) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = IncrementalRefreshState::VolumeChanged;
            r.reason = "volume_changed";
            return r;
        }

        UsnJournalState st{};
        if (reader.query(st) != UsnCapability::Supported) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = IncrementalRefreshState::NeedsFullRebuild;
            r.reason = "journal_query_failed";
            return r;
        }
        if (st.journalId != cp->usnJournalId) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = IncrementalRefreshState::JournalChanged;
            r.reason = "journal_changed";
            return r;
        }
        if (cp->nextUsn < st.lowestValidUsn) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = IncrementalRefreshState::HistoryLost;
            r.reason = "history_lost";
            return r;
        }

        r.incrementalState = IncrementalRefreshState::Supported;
        r.outcome = (cp->nextUsn >= st.nextUsn)
                        ? IndexRefreshOutcome::AlreadyCurrent
                        : IndexRefreshOutcome::Refreshed;  // "would refresh"
        r.reason = "ok";
        return r;
    } catch (const std::exception& ex) {
        r.outcome = IndexRefreshOutcome::Failed;
        r.error = ex.what();
        r.reason = "probe_failed";
        return r;
    }
}

IndexRefreshResult refreshIndex(const std::wstring& rootPath, std::stop_token stop)
{
    const auto started = std::chrono::steady_clock::now();
    IndexRefreshResult r = makeProbeBase(rootPath);

    if (!indexDatabaseExists(r.location)) {
        r.outcome = IndexRefreshOutcome::IndexNotFound;
        r.reason = "index_not_found";
        r.incrementalState = IncrementalRefreshState::Unavailable;
        return r;
    }

    try {
        // Probe first without writing.
        auto probe = probeIncremental(rootPath);
        r.incrementalState = probe.incrementalState;
        r.checkpoint = probe.checkpoint;
        r.root = probe.root;
        if (probe.outcome == IndexRefreshOutcome::IndexNotFound ||
            probe.outcome == IndexRefreshOutcome::Failed) {
            r.outcome = probe.outcome;
            r.reason = probe.reason;
            r.error = probe.error;
            return r;
        }
        if (probe.outcome == IndexRefreshOutcome::FullRebuildRequired) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.reason = probe.reason;
            return r;
        }
        if (probe.outcome == IndexRefreshOutcome::AlreadyCurrent) {
            r.outcome = IndexRefreshOutcome::AlreadyCurrent;
            r.reason = "already_current";
            r.elapsedSeconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              started)
                    .count();
            return r;
        }

        if (stop.stop_requested()) {
            r.outcome = IndexRefreshOutcome::Cancelled;
            r.reason = "cancelled";
            return r;
        }

        UsnJournalReader reader;
        if (UsnJournalReader::tryOpen(r.location.rootPath, reader) !=
            UsnCapability::Supported) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.reason = "journal_unavailable";
            r.incrementalState = IncrementalRefreshState::NeedsFullRebuild;
            return r;
        }

        UsnJournalState journal{};
        if (reader.query(journal) != UsnCapability::Supported) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.reason = "journal_query_failed";
            return r;
        }
        if (journal.journalId != r.checkpoint.usnJournalId) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = IncrementalRefreshState::JournalChanged;
            r.reason = "journal_changed";
            return r;
        }

        std::unordered_map<std::uint64_t, CoalescedChange> changes;
        // Exclusive cursor: first USN not yet consumed. Must be a driver-issued
        // continuation or journal NextUsn — never record.usn+1 (USNs are sparse
        // journal offsets; misaligned StartUsn → ERROR_INVALID_PARAMETER).
        std::uint64_t nextCursor = r.checkpoint.nextUsn;
        r.diagStartUsn = r.checkpoint.nextUsn;
        r.diagJournalNextUsn = journal.nextUsn;
        r.diagJournalLowestUsn = journal.lowestValidUsn;

        const UsnCapability readCap = reader.readSince(
            r.checkpoint.nextUsn,
            [&](const UsnChangeRecord& rec) -> bool {
                if (stop.stop_requested()) {
                    return false;
                }
                ++r.journalRecordsSeen;
                coalesce(changes, rec);
                return true;
            },
            nextCursor, stop);

        r.diagContinuationUsn = nextCursor;
        r.diagCoalescedFrns = changes.size();

        if (stop.stop_requested()) {
            r.outcome = IndexRefreshOutcome::Cancelled;
            r.reason = "cancelled";
            // Checkpoint NOT advanced.
            return r;
        }
        if (readCap == UsnCapability::HistoryLost ||
            readCap == UsnCapability::JournalChanged) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.incrementalState = (readCap == UsnCapability::HistoryLost)
                                     ? IncrementalRefreshState::HistoryLost
                                     : IncrementalRefreshState::JournalChanged;
            r.reason = toString(readCap);
            return r;
        }
        if (readCap != UsnCapability::Supported) {
            r.outcome = IndexRefreshOutcome::FullRebuildRequired;
            r.reason = toString(readCap);
            return r;
        }

        // Re-query tip. Clamp continuation to [checkpoint, tip]. Never invent a
        // cursor past the live tip, behind the prior checkpoint, or via
        // record.usn+1. Driver READ continuation already advances past
        // ReasonMask-filtered records, so empty matching sets still catch up.
        if (reader.query(journal) == UsnCapability::Supported) {
            r.diagJournalNextUsn = journal.nextUsn;
            r.diagJournalLowestUsn = journal.lowestValidUsn;
            if (nextCursor < r.checkpoint.nextUsn) {
                nextCursor = r.checkpoint.nextUsn;
            }
            if (nextCursor > journal.nextUsn) {
                nextCursor = journal.nextUsn;
            }
        }

        auto store = IndexStore::openReadWrite(r.location);
        auto meta = store.readRootMeta();
        if (!meta) {
            r.outcome = IndexRefreshOutcome::Failed;
            r.reason = "index_corrupt";
            return r;
        }
        r.root = *meta;
        const FileTimeTicks now = nowFileTime();
        const std::wstring root = r.location.rootPath;

        SqliteTxn txn(store.db());
        std::unordered_set<std::int64_t> dirtyDirs;

        for (const auto& [fileId, ch] : changes) {
            if (stop.stop_requested()) {
                txn.rollback();
                r.outcome = IndexRefreshOutcome::Cancelled;
                r.reason = "cancelled";
                return r;
            }

            // Resolve path: live OpenFileById, else DB. Rebase onto the
            // indexed root spelling so 8.3 TEMP and long GetFinalPathName
            // forms share one prefix (path lookups and stored rows).
            std::wstring path = pathFromFileId(reader.volumeHandle(), fileId);
            if (!path.empty()) {
                path = rebasePathOntoRoot(path, root);
            }
            std::optional<std::int64_t> existingId = findEntryIdByFileId(store.db(), fileId);

            if (path.empty() && existingId) {
                SqliteStmt s(store.db(), "SELECT path FROM entries WHERE id=?1;");
                s.bindInt64(1, *existingId);
                if (s.step()) {
                    path = s.columnText16(0);
                }
            }

            const bool underRoot =
                !path.empty() && pathIsUnderRoot(path, root);
            const bool wasInIndex = existingId.has_value();

            // Outside root: only care if it was previously inside (moved out).
            if (!underRoot && !wasInIndex) {
                continue;
            }
            ++r.recordsInRoot;

            if ((ch.sawDelete && !ch.sawCreate && !ch.sawRenameNew) ||
                (wasInIndex && !underRoot)) {
                // Deleted or moved out of root.
                if (existingId) {
                    // Mark parent dirty before delete.
                    SqliteStmt p(store.db(),
                                 "SELECT parent_id FROM entries WHERE id=?1;");
                    p.bindInt64(1, *existingId);
                    if (p.step()) {
                        const auto parentId = p.columnInt64(0);
                        if (parentId > 0) {
                            dirtyDirs.insert(parentId);
                        }
                    }
                    deleteEntrySubtree(store.db(), *existingId);
                    ++r.removed;
                    ++r.rowsChanged;
                    if (ch.sawRenameOld || ch.sawRenameNew) {
                        ++r.renamed;
                    }
                }
                continue;
            }

            if (!underRoot) {
                continue;
            }

            // Live identity for upsert.
            auto identity = queryFileIdentity(path);
            if (!identity) {
                // Race: deleted after USN — treat as remove if known.
                if (existingId) {
                    SqliteStmt p(store.db(),
                                 "SELECT parent_id FROM entries WHERE id=?1;");
                    p.bindInt64(1, *existingId);
                    if (p.step()) {
                        const auto parentId = p.columnInt64(0);
                        if (parentId > 0) {
                            dirtyDirs.insert(parentId);
                        }
                    }
                    deleteEntrySubtree(store.db(), *existingId);
                    ++r.removed;
                    ++r.rowsChanged;
                }
                continue;
            }

            // Parent entry id.
            std::int64_t parentEntryId = 0;
            if (win32PathsEqual(path, root)) {
                parentEntryId = 0;  // root has null parent — handled below
            } else {
                const std::wstring parentPath = parentPathOf(path);
                if (auto byFileId =
                        findDirIdByFileId(store.db(), ch.parentFileId)) {
                    parentEntryId = *byFileId;
                } else if (auto byPath =
                               findEntryIdByPath(store.db(), parentPath)) {
                    parentEntryId = *byPath;
                } else if (auto ensured = ensureDirUnderRoot(
                               store.db(), parentPath, root, now, r,
                               dirtyDirs)) {
                    // Same USN window often creates parent dir + children with
                    // unordered FRN apply order. Materialize live parents under
                    // root instead of fail-closed missing_parent.
                    parentEntryId = *ensured;
                } else {
                    // Parent still unresolved: under-root gap → full rebuild;
                    // otherwise treat as root child when parent is the root.
                    if (pathIsUnderRoot(parentPath, root) &&
                        !win32PathsEqual(parentPath, root)) {
                        txn.rollback();
                        r.outcome = IndexRefreshOutcome::FullRebuildRequired;
                        r.reason = "missing_parent";
                        r.incrementalState =
                            IncrementalRefreshState::NeedsFullRebuild;
                        return r;
                    }
                    auto rootId = findEntryIdByPath(store.db(), root);
                    if (!rootId) {
                        SqliteStmt rs(
                            store.db(),
                            "SELECT id FROM entries WHERE root_id=1 AND "
                            "parent_id IS NULL LIMIT 1;");
                        if (rs.step()) {
                            parentEntryId = rs.columnInt64(0);
                        }
                    } else {
                        parentEntryId = *rootId;
                    }
                }
            }

            if (parentEntryId == 0 && !win32PathsEqual(path, root)) {
                SqliteStmt rs(
                    store.db(),
                    "SELECT id FROM entries WHERE root_id=1 AND parent_id IS NULL "
                    "LIMIT 1;");
                if (rs.step()) {
                    parentEntryId = rs.columnInt64(0);
                }
            }

            const bool isNew = !existingId.has_value();
            if (ch.sawRenameNew || ch.sawRenameOld) {
                ++r.renamed;
            }

            if (win32PathsEqual(path, root)) {
                // Update root node metadata only.
                auto rootId = findEntryIdByFileId(store.db(), identity->fileId);
                if (!rootId) {
                    SqliteStmt rs(
                        store.db(),
                        "SELECT id FROM entries WHERE root_id=1 AND "
                        "parent_id IS NULL LIMIT 1;");
                    if (rs.step()) {
                        rootId = rs.columnInt64(0);
                    }
                }
                if (rootId) {
                    dirtyDirs.insert(*rootId);
                }
            } else {
                const UpsertTouch touch = upsertEntryFromDisk(
                    store.db(), path, *identity, parentEntryId, now, r);
                (void)isNew;
                // New parent (and old parent on rename/move) need aggregate recompute.
                if (parentEntryId > 0) {
                    dirtyDirs.insert(parentEntryId);
                }
                if (touch.oldParentId > 0 &&
                    touch.oldParentId != parentEntryId) {
                    dirtyDirs.insert(touch.oldParentId);
                }
                if (touch.entryId > 0 && identity->isDirectory) {
                    dirtyDirs.insert(touch.entryId);
                }
            }
        }

        // Recompute aggregates for dirty directories (and ancestors).
        // Sort by path depth descending so children recompute first.
        std::vector<std::pair<int, std::int64_t>> byDepth;
        byDepth.reserve(dirtyDirs.size());
        for (const std::int64_t id : dirtyDirs) {
            SqliteStmt s(store.db(), "SELECT path, kind FROM entries WHERE id=?1;");
            s.bindInt64(1, id);
            if (!s.step() || s.columnInt64(1) != 1) {
                continue;
            }
            const std::wstring p = s.columnText16(0);
            int depth = 0;
            for (wchar_t ch : p) {
                if (ch == L'\\' || ch == L'/') {
                    ++depth;
                }
            }
            byDepth.emplace_back(depth, id);
        }
        std::sort(byDepth.begin(), byDepth.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        std::unordered_set<std::int64_t> recomputed;
        for (const auto& [depth, id] : byDepth) {
            (void)depth;
            recomputeAncestors(store.db(), id, now, r);
            recomputed.insert(id);
        }

        refreshRootCounts(store.db(), r.root);
        r.root.status = IndexStatus::Ready;
        // Keep original full indexed_at; only counts change.
        store.writeRootMeta(r.root);

        // Advance checkpoint ONLY after successful delta apply.
        RefreshCheckpoint newCp = r.checkpoint;
        newCp.nextUsn = nextCursor;
        newCp.lastRefreshAtTicks = now;
        newCp.lastRefreshMethod = "usn";
        newCp.status = "ready";
        newCp.lowestValidUsnAtCapture = journal.lowestValidUsn;
        upsertCheckpoint(store.db(), newCp);
        r.checkpoint = newCp;
        r.diagCommittedNextUsn = nextCursor;

        txn.commit();
        r.outcome = IndexRefreshOutcome::Refreshed;
        r.reason = "ok";
        r.incrementalState = IncrementalRefreshState::Supported;
    } catch (const std::exception& ex) {
        r.outcome = IndexRefreshOutcome::Failed;
        r.error = ex.what();
        r.reason = "refresh_failed";
    } catch (...) {
        r.outcome = IndexRefreshOutcome::Failed;
        r.reason = "refresh_failed";
        r.error = "unknown";
    }

    r.elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
            .count();
    return r;
}

}  // namespace spacelens
