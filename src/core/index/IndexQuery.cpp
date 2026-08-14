#include "core/index/IndexQuery.hpp"

#include "core/FileTime.hpp"
#include "sqlite3.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <chrono>
#include <limits>
#include <sstream>
#include <string_view>

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

/// Escape LIKE wildcards so user search text is literal substring match.
std::string escapeLike(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '%' || ch == '_' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

/// Same escaping for UTF-16 paths. Required because Windows paths contain `\`
/// and pathPrefix uses `LIKE ? ESCAPE '\'`.
std::wstring escapeLikeWide(std::wstring_view text)
{
    std::wstring out;
    out.reserve(text.size() * 2);
    for (wchar_t ch : text) {
        if (ch == L'%' || ch == L'_' || ch == L'\\') {
            out.push_back(L'\\');
        }
        out.push_back(ch);
    }
    return out;
}

/// Exact bind keeps a drive-root trailing slash (`D:\`). LIKE is built from
/// the slash-stripped base (`D:`) so ESCAPE '\' yields `D:\\%` and matches
/// `D:\Users\...`. Binding the unstripped `D:\` would LIKE-escape into
/// `D:\\\\%` and match nothing.
void bindPathPrefix(SqliteStmt& stmt, int& idx, std::wstring prefix)
{
    for (wchar_t& ch : prefix) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    while (prefix.size() > 3 &&
           (prefix.back() == L'\\' || prefix.back() == L'/')) {
        prefix.pop_back();
    }
    stmt.bindText16(idx++, prefix);
    std::wstring likeBase = prefix;
    while (!likeBase.empty() &&
           (likeBase.back() == L'\\' || likeBase.back() == L'/')) {
        likeBase.pop_back();
    }
    std::wstring likePat = escapeLikeWide(likeBase);
    likePat.push_back(L'\\');
    likePat.push_back(L'\\');
    likePat.push_back(L'%');
    stmt.bindText16(idx++, likePat);
}

void appendInList(std::ostringstream& sql, const char* column,
                  const std::vector<std::string>& values)
{
    sql << " AND " << column << " IN (";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            sql << ',';
        }
        sql << '?';
    }
    sql << ')';
}

void appendOrderBy(std::ostringstream& sql, IndexSortKey sortBy, bool descending)
{
    const char* dir = descending ? "DESC" : "ASC";
    switch (sortBy) {
    case IndexSortKey::Name:
        sql << " ORDER BY name COLLATE NOCASE " << dir << ", path ASC";
        break;
    case IndexSortKey::LastWrite:
        sql << " ORDER BY last_write_ticks " << dir << ", path ASC";
        break;
    case IndexSortKey::Classification:
        sql << " ORDER BY classification COLLATE NOCASE " << dir
            << ", sz DESC, path ASC";
        break;
    case IndexSortKey::CandidateStrength:
        // Strong first when descending; reverse when ascending.
        sql << " ORDER BY CASE candidate_strength "
               "WHEN 'Strong' THEN 0 WHEN 'Moderate' THEN 1 "
               "WHEN 'ReviewOnly' THEN 2 ELSE 3 END "
            << (descending ? "ASC" : "DESC") << ", sz DESC, path ASC";
        break;
    case IndexSortKey::OpportunityRank:
        // Must match StorageIntelligence sortOpportunities / opportunity_rank_v2.
        sql << " ORDER BY CASE candidate_strength "
               "WHEN 'Strong' THEN 3 WHEN 'Moderate' THEN 2 "
               "WHEN 'ReviewOnly' THEN 1 ELSE 0 END DESC, "
               "CASE confidence WHEN 'High' THEN 3 WHEN 'Medium' THEN 2 "
               "WHEN 'Low' THEN 1 ELSE 0 END DESC, "
               "sz DESC, path COLLATE NOCASE ASC";
        break;
    case IndexSortKey::Size:
    default:
        sql << " ORDER BY sz " << dir << ", path ASC";
        break;
    }
}

