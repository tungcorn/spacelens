#include "core/index/PhysicalAccounting.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spacelens {
namespace {

struct IdentAgg {
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    std::uint32_t filesystemLinks = 0;
    std::uint32_t observedInIndex = 0;
    std::uint32_t observedInCandidate = 0;
    bool sparse = false;
    bool compressed = false;
};

void mergeIdent(IdentAgg& dst, const IdentAgg& src)
{
    dst.observedInCandidate += src.observedInCandidate;
    if (src.filesystemLinks > dst.filesystemLinks) {
        dst.filesystemLinks = src.filesystemLinks;
    }
    if (src.observedInIndex > dst.observedInIndex) {
        dst.observedInIndex = src.observedInIndex;
    }
    dst.sparse = dst.sparse || src.sparse;
    dst.compressed = dst.compressed || src.compressed;
    if (src.allocationKnown && src.allocatedBytes.has_value()) {
        if (!dst.allocationKnown || !dst.allocatedBytes.has_value()) {
            dst.allocatedBytes = src.allocatedBytes;
            dst.allocationKnown = true;
        }
    }
}

IdentityAllocation toAllocation(const StorageIdentity& id, const IdentAgg& agg)
{
    IdentityAllocation out;
    out.identity = id;
    out.allocatedBytes = agg.allocatedBytes;
    out.allocationKnown = agg.allocationKnown && agg.allocatedBytes.has_value();
    out.filesystemLinks = agg.filesystemLinks;
    out.observedInIndex = agg.observedInIndex;
    out.observedInCandidate = agg.observedInCandidate;
    out.sparse = agg.sparse;
    out.compressed = agg.compressed;
    return out;
}

}  // namespace

PhysicalRow physicalRowFromIdentity(const FileIdentity& id,
                                    bool directoryHandle) noexcept
{
    PhysicalRow row;
    row.volumeSerial = id.volumeSerial;
    row.sparse = id.sparse;
    row.compressed = id.compressed;
    if (directoryHandle) {
        // Directory handle allocation is metadata, not subtree reclaim.
        return row;
    }
    row.hardLinkCount = id.numberOfLinks;
    if (id.allocationKnown && id.allocatedBytes.has_value()) {
        row.allocatedBytes = id.allocatedBytes;
        row.allocationKnown = true;
    }
    return row;
}

bool indexHasPhysicalAccounting(SqliteDb& db)
{
    try {
        SqliteStmt stmt(db, "SELECT value FROM meta WHERE key = ?1;");
        stmt.bindText(1, kPhysicalAccountingMetaKey);
        if (!stmt.step()) {
            return false;
        }
        return stmt.columnText(0) == "1";
    } catch (...) {
        return false;
    }
}

void writePhysicalAccountingFlag(SqliteDb& db, bool enabled)
{
    SqliteStmt del(db, "DELETE FROM meta WHERE key = ?1;");
    del.bindText(1, kPhysicalAccountingMetaKey);
    del.stepDone();
    SqliteStmt ins(db, "INSERT INTO meta(key, value) VALUES(?1, ?2);");
    ins.bindText(1, kPhysicalAccountingMetaKey);
    ins.bindText(2, enabled ? "1" : "0");
    ins.stepDone();
}

