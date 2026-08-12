#include "TestRunner.hpp"

#include "core/DirectoryTree.hpp"

using spacelens::ByteSize;
using spacelens::DirectoryTree;

SPACELENS_TEST(DirectoryTree_empty_root)
{
    DirectoryTree tree;
    const auto root = tree.createRoot(L"C:\\empty");
    tree.recomputeAggregates();

    SPACELENS_REQUIRE(tree.directoryCount() == 1);
    SPACELENS_REQUIRE(tree.fileCount() == 0);
    SPACELENS_REQUIRE(tree.dir(root).recursiveSize == 0);
    SPACELENS_REQUIRE(tree.dir(root).totalFileCount == 0);
    SPACELENS_REQUIRE(tree.pathOfDirectory(root) == L"C:\\empty");
}

SPACELENS_TEST(DirectoryTree_aggregation_invariant)
{
    DirectoryTree tree;
    const auto root = tree.createRoot(L"R");
    const auto a = tree.addDirectory(root, L"A");
    const auto b = tree.addDirectory(root, L"B");
    const auto a1 = tree.addDirectory(a, L"A1");

    tree.addFile(root, L"root.txt", 10);
    tree.addFile(a, L"a.bin", 20);
    tree.addFile(a1, L"deep.bin", 30);
    tree.addFile(b, L"b.bin", 40);
    tree.addFile(b, L"empty.dat", 0);

    tree.recomputeAggregates();

    SPACELENS_REQUIRE(tree.dir(a1).recursiveSize == 30);
    SPACELENS_REQUIRE(tree.dir(a).recursiveSize == 50);   // 20 + 30
    SPACELENS_REQUIRE(tree.dir(b).recursiveSize == 40);
    SPACELENS_REQUIRE(tree.dir(root).recursiveSize == 100); // 10+50+40
    SPACELENS_REQUIRE(tree.dir(root).totalFileCount == 5);
    SPACELENS_REQUIRE(tree.dir(root).directFileSize == 10);

    // Conservation: root recursive size equals sum of every file exactly once.
    ByteSize sum = 0;
    for (std::size_t i = 0; i < tree.fileCount(); ++i) {
        sum += tree.file(static_cast<spacelens::FileIndex>(i)).size;
    }
    SPACELENS_REQUIRE(sum == tree.dir(root).recursiveSize);
}

SPACELENS_TEST(DirectoryTree_path_reconstruction)
{
    DirectoryTree tree;
    const auto root = tree.createRoot(L"C:\\Users");
    const auto docs = tree.addDirectory(root, L"Docs");
    const auto fi = tree.addFile(docs, L"note.txt", 5);
    tree.recomputeAggregates();

    SPACELENS_REQUIRE(tree.pathOfDirectory(docs) == L"C:\\Users\\Docs");
    SPACELENS_REQUIRE(tree.pathOfFile(fi) == L"C:\\Users\\Docs\\note.txt");
}

SPACELENS_TEST(DirectoryTree_unicode_names)
{
    DirectoryTree tree;
    const auto root = tree.createRoot(L"D:\\数据");
    const auto child = tree.addDirectory(root, L"フォルダ");
    const auto fi = tree.addFile(child, L"файл.bin", 7);
    tree.recomputeAggregates();

    SPACELENS_REQUIRE(tree.pathOfFile(fi) == L"D:\\数据\\フォルダ\\файл.bin");
    SPACELENS_REQUIRE(tree.dir(root).recursiveSize == 7);
}