void appendCommonFilters(std::ostringstream& sql, const IndexQuerySpec& spec)
{
    if (spec.includeFiles && !spec.includeDirectories) {
        sql << " AND kind = 0";
    } else if (!spec.includeFiles && spec.includeDirectories) {
        sql << " AND kind = 1";
    }

    if (spec.minSize) {
        sql << " AND (CASE WHEN kind = 1 THEN recursive_size ELSE size_bytes "
               "END) >= ?";
    }
    if (!spec.extension.empty()) {
        sql << " AND kind = 0 AND extension = ?";
    }
    if (spec.olderThanDays && *spec.olderThanDays > 0) {
        // Activity: files use last_write_ticks; dirs store newest descendant
        // write in last_write_ticks at index build time.
        sql << " AND last_write_ticks > 0 AND last_write_ticks <= ?";
    }
    if (!spec.classifications.empty()) {
        appendInList(sql, "classification", spec.classifications);
    } else if (!spec.classification.empty()) {
        sql << " AND classification = ?";
    }
    if (!spec.candidateStrengths.empty()) {
        appendInList(sql, "candidate_strength", spec.candidateStrengths);
    } else if (!spec.candidateStrength.empty()) {
        sql << " AND candidate_strength = ?";
    }
    if (!spec.reclaimability.empty()) {
        sql << " AND reclaimability = ?";
    }
    if (!spec.searchText.empty()) {
        // Case-insensitive substring on name / path / extension (ASCII NOCASE;
        // non-ASCII letters follow SQLite default Unicode folding limits).
        sql << " AND (name LIKE ? ESCAPE '\\' COLLATE NOCASE"
               " OR path LIKE ? ESCAPE '\\' COLLATE NOCASE"
               " OR extension LIKE ? ESCAPE '\\' COLLATE NOCASE)";
    }
    if (!spec.browsePath.empty()) {
        sql << " AND parent_id = (SELECT id FROM entries WHERE root_id = 1 AND "
               "path = ? LIMIT 1)";
    } else if (!spec.pathPrefix.empty()) {
        sql << " AND (path = ? COLLATE NOCASE OR path LIKE ? ESCAPE '\\' "
               "COLLATE NOCASE)";
    }
}

void bindFilters(SqliteStmt& stmt, int& idx, const IndexQuerySpec& spec,
                 FileTimeTicks now)
{
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
    if (!spec.classifications.empty()) {
        for (const auto& c : spec.classifications) {
            stmt.bindText(idx++, c);
        }
    } else if (!spec.classification.empty()) {
        stmt.bindText(idx++, spec.classification);
    }
    if (!spec.candidateStrengths.empty()) {
        for (const auto& s : spec.candidateStrengths) {
            stmt.bindText(idx++, s);
        }
    } else if (!spec.candidateStrength.empty()) {
        stmt.bindText(idx++, spec.candidateStrength);
    }
    if (!spec.reclaimability.empty()) {
        stmt.bindText(idx++, spec.reclaimability);
    }
    if (!spec.searchText.empty()) {
        const std::string pattern =
            std::string("%") + escapeLike(spec.searchText) + "%";
        stmt.bindText(idx++, pattern);
        stmt.bindText(idx++, pattern);
        stmt.bindText(idx++, pattern);
    }
    if (!spec.browsePath.empty()) {
        stmt.bindText16(idx++, spec.browsePath);
    } else if (!spec.pathPrefix.empty()) {
        bindPathPrefix(stmt, idx, spec.pathPrefix);
    }
}

constexpr const char* kHitSelect =
    "SELECT path, name, kind, "
    "CASE WHEN kind = 1 THEN recursive_size ELSE size_bytes END AS sz, "
    "extension, classification, confidence, rule_id, location_safety, "
    "reclaimability, candidate_strength, last_write_ticks ";

IndexHit readHit(SqliteStmt& stmt)
{
    IndexHit hit;
    hit.path = stmt.columnText16(0);
    hit.name = stmt.columnText16(1);
    hit.kind = stmt.columnInt64(2) == 1 ? IndexEntryKind::Directory
                                        : IndexEntryKind::File;
    hit.size_bytes = static_cast<ByteSize>(stmt.columnInt64(3));
    hit.extension = stmt.columnText(4);
    hit.classification = stmt.columnText(5);
    hit.confidence = stmt.columnText(6);
    hit.rule_id = stmt.columnText(7);
    hit.location_safety = stmt.columnText(8);
    hit.reclaimability = stmt.columnText(9);
    hit.candidate_strength = stmt.columnText(10);
    hit.last_write_ticks = static_cast<std::uint64_t>(stmt.columnInt64(11));
    return hit;
}

void appendOpportunityInclusion(std::ostringstream& sql, bool includeOldLarge)
{
    sql << " AND candidate_strength <> '' AND candidate_strength <> 'None'"
           " AND location_safety <> 'Protected'"
           " AND (CASE WHEN kind = 1 THEN recursive_size ELSE size_bytes "
           "END) >= ?";
    sql << " AND ("
           "(reclaimability IN ('LikelyRegenerable','PossiblyRegenerable')"
           " AND confidence IN ('High','Medium'))";
    if (includeOldLarge) {
        sql << " OR (kind = 0 AND last_write_ticks > 0 AND last_write_ticks <= ?)";
    }
    sql << ")";
}

