#include "core/DirectoryTree.hpp"

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
                                 std::uint32_t attributes)
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
    entry.attributes = attributes;
    m_files.push_back(std::move(entry));

    auto& dirNode = m_dirs[parent];
    dirNode.files.push_back(index);
    dirNode.directFileSize += size;
    dirNode.fileCount += 1;
    return index;
}

void DirectoryTree::recomputeNode(DirIndex index)
{
    auto& node = m_dirs[index];
    ByteSize recursive = node.directFileSize;
    std::uint64_t totalFiles = node.fileCount;

    for (const DirIndex child : node.children) {
        recomputeNode(child);
        recursive += m_dirs[child].recursiveSize;
        totalFiles += m_dirs[child].totalFileCount;
    }

    node.recursiveSize = recursive;
    node.totalFileCount = totalFiles;
}

void DirectoryTree::recomputeAggregates()
{
    if (m_root == InvalidDirIndex) {
        return;
    }
    recomputeNode(m_root);
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
