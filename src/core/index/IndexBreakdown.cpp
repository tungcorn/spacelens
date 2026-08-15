#include "core/index/IndexBreakdown.hpp"

#include "core/index/IndexPathPrefix.hpp"
#include "core/index/IndexRefresh.hpp"
#include "core/index/IndexSnapshot.hpp"
#include "sqlite3.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

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

IndexPublishMetadata publishMetadataFrom(const IndexRootInfo& root, SqliteDb& db)
{
    IndexPublishMetadata meta;
    meta.rootIndexedAtTicks = root.indexedAtTicks;
    meta.rootIndexedAtIso = root.indexedAtIso;
    if (auto cp = readRefreshCheckpoint(db)) {
        meta.lastRefreshAtTicks = cp->lastRefreshAtTicks;
        meta.lastRefreshMethod = cp->lastRefreshMethod;
        meta.fullIndexedAtTicks = cp->fullIndexedAtTicks;
    }
    return meta;
}

void attachSnapshot(IndexedBreakdown& r, SqliteDb& db, FileTimeTicks now,
                    const std::optional<std::uint64_t>& maxAge)
{
    r.snapshot = evaluateIndexSnapshot(publishMetadataFrom(r.root, db), now);
    r.ageDecision = evaluateIndexAgeGate(r.snapshot, maxAge);
}

bool applyAgeGate(IndexedBreakdown& r)
{
    if (r.ageDecision.result == IndexAgeGateResult::TooOld) {
        r.ok = false;
        r.error = "index_too_old";
        return false;
    }
    if (r.ageDecision.result == IndexAgeGateResult::FreshnessUnknown) {
        r.ok = false;
        r.error = "index_freshness_unknown";
        return false;
    }
    return true;
}

struct ProgressCancel {
    std::stop_token stop;
};

int sqliteProgressCancel(void* ctx)
{
    const auto* cancel = static_cast<const ProgressCancel*>(ctx);
    return (cancel != nullptr && cancel->stop.stop_requested()) ? 1 : 0;
}

struct DeferredReadTxn {
    explicit DeferredReadTxn(SqliteDb& db)
        : db_(&db)
    {
        db_->exec("BEGIN DEFERRED;");
    }
    ~DeferredReadTxn()
    {
        if (!done_ && db_ != nullptr && db_->isOpen()) {
            try {
                db_->exec("ROLLBACK;");
            } catch (...) {
            }
        }
    }
    void commit()
    {
        if (done_ || db_ == nullptr) {
            return;
        }
        db_->exec("COMMIT;");
        done_ = true;
    }

    SqliteDb* db_ = nullptr;
    bool done_ = false;
};

bool addChecked(ByteSize& acc, ByteSize delta, bool& overflow)
{
    if (delta > std::numeric_limits<ByteSize>::max() - acc) {
        acc = std::numeric_limits<ByteSize>::max();
        overflow = true;
        return false;
    }
    acc += delta;
    return true;
}

bool addCheckedCount(std::uint64_t& acc, std::uint64_t delta, bool& overflow)
{
    if (delta > std::numeric_limits<std::uint64_t>::max() - acc) {
        acc = std::numeric_limits<std::uint64_t>::max();
        overflow = true;
        return false;
    }
    acc += delta;
    return true;
}

