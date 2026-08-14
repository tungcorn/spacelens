#include "core/index/IndexBuilder.hpp"

#include "core/Classification.hpp"
#include "core/FileTime.hpp"
#include "core/ReclaimAnalysis.hpp"
#include "core/SafetyPolicy.hpp"
#include "core/ScanEngine.hpp"
#include "core/index/IndexRefresh.hpp"
#include "platform/windows/FileIdentity.hpp"
#include "platform/windows/WindowsFileEnumerator.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <chrono>
#include <cwctype>
#include <unordered_map>
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

Classification classifyDirFromTree(const DirectoryTree& tree, DirIndex idx)
{
    return classifyDirectoryFromTree(tree, idx);
}

}  // namespace

IndexBuildResult buildIndexFromScan(const ScanResult& scan,
                                    const std::wstring& rootPath,
                                    std::stop_token stop)
{
    IndexBuildResult result;
    const auto started = std::chrono::steady_clock::now();
    result.location = locateIndex(rootPath);

    if (scan.state == ScanState::Cancelled) {
        result.state = IndexBuildState::Cancelled;
        result.error = "scan cancelled";
        return result;
    }
    if (scan.state != ScanState::Completed || scan.tree.empty()) {
        result.state = IndexBuildState::Failed;
        result.error = "scan did not complete successfully";
        return result;
    }

    try {
        auto store = IndexStore::createStaging(result.location);
        const FileTimeTicks now = nowFileTime();

        IndexRootInfo meta;
        meta.rootId = 1;
        meta.rootPath = result.location.rootPath;
        meta.rootKey = result.location.rootKey;
        meta.schemaVersion = kIndexSchemaVersion;
        meta.indexedAtTicks = now;
        meta.indexedAtIso = fileTimeTicksToIsoUtc(now);
        meta.fileCount = scan.progress.filesSeen;
        meta.dirCount = scan.progress.directoriesSeen;
        meta.logicalBytes = scan.tree.dir(scan.tree.root()).recursiveSize;
        meta.status = IndexStatus::Building;
        store.writeRootMeta(meta);

        // Map DirIndex -> entry id. Files get sequential ids after dirs.
        std::unordered_map<DirIndex, std::int64_t> dirEntryIds;
        dirEntryIds.reserve(scan.tree.directoryCount());

        std::int64_t nextId = 1;
        {
            // Statement must be finalized before the connection is closed for publish.
            SqliteStmt insert(
                store.db(),
                "INSERT INTO entries("
                "id, root_id, parent_id, kind, name, path, size_bytes, recursive_size, "
                "extension, last_write_ticks, last_access_ticks, attributes, is_reparse, "
                "classification, confidence, rule_id, location_safety, reclaimability, "
                "candidate_strength, newest_descendant_write, oldest_descendant_write, "
                "file_id, parent_file_id) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,"
                "?19,?20,?21,?22,?23);");

            SqliteTxn txn(store.db());
            std::unordered_map<DirIndex, std::uint64_t> dirFileIds;
            dirFileIds.reserve(scan.tree.directoryCount());

            // Insert directories first so parents exist.
            const std::size_t dirCount = scan.tree.directoryCount();
            for (std::size_t i = 0; i < dirCount; ++i) {
                if (stop.stop_requested()) {
                    txn.rollback();
                    insert.finalize();
                    store.db().close();
                    discardStagingDatabase(result.location);
                    result.state = IndexBuildState::Cancelled;
                    result.error = "index build cancelled";
                    return result;
                }
                const DirIndex di = static_cast<DirIndex>(i);
                const auto& node = scan.tree.dir(di);
                const std::wstring path = scan.tree.pathOfDirectory(di);
                const auto cls = classifyDirFromTree(scan.tree, di);
                const auto safety = classifyLocation(path);
                const auto reclaim = analyzeItem(
                    path, ItemKind::Directory, node.recursiveSize,
                    node.newestDescendantWrite, cls, safety, now, 0);

                std::uint64_t fileId = 0;
                std::uint64_t parentFileId = 0;
                if (auto id = queryFileIdentity(path)) {
                    fileId = id->fileId;
                }
                if (node.parent != InvalidDirIndex) {
                    parentFileId = dirFileIds[node.parent];
                }
                dirFileIds[di] = fileId;

                const std::int64_t id = nextId++;
                dirEntryIds[di] = id;

                insert.reset();
                insert.clearBindings();
                insert.bindInt64(1, id);
                insert.bindInt64(2, 1);
                if (node.parent == InvalidDirIndex) {
                    insert.bindNull(3);
                } else {
                    insert.bindInt64(3, dirEntryIds[node.parent]);
                }
                insert.bindInt64(4, 1);  // directory
                insert.bindText16(5, node.name);
                insert.bindText16(6, path);
                insert.bindInt64(7, static_cast<std::int64_t>(node.recursiveSize));
                insert.bindInt64(8, static_cast<std::int64_t>(node.recursiveSize));
                insert.bindText(9, "");
                insert.bindInt64(10,
                                 static_cast<std::int64_t>(node.newestDescendantWrite));
                insert.bindInt64(11, 0);
                insert.bindInt64(12, 0);
                insert.bindInt64(13, 0);
                insert.bindText(14, toString(cls.category));
                insert.bindText(15, toString(cls.confidence));
                insert.bindText(16, cls.ruleId);
                insert.bindText(17, toString(safety));
                insert.bindText(18, toString(reclaim.reclaimability));
                insert.bindText(19, toString(reclaim.strength));
                insert.bindInt64(
                    20, static_cast<std::int64_t>(node.newestDescendantWrite));
                insert.bindInt64(
                    21, static_cast<std::int64_t>(node.oldestDescendantWrite));
                insert.bindInt64(22, static_cast<std::int64_t>(fileId));
                insert.bindInt64(23, static_cast<std::int64_t>(parentFileId));
                insert.stepDone();
            }

            const std::size_t fileCount = scan.tree.fileCount();
            for (std::size_t i = 0; i < fileCount; ++i) {
                if (stop.stop_requested()) {
                    txn.rollback();
                    insert.finalize();
                    store.db().close();
                    discardStagingDatabase(result.location);
                    result.state = IndexBuildState::Cancelled;
                    result.error = "index build cancelled";
                    return result;
                }
                const FileIndex fi = static_cast<FileIndex>(i);
                const auto& file = scan.tree.file(fi);
                const std::wstring path = scan.tree.pathOfFile(fi);
                const auto cls = classifyFile(file.name, path);
                const auto safety = classifyLocation(path);
                const auto reclaim = analyzeItem(
                    path, ItemKind::File, file.size, file.lastWriteTime, cls,
                    safety, now, file.lastAccessTime);

                std::uint64_t fileId = 0;
                std::uint64_t parentFileId = 0;
                if (auto id = queryFileIdentity(path)) {
                    fileId = id->fileId;
                }
                if (file.parent != InvalidDirIndex) {
                    parentFileId = dirFileIds[file.parent];
                }

                const std::int64_t id = nextId++;
                insert.reset();
                insert.clearBindings();
                insert.bindInt64(1, id);
                insert.bindInt64(2, 1);
                if (file.parent == InvalidDirIndex) {
                    insert.bindNull(3);
                } else {
                    insert.bindInt64(3, dirEntryIds[file.parent]);
                }
                insert.bindInt64(4, 0);  // file
                insert.bindText16(5, file.name);
                insert.bindText16(6, path);
                insert.bindInt64(7, static_cast<std::int64_t>(file.size));
                insert.bindInt64(8, static_cast<std::int64_t>(file.size));
                insert.bindText(9, extensionLower(file.name));
                insert.bindInt64(10, static_cast<std::int64_t>(file.lastWriteTime));
                insert.bindInt64(11, static_cast<std::int64_t>(file.lastAccessTime));
                insert.bindInt64(12, static_cast<std::int64_t>(file.attributes));
                insert.bindInt64(13, 0);
                insert.bindText(14, toString(cls.category));
                insert.bindText(15, toString(cls.confidence));
                insert.bindText(16, cls.ruleId);
                insert.bindText(17, toString(safety));
                insert.bindText(18, toString(reclaim.reclaimability));
                insert.bindText(19, toString(reclaim.strength));
                insert.bindInt64(20, 0);
                insert.bindInt64(21, 0);
                insert.bindInt64(22, static_cast<std::int64_t>(fileId));
                insert.bindInt64(23, static_cast<std::int64_t>(parentFileId));
                insert.stepDone();
            }

            meta.status = IndexStatus::Ready;
            store.writeRootMeta(meta);
            // USN checkpoint after body is written; still inside the same txn.
            writeRefreshCheckpointAfterFullBuild(store, result.location.rootPath,
                                                 meta.indexedAtTicks);
            txn.commit();
            insert.finalize();
        }

        // Close DB before publish (Windows file locks).
        store.db().close();

        if (stop.stop_requested()) {
            discardStagingDatabase(result.location);
            result.state = IndexBuildState::Cancelled;
            result.error = "index build cancelled before publish";
            return result;
        }

        if (!publishIndexDatabase(result.location)) {
            discardStagingDatabase(result.location);
            result.state = IndexBuildState::Failed;
            result.error = "failed to publish index database";
            return result;
        }

        result.state = IndexBuildState::Completed;
        result.root = meta;
    } catch (const std::exception& ex) {
        discardStagingDatabase(result.location);
        result.state = IndexBuildState::Failed;
        result.error = ex.what();
    } catch (...) {
        discardStagingDatabase(result.location);
        result.state = IndexBuildState::Failed;
        result.error = "unknown index build failure";
    }

    const auto ended = std::chrono::steady_clock::now();
    result.elapsedSeconds =
        std::chrono::duration<double>(ended - started).count();
    return result;
}

IndexBuildResult buildIndexForRoot(const std::wstring& rootPath,
                                   std::stop_token stop)
{
    WindowsFileEnumerator enumerator;
    ScanEngine engine(enumerator);
    ScanOptions options;
    options.topFileCount = 0;
    auto scan = engine.scan(rootPath, options, stop);
    return buildIndexFromScan(scan, rootPath, stop);
}

}  // namespace spacelens