void appendOpportunityExtras(std::ostringstream& sql,
                             const IndexedOpportunitySpec& spec)
{
    if (spec.classification) {
        sql << " AND classification = ?";
    }
    if (!spec.pathPrefix.empty()) {
        sql << " AND (path = ? COLLATE NOCASE OR path LIKE ? ESCAPE '\\' "
               "COLLATE NOCASE)";
    }
    if (!spec.excludePath.empty()) {
        sql << " AND path <> ? COLLATE NOCASE";
    }
}

void bindOpportunityFilters(SqliteStmt& stmt, int& idx,
                            const IndexedOpportunitySpec& spec, FileTimeTicks now)
{
    stmt.bindInt64(idx++, static_cast<std::int64_t>(spec.minSize));
    if (spec.olderThanDays > 0) {
        const FileTimeTicks cutoff =
            now > daysToTicks(spec.olderThanDays) ? now - daysToTicks(spec.olderThanDays)
                                                  : 0;
        stmt.bindInt64(idx++, static_cast<std::int64_t>(cutoff));
    }
    if (spec.classification) {
        stmt.bindText(idx++, *spec.classification);
    }
    if (!spec.pathPrefix.empty()) {
        bindPathPrefix(stmt, idx, spec.pathPrefix);
    }
    if (!spec.excludePath.empty()) {
        std::wstring exclude = spec.excludePath;
        for (wchar_t& ch : exclude) {
            if (ch == L'/') {
                ch = L'\\';
            }
        }
        while (exclude.size() > 3 &&
               (exclude.back() == L'\\' || exclude.back() == L'/')) {
            exclude.pop_back();
        }
        stmt.bindText16(idx++, exclude);
    }
}

struct ProgressCancel {
    std::stop_token stop;
};