/// Read a non-negative aggregate. Integer SUM that no longer fits int64 is
/// either reported as REAL or rejected with "integer overflow" (this
/// amalgamation). Never wrap. REAL values at or above 2^63 are exact powers
/// of two in IEEE-754 and still fit uint64 — keep the value and mark estimated.
/// `overflow` means this value itself could not be represented; it must not
/// poison an independent accumulator such as extension `other`.
ByteSize readSaturatingSum(const SqliteStmt& stmt, int index, bool& overflow,
                           bool& estimated)
{
    const int type = stmt.columnType(index);
    if (type == SQLITE_NULL) {
        return 0;
    }
    if (type == SQLITE_FLOAT) {
        estimated = true;
        const double value = stmt.columnDouble(index);
        if (!std::isfinite(value) || value < 0.0) {
            overflow = true;
            return std::numeric_limits<ByteSize>::max();
        }
        const double maxU64 =
            static_cast<double>(std::numeric_limits<ByteSize>::max());
        if (value > maxU64) {
            overflow = true;
            return std::numeric_limits<ByteSize>::max();
        }
        return static_cast<ByteSize>(value);
    }
    const std::int64_t raw = stmt.columnInt64(index);
    if (raw < 0) {
        overflow = true;
        return std::numeric_limits<ByteSize>::max();
    }
    return static_cast<ByteSize>(raw);
}

bool isIntegerOverflow(const SqliteError& ex)
{
    const std::string message = ex.what();
    return message.find("integer overflow") != std::string::npos;
}

std::uint64_t readCount(const SqliteStmt& stmt, int index, bool& overflow)
{
    const std::int64_t raw = stmt.columnInt64(index);
    if (raw < 0) {
        overflow = true;
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(raw);
}

void appendFileScope(std::ostringstream& sql, bool hasPrefix)
{
    sql << " FROM entries WHERE root_id = 1 AND kind = 0";
    if (hasPrefix) {
        sql << kIndexedPathPrefixSql;
    }
}

void bindOptionalPrefix(SqliteStmt& stmt, int& idx, const std::wstring& prefix)
{
    if (!prefix.empty()) {
        bindIndexedPathPrefix(stmt, idx, prefix);
    }
}

constexpr std::array<WriteAgeBucket, 6> kAgeOrder{
    WriteAgeBucket::Lt30d,   WriteAgeBucket::D30_90, WriteAgeBucket::D90_365,
    WriteAgeBucket::Ge365d,  WriteAgeBucket::Unknown, WriteAgeBucket::Future};

WriteAgeBucket bucketFromKey(std::string_view key) noexcept
{
    if (key == "lt_30d") {
        return WriteAgeBucket::Lt30d;
    }
    if (key == "d30_90") {
        return WriteAgeBucket::D30_90;
    }
    if (key == "d90_365") {
        return WriteAgeBucket::D90_365;
    }
    if (key == "ge_365d") {
        return WriteAgeBucket::Ge365d;
    }
    if (key == "future") {
        return WriteAgeBucket::Future;
    }
    return WriteAgeBucket::Unknown;
}

}  // namespace

const char* toString(WriteAgeBucket bucket) noexcept
{
    switch (bucket) {
    case WriteAgeBucket::Lt30d:
        return "lt_30d";
    case WriteAgeBucket::D30_90:
        return "d30_90";
    case WriteAgeBucket::D90_365:
        return "d90_365";
    case WriteAgeBucket::Ge365d:
        return "ge_365d";
    case WriteAgeBucket::Unknown:
        return "unknown";
    case WriteAgeBucket::Future:
        return "future";
    }
    return "unknown";
}

const char* writeAgeBucketHuman(WriteAgeBucket bucket) noexcept
{
    switch (bucket) {
    case WriteAgeBucket::Lt30d:
        return "<30d";
    case WriteAgeBucket::D30_90:
        return "30-90d";
    case WriteAgeBucket::D90_365:
        return "90-365d";
    case WriteAgeBucket::Ge365d:
        return ">=365d";
    case WriteAgeBucket::Unknown:
        return "unknown";
    case WriteAgeBucket::Future:
        return "future";
    }
    return "unknown";
}

WriteAgeBucket classifyWriteAgeBucket(FileTimeTicks then,
                                      FileTimeTicks now) noexcept
{
    if (then == 0) {
        return WriteAgeBucket::Unknown;
    }
    if (now == 0 || then > now) {
        return WriteAgeBucket::Future;
    }
    const std::uint64_t age = ageDays(then, now);
    if (age < 30) {
        return WriteAgeBucket::Lt30d;
    }
    if (age < 90) {
        return WriteAgeBucket::D30_90;
    }
    if (age < 365) {
        return WriteAgeBucket::D90_365;
    }
    return WriteAgeBucket::Ge365d;
}

