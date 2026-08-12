#include "TestRunner.hpp"

#include "core/ScanEngine.hpp"
#include "platform/windows/WindowsFileEnumerator.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::wstring makeTempTree()
{
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    const fs::path root =
        fs::path(tempPath) / L"spacelens_test_scan";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"sub" / L"deep", ec);

    {
        std::ofstream f(root / L"root.txt", std::ios::binary);
        f << "hello";  // 5 bytes
    }
    {
        std::ofstream f(root / L"sub" / L"a.bin", std::ios::binary);
        const char data[100] = {};
        f.write(data, 100);
    }
    {
        std::ofstream f(root / L"sub" / L"deep" / L"b.bin", std::ios::binary);
        const char data[50] = {};
        f.write(data, 50);
    }
    return root.wstring();
}

}  // namespace

SPACELENS_TEST(Windows_scan_temp_tree)
{
    const std::wstring root = makeTempTree();
    spacelens::WindowsFileEnumerator enumerator;
    spacelens::ScanEngine engine(enumerator);
    spacelens::ScanOptions options;
    options.topFileCount = 10;

    const auto result = engine.scan(root, options);
    SPACELENS_REQUIRE(result.state == spacelens::ScanState::Completed);
    SPACELENS_REQUIRE(result.progress.filesSeen == 3);
    SPACELENS_REQUIRE(result.tree.dir(result.tree.root()).recursiveSize == 155);

    // Cleanup
    std::error_code ec;
    fs::remove_all(root, ec);
}