int sqliteProgressCancel(void* ctx)
{
    const auto* cancel = static_cast<const ProgressCancel*>(ctx);
    return (cancel != nullptr && cancel->stop.stop_requested()) ? 1 : 0;
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
    const auto t0 = std::chrono::steady_clock::now();
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

        if (!spec.includeFiles && !spec.includeDirectories) {
            r.ok = true;
            r.query_elapsed_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            return r;
        }

        std::ostringstream sql;
        sql << "SELECT path, name, kind, "
            << "CASE WHEN kind = 1 THEN recursive_size ELSE size_bytes END AS sz, "
            << "extension, classification, confidence, rule_id, location_safety, "
            << "reclaimability, candidate_strength, last_write_ticks "
            << "FROM entries WHERE root_id = 1";
        appendCommonFilters(sql, spec);
        appendOrderBy(sql, spec.sortBy, spec.sortDescending);
        sql << " LIMIT ?";

        std::ostringstream count;
        count << "SELECT COUNT(*), COALESCE(SUM(CASE WHEN kind = 1 THEN "
                 "recursive_size ELSE size_bytes END),0) FROM entries WHERE "
                 "root_id = 1";
        appendCommonFilters(count, spec);

        {
            SqliteStmt cstmt(store.db(), count.str());
            int idx = 1;
            bindFilters(cstmt, idx, spec, now);
            if (cstmt.step()) {
                r.matched_items =
                    static_cast<std::uint64_t>(cstmt.columnInt64(0));
                r.matched_logical_bytes =
                    static_cast<ByteSize>(cstmt.columnInt64(1));
            }
        }

        SqliteStmt stmt(store.db(), sql.str());
        int idx = 1;
        bindFilters(stmt, idx, spec, now);
        stmt.bindInt64(idx++, static_cast<std::int64_t>(spec.limit));

        while (stmt.step()) {
            IndexHit hit;
            hit.path = stmt.columnText16(0);
            hit.name = stmt.columnText16(1);
            hit.kind = stmt.columnInt64(2) == 1 ? IndexEntryKind::Directory
                                                : IndexEntryKind::File;
            hit.size_bytes = static_cast<ByteSize>(stmt.columnInt64(3));
            hit.extension = stmt.columnText(4);
            hit.classification = stmt.columnText(5);
            hit.confidence = stmt.columnText(6);
            hit.rule_id = stmt.columnText(7);
            hit.location_safety = stmt.columnText(8);
            hit.reclaimability = stmt.columnText(9);
            hit.candidate_strength = stmt.columnText(10);
            hit.last_write_ticks =
                static_cast<std::uint64_t>(stmt.columnInt64(11));
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

    r.query_elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
    return r;
}

IndexedOpportunityFetch queryIndexedOpportunities(
    const std::wstring& rootPath, const IndexedOpportunitySpec& spec,
    std::stop_token stop)
{
    const auto t0 = std::chrono::steady_clock::now();
    IndexedOpportunityFetch r;
    r.location = locateIndex(rootPath);
    r.root.rootPath = r.location.rootPath;
    auto finish = [&]() {
        r.query_elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        return r;
    };

    if (spec.matchNone) {
        r.ok = true;
        return finish();
    }
    if (!indexDatabaseExists(r.location)) {
        r.error = "index_not_found";
        return finish();
    }
    if (stop.stop_requested()) {
        r.error = "cancelled";
        return finish();
    }

    try {
        auto store = IndexStore::openRead(r.location);
        auto meta = store.readRootMeta();
        if (!meta || meta->status != IndexStatus::Ready) {
            r.error = meta ? "index_not_ready" : "index_corrupt";
            return finish();
        }
        r.root = *meta;
        const FileTimeTicks now =
            spec.nowTicks != 0 ? spec.nowTicks : nowFileTime();
        r.age_ms = ageMs(r.root.indexedAtTicks, now);

        ProgressCancel cancel{stop};
        sqlite3_progress_handler(store.db().handle(), 1000, sqliteProgressCancel,
                                 &cancel);

        const bool includeOldLarge = spec.olderThanDays > 0;
        const std::size_t publicLimit =
            spec.limit == 0 ? 20 : spec.limit;
        const std::size_t aggregateLimit =
            spec.aggregateLimit == 0 ? 50000 : spec.aggregateLimit;

        std::ostringstream where;
        where << "FROM entries WHERE root_id = 1";
        appendOpportunityInclusion(where, includeOldLarge);
        appendOpportunityExtras(where, spec);

        {
            std::ostringstream countSql;
            countSql << "SELECT COUNT(*) " << where.str();
            SqliteStmt count(store.db(), countSql.str());
            int idx = 1;
            bindOpportunityFilters(count, idx, spec, now);
            if (count.step()) {
                r.matchedItems = static_cast<std::uint64_t>(count.columnInt64(0));
            }
        }
        if (stop.stop_requested()) {
            sqlite3_progress_handler(store.db().handle(), 0, nullptr, nullptr);
            r.error = "cancelled";
            return finish();
        }

        {
            std::ostringstream topSql;
            topSql << kHitSelect << where.str();
            appendOrderBy(topSql, IndexSortKey::OpportunityRank, true);
            topSql << " LIMIT ?";
            SqliteStmt top(store.db(), topSql.str());
            int idx = 1;
            bindOpportunityFilters(top, idx, spec, now);
            top.bindInt64(idx++, static_cast<std::int64_t>(publicLimit + 1));
            while (top.step()) {
                r.topHits.push_back(readHit(top));
            }
            r.sqlTopRows = r.topHits.size();
        }
        if (stop.stop_requested()) {
            sqlite3_progress_handler(store.db().handle(), 0, nullptr, nullptr);
            r.error = "cancelled";
            return finish();
        }

        if (r.matchedItems > 0 && r.matchedItems <= aggregateLimit) {
            std::ostringstream aggSql;
            aggSql << kHitSelect << where.str();
            appendOrderBy(aggSql, IndexSortKey::OpportunityRank, true);
            SqliteStmt agg(store.db(), aggSql.str());
            int idx = 1;
            bindOpportunityFilters(agg, idx, spec, now);
            while (agg.step()) {
                if (stop.stop_requested()) {
                    break;
                }
                r.aggregateHits.push_back(readHit(agg));
            }
            r.sqlAggregateRows = r.aggregateHits.size();
            r.aggregatesCapped = r.aggregateHits.size() < r.matchedItems;
        } else if (r.matchedItems > aggregateLimit) {
            r.aggregatesCapped = true;
            r.aggregateHits = r.topHits;
            r.sqlAggregateRows = r.aggregateHits.size();
        }

        sqlite3_progress_handler(store.db().handle(), 0, nullptr, nullptr);
        if (stop.stop_requested()) {
            r.error = "cancelled";
            return finish();
        }
        r.ok = true;
    } catch (const SqliteError& ex) {
        r.ok = false;
        r.error = ex.what();
        if (stop.stop_requested() ||
            r.error.find("interrupt") != std::string::npos ||
            r.error.find("cancelled") != std::string::npos) {
            r.error = "cancelled";
        } else if (r.error.find("not found") != std::string::npos) {
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
    return finish();
}

DuplicateCandidateQueryResult queryDuplicateSizeCandidates(
    IndexStore& store,
    ByteSize minimumSize)
{
    DuplicateCandidateQueryResult result;
    result.minimumSize = minimumSize;
    result.location = store.location();
    try {
        if (auto meta = store.readRootMeta()) {
            result.root = *meta;
        }
        result.root.rootPath = result.location.rootPath.empty()
                                   ? result.root.rootPath
                                   : result.location.rootPath;
        result.ageMs = ageMs(result.root.indexedAtTicks, nowFileTime());

        const char* sql =
            "SELECT path, name, size_bytes, last_write_ticks, attributes, "
            "is_reparse FROM entries WHERE root_id = 1 AND kind = 0 AND "
            "size_bytes > 0 AND size_bytes >= ?1 AND is_reparse = 0 AND "
            "size_bytes IN (SELECT size_bytes FROM entries WHERE root_id = 1 "
            "AND kind = 0 AND size_bytes > 0 AND size_bytes >= ?1 AND "
            "is_reparse = 0 GROUP BY size_bytes HAVING COUNT(*) >= 2) "
            "ORDER BY size_bytes DESC, path COLLATE NOCASE ASC, path ASC";

        SqliteStmt stmt(store.db(), sql);
        stmt.bindInt64(1, static_cast<std::int64_t>(minimumSize));

        DuplicateSizeBucket* current = nullptr;
        bool saturated = false;
        while (stmt.step()) {
            DuplicateIndexCandidate file;
            file.path = stmt.columnText16(0);
            file.name = stmt.columnText16(1);
            file.logicalSize = static_cast<ByteSize>(stmt.columnInt64(2));
            file.lastWriteTicks = static_cast<FileTimeTicks>(stmt.columnInt64(3));
            file.attributes = static_cast<std::uint32_t>(stmt.columnInt64(4));
            file.indexedAsReparse = stmt.columnInt64(5) != 0;
            if (current == nullptr || current->logicalSize != file.logicalSize) {
                result.buckets.push_back({});
                current = &result.buckets.back();
                current->logicalSize = file.logicalSize;
            }
            current->files.push_back(std::move(file));
            ++result.candidateFiles;
            const ByteSize maxBytes = std::numeric_limits<ByteSize>::max();
            if (maxBytes - result.candidateBytes < current->logicalSize) {
                result.candidateBytes = maxBytes;
                saturated = true;
            } else if (!saturated) {
                result.candidateBytes += current->logicalSize;
            }
        }
        result.ok = true;
    } catch (const SqliteError& ex) {
        result.ok = false;
        result.error = ex.what();
        if (result.error.find("not found") != std::string::npos) {
            result.error = "index_not_found";
        } else if (result.error.find("unsupported") != std::string::npos) {
            result.error = "unsupported_schema";
        } else {
            result.error = "index_query_failed";
        }
    } catch (...) {
        result.ok = false;
        result.error = "index_query_failed";
    }
    return result;
}

DuplicateCandidateQueryResult queryDuplicateSizeCandidates(
    const std::wstring& rootPath,
    ByteSize minimumSize)
{
    DuplicateCandidateQueryResult result;
    result.minimumSize = minimumSize;
    result.location = locateIndex(rootPath);
    result.root.rootPath = result.location.rootPath;
    if (!indexDatabaseExists(result.location)) {
        result.error = "index_not_found";
        return result;
    }
    try {
        auto store = IndexStore::openRead(result.location);
        auto meta = store.readRootMeta();
        if (!meta || meta->status != IndexStatus::Ready) {
            result.error = meta ? "index_not_ready" : "index_corrupt";
            result.root = meta.value_or(result.root);
            return result;
        }
        result = queryDuplicateSizeCandidates(store, minimumSize);
        result.minimumSize = minimumSize;
        result.root = *meta;
        result.location = locateIndex(rootPath);
        result.ageMs = ageMs(result.root.indexedAtTicks, nowFileTime());
    } catch (const SqliteError& ex) {
        result.ok = false;
        result.error = ex.what();
        if (result.error.find("not found") != std::string::npos) {
            result.error = "index_not_found";
        } else if (result.error.find("unsupported") != std::string::npos) {
            result.error = "unsupported_schema";
        } else {
            result.error = "index_open_failed";
        }
    } catch (...) {
        result.ok = false;
        result.error = "index_open_failed";
    }
    return result;
}

}  // namespace spacelens
