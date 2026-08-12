#include "core/index/IndexQuery.hpp"

#include "core/FileTime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <sstream>

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

std::uint64_t ageMs(std::uint64_t indexedAtTicks, FileTimeTicks now)
{
    if (indexedAtTicks == 0 || now == 0 || indexedAtTicks > now) {
        return 0;
    }
    return (now - indexedAtTicks) / 10'000ULL;  // 100ns -> ms
}

IndexQueryResult fail(const std::wstring& rootPath, const char* error)
{
    IndexQueryResult r;
    r.ok = false;
    r.error = error;
    r.location = locateIndex(rootPath);
    r.root.rootPath = r.location.rootPath;
    return r;
}

}  // namespace

IndexQueryResult indexStatus(const std::wstring& rootPath)
{
    IndexQueryResult r;
    r.location = locateIndex(rootPath);
    r.root.rootPath = r.location.rootPath;
    if (!indexDatabaseExists(r.location)) {
        r.error = "index_not_found";
        return r;
    }
    try {
        auto store = IndexStore::openRead(r.location);
        auto meta = store.readRootMeta();
        if (!meta) {
            r.error = "index_corrupt";
            return r;
        }
        r.root = *meta;
        r.ok = true;
        r.age_ms = ageMs(r.root.indexedAtTicks, nowFileTime());
    } catch (const SqliteError& ex) {
        r.error = ex.what();
        if (r.error.find("not found") != std::string::npos) {
            r.error = "index_not_found";
        } else if (r.error.find("unsupported") != std::string::npos) {
            r.error = "unsupported_schema";
        }
    } catch (...) {
        r.error = "index_open_failed";
    }
    return r;
}

