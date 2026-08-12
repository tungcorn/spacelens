#include "core/DirectoryTree.hpp"

#include "core/FileTime.hpp"

#include <algorithm>
#include <stdexcept>

namespace spacelens {
namespace {

void appendPathComponent(std::wstring& out, std::wstring_view component)
{
    if (component.empty()) {
        return;
    }
    if (out.empty()) {
        out.assign(component);
        return;
    }
    // Drive roots like L"C:\" already end with separator.
    const wchar_t last = out.back();
    if (last != L'\\' && last != L'/') {
        out.push_back(L'\\');
    }
    out.append(component);
}

}  // namespace

const DirectoryNode& DirectoryTree::dir(DirIndex index) const
{
    return m_dirs.at(index);
}

DirectoryNode& DirectoryTree::dir(DirIndex index)
{
    return m_dirs.at(index);
}

const FileEntry& DirectoryTree::file(FileIndex index) const
{
    return m_files.at(index);
}

FileEntry& DirectoryTree::file(FileIndex index)
{
    return m_files.at(index);
}

DirIndex DirectoryTree::createRoot(std::wstring name)
{
    if (!m_dirs.empty()) {
        throw std::logic_error("DirectoryTree root already created");
    }
    DirectoryNode node;
    node.name = std::move(name);
    node.parent = InvalidDirIndex;
    m_dirs.push_back(std::move(node));
    m_root = 0;
    return m_root;
}

DirIndex DirectoryTree::addDirectory(DirIndex parent, std::wstring name)
{
    if (parent >= m_dirs.size()) {
        throw std::out_of_range("addDirectory: invalid parent");
    }
    const DirIndex index = static_cast<DirIndex>(m_dirs.size());
    DirectoryNode node;
    node.name = std::move(name);
    node.parent = parent;
    m_dirs.push_back(std::move(node));
    m_dirs[parent].children.push_back(index);
    m_dirs[parent].childDirCount += 1;
    return index;
}

FileIndex DirectoryTree::addFile(DirIndex parent,
                                 std::wstring name,
                                 ByteSize size,
                                 std::uint64_t lastWriteTime,
                                 std::uint32_t attributes,
                                 std::uint64_t lastAccessTime)
{
    if (parent >= m_dirs.size()) {
        throw std::out_of_range("addFile: invalid parent");
    }
    const FileIndex index = static_cast<FileIndex>(m_files.size());
    FileEntry entry;
    entry.name = std::move(name);
    entry.parent = parent;
    entry.size = size;
    entry.lastWriteTime = lastWriteTime;
    entry.lastAccessTime = lastAccessTime;
    entry.attributes = attributes;
    m_files.push_back(std::move(entry));

    auto& dirNode = m_dirs[parent];
    dirNode.files.push_back(index);
    dirNode.directFileSize += size;
    dirNode.fileCount += 1;
    return index;
}

void DirectoryTree::recomputeNode(DirIndex index, std::uint64_t nowFileTime)
{
    auto& node = m_dirs[index];
    node.newestDescendantWrite = 0;
    node.oldestDescendantWrite = 0;
    node.filesModifiedLast30Days = 0;
    node.filesModifiedLast90Days = 0;
    node.filesModifiedLast180Days = 0;
    node.filesModifiedLast365Days = 0;
    node.bytesModifiedLast30Days = 0;
    node.bytesModifiedLast90Days = 0;
    node.bytesModifiedLast180Days = 0;
    node.bytesModifiedLast365Days = 0;

    ByteSize recursive = node.directFileSize;
    std::uint64_t totalFiles = node.fileCount;

    for (const FileIndex fileIndex : node.files) {
        const FileEntry& file = m_files[fileIndex];
        if (file.lastWriteTime != 0) {
            if (node.newestDescendantWrite == 0 ||
                file.lastWriteTime > node.newestDescendantWrite) {
                node.newestDescendantWrite = file.lastWriteTime;
            }
            if (node.oldestDescendantWrite == 0 ||
                file.lastWriteTime < node.oldestDescendantWrite) {
                node.oldestDescendantWrite = file.lastWriteTime;
            }
        }

        if (nowFileTime == 0 || file.lastWriteTime == 0 ||
            file.lastWriteTime > nowFileTime) {
            continue;
        }

        const std::uint64_t age = ageDays(file.lastWriteTime, nowFileTime);
        if (age <= 30) {
            node.filesModifiedLast30Days += 1;
            node.bytesModifiedLast30Days += file.size;
        }
        if (age <= 90) {
            node.filesModifiedLast90Days += 1;
            node.bytesModifiedLast90Days += file.size;
        }
        if (age <= 180) {
            node.filesModifiedLast180Days += 1;
            node.bytesModifiedLast180Days += file.size;
        }
        if (age <= 365) {
            node.filesModifiedLast365Days += 1;
            node.bytesModifiedLast365Days += file.size;
        }
    }

    for (const DirIndex child : node.children) {
        recomputeNode(child, nowFileTime);
        const auto& childNode = m_dirs[child];
        recursive += childNode.recursiveSize;
        totalFiles += childNode.totalFileCount;

        if (childNode.newestDescendantWrite != 0 &&
            (node.newestDescendantWrite == 0 ||
             childNode.newestDescendantWrite > node.newestDescendantWrite)) {
            node.newestDescendantWrite = childNode.newestDescendantWrite;
        }
        if (childNode.oldestDescendantWrite != 0 &&
            (node.oldestDescendantWrite == 0 ||
             childNode.oldestDescendantWrite < node.oldestDescendantWrite)) {
            node.oldestDescendantWrite = childNode.oldestDescendantWrite;
        }

        node.filesModifiedLast30Days += childNode.filesModifiedLast30Days;
        node.filesModifiedLast90Days += childNode.filesModifiedLast90Days;
        node.filesModifiedLast180Days += childNode.filesModifiedLast180Days;
        node.filesModifiedLast365Days += childNode.filesModifiedLast365Days;
        node.bytesModifiedLast30Days += childNode.bytesModifiedLast30Days;
        node.bytesModifiedLast90Days += childNode.bytesModifiedLast90Days;
        node.bytesModifiedLast180Days += childNode.bytesModifiedLast180Days;
        node.bytesModifiedLast365Days += childNode.bytesModifiedLast365Days;
    }

    node.recursiveSize = recursive;
    node.totalFileCount = totalFiles;
}

void DirectoryTree::recomputeAggregates(std::uint64_t nowFileTime)
{
    if (m_root == InvalidDirIndex) {
        return;
    }
    recomputeNode(m_root, nowFileTime);
}

std::wstring DirectoryTree::pathOfDirectory(DirIndex index) const
{
    std::vector<DirIndex> chain;
    for (DirIndex cur = index; cur != InvalidDirIndex; cur = m_dirs.at(cur).parent) {
        chain.push_back(cur);
    }
    std::reverse(chain.begin(), chain.end());

    std::wstring path;
    for (const DirIndex id : chain) {
        appendPathComponent(path, m_dirs[id].name);
    }
    return path;
}

std::wstring DirectoryTree::pathOfFile(FileIndex index) const
{
    const FileEntry& entry = m_files.at(index);
    std::wstring path = pathOfDirectory(entry.parent);
    appendPathComponent(path, entry.name);
    return path;
}

std::vector<DirIndex> DirectoryTree::largestChildDirectories(DirIndex parent,
                                                             std::size_t limit) const
{
    if (parent >= m_dirs.size()) {
        throw std::out_of_range("largestChildDirectories: invalid parent");
    }
    std::vector<DirIndex> children = m_dirs[parent].children;
    std::sort(children.begin(), children.end(), [this](DirIndex a, DirIndex b) {
        const auto& da = m_dirs[a];
        const auto& db = m_dirs[b];
        if (da.recursiveSize != db.recursiveSize) {
            return da.recursiveSize > db.recursiveSize;
        }
        return da.name < db.name;
    });
    if (children.size() > limit) {
        children.resize(limit);
    }
    return children;
}

}  // namespace spacelens