IndexedBreakdown queryIndexedBreakdown(const std::wstring& rootPath,
                                       const IndexedBreakdownSpec& spec,
                                       std::stop_token stop)
{
    const auto t0 = std::chrono::steady_clock::now();
    IndexedBreakdown r;
    r.location = locateIndex(rootPath);
    r.root.rootPath = r.location.rootPath;
    r.under = spec.pathPrefix;
    r.limit = spec.limit == 0 ? kDefaultBreakdownLimit : spec.limit;
    if (r.limit > kMaxBreakdownLimit) {
        r.limit = kMaxBreakdownLimit;
    }
    auto finish = [&]() {
        r.query_elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        return r;
    };

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

        ProgressCancel cancel{stop};
        sqlite3_progress_handler(store.db().handle(), 1000, sqliteProgressCancel,
                                 &cancel);
        DeferredReadTxn txn(store.db());
        attachSnapshot(r, store.db(), now, spec.maxIndexAgeSeconds);
        if (!applyAgeGate(r)) {
            sqlite3_progress_handler(store.db().handle(), 0, nullptr, nullptr);
            txn.commit();
            return finish();
        }
        if (stop.stop_requested()) {
            sqlite3_progress_handler(store.db().handle(), 0, nullptr, nullptr);
            r.error = "cancelled";
            return finish();
        }

        const bool hasPrefix = !spec.pathPrefix.empty();
        bool overflow = false;
        bool estimated = false;
        const FileTimeTicks cut30 =
            now > daysToTicks(30) ? now - daysToTicks(30) : 0;
        const FileTimeTicks cut90 =
            now > daysToTicks(90) ? now - daysToTicks(90) : 0;
        const FileTimeTicks cut365 =
            now > daysToTicks(365) ? now - daysToTicks(365) : 0;

        auto runAggregates = [&](bool realSum) {
            r.by_classification.clear();
            r.by_extension.clear();
            r.extension_other = {};
            r.by_last_write_age.clear();
            r.total_file_count = 0;
            r.total_logical_bytes = 0;
            const char* sumExpr =
                realSum ? "COALESCE(SUM(CAST(size_bytes AS REAL)), 0)"
                        : "COALESCE(SUM(size_bytes), 0)";
            const char* orderSum =
                realSum ? "SUM(CAST(size_bytes AS REAL))" : "SUM(size_bytes)";

            {
                std::ostringstream sql;
                sql << "SELECT COUNT(*), " << sumExpr;
                appendFileScope(sql, hasPrefix);
                SqliteStmt stmt(store.db(), sql.str());
                int idx = 1;
                bindOptionalPrefix(stmt, idx, spec.pathPrefix);
                if (stmt.step()) {
                    r.total_file_count = readCount(stmt, 0, overflow);
                    r.total_logical_bytes =
                        readSaturatingSum(stmt, 1, overflow, estimated);
                }
            }

            {
                std::ostringstream sql;
                sql << "SELECT classification, COUNT(*), " << sumExpr;
                appendFileScope(sql, hasPrefix);
                sql << " GROUP BY classification ORDER BY " << orderSum
                    << " DESC, classification ASC";
                SqliteStmt stmt(store.db(), sql.str());
                int idx = 1;
                bindOptionalPrefix(stmt, idx, spec.pathPrefix);
                while (stmt.step()) {
                    BreakdownGroup g;
                    g.key = stmt.columnText(0);
                    g.file_count = readCount(stmt, 1, overflow);
                    g.logical_bytes =
                        readSaturatingSum(stmt, 2, overflow, estimated);
                    r.by_classification.push_back(std::move(g));
                }
            }

            {
                std::ostringstream sql;
                sql << "SELECT extension, COUNT(*), " << sumExpr;
                appendFileScope(sql, hasPrefix);
                sql << " GROUP BY extension ORDER BY " << orderSum
                    << " DESC, extension ASC";
                SqliteStmt stmt(store.db(), sql.str());
                int idx = 1;
                bindOptionalPrefix(stmt, idx, spec.pathPrefix);
                std::size_t seen = 0;
                while (stmt.step()) {
                    const std::uint64_t count = readCount(stmt, 1, overflow);
                    const ByteSize bytes =
                        readSaturatingSum(stmt, 2, overflow, estimated);
                    if (seen < r.limit) {
                        BreakdownGroup g;
                        g.key = stmt.columnText(0);
                        g.file_count = count;
                        g.logical_bytes = bytes;
                        r.by_extension.push_back(std::move(g));
                        ++seen;
                    } else {
                        ++r.extension_other.extension_groups;
                        addCheckedCount(r.extension_other.file_count, count,
                                        overflow);
                        addChecked(r.extension_other.logical_bytes, bytes,
                                   overflow);
                    }
                }
            }

            std::array<BreakdownGroup, 6> age{};
            for (std::size_t i = 0; i < age.size(); ++i) {
                age[i].key = toString(kAgeOrder[i]);
            }
            {
                std::ostringstream sql;
                sql << "SELECT CASE"
                       " WHEN last_write_ticks IS NULL OR last_write_ticks = 0 "
                       "THEN 'unknown'"
                       " WHEN last_write_ticks > ? THEN 'future'"
                       " WHEN last_write_ticks > ? THEN 'lt_30d'"
                       " WHEN last_write_ticks > ? THEN 'd30_90'"
                       " WHEN last_write_ticks > ? THEN 'd90_365'"
                       " ELSE 'ge_365d' END AS bucket,"
                       " COUNT(*), "
                    << sumExpr;
                appendFileScope(sql, hasPrefix);
                sql << " GROUP BY bucket";
                SqliteStmt stmt(store.db(), sql.str());
                int idx = 1;
                stmt.bindInt64(idx++, static_cast<std::int64_t>(now));
                stmt.bindInt64(idx++, static_cast<std::int64_t>(cut30));
                stmt.bindInt64(idx++, static_cast<std::int64_t>(cut90));
                stmt.bindInt64(idx++, static_cast<std::int64_t>(cut365));
                bindOptionalPrefix(stmt, idx, spec.pathPrefix);
                while (stmt.step()) {
                    const WriteAgeBucket bucket = bucketFromKey(stmt.columnText(0));
                    for (std::size_t i = 0; i < kAgeOrder.size(); ++i) {
                        if (kAgeOrder[i] == bucket) {
                            age[i].file_count = readCount(stmt, 1, overflow);
                            age[i].logical_bytes = readSaturatingSum(
                                stmt, 2, overflow, estimated);
                            break;
                        }
                    }
                }
            }
            r.by_last_write_age.assign(age.begin(), age.end());
        };

        try {
            runAggregates(false);
        } catch (const SqliteError& ex) {
            if (!isIntegerOverflow(ex)) {
                throw;
            }
            estimated = true;
            overflow = false;
            runAggregates(true);
        }
        r.logical_bytes_estimated = estimated || overflow;
        r.ok = true;
        sqlite3_progress_handler(store.db().handle(), 0, nullptr, nullptr);
        txn.commit();
    } catch (const SqliteError& ex) {
        r.ok = false;
        r.error = ex.what();
        if (r.error.find("not found") != std::string::npos) {
            r.error = "index_not_found";
        } else if (r.error.find("unsupported") != std::string::npos) {
            r.error = "unsupported_schema";
        } else if (r.error.find("interrupt") != std::string::npos ||
                   r.error.find("cancelled") != std::string::npos) {
            r.error = "cancelled";
        } else {
            r.error = "index_query_failed";
        }
    } catch (...) {
        r.ok = false;
        r.error = "index_query_failed";
    }
    return finish();
}

}  // namespace spacelens