IndexQueryResult queryIndex(const std::wstring& rootPath,
                            const IndexQuerySpec& spec)
{
    IndexQueryResult r;
    r.location = locateIndex(rootPath);
    r.root.rootPath = r.location.rootPath;
    if (!indexDatabaseExists(r.location)) {
        r.error = "index_not_found";
        return r;
    }

    try {
        auto store = IndexStore::openRead(r.location);
        auto meta = store.readRootMeta();
        if (!meta || meta->status != IndexStatus::Ready) {
            r.error = meta ? "index_not_ready" : "index_corrupt";
            return r;
        }
        r.root = *meta;
        const FileTimeTicks now =
            spec.nowTicks != 0 ? spec.nowTicks : nowFileTime();
        r.age_ms = ageMs(r.root.indexedAtTicks, now);

        std::ostringstream sql;
        sql << "SELECT path, kind, "
            << "CASE WHEN kind = 1 THEN recursive_size ELSE size_bytes END AS sz, "
            << "classification, confidence, location_safety, reclaimability, "
            << "candidate_strength, last_write_ticks "
            << "FROM entries WHERE root_id = 1";

        if (spec.includeFiles && !spec.includeDirectories) {
            sql << " AND kind = 0";
        } else if (!spec.includeFiles && spec.includeDirectories) {
            sql << " AND kind = 1";
        } else if (!spec.includeFiles && !spec.includeDirectories) {
            r.ok = true;
            return r;
        }

        if (spec.minSize) {
            sql << " AND (CASE WHEN kind = 1 THEN recursive_size ELSE size_bytes END) >= ?";
        }
        if (!spec.extension.empty()) {
            sql << " AND kind = 0 AND extension = ?";
        }
        if (spec.olderThanDays && *spec.olderThanDays > 0) {
            // Activity: files use last_write_ticks; dirs use newest_descendant_write
            // stored in last_write_ticks for dirs during build.
            sql << " AND last_write_ticks > 0 AND last_write_ticks <= ?";
        }
        if (!spec.classification.empty()) {
            sql << " AND classification = ?";
        }
        if (!spec.candidateStrength.empty()) {
            sql << " AND candidate_strength = ?";
        }

        sql << " ORDER BY sz DESC, path ASC LIMIT ?";

        // Count query (same filters, no limit) for matched_items / bytes.
        std::string countSql = "SELECT COUNT(*), COALESCE(SUM("
                               "CASE WHEN kind = 1 THEN recursive_size ELSE size_bytes END"
                               "),0) FROM entries WHERE root_id = 1";
        {
            // rebuild filters into countSql by parsing — simpler to duplicate.
        }
        std::ostringstream count;
        count << "SELECT COUNT(*), COALESCE(SUM(CASE WHEN kind = 1 THEN "
                 "recursive_size ELSE size_bytes END),0) FROM entries WHERE "
                 "root_id = 1";
        if (spec.includeFiles && !spec.includeDirectories) {
            count << " AND kind = 0";
        } else if (!spec.includeFiles && spec.includeDirectories) {
            count << " AND kind = 1";
        }
        if (spec.minSize) {
            count << " AND (CASE WHEN kind = 1 THEN recursive_size ELSE "
                     "size_bytes END) >= ?";
        }
        if (!spec.extension.empty()) {
            count << " AND kind = 0 AND extension = ?";
        }
        if (spec.olderThanDays && *spec.olderThanDays > 0) {
            count << " AND last_write_ticks > 0 AND last_write_ticks <= ?";
        }
        if (!spec.classification.empty()) {
            count << " AND classification = ?";
        }
        if (!spec.candidateStrength.empty()) {
            count << " AND candidate_strength = ?";
        }

        auto bindFilters = [&](SqliteStmt& stmt, int& idx) {
            if (spec.minSize) {
                stmt.bindInt64(idx++, static_cast<std::int64_t>(*spec.minSize));
            }
            if (!spec.extension.empty()) {
                stmt.bindText(idx++, spec.extension);
            }
            if (spec.olderThanDays && *spec.olderThanDays > 0) {
                const FileTimeTicks cutoff =
                    now > daysToTicks(*spec.olderThanDays)
                        ? now - daysToTicks(*spec.olderThanDays)
                        : 0;
                stmt.bindInt64(idx++, static_cast<std::int64_t>(cutoff));
            }
            if (!spec.classification.empty()) {
                stmt.bindText(idx++, spec.classification);
            }
            if (!spec.candidateStrength.empty()) {
                stmt.bindText(idx++, spec.candidateStrength);
            }
        };

        {
            SqliteStmt cstmt(store.db(), count.str());
            int idx = 1;
            bindFilters(cstmt, idx);
            if (cstmt.step()) {
                r.matched_items =
                    static_cast<std::uint64_t>(cstmt.columnInt64(0));
                r.matched_logical_bytes =
                    static_cast<ByteSize>(cstmt.columnInt64(1));
            }
        }

        SqliteStmt stmt(store.db(), sql.str());
        int idx = 1;
        bindFilters(stmt, idx);
        stmt.bindInt64(idx++, static_cast<std::int64_t>(spec.limit));

        while (stmt.step()) {
            IndexHit hit;
            hit.path = stmt.columnText16(0);
            hit.kind = stmt.columnInt64(1) == 1 ? IndexEntryKind::Directory
                                                : IndexEntryKind::File;
            hit.size_bytes = static_cast<ByteSize>(stmt.columnInt64(2));
            hit.classification = stmt.columnText(3);
            hit.confidence = stmt.columnText(4);
            hit.location_safety = stmt.columnText(5);
            hit.reclaimability = stmt.columnText(6);
            hit.candidate_strength = stmt.columnText(7);
            hit.last_write_ticks =
                static_cast<std::uint64_t>(stmt.columnInt64(8));
            r.hits.push_back(std::move(hit));
        }
        r.returned_items = r.hits.size();
        r.ok = true;
    } catch (const SqliteError& ex) {
        r.ok = false;
        r.error = ex.what();
        if (r.error.find("not found") != std::string::npos) {
            r.error = "index_not_found";
        } else if (r.error.find("unsupported") != std::string::npos) {
            r.error = "unsupported_schema";
        } else {
            r.error = "index_query_failed";
        }
    } catch (...) {
        r.ok = false;
        r.error = "index_query_failed";
    }
    return r;
}

}  // namespace spacelens