bool finalizePhysicalAccounting(SqliteDb& db, bool markComplete,
                                std::stop_token stop)
{
    if (stop.stop_requested()) {
        return false;
    }

    db.exec(R"SQL(
CREATE TEMP TABLE IF NOT EXISTS sl_identity_obs (
  volume_serial INTEGER NOT NULL,
  file_id INTEGER NOT NULL,
  obs INTEGER NOT NULL,
  PRIMARY KEY (volume_serial, file_id)
);
DELETE FROM sl_identity_obs;
INSERT INTO sl_identity_obs(volume_serial, file_id, obs)
  SELECT volume_serial, file_id, COUNT(*)
  FROM entries
  WHERE kind = 0 AND file_id != 0
  GROUP BY volume_serial, file_id;
UPDATE entries
  SET observed_link_count = COALESCE((
    SELECT obs FROM sl_identity_obs
    WHERE sl_identity_obs.volume_serial = entries.volume_serial
      AND sl_identity_obs.file_id = entries.file_id
  ), 0)
  WHERE kind = 0 AND file_id != 0;
UPDATE entries
  SET hard_link_coverage = CASE
    WHEN file_id = 0 OR hard_link_count = 0 THEN 'unknown'
    WHEN observed_link_count < hard_link_count THEN 'incomplete'
    ELSE 'complete'
  END
  WHERE kind = 0;
DROP TABLE sl_identity_obs;
)SQL");

    if (stop.stop_requested()) {
        return false;
    }

    struct DirRec {
        std::int64_t id = 0;
        std::int64_t parentId = 0;
        bool hasParent = false;
        std::vector<std::int64_t> children;
    };

    std::unordered_map<std::int64_t, DirRec> dirs;
    std::int64_t rootId = 0;
    {
        SqliteStmt stmt(db,
                        "SELECT id, parent_id FROM entries WHERE kind = 1;");
        while (stmt.step()) {
            DirRec rec;
            rec.id = stmt.columnInt64(0);
            if (stmt.columnType(1) != 5) {
                rec.parentId = stmt.columnInt64(1);
                rec.hasParent = true;
            }
            dirs.emplace(rec.id, rec);
            if (!rec.hasParent) {
                rootId = rec.id;
            }
        }
    }
    for (auto& [id, rec] : dirs) {
        (void)id;
        if (rec.hasParent) {
            if (auto it = dirs.find(rec.parentId); it != dirs.end()) {
                it->second.children.push_back(rec.id);
            }
        }
    }

    using IdentMap =
        std::unordered_map<StorageIdentity, IdentAgg, StorageIdentityHash>;
    std::unordered_map<std::int64_t, IdentMap> maps;
    maps.reserve(dirs.size());

    {
        SqliteStmt stmt(
            db,
            "SELECT parent_id, volume_serial, file_id, allocated_bytes, "
            "allocation_known, hard_link_count, observed_link_count, sparse, "
            "compressed FROM entries WHERE kind = 0;");
        while (stmt.step()) {
            if (stop.stop_requested()) {
                return false;
            }
            if (stmt.columnType(0) == 5) {
                continue;
            }
            const std::int64_t parentId = stmt.columnInt64(0);
            StorageIdentity id;
            id.volumeSerial = static_cast<std::uint64_t>(stmt.columnInt64(1));
            id.fileId = static_cast<std::uint64_t>(stmt.columnInt64(2));
            IdentAgg agg;
            if (stmt.columnType(3) != 5) {
                agg.allocatedBytes = static_cast<ByteSize>(stmt.columnInt64(3));
            }
            agg.allocationKnown = stmt.columnInt64(4) != 0 &&
                                  agg.allocatedBytes.has_value();
            agg.filesystemLinks =
                static_cast<std::uint32_t>(stmt.columnInt64(5));
            agg.observedInIndex =
                static_cast<std::uint32_t>(stmt.columnInt64(6));
            agg.observedInCandidate = 1;
            agg.sparse = stmt.columnInt64(7) != 0;
            agg.compressed = stmt.columnInt64(8) != 0;
            if (!id.valid()) {
                // Unknown identity: still count as a distinct unknown slot so
                // directory coverage cannot become complete.
                id.fileId = static_cast<std::uint64_t>(0x9e3779b97f4a7c15ull) ^
                            static_cast<std::uint64_t>(parentId) ^
                            static_cast<std::uint64_t>(maps[parentId].size() + 1);
                id.volumeSerial = 0;
                agg.filesystemLinks = 0;
            }
            mergeIdent(maps[parentId][id], agg);
        }
    }

    std::vector<std::int64_t> order;
    order.reserve(dirs.size());
    {
        std::vector<std::int64_t> stack;
        if (rootId != 0) {
            stack.push_back(rootId);
        }
        for (const auto& [id, rec] : dirs) {
            (void)rec;
            if (id != rootId) {
                // Unattached dirs (shouldn't happen) still get processed.
            }
        }
        while (!stack.empty()) {
            const std::int64_t id = stack.back();
            stack.pop_back();
            order.push_back(id);
            if (auto it = dirs.find(id); it != dirs.end()) {
                for (const std::int64_t child : it->second.children) {
                    stack.push_back(child);
                }
            }
        }
        // Also include dirs not reached from root.
        for (const auto& [id, rec] : dirs) {
            (void)rec;
            if (std::find(order.begin(), order.end(), id) == order.end()) {
                order.push_back(id);
            }
        }
    }

    SqliteStmt upd(
        db,
        "UPDATE entries SET allocated_bytes=?1, allocation_known=?2, "
        "hard_link_coverage=?3, sparse=?4, compressed=?5 WHERE id=?6 AND kind=1;");

    // Post-order: children first (reverse of preorder stack walk).
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (stop.stop_requested()) {
            return false;
        }
        const std::int64_t id = *it;
        auto& mine = maps[id];
        const auto dirIt = dirs.find(id);
        if (dirIt == dirs.end()) {
            continue;
        }
        for (const std::int64_t child : dirIt->second.children) {
            auto childIt = maps.find(child);
            if (childIt == maps.end()) {
                continue;
            }
            for (const auto& [ident, agg] : childIt->second) {
                mergeIdent(mine[ident], agg);
            }
            maps.erase(childIt);
        }

        std::vector<IdentityAllocation> items;
        items.reserve(mine.size());
        bool sparse = false;
        bool compressed = false;
        for (const auto& [ident, agg] : mine) {
            items.push_back(toAllocation(ident, agg));
            sparse = sparse || agg.sparse;
            compressed = compressed || agg.compressed;
        }
        const UniqueAllocation sum = summarizeIdentities(items);
        upd.reset();
        upd.clearBindings();
        if (sum.uniqueAllocatedBytes.has_value()) {
            upd.bindInt64(1, static_cast<std::int64_t>(*sum.uniqueAllocatedBytes));
        } else {
            upd.bindNull(1);
        }
        upd.bindInt64(2, sum.allAllocationKnown ? 1 : 0);
        upd.bindText(3, toString(sum.coverage));
        upd.bindInt64(4, sparse ? 1 : 0);
        upd.bindInt64(5, compressed ? 1 : 0);
        upd.bindInt64(6, id);
        upd.stepDone();
    }

    if (markComplete && !stop.stop_requested()) {
        writePhysicalAccountingFlag(db, true);
    }
    return !stop.stop_requested();
}

}  // namespace spacelens
