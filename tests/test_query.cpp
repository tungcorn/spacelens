#include "TestRunner.hpp"

#include "core/DirectoryTree.hpp"
#include "core/Query.hpp"

using spacelens::DirectoryTree;
using spacelens::topDirectories;

SPACELENS_TEST(Query_topDirectories_order)
{
    DirectoryTree tree;
    const auto root = tree.createRoot(L"R");
    const auto a = tree.addDirectory(root, L"A");
    const auto b = tree.addDirectory(root, L"B");
    tree.addFile(a, L"big", 1000);
    tree.addFile(b, L"small", 10);
    tree.addFile(root, L"mid", 100);
    tree.recomputeAggregates();

    const auto top = topDirectories(tree, 2);
    SPACELENS_REQUIRE(top.size() == 2);
    // Root recursive = 1110 is largest; then A = 1000
    SPACELENS_REQUIRE(top[0].size_bytes == 1110);
    SPACELENS_REQUIRE(top[1].size_bytes == 1000);
}

SPACELENS_TEST(Query_topDirectories_limit_zero)
{
    DirectoryTree tree;
    tree.createRoot(L"R");
    tree.recomputeAggregates();
    SPACELENS_REQUIRE(topDirectories(tree, 0).empty());
}
