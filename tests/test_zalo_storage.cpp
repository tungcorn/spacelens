#include "TestRunner.hpp"

#include "core/Json.hpp"
#include "core/JsonValue.hpp"
#include "core/ZaloStorageInspector.hpp"
#include "platform/windows/FileIdentity.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winioctl.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace spacelens;
namespace fs = std::filesystem;

namespace {

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : m_handle(handle)
    {
    }

    ~ScopedWinHandle()
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_handle);
        }
    }

    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

class TempFixture {
public:
    TempFixture()
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_root = fs::temp_directory_path() / "spacelens_zalo_tests" /
                 std::to_string(stamp);
        std::error_code ec;
        fs::create_directories(m_root, ec);
        if (ec) {
            throw spacelens::test::Failure("cannot create Zalo fixture root");
        }
    }

    ~TempFixture()
    {
        std::error_code ec;
        fs::remove_all(m_root, ec);
    }

    TempFixture(const TempFixture&) = delete;
    TempFixture& operator=(const TempFixture&) = delete;

    [[nodiscard]] const fs::path& root() const noexcept { return m_root; }

private:
    fs::path m_root;
};

void writeBytes(const fs::path& path, std::size_t size)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        throw spacelens::test::Failure("cannot create Zalo fixture directory");
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw spacelens::test::Failure("cannot create Zalo fixture file");
    }
    std::string bytes(size, 'z');
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw spacelens::test::Failure("cannot write Zalo fixture file");
    }
}

void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        throw spacelens::test::Failure("cannot create Zalo fixture directory");
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw spacelens::test::Failure("cannot create Zalo fixture file");
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
        throw spacelens::test::Failure("cannot write Zalo fixture file");
    }
}

bool writeAlternateDataStream(const fs::path& path, std::size_t size,
                              DWORD* error = nullptr)
{
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        if (error != nullptr) {
            *error = ERROR_FILE_TOO_LARGE;
        }
        return false;
    }

    const std::wstring streamPath = path.wstring() + L":spacelens-test";
    const HANDLE handle = ::CreateFileW(
        streamPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }

    const std::string bytes(size, 'a');
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                                &written, nullptr) != FALSE &&
                    written == bytes.size();
    const DWORD writeError = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(handle);
    if (!ok && error != nullptr) {
        *error = writeError;
    }
    return ok;
}

void writeText(const fs::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw spacelens::test::Failure("cannot create Zalo config file");
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

ZaloDiscoveryOptions explicitOptions(const fs::path& path)
{
    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.explicitRoots = {path.wstring()};
    return options;
}

const ZaloEntry* findEntry(const ZaloAccountReport& account,
                           std::string_view category,
                           ZaloEntryKind kind = ZaloEntryKind::File)
{
    for (const auto& entry : account.entries) {
        if (entry.categoryAlias == category && entry.kind == kind) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<std::string> entryIds(const ZaloStorageReport& report)
{
    std::vector<std::string> ids;
    for (const auto& account : report.accounts) {
        for (const auto& entry : account.entries) {
            ids.push_back(entry.entryId);
        }
    }
    return ids;
}

std::vector<std::uint8_t> makeTestJpeg()
{
    return {
        0xff, 0xd8,
        0xff, 0xe0, 0x00, 0x10,
        0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0xff, 0xc0, 0x00, 0x11,
        0x08, 0x00, 0x10, 0x00, 0x20, 0x03,
        0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
        0xff, 0xda, 0x00, 0x0c,
        0x03, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
        0x00, 0x3f, 0x00,
        0x11, 0x22, 0xff, 0x00, 0x33,
        0xff, 0xd9};
}

void requirePrivacySafeContent(const ZaloContentResult& result,
                               const fs::path& nativePath)
{
    const std::string pathText = nativePath.string();
    const std::string filename = nativePath.filename().string();
    const auto safeText = [&](std::string_view text) {
        return text.find(pathText) == std::string_view::npos &&
               text.find(filename) == std::string_view::npos &&
               text.find("hash") == std::string_view::npos &&
               text.find("Hash") == std::string_view::npos &&
               text.find("SHA") == std::string_view::npos &&
               text.find('\\') == std::string_view::npos &&
               text.find('/') == std::string_view::npos;
    };

    SPACELENS_REQUIRE(safeText(result.description));
    for (const auto code : result.evidence) {
        SPACELENS_REQUIRE(safeText(toString(code)));
    }
}

bool createDirectoryJunction(const fs::path& link, const fs::path& target)
{
    std::error_code ec;
    fs::create_directories(link.parent_path(), ec);
    if (ec || ::CreateDirectoryW(link.wstring().c_str(), nullptr) == FALSE) {
        return false;
    }

    std::wstring printName = fs::absolute(target, ec).lexically_normal().wstring();
    if (ec || printName.empty()) {
        (void)::RemoveDirectoryW(link.wstring().c_str());
        return false;
    }
    std::wstring substituteName;
    if (printName.rfind(L"\\\\", 0) == 0) {
        substituteName = L"\\??\\UNC\\" + printName.substr(2);
    } else {
        substituteName = L"\\??\\" + printName;
    }

    struct MountPointBufferHeader {
        DWORD reparseTag;
        WORD reparseDataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
    };

    const std::size_t substituteBytes =
        substituteName.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) +
                                  printBytes + sizeof(wchar_t);
    const std::size_t bufferBytes = sizeof(MountPointBufferHeader) + pathBytes;
    if (bufferBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE ||
        pathBytes + 8U > std::numeric_limits<WORD>::max()) {
        (void)::RemoveDirectoryW(link.wstring().c_str());
        return false;
    }

    std::vector<std::byte> buffer(bufferBytes);
    auto* header = reinterpret_cast<MountPointBufferHeader*>(buffer.data());
    header->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    header->reparseDataLength = static_cast<WORD>(8U + pathBytes);
    header->reserved = 0;
    header->substituteNameOffset = 0;
    header->substituteNameLength = static_cast<WORD>(substituteBytes);
    header->printNameOffset =
        static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    header->printNameLength = static_cast<WORD>(printBytes);

    std::byte* pathBuffer = buffer.data() + sizeof(MountPointBufferHeader);
    std::memcpy(pathBuffer, substituteName.data(), substituteBytes);
    std::memcpy(pathBuffer + substituteBytes + sizeof(wchar_t),
                printName.data(), printBytes);

    const HANDLE raw = ::CreateFileW(
        link.wstring().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ScopedWinHandle handle(raw);
    DWORD returned = 0;
    const bool created =
        handle.valid() &&
        ::DeviceIoControl(raw, FSCTL_SET_REPARSE_POINT, buffer.data(),
                          static_cast<DWORD>(buffer.size()), nullptr, 0, &returned,
                          nullptr) != FALSE;
    if (!created) {
        (void)::RemoveDirectoryW(link.wstring().c_str());
    }
    return created;
}

bool createUnprivilegedSymlink(const fs::path& link, const fs::path& target,
                               bool directory)
{
    DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0U;
    flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (::CreateSymbolicLinkW(link.wstring().c_str(), target.wstring().c_str(),
                             flags) != FALSE) {
        return true;
    }
    return directory && createDirectoryJunction(link, target);
}

bool enableCaseSensitiveDirectory(const fs::path& path)
{
    const HANDLE raw = ::CreateFileW(
        path.wstring().c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    ScopedWinHandle handle(raw);
    if (!handle.valid()) {
        return false;
    }
    FILE_CASE_SENSITIVE_INFORMATION information{};
    if (::GetFileInformationByHandleEx(
            raw, FileCaseSensitiveInfo, &information,
            sizeof(information)) == FALSE) {
        return false;
    }
    information.Flags |= FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    return ::SetFileInformationByHandle(
               raw, FileCaseSensitiveInfo, &information,
               sizeof(information)) != FALSE;
}

std::wstring extendedLengthPath(const fs::path& path)
{
    std::wstring absolute = fs::absolute(path.parent_path()).wstring();
    if (!absolute.empty() && absolute.back() != L'\\' &&
        absolute.back() != L'/') {
        absolute.push_back(L'\\');
    }
    // Resolve only the parent. std::filesystem normalizes a trailing dot when
    // absolute() receives the full path, which would defeat this fixture.
    absolute.append(path.filename().wstring());
    if (absolute.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + absolute.substr(2);
    }
    return L"\\\\?\\" + absolute;
}

bool writeExtendedNameFile(const fs::path& path, std::size_t size,
                           DWORD* error = nullptr)
{
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        if (error != nullptr) {
            *error = ERROR_FILE_TOO_LARGE;
        }
        return false;
    }
    const std::wstring extended = extendedLengthPath(path);
    const HANDLE handle = ::CreateFileW(
        extended.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_POSIX_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    const std::string bytes(size, 'x');
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                                &written, nullptr) != FALSE &&
                    written == bytes.size();
    const DWORD writeError = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(handle);
    if (!ok && error != nullptr) {
        *error = writeError;
    }
    return ok;
}

bool deleteExtendedNameFile(const fs::path& path)
{
    const std::wstring extended = extendedLengthPath(path);
    return ::DeleteFileW(extended.c_str()) != FALSE;
}

std::vector<std::uint8_t> readBytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw spacelens::test::Failure("cannot read Zalo fixture file");
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

std::size_t directoryEntryCount(const fs::path& path)
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(path)) {
        ++count;
    }
    return count;
}

bool sameStableIdentity(const FileIdentity& left, const FileIdentity& right)
{
    const bool sameFileId =
        left.volumeSerial == right.volumeSerial &&
        left.fileId128Known == right.fileId128Known &&
        left.fileIndex64Known == right.fileIndex64Known &&
        ((!left.fileId128Known || left.fileId128 == right.fileId128) &&
         (!left.fileIndex64Known || left.fileId == right.fileId));
    return sameFileId && left.identityKnown == right.identityKnown &&
           left.isDirectory == right.isDirectory &&
           left.sizeBytes == right.sizeBytes &&
           left.logicalSizeKnown == right.logicalSizeKnown &&
           left.allocatedBytes == right.allocatedBytes &&
           left.allocationKnown == right.allocationKnown &&
           left.numberOfLinks == right.numberOfLinks &&
           left.linkCountKnown == right.linkCountKnown &&
           left.sparse == right.sparse && left.compressed == right.compressed &&
           left.creationTimeTicks == right.creationTimeTicks &&
           left.changeTimeTicks == right.changeTimeTicks &&
           left.lastWriteTicks == right.lastWriteTicks &&
           left.basicMetadataKnown == right.basicMetadataKnown &&
           left.attributes == right.attributes &&
           left.observationConsistent == right.observationConsistent;
}

std::string identityToken(const FileIdentity& identity)
{
    if (identity.fileId128Known) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string token;
        token.reserve(identity.fileId128.size() * 2U);
        for (const std::uint8_t byte : identity.fileId128) {
            token.push_back(digits[byte >> 4U]);
            token.push_back(digits[byte & 0x0fU]);
        }
        return token;
    }
    if (identity.fileIndex64Known) {
        return std::to_string(identity.fileId);
    }
    return {};
}

bool isHexDigit(char value) noexcept
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool containsHexDigest(std::string_view text) noexcept
{
    std::size_t run = 0;
    for (const char value : text) {
        if (isHexDigit(value)) {
            ++run;
            if (run >= 64U) {
                return true;
            }
        } else {
            run = 0;
        }
    }
    return false;
}

void requirePrivacySafeExactCopy(
    const ZaloExactCopyReport& report, const std::vector<fs::path>& nativePaths,
    std::string_view privateText,
    const std::vector<FileIdentity>& identities = {})
{
    const std::string json = report.toJson();
    const auto parsed = json::parseJson(json);
    SPACELENS_REQUIRE(parsed.ok);
    for (const auto& path : nativePaths) {
        SPACELENS_REQUIRE(json.find(path.string()) == std::string::npos);
        const std::string filename = path.filename().string();
        if (filename != "comparison" && filename != "outside") {
            SPACELENS_REQUIRE(json.find(filename) == std::string::npos);
        }
    }
    if (!privateText.empty()) {
        SPACELENS_REQUIRE(json.find(privateText) == std::string::npos);
    }
    SPACELENS_REQUIRE(json.find("\"file_id\":") == std::string::npos);
    SPACELENS_REQUIRE(json.find("volume_serial") == std::string::npos);
    SPACELENS_REQUIRE(json.find("file_identity") == std::string::npos);
    SPACELENS_REQUIRE(json.find("account_id") == std::string::npos);
    SPACELENS_REQUIRE(json.find("\"identity\"") == std::string::npos);
    SPACELENS_REQUIRE(json.find("\"digest\"") == std::string::npos);
    SPACELENS_REQUIRE(!containsHexDigest(json));
    SPACELENS_REQUIRE(json.find("account-") == std::string::npos);
    for (const auto& identity : identities) {
        const std::string token = identityToken(identity);
        if (token.size() >= 8U) {
            SPACELENS_REQUIRE(json.find(token) == std::string::npos);
        }
    }
}

const ZaloExactCopyMatch* findExactCopyMatch(
    const ZaloExactCopyReport& report, std::string_view entryId)
{
    for (const auto& match : report.matches) {
        if (match.zaloEntryId == entryId) {
            return &match;
        }
    }
    return nullptr;
}

}  // namespace

SPACELENS_TEST(Zalo_exact_media_account_and_download_shapes_are_bounded)
{
    TempFixture fixture;
    const fs::path media = fixture.root() / "media-root";
    const fs::path accountOne = media / "opaque-account-7f91";
    const fs::path accountTwo = media / "opaque-account-b2d4";
    writeBytes(accountOne / "ZaloDownloads" / "cache" / "a.bin", 3);
    writeBytes(accountOne / "ZaloDownloads" / "fileNoise" / "b.bin", 4);
    writeBytes(accountTwo / "ZaloDownloads" / "picture" / "c.bin", 5);
    writeBytes(media / "Chromium" / "profile" / "should-not-scan.bin", 99);
    writeBytes(media / "config" / "should-not-scan.bin", 99);
    writeBytes(media / "logs" / "should-not-scan.bin", 99);

    const auto mediaReport = inspectZaloStorage(explicitOptions(media));
    SPACELENS_REQUIRE(mediaReport.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(mediaReport.accounts.size(), 2ULL);
    SPACELENS_REQUIRE_EQ(mediaReport.roots.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(mediaReport.roots.front().accountAliases.size(), 2ULL);
    SPACELENS_REQUIRE(mediaReport.accounts[0].accountAlias !=
                      mediaReport.accounts[1].accountAlias);
    SPACELENS_REQUIRE(findEntry(mediaReport.accounts[0], "cache") != nullptr);
    SPACELENS_REQUIRE(findEntry(mediaReport.accounts[0], "file-noise") != nullptr);
    SPACELENS_REQUIRE(findEntry(mediaReport.accounts[1], "picture") != nullptr);

    const auto accountReport =
        inspectZaloStorage(explicitOptions(accountOne));
    SPACELENS_REQUIRE(accountReport.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(accountReport.accounts.size(), 1ULL);

    const auto exactReport = inspectZaloStorage(
        explicitOptions(accountOne / "ZaloDownloads"));
    SPACELENS_REQUIRE(exactReport.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(exactReport.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(exactReport.accounting.pathCount, 2ULL);
}

SPACELENS_TEST(Zalo_exact_names_are_matched_case_insensitively)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "zAlOdOwNlOaDs";
    writeBytes(root / "CaChE" / "one.bin", 6);

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.front().unsafeEntriesSkipped, 0ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.front().otherErrors, 0ULL);
    SPACELENS_REQUIRE(findEntry(report.accounts.front(), "cache") != nullptr);
}

SPACELENS_TEST(Zalo_reserved_device_entry_is_skipped_as_unsafe)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path ambiguous = root / "cache" / "NUL.txt";
    std::error_code ec;
    fs::create_directories(ambiguous.parent_path(), ec);
    SPACELENS_REQUIRE(!ec);
    DWORD createError = ERROR_SUCCESS;
    if (!writeExtendedNameFile(ambiguous, 7, &createError)) {
        std::cout << "[ SKIP ] Zalo_reserved_device_entry_is_skipped_as_unsafe — "
                     "extended-name file creation failed (error "
                  << createError << ")\n";
        return;
    }

    const auto report = inspectZaloStorage(explicitOptions(root));
    const bool deleted = deleteExtendedNameFile(ambiguous);
    SPACELENS_REQUIRE(deleted);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 0ULL);
    SPACELENS_REQUIRE(!report.accounting.pathVisibleLogicalKnown);
    SPACELENS_REQUIRE(!report.accounting.uniqueLogicalKnown);
    const auto& account = report.accounts.front();
    SPACELENS_REQUIRE_EQ(account.unsafeEntriesSkipped, 1ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.pathCount, 0ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.partialKnownUniqueLogicalBytes, 0ULL);
}

SPACELENS_TEST(Zalo_console_device_entries_are_skipped_as_unsafe)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path conin = root / "cache" / "CONIN$";
    const fs::path conout = root / "cache" / "CONOUT$";
    std::error_code ec;
    fs::create_directories(conin.parent_path(), ec);
    SPACELENS_REQUIRE(!ec);

    DWORD coninError = ERROR_SUCCESS;
    if (!writeExtendedNameFile(conin, 7, &coninError)) {
        std::cout << "[ SKIP ] Zalo_console_device_entries_are_skipped_as_unsafe — "
                     "CONIN$ creation failed (error "
                  << coninError << ")\n";
        return;
    }
    DWORD conoutError = ERROR_SUCCESS;
    if (!writeExtendedNameFile(conout, 8, &conoutError)) {
        (void)deleteExtendedNameFile(conin);
        std::cout << "[ SKIP ] Zalo_console_device_entries_are_skipped_as_unsafe — "
                     "CONOUT$ creation failed (error "
                  << conoutError << ")\n";
        return;
    }

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(deleteExtendedNameFile(conin));
    SPACELENS_REQUIRE(deleteExtendedNameFile(conout));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 0ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.front().unsafeEntriesSkipped, 2ULL);
}

SPACELENS_TEST(Zalo_superscript_device_entry_is_skipped_as_unsafe)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path ambiguous = root / "cache" / fs::path(L"COM¹.txt");
    std::error_code ec;
    fs::create_directories(ambiguous.parent_path(), ec);
    SPACELENS_REQUIRE(!ec);
    DWORD createError = ERROR_SUCCESS;
    if (!writeExtendedNameFile(ambiguous, 7, &createError)) {
        std::cout << "[ SKIP ] Zalo_superscript_device_entry_is_skipped_as_unsafe — "
                     "extended-name file creation failed (error "
                  << createError << ")\n";
        return;
    }

    const auto report = inspectZaloStorage(explicitOptions(root));
    const bool deleted = deleteExtendedNameFile(ambiguous);
    SPACELENS_REQUIRE(deleted);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 0ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.front().unsafeEntriesSkipped, 1ULL);
}

SPACELENS_TEST(Zalo_trailing_dot_entry_is_skipped_as_unsafe)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path safe = root / "cache" / "ambiguous";
    const fs::path ambiguous = root / "cache" / "ambiguous.";
    writeBytes(safe, 5);
    DWORD createError = ERROR_SUCCESS;
    if (!writeExtendedNameFile(ambiguous, 7, &createError)) {
        std::cout << "[ SKIP ] Zalo_trailing_dot_entry_is_skipped_as_unsafe — "
                     "extended-name file creation failed (error "
                  << createError << ")\n";
        return;
    }

    const auto report = inspectZaloStorage(explicitOptions(root));
    const bool deleted = deleteExtendedNameFile(ambiguous);
    SPACELENS_REQUIRE(deleted);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE(!report.accounting.pathVisibleLogicalKnown);
    SPACELENS_REQUIRE(!report.accounting.uniqueLogicalKnown);
    const auto& account = report.accounts.front();
    SPACELENS_REQUIRE_EQ(account.unsafeEntriesSkipped, 1ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.partialKnownUniqueLogicalBytes, 5ULL);
}

SPACELENS_TEST(Zalo_empty_category_directory_is_complete)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    std::error_code ec;
    fs::create_directories(root / "cache", ec);
    SPACELENS_REQUIRE(!ec);

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 0ULL);
    SPACELENS_REQUIRE(report.accounting.pathVisibleLogicalKnown);
    SPACELENS_REQUIRE_EQ(report.accounting.pathVisibleLogicalBytes, 0ULL);
}

SPACELENS_TEST(Zalo_incomplete_subtree_downgrades_exact_accounting)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path category = root / "cache";
    writeBytes(category / "hidden.bin", 23);
    ScopedWinHandle lock(::CreateFileW(
        category.wstring().c_str(), FILE_LIST_DIRECTORY, 0, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!lock.valid()) {
        std::cout << "[ SKIP ] Zalo_incomplete_subtree_downgrades_exact_accounting — "
                     "exclusive directory open failed\n";
        return;
    }

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 0ULL);
    SPACELENS_REQUIRE(!report.accounting.pathVisibleLogicalKnown);
    SPACELENS_REQUIRE(!report.accounting.uniqueLogicalKnown);
    SPACELENS_REQUIRE(!report.accounting.uniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE(
        report.accounting.partialKnownUniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE(
        !report.accounting.allObservedPathReleaseBytes.has_value());
    SPACELENS_REQUIRE(report.accounting.hardLinkCoverage ==
                      HardLinkCoverage::Unknown);
    const auto& account = report.accounts.front().accounting;
    SPACELENS_REQUIRE(!account.pathVisibleLogicalKnown);
    SPACELENS_REQUIRE(!account.uniqueLogicalKnown);
    SPACELENS_REQUIRE(!account.uniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE(account.hardLinkCoverage == HardLinkCoverage::Unknown);
}

SPACELENS_TEST(Zalo_category_alias_survives_entry_vector_growth)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    for (std::size_t i = 0; i < 128; ++i) {
        writeBytes(root / "cache" / ("item-" + std::to_string(i) + ".bin"), 1);
    }

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 128ULL);
    std::size_t files = 0;
    for (const auto& entry : report.accounts.front().entries) {
        if (entry.kind == ZaloEntryKind::File) {
            ++files;
            SPACELENS_REQUIRE(entry.categoryAlias == "cache");
        }
    }
    SPACELENS_REQUIRE_EQ(files, 128ULL);
}

SPACELENS_TEST(Zalo_discover_accounts_false_does_not_expand_media_children)
{
    TempFixture fixture;
    const fs::path media = fixture.root() / "media";
    writeBytes(media / "opaque-account" / "ZaloDownloads" / "cache" /
                   "hidden.bin",
               1);

    ZaloDiscoveryOptions options = explicitOptions(media);
    options.discoverAccounts = false;
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::InvalidRoot);
    SPACELENS_REQUIRE(report.accounts.empty());
    SPACELENS_REQUIRE(report.roots.empty());
}

SPACELENS_TEST(Zalo_inaccessible_media_child_makes_discovery_partial)
{
    TempFixture fixture;
    const fs::path media = fixture.root() / "media";
    const fs::path readable = media / "readable-account";
    const fs::path denied = media / "denied-account";
    writeBytes(readable / "ZaloDownloads" / "cache" / "visible.bin", 1);
    writeBytes(denied / "ZaloDownloads" / "cache" / "hidden.bin", 1);

    ScopedWinHandle lock(::CreateFileW(
        denied.wstring().c_str(), FILE_LIST_DIRECTORY, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!lock.valid()) {
        std::cout << "[ SKIP ] Zalo_inaccessible_media_child_makes_discovery_partial — "
                     "exclusive directory open failed\n";
        return;
    }

    const auto report = inspectZaloStorage(explicitOptions(media));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 1ULL);
}

SPACELENS_TEST(Zalo_all_unavailable_media_children_report_partial)
{
    TempFixture fixture;
    const fs::path media = fixture.root() / "media";
    const fs::path denied = media / "opaque-account";
    writeBytes(denied / "ZaloDownloads" / "cache" / "hidden.bin", 1);

    ScopedWinHandle lock(::CreateFileW(
        denied.wstring().c_str(), FILE_LIST_DIRECTORY, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!lock.valid()) {
        std::cout << "[ SKIP ] Zalo_all_unavailable_media_children_report_partial — "
                     "exclusive directory open failed\n";
        return;
    }

    const auto report = discoverZaloRoots(explicitOptions(media));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE(report.roots.empty());
}

SPACELENS_TEST(Zalo_default_candidates_use_media_fallback_not_ZaloPC)
{
    TempFixture fixture;
    const fs::path roaming = fixture.root() / "appdata";
    const fs::path local = fixture.root() / "local";
    writeBytes(roaming / "ZaloData" / "media" / "account-x" /
                   "ZaloDownloads" / "voice" / "v.bin",
               7);
    writeBytes(roaming / "ZaloPC" / "Accounts" / "raw-token" / "bad.bin", 99);
    writeBytes(local / "ZaloPC" / "Accounts" / "raw-token" / "bad.bin", 99);

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = true;
    options.roamingAppDataRoot = roaming.wstring();
    options.localAppDataRoot = local.wstring();
    options.documentsRoot = (fixture.root() / "documents").wstring();
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE(findEntry(report.accounts.front(), "voice") != nullptr);
}

SPACELENS_TEST(Zalo_config_allowlist_reads_one_exact_key_path_only)
{
    TempFixture fixture;
    const fs::path media = fixture.root() / "configured-media";
    const fs::path unrelated = fixture.root() / "unrelated-media";
    writeBytes(media / "opaque-account" / "ZaloDownloads" / "resource" /
                   "r.bin",
               8);
    writeBytes(unrelated / "opaque-account" / "ZaloDownloads" / "cache" /
                   "wrong.bin",
               9);
    const fs::path config = fixture.root() / "config.json";
    const std::string json =
        "{\"unrelated\":" + jsonString(unrelated.wstring()) +
        ",\"nested\":{\"mediaRoot\":" + jsonString(media.wstring()) +
        "}}";
    writeText(config, json);

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.configPath = config.wstring();
    options.configJsonKeyPaths = {{"nested", "mediaRoot"}};
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE(findEntry(report.accounts.front(), "resource") != nullptr);
    SPACELENS_REQUIRE(findEntry(report.accounts.front(), "cache") == nullptr);

    ZaloDiscoveryOptions missingOnly = options;
    missingOnly.configJsonKeyPaths = {{"missing"}};
    const auto rejected = discoverZaloRoots(missingOnly);
    SPACELENS_REQUIRE(rejected.status == ZaloStorageStatus::NoRoots);
    SPACELENS_REQUIRE(rejected.roots.empty());
}

SPACELENS_TEST(Zalo_inaccessible_allowlisted_config_root_is_not_reported_absent)
{
    TempFixture fixture;
    const fs::path media = fixture.root() / "configured-media";
    writeBytes(media / "opaque-account" / "ZaloDownloads" / "cache" /
                   "hidden.bin",
               1);
    const fs::path config = fixture.root() / "config.json";
    writeText(config,
              "{\"nested\":{\"mediaRoot\":" +
                  jsonString(media.wstring()) + "}}");

    ScopedWinHandle lock(::CreateFileW(
        media.wstring().c_str(), FILE_LIST_DIRECTORY, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!lock.valid()) {
        std::cout << "[ SKIP ] Zalo_inaccessible_allowlisted_config_root_is_not_reported_absent — "
                     "exclusive directory open failed\n";
        return;
    }

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.configPath = config.wstring();
    options.configJsonKeyPaths = {{"nested", "mediaRoot"}};
    const auto report = discoverZaloRoots(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::ConfigUnavailable);
    SPACELENS_REQUIRE(report.roots.empty());
}

SPACELENS_TEST(Zalo_config_through_intermediate_reparse_is_rejected)
{
    TempFixture fixture;
    const fs::path targetDir = fixture.root() / "target";
    const fs::path aliasDir = fixture.root() / "config-alias";
    const fs::path media = fixture.root() / "configured-media";
    writeBytes(media / "opaque-account" / "ZaloDownloads" / "cache" /
                   "must-not-scan.bin",
               1);
    std::error_code ec;
    fs::create_directories(targetDir, ec);
    SPACELENS_REQUIRE(!ec);
    writeText(targetDir / "config.json",
              "{\"nested\":{\"mediaRoot\":" +
                  jsonString(media.wstring()) + "}}");
    if (!createDirectoryJunction(aliasDir, targetDir)) {
        std::cout << "[ SKIP ] Zalo_config_through_intermediate_reparse_is_rejected — "
                     "directory junction fixture creation failed\n";
        return;
    }

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.configPath = (aliasDir / "config.json").wstring();
    options.configJsonKeyPaths = {{"nested", "mediaRoot"}};
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(::RemoveDirectoryW(aliasDir.wstring().c_str()) != FALSE);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::ConfigUnavailable);
    SPACELENS_REQUIRE(report.accounts.empty());
}

SPACELENS_TEST(Zalo_public_report_has_opaque_safe_deterministic_aliases)
{
    TempFixture fixture;
    const fs::path first = fixture.root() / "z-media";
    const fs::path second = fixture.root() / "a-media";
    writeBytes(first / "token-AAAAAAAA" / "ZaloDownloads" / "cache" /
                   "leaf-1111.bin",
               2);
    writeBytes(second / "token-BBBBBBBB" / "ZaloDownloads" / "mystery-991" /
                   "leaf-2222.bin",
               3);

    ZaloDiscoveryOptions forward;
    forward.includeDefaultRoots = false;
    forward.explicitRoots = {first.wstring(), second.wstring()};
    const auto one = inspectZaloStorage(forward);
    ZaloDiscoveryOptions reverse = forward;
    reverse.explicitRoots = {second.wstring(), first.wstring()};
    const auto two = inspectZaloStorage(reverse);

    SPACELENS_REQUIRE(one.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(two.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(entryIds(one), entryIds(two));
    SPACELENS_REQUIRE_EQ(one.accounts.size(), two.accounts.size());
    for (std::size_t i = 0; i < one.accounts.size(); ++i) {
        SPACELENS_REQUIRE(one.accounts[i].accountAlias ==
                          two.accounts[i].accountAlias);
        for (std::size_t j = 0; j < one.accounts[i].entries.size(); ++j) {
            const auto& left = one.accounts[i].entries[j];
            const auto& right = two.accounts[i].entries[j];
            SPACELENS_REQUIRE(left.entryId == right.entryId);
            SPACELENS_REQUIRE(left.entryId.find("token-") == std::string::npos);
            SPACELENS_REQUIRE(left.entryId.find("leaf-") == std::string::npos);
            SPACELENS_REQUIRE(left.categoryAlias.find("mystery") ==
                              std::string::npos);
            SPACELENS_REQUIRE(left.categoryAlias == "cache" ||
                              left.categoryAlias.rfind("other-", 0) == 0);
        }
    }
}

SPACELENS_TEST(Zalo_case_sensitive_sibling_accounts_are_not_lexically_deduped)
{
    TempFixture fixture;
    const fs::path media = fixture.root() / "case-sensitive-media";
    std::error_code ec;
    fs::create_directories(media, ec);
    SPACELENS_REQUIRE(!ec);
    if (!enableCaseSensitiveDirectory(media)) {
        std::cout << "[ SKIP ] "
                     "Zalo_case_sensitive_sibling_accounts_are_not_lexically_deduped"
                     " — case-sensitive directory precondition unavailable\n";
        return;
    }

    const fs::path upper = media / "AccountCase";
    const fs::path lower = media / "accountcase";
    if (::CreateDirectoryW(upper.wstring().c_str(), nullptr) == FALSE ||
        ::CreateDirectoryW(lower.wstring().c_str(), nullptr) == FALSE) {
        std::cout << "[ SKIP ] "
                     "Zalo_case_sensitive_sibling_accounts_are_not_lexically_deduped"
                     " — distinct case-sensitive siblings unavailable\n";
        return;
    }
    writeBytes(upper / "ZaloDownloads" / "cache" / "upper.bin", 3);
    writeBytes(lower / "ZaloDownloads" / "cache" / "lower.bin", 4);

    const auto report = inspectZaloStorage(explicitOptions(media));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 2ULL);
    SPACELENS_REQUIRE_EQ(report.roots.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 2ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathVisibleLogicalBytes, 7ULL);
}

SPACELENS_TEST(Zalo_case_only_intermediate_reparse_alias_is_rejected)
{
    TempFixture fixture;
    const fs::path parent = fixture.root() / "case-sensitive-parent";
    std::error_code ec;
    fs::create_directories(parent, ec);
    SPACELENS_REQUIRE(!ec);
    if (!enableCaseSensitiveDirectory(parent)) {
        std::cout << "[ SKIP ] Zalo_case_only_intermediate_reparse_alias_is_rejected"
                     " — case-sensitive directory precondition unavailable\n";
        return;
    }

    const fs::path target = parent / "accountcase";
    const fs::path alias = parent / "AccountCase";
    writeBytes(target / "ZaloDownloads" / "cache" / "must-not-scan.bin", 7);
    if (!createDirectoryJunction(alias, target)) {
        std::cout << "[ SKIP ] Zalo_case_only_intermediate_reparse_alias_is_rejected"
                     " — directory junction fixture creation failed\n";
        return;
    }

    const auto report = inspectZaloStorage(
        explicitOptions(alias / "ZaloDownloads"));
    SPACELENS_REQUIRE(::RemoveDirectoryW(alias.wstring().c_str()) != FALSE);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::InvalidRoot);
    SPACELENS_REQUIRE(report.accounts.empty());
    SPACELENS_REQUIRE(report.roots.empty());
    SPACELENS_REQUIRE_EQ(report.discovery.rejectedRootCount, 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 0ULL);
}

SPACELENS_TEST(Zalo_case_sensitive_sibling_roots_are_not_lexically_deduped)
{
    TempFixture fixture;
    const fs::path parent = fixture.root() / "case-sensitive-roots";
    std::error_code ec;
    fs::create_directories(parent, ec);
    SPACELENS_REQUIRE(!ec);
    if (!enableCaseSensitiveDirectory(parent)) {
        std::cout << "[ SKIP ] Zalo_case_sensitive_sibling_roots_are_not_lexically_deduped"
                     " — case-sensitive directory precondition unavailable\n";
        return;
    }

    const fs::path upper = parent / "MediaCase";
    const fs::path lower = parent / "mediacase";
    if (::CreateDirectoryW(upper.wstring().c_str(), nullptr) == FALSE ||
        ::CreateDirectoryW(lower.wstring().c_str(), nullptr) == FALSE) {
        std::cout << "[ SKIP ] Zalo_case_sensitive_sibling_roots_are_not_lexically_deduped"
                     " — distinct case-sensitive siblings unavailable\n";
        return;
    }
    writeBytes(upper / "ZaloDownloads" / "cache" / "upper.bin", 5);
    writeBytes(lower / "ZaloDownloads" / "cache" / "lower.bin", 6);

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.explicitRoots = {upper.wstring(), lower.wstring()};
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.roots.size(), 2ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 2ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 2ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathVisibleLogicalBytes, 11ULL);
}

SPACELENS_TEST(Zalo_overlapping_roots_are_deduplicated_before_accounting)
{
    TempFixture fixture;
    const fs::path media = fixture.root() / "media";
    const fs::path account = media / "opaque-account";
    writeBytes(account / "ZaloDownloads" / "cache" / "one.bin", 11);

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.explicitRoots = {media.wstring(), account.wstring(),
                             (account / "ZaloDownloads").wstring()};
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.front().accounting.pathCount, 1ULL);
}

SPACELENS_TEST(Zalo_high_cardinality_overlaps_keep_deterministic_owners)
{
    TempFixture fixture;
    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    constexpr std::size_t kAccountCount = 192;
    for (std::size_t i = 0; i < kAccountCount; ++i) {
        const fs::path outer =
            fixture.root() / ("group-" + std::to_string(i)) / "ZaloDownloads";
        const fs::path nested = outer / "nested" / "ZaloDownloads";
        writeBytes(outer / "cache" / "outer.bin", 1);
        std::error_code ec;
        fs::create_directories(nested / "cache", ec);
        SPACELENS_REQUIRE(!ec);
        options.explicitRoots.push_back(nested.wstring());
        options.explicitRoots.push_back(outer.wstring());
    }

    // Keep a shared ancestor's directory metadata changing while path
    // components are retained. This must not look like an identity change: the
    // object ID and no-follow binding stay stable even though LastWriteTime does
    // not.
    const fs::path churnPath = fixture.root() / "ancestor-metadata-churn.tmp";
    const std::wstring churnNative = churnPath.wstring();
    std::atomic_bool churned = false;
    std::jthread churner([churnNative, &churned](std::stop_token stop) {
        while (!stop.stop_requested()) {
            const HANDLE handle = ::CreateFileW(
                churnNative.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                ::CloseHandle(handle);
                (void)::DeleteFileW(churnNative.c_str());
                churned.store(true, std::memory_order_release);
            }
            (void)::SwitchToThread();
        }
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!churned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SPACELENS_REQUIRE(churned.load(std::memory_order_acquire));
    const auto report = inspectZaloStorage(options);
    churner.request_stop();
    churner.join();
    (void)::DeleteFileW(churnPath.wstring().c_str());

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.roots.size(), kAccountCount);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), kAccountCount);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, kAccountCount);
    SPACELENS_REQUIRE_EQ(report.accounting.pathVisibleLogicalBytes,
                         kAccountCount);
    for (const auto& account : report.accounts) {
        SPACELENS_REQUIRE_EQ(account.accounting.pathCount, 1ULL);
    }
}

SPACELENS_TEST(Zalo_high_cardinality_media_root_discovers_sibling_accounts)
{
    TempFixture fixture;
    const fs::path media = fixture.root() / "media";
    constexpr std::size_t kAccountCount = 256;
    for (std::size_t i = 0; i < kAccountCount; ++i) {
        const fs::path account =
            media / ("account-" + std::to_string(1000U + i));
        writeBytes(account / "ZaloDownloads" / "cache" / "one.bin", 1);
    }

    const auto report = inspectZaloStorage(explicitOptions(media));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.roots.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), kAccountCount);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, kAccountCount);
    SPACELENS_REQUIRE_EQ(report.accounting.pathVisibleLogicalBytes,
                         kAccountCount);
    for (const auto& account : report.accounts) {
        SPACELENS_REQUIRE_EQ(account.accounting.pathCount, 1ULL);
        SPACELENS_REQUIRE_EQ(account.accounting.pathVisibleLogicalBytes, 1ULL);
    }
}

SPACELENS_TEST(Zalo_nested_account_roots_are_not_scanned_twice)
{
    TempFixture fixture;
    const fs::path outer = fixture.root() / "ZaloDownloads";
    const fs::path inner = outer / "nested" / "ZaloDownloads";
    writeBytes(inner / "cache" / "one.bin", 13);

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.explicitRoots = {outer.wstring(), inner.wstring()};
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.roots.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathVisibleLogicalBytes, 13ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.hardLinkAliasPathCount, 0ULL);
}

SPACELENS_TEST(Zalo_intermediate_reparse_alias_account_root_is_scanned_once)
{
    TempFixture fixture;
    const fs::path real = fixture.root() / "real";
    const fs::path alias = fixture.root() / "alias";
    writeBytes(real / "ZaloDownloads" / "cache" / "one.bin", 17);
    if (!createUnprivilegedSymlink(alias, real, true)) {
        std::cout << "[ SKIP ] Zalo_intermediate_reparse_alias_account_root_is_scanned_once — "
                     "directory reparse fixture creation failed\n";
        return;
    }

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.explicitRoots = {(real / "ZaloDownloads").wstring(),
                             (alias / "ZaloDownloads").wstring()};
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(::RemoveDirectoryW(alias.wstring().c_str()) != FALSE);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.roots.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.discovery.rejectedRootCount, 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathVisibleLogicalBytes, 17ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.hardLinkAliasPathCount, 0ULL);
}

SPACELENS_TEST(Zalo_hardlinks_external_links_and_same_size_files_are_honest)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path original = root / "cache" / "original.bin";
    const fs::path alias = root / "cache" / "alias.bin";
    const fs::path separate = root / "file" / "separate.bin";
    const fs::path external = fixture.root() / "external-link.bin";
    writeBytes(original, 4096);
    writeBytes(separate, 4096);
    if (!::CreateHardLinkW(alias.wstring().c_str(), original.wstring().c_str(),
                           nullptr)) {
        std::cout << "[ SKIP ] Zalo_hardlinks_external_links_and_same_size_files_are_honest — "
                     "CreateHardLinkW failed\n";
        return;
    }
    if (!::CreateHardLinkW(external.wstring().c_str(), original.wstring().c_str(),
                           nullptr)) {
        std::cout << "[ SKIP ] Zalo_hardlinks_external_links_and_same_size_files_are_honest — "
                     "external CreateHardLinkW failed\n";
        return;
    }

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    const auto& account = report.accounts.front();
    SPACELENS_REQUIRE_EQ(account.accounting.pathCount, 3ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.uniqueLogicalBytes, 8192ULL);
    SPACELENS_REQUIRE(account.accounting.uniqueLogicalKnown);
    SPACELENS_REQUIRE(account.accounting.uniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE(account.accounting.hardLinkCoverage ==
                      HardLinkCoverage::Incomplete);
    SPACELENS_REQUIRE(account.accounting.allObservedPathReleaseBytes.has_value());
    SPACELENS_REQUIRE_EQ(account.accounting.hardLinkAliasPathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.hardLinkAliasBytes, 4096ULL);

    const auto* cache = findEntry(account, "cache");
    const auto* file = findEntry(account, "file");
    SPACELENS_REQUIRE(cache != nullptr);
    SPACELENS_REQUIRE(file != nullptr);
    SPACELENS_REQUIRE(cache->filesystemLinkCount >= 3U);
    SPACELENS_REQUIRE_EQ(cache->observedPathCount, 2U);
    SPACELENS_REQUIRE(cache->singlePathReleaseBytes.has_value());
    SPACELENS_REQUIRE_EQ(*cache->singlePathReleaseBytes, 0ULL);
    SPACELENS_REQUIRE(cache->allObservedPathReleaseBytes.has_value());
    SPACELENS_REQUIRE_EQ(*cache->allObservedPathReleaseBytes, 0ULL);
    SPACELENS_REQUIRE(file->filesystemLinkCount == 1U);
    SPACELENS_REQUIRE(file->observedPathCount == 1U);
    SPACELENS_REQUIRE(file->singlePathReleaseBytes.has_value());
    SPACELENS_REQUIRE(file->allObservedPathReleaseBytes.has_value());
    SPACELENS_REQUIRE_EQ(*account.accounting.allObservedPathReleaseBytes,
                         *file->allObservedPathReleaseBytes);
}

SPACELENS_TEST(Zalo_named_stream_allocation_is_counted_physically)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path filePath = root / "cache" / "with-stream.bin";
    constexpr std::size_t kStreamBytes = 64U * 1024U;
    writeBytes(filePath, 1);

    const auto before = queryFileIdentity(filePath.wstring());
    if (!before || !before->allocationKnown || !before->allocatedBytes.has_value()) {
        std::cout << "[ SKIP ] Zalo_named_stream_allocation_is_counted_physically — "
                     "baseline allocation unavailable\n";
        return;
    }
    const ByteSize baselineAllocation = *before->allocatedBytes;

    DWORD streamError = ERROR_SUCCESS;
    if (!writeAlternateDataStream(filePath, kStreamBytes, &streamError)) {
        std::cout << "[ SKIP ] Zalo_named_stream_allocation_is_counted_physically — "
                     "alternate stream creation failed (error "
                  << streamError << ")\n";
        return;
    }

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    const auto& account = report.accounts.front();
    SPACELENS_REQUIRE_EQ(account.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.pathVisibleLogicalBytes, 1ULL);
    SPACELENS_REQUIRE(account.accounting.uniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE(*account.accounting.uniqueAllocatedBytes >
                      baselineAllocation);

    const ZaloEntry* entry = findEntry(account, "cache");
    SPACELENS_REQUIRE(entry != nullptr);
    SPACELENS_REQUIRE(entry->allocationKnown);
    SPACELENS_REQUIRE(entry->allocatedBytes.has_value());
    SPACELENS_REQUIRE(*entry->allocatedBytes > baselineAllocation);
    SPACELENS_REQUIRE(entry->singlePathReleaseBytes.has_value());
    SPACELENS_REQUIRE_EQ(*entry->singlePathReleaseBytes,
                         *entry->allocatedBytes);
}

SPACELENS_TEST(Zalo_reparse_points_are_not_followed_and_mark_coverage_partial)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path outside = fixture.root() / "outside";
    writeBytes(outside / "not-visible.bin", 17);
    writeBytes(root / "cache" / "visible.bin", 1);
    const bool directoryLink = createUnprivilegedSymlink(
        root / "cache" / "dir.link", outside, true);
    const bool fileLink = createUnprivilegedSymlink(
        root / "cache" / "file.link", outside / "not-visible.bin", false);

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial ||
                      report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 1ULL);
    if (directoryLink) {
        bool sawDirectory = false;
        for (const auto& entry : report.accounts.front().entries) {
            if (entry.kind == ZaloEntryKind::ReparseDirectory) {
                sawDirectory = true;
            }
            SPACELENS_REQUIRE(entry.entryId.find("not-visible") ==
                              std::string::npos);
        }
        SPACELENS_REQUIRE(sawDirectory);
    }
    if (fileLink) {
        bool sawFile = false;
        for (const auto& entry : report.accounts.front().entries) {
            if (entry.kind == ZaloEntryKind::ReparseFile) {
                sawFile = true;
            }
        }
        SPACELENS_REQUIRE(sawFile);
    }
}

SPACELENS_TEST(Zalo_cancelled_result_is_typed_and_empty)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    writeBytes(root / "cache" / "file.bin", 12);
    std::stop_source source;
    source.request_stop();

    const auto report = inspectZaloStorage(explicitOptions(root), source.get_token());
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Cancelled);
    SPACELENS_REQUIRE(report.cancelled());
    SPACELENS_REQUIRE(report.discovery.status == ZaloStorageStatus::Cancelled);
    SPACELENS_REQUIRE(report.discovery.roots.empty());
    SPACELENS_REQUIRE(report.roots.empty());
    SPACELENS_REQUIRE(report.accounts.empty());
    SPACELENS_REQUIRE_EQ(report.discovery.rejectedRootCount, 0ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 0ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.uniqueLogicalBytes, 0ULL);
    SPACELENS_REQUIRE(!report.accounting.uniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE(!report.accounting.allObservedPathReleaseBytes.has_value());
}

SPACELENS_TEST(Zalo_embedded_nul_explicit_root_is_rejected)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    writeBytes(root / "cache" / "must-not-scan.bin", 9);
    std::wstring invalid = root.wstring();
    invalid.push_back(L'\0');
    invalid.append(L"ignored");

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.explicitRoots = {invalid};
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::InvalidRoot);
    SPACELENS_REQUIRE(report.accounts.empty());
    SPACELENS_REQUIRE_EQ(report.discovery.rejectedRootCount, 1ULL);
}

SPACELENS_TEST(Zalo_mixed_separator_namespace_explicit_root_is_rejected)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    writeBytes(root / "cache" / "must-not-scan.bin", 9);

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.explicitRoots = {L"//?/" + root.wstring()};
    const auto report = discoverZaloRoots(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::InvalidRoot);
    SPACELENS_REQUIRE(report.roots.empty());
    SPACELENS_REQUIRE_EQ(report.rejectedRootCount, 1ULL);
}

SPACELENS_TEST(Zalo_extended_namespace_explicit_root_is_rejected)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    writeBytes(root / "cache" / "must-not-scan.bin", 9);

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.explicitRoots = {extendedLengthPath(root)};
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::InvalidRoot);
    SPACELENS_REQUIRE(report.accounts.empty());
    SPACELENS_REQUIRE_EQ(report.discovery.rejectedRootCount, 1ULL);
}

SPACELENS_TEST(Zalo_ambiguous_intermediate_component_is_rejected)
{
    TempFixture fixture;
    const fs::path account = fixture.root() / "account";
    const fs::path root = account / "ZaloDownloads";
    writeBytes(root / "cache" / "must-not-scan.bin", 9);
    const std::wstring ambiguous =
        (fixture.root() / "account.").wstring() + L"\\ZaloDownloads";

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = false;
    options.explicitRoots = {ambiguous};
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::InvalidRoot);
    SPACELENS_REQUIRE(report.accounts.empty());
    SPACELENS_REQUIRE_EQ(report.discovery.rejectedRootCount, 1ULL);
}

SPACELENS_TEST(Zalo_locked_explicit_root_is_partial_not_invalid)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    writeBytes(root / "cache" / "locked.bin", 1);
    ScopedWinHandle lock(::CreateFileW(
        root.wstring().c_str(), FILE_LIST_DIRECTORY, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!lock.valid()) {
        std::cout << "[ SKIP ] Zalo_locked_explicit_root_is_partial_not_invalid — "
                     "exclusive directory open failed\n";
        return;
    }

    const auto report = discoverZaloRoots(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE(report.roots.empty());
}

SPACELENS_TEST(Zalo_explicit_default_candidate_preserves_rejection_provenance)
{
    TempFixture fixture;
    const fs::path valid = fixture.root() / "valid" / "ZaloDownloads";
    const fs::path documents = fixture.root() / "documents";
    const fs::path missing = documents / "Zalo Received Files";
    writeBytes(valid / "cache" / "one.bin", 1);

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = true;
    options.explicitRoots = {valid.wstring(), missing.wstring()};
    options.documentsRoot = documents.wstring();
    options.roamingAppDataRoot = (fixture.root() / "roaming").wstring();
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.discovery.rejectedRootCount, 1ULL);
}

SPACELENS_TEST(Zalo_default_and_case_only_explicit_alias_share_physical_root)
{
    TempFixture fixture;
    const fs::path documents = fixture.root() / "documents";
    const fs::path defaultRoot = documents / "Zalo Received Files";
    const fs::path explicitAlias = documents / "zalo received files";
    writeBytes(defaultRoot / "cache" / "one.bin", 1);

    std::error_code ec;
    if (!fs::exists(explicitAlias, ec) || ec) {
        std::cout << "[ SKIP ] "
                     "Zalo_default_and_case_only_explicit_alias_share_physical_root"
                     " — ordinary case-insensitive TEMP directory unavailable\n";
        return;
    }

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = true;
    options.explicitRoots = {explicitAlias.wstring()};
    options.documentsRoot = documents.wstring();
    options.roamingAppDataRoot = (fixture.root() / "roaming").wstring();
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.discovery.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.discovery.rejectedRootCount, 0ULL);
    SPACELENS_REQUIRE_EQ(report.roots.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.front().accounting.pathCount, 1ULL);
}

SPACELENS_TEST(Zalo_short_path_alias_preserves_default_received_files_shape)
{
    TempFixture fixture;
    const fs::path documents = fixture.root() / "documents-with-long-name";
    const fs::path defaultRoot = documents / "Zalo Received Files";
    writeBytes(defaultRoot / "cache" / "one.bin", 1);

    const std::wstring longPath = defaultRoot.wstring();
    const DWORD required = ::GetShortPathNameW(longPath.c_str(), nullptr, 0);
    if (required == 0) {
        std::cout << "[ SKIP ] "
                     "Zalo_short_path_alias_preserves_default_received_files_shape"
                     " — short-path aliases unavailable\n";
        return;
    }
    std::vector<wchar_t> buffer(required);
    const DWORD written = ::GetShortPathNameW(
        longPath.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        std::cout << "[ SKIP ] "
                     "Zalo_short_path_alias_preserves_default_received_files_shape"
                     " — short-path alias query failed\n";
        return;
    }
    const std::wstring shortPath(buffer.data(), written);
    if (fs::path(shortPath).filename().wstring().find(L'~') ==
        std::wstring::npos) {
        std::cout << "[ SKIP ] "
                     "Zalo_short_path_alias_preserves_default_received_files_shape"
                     " — no distinct 8.3 root alias exists\n";
        return;
    }

    ZaloDiscoveryOptions options;
    options.includeDefaultRoots = true;
    options.explicitRoots = {shortPath};
    options.documentsRoot = documents.wstring();
    options.roamingAppDataRoot = (fixture.root() / "roaming").wstring();
    const auto report = inspectZaloStorage(options);
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.discovery.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.discovery.rejectedRootCount, 0ULL);
    SPACELENS_REQUIRE_EQ(report.roots.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(report.accounting.pathVisibleLogicalBytes, 1ULL);
}

SPACELENS_TEST(Zalo_missing_explicit_root_is_invalid)
{
    TempFixture fixture;
    const auto report = discoverZaloRoots(
        explicitOptions(fixture.root() / "missing"));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::InvalidRoot);
    SPACELENS_REQUIRE(report.roots.empty());
}

SPACELENS_TEST(Zalo_content_identifies_extensionless_jpeg_without_changing_accounting)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path payload = root / "cache" / "opaque-content-token";
    const auto jpeg = makeTestJpeg();
    writeBytes(payload, jpeg);

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    const auto& account = report.accounts.front();
    SPACELENS_REQUIRE(account.complete);
    SPACELENS_REQUIRE_EQ(account.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.pathVisibleLogicalBytes,
                         static_cast<ByteSize>(jpeg.size()));
    SPACELENS_REQUIRE(account.accounting.uniqueLogicalKnown);
    SPACELENS_REQUIRE_EQ(account.accounting.uniqueLogicalBytes,
                         static_cast<ByteSize>(jpeg.size()));
    SPACELENS_REQUIRE_EQ(account.accounting.uniqueIdentityCount, 1ULL);
    SPACELENS_REQUIRE(account.accounting.uniqueAllocatedBytes.has_value());

    const ZaloEntry* entry = findEntry(account, "cache");
    SPACELENS_REQUIRE(entry != nullptr);
    SPACELENS_REQUIRE(entry->contentIdentification.has_value());
    const auto& content = *entry->contentIdentification;
    SPACELENS_REQUIRE(content.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(content.type == ZaloContentType::Jpeg);
    SPACELENS_REQUIRE(!content.wrapper);
    SPACELENS_REQUIRE_EQ(content.payloadOffset, 0ULL);
    SPACELENS_REQUIRE_EQ(content.payloadLength,
                         static_cast<ByteSize>(jpeg.size()));
    SPACELENS_REQUIRE(content.jpegDimensions.has_value());
    SPACELENS_REQUIRE_EQ(content.jpegDimensions->width, 32U);
    SPACELENS_REQUIRE_EQ(content.jpegDimensions->height, 16U);
    requirePrivacySafeContent(content, payload);
}

SPACELENS_TEST(Zalo_content_unknown_bytes_leave_physical_accounting_unchanged)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path payload = root / "cache" / "arbitrary-content-token";
    writeBytes(payload, std::vector<std::uint8_t>(37U, 0x7bU));

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    const auto& account = report.accounts.front();
    SPACELENS_REQUIRE(account.complete);
    SPACELENS_REQUIRE_EQ(account.accounting.pathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.pathVisibleLogicalBytes, 37ULL);
    SPACELENS_REQUIRE(account.accounting.uniqueLogicalKnown);
    SPACELENS_REQUIRE_EQ(account.accounting.uniqueLogicalBytes, 37ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.uniqueIdentityCount, 1ULL);

    const ZaloEntry* entry = findEntry(account, "cache");
    SPACELENS_REQUIRE(entry != nullptr);
    SPACELENS_REQUIRE(entry->contentIdentification.has_value());
    const auto& content = *entry->contentIdentification;
    SPACELENS_REQUIRE(content.status == ZaloContentStatus::Unknown);
    SPACELENS_REQUIRE(content.type == ZaloContentType::Unknown);
    requirePrivacySafeContent(content, payload);
}

SPACELENS_TEST(Zalo_content_hard_link_aliases_share_one_privacy_safe_result)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path original = root / "cache" / "first-content-token";
    const fs::path alias = root / "file" / "second-content-token";
    const auto jpeg = makeTestJpeg();
    writeBytes(original, jpeg);
    std::error_code directoryError;
    fs::create_directories(alias.parent_path(), directoryError);
    SPACELENS_REQUIRE(!directoryError);
    if (!::CreateHardLinkW(alias.wstring().c_str(), original.wstring().c_str(),
                           nullptr)) {
        std::cout << "[ SKIP ] "
                     "Zalo_content_hard_link_aliases_share_one_privacy_safe_result"
                     " — CreateHardLinkW failed\n";
        return;
    }

    const auto report = inspectZaloStorage(explicitOptions(root));
    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    const auto& account = report.accounts.front();
    SPACELENS_REQUIRE(account.complete);
    SPACELENS_REQUIRE_EQ(account.accounting.pathCount, 2ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.pathVisibleLogicalBytes,
                         static_cast<ByteSize>(jpeg.size() * 2U));
    SPACELENS_REQUIRE(account.accounting.uniqueLogicalKnown);
    SPACELENS_REQUIRE_EQ(account.accounting.uniqueLogicalBytes,
                         static_cast<ByteSize>(jpeg.size()));
    SPACELENS_REQUIRE_EQ(account.accounting.uniqueIdentityCount, 1ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.hardLinkAliasPathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(account.accounting.hardLinkAliasBytes,
                         static_cast<ByteSize>(jpeg.size()));
    SPACELENS_REQUIRE(account.accounting.hardLinkCoverage ==
                      HardLinkCoverage::Complete);

    const ZaloEntry* first = findEntry(account, "cache");
    const ZaloEntry* second = findEntry(account, "file");
    SPACELENS_REQUIRE(first != nullptr);
    SPACELENS_REQUIRE(second != nullptr);
    SPACELENS_REQUIRE(first->contentIdentification.has_value());
    SPACELENS_REQUIRE(second->contentIdentification.has_value());
    const auto& firstContent = *first->contentIdentification;
    const auto& secondContent = *second->contentIdentification;
    SPACELENS_REQUIRE(firstContent.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(secondContent.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(firstContent.type == ZaloContentType::Jpeg);
    SPACELENS_REQUIRE(secondContent.type == ZaloContentType::Jpeg);
    SPACELENS_REQUIRE(firstContent.method == secondContent.method);
    SPACELENS_REQUIRE(firstContent.confidence == secondContent.confidence);
    SPACELENS_REQUIRE(firstContent.wrapper == secondContent.wrapper);
    SPACELENS_REQUIRE(firstContent.payloadOffset == secondContent.payloadOffset);
    SPACELENS_REQUIRE(firstContent.payloadLength == secondContent.payloadLength);
    SPACELENS_REQUIRE(firstContent.jpegDimensions.has_value());
    SPACELENS_REQUIRE(secondContent.jpegDimensions.has_value());
    SPACELENS_REQUIRE_EQ(firstContent.jpegDimensions->width,
                         secondContent.jpegDimensions->width);
    SPACELENS_REQUIRE_EQ(firstContent.jpegDimensions->height,
                         secondContent.jpegDimensions->height);
    SPACELENS_REQUIRE(firstContent.evidence == secondContent.evidence);
    SPACELENS_REQUIRE(firstContent.description == secondContent.description);
    requirePrivacySafeContent(firstContent, original);
    requirePrivacySafeContent(secondContent, alias);
}

SPACELENS_TEST(Zalo_exact_copy_matches_explicit_external_full_file_and_preserves_source)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "cache" / "source.bin";
    const fs::path external = fixture.root() / "comparison" / "copy.bin";
    const std::vector<std::uint8_t> bytes{
        0x00U, 0x17U, 0x42U, 0x7fU, 0x80U, 0xa5U, 0xd3U, 0xffU,
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U};
    writeBytes(source, bytes);
    writeBytes(external, bytes);

    const auto sourceBefore = queryFileIdentity(source.wstring());
    const auto externalBefore = queryFileIdentity(external.wstring());
    const auto rootBefore = queryFileIdentity(root.wstring());
    SPACELENS_REQUIRE(sourceBefore.has_value());
    SPACELENS_REQUIRE(externalBefore.has_value());
    SPACELENS_REQUIRE(rootBefore.has_value());
    const auto sourceBytesBefore = readBytes(source);
    const std::size_t rootEntriesBefore = directoryEntryCount(root);

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {external.wstring()};
    const auto report = inspectZaloStorage(options);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.enabled);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonPathsRequested, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonFilesEnumerated, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonFilesHashed, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.uniqueComparisonFiles, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.zaloEntriesConsidered, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.matches.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonScopeAliases.size(), 1ULL);

    const auto& account = report.accounts.front();
    const ZaloEntry* entry = findEntry(account, "cache");
    SPACELENS_REQUIRE(entry != nullptr);
    const ZaloExactCopyMatch* match =
        findExactCopyMatch(report.exactCopy, entry->entryId);
    SPACELENS_REQUIRE(match != nullptr);
    SPACELENS_REQUIRE_EQ(match->comparisonFileId, "comparison-file-000001");
    SPACELENS_REQUIRE_EQ(match->comparisonScopeAlias, "comparison-scope-01");
    SPACELENS_REQUIRE_EQ(match->proofMethod, "full_sha256");
    SPACELENS_REQUIRE_EQ(match->payloadKind, "full_file");
    SPACELENS_REQUIRE_EQ(match->matchedBytes,
                         static_cast<ByteSize>(bytes.size()));
    SPACELENS_REQUIRE_EQ(match->zaloHardLinkAliasCount, 0ULL);
    SPACELENS_REQUIRE_EQ(match->comparisonHardLinkAliasCount, 0ULL);

    requirePrivacySafeExactCopy(report.exactCopy, {source, external}, {} ,
                                {*sourceBefore, *externalBefore});
    const auto sourceAfter = queryFileIdentity(source.wstring());
    const auto rootAfter = queryFileIdentity(root.wstring());
    SPACELENS_REQUIRE(sourceAfter.has_value());
    SPACELENS_REQUIRE(rootAfter.has_value());
    SPACELENS_REQUIRE(sameStableIdentity(*sourceBefore, *sourceAfter));
    SPACELENS_REQUIRE(sameStableIdentity(*rootBefore, *rootAfter));
    SPACELENS_REQUIRE(readBytes(source) == sourceBytesBefore);
    SPACELENS_REQUIRE_EQ(directoryEntryCount(root), rootEntriesBefore);
}

SPACELENS_TEST(Zalo_exact_copy_reports_zalo_hard_link_aliases_per_match)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "cache" / "source.bin";
    const fs::path sourceAlias = root / "cache" / "source-alias.bin";
    const fs::path external = fixture.root() / "comparison" / "copy.bin";
    const std::vector<std::uint8_t> bytes(2048U, 0x5cU);
    writeBytes(source, bytes);
    writeBytes(external, bytes);
    if (!::CreateHardLinkW(sourceAlias.wstring().c_str(),
                           source.wstring().c_str(), nullptr)) {
        std::cout << "[ SKIP ] "
                     "Zalo_exact_copy_reports_zalo_hard_link_aliases_per_match"
                     " — Zalo hard-link fixture capability unavailable\\n";
        return;
    }

    const auto sourceBefore = queryFileIdentity(source.wstring());
    SPACELENS_REQUIRE(sourceBefore.has_value());
    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {external.wstring()};
    const auto report = inspectZaloStorage(options);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.exactCopy.zaloEntriesConsidered, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.zaloHardLinkAliasesCollapsed, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.matches.size(), 1ULL);
    const ZaloExactCopyMatch& match = report.exactCopy.matches.front();
    SPACELENS_REQUIRE_EQ(match.zaloHardLinkAliasCount, 1ULL);
    SPACELENS_REQUIRE_EQ(match.comparisonHardLinkAliasCount, 0ULL);
    SPACELENS_REQUIRE_EQ(match.matchedBytes,
                         static_cast<ByteSize>(bytes.size()));
    requirePrivacySafeExactCopy(report.exactCopy,
                                {source, sourceAlias, external}, {},
                                {*sourceBefore});
}

SPACELENS_TEST(Zalo_exact_copy_same_size_different_content_is_not_a_match)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "cache" / "source.bin";
    const fs::path external = fixture.root() / "comparison" / "different.bin";
    writeBytes(source, std::vector<std::uint8_t>(4096U, 0x31U));
    writeBytes(external, std::vector<std::uint8_t>(4096U, 0x32U));

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {external.wstring()};
    const auto report = inspectZaloStorage(options);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonFilesHashed, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.zaloEntriesConsidered, 1ULL);
    SPACELENS_REQUIRE(report.exactCopy.matches.empty());
    SPACELENS_REQUIRE_EQ(report.exactCopy.skippedSameIdentity, 0ULL);
    requirePrivacySafeExactCopy(report.exactCopy, {source, external}, {});
}

SPACELENS_TEST(Zalo_exact_copy_compares_wrapped_payload_to_external_full_file)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "picture" / "wrapped-source.bin";
    const fs::path external = fixture.root() / "comparison" / "full-image.bin";
    const auto jpeg = makeTestJpeg();
    const std::vector<std::uint8_t> prefix{0x5aU, 0x41U, 0x4cU, 0x4fU,
                                           0x00U, 0x7fU};
    const std::vector<std::uint8_t> suffix{0xdeU, 0xadU, 0xbeU};
    std::vector<std::uint8_t> wrapped = prefix;
    wrapped.insert(wrapped.end(), jpeg.begin(), jpeg.end());
    wrapped.insert(wrapped.end(), suffix.begin(), suffix.end());
    writeBytes(source, wrapped);
    writeBytes(external, jpeg);

    const auto sourceBefore = queryFileIdentity(source.wstring());
    SPACELENS_REQUIRE(sourceBefore.has_value());
    const auto sourceBytesBefore = readBytes(source);

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {external.wstring()};
    const auto report = inspectZaloStorage(options);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.exactCopy.matches.size(), 1ULL);
    const auto& account = report.accounts.front();
    const ZaloEntry* entry = findEntry(account, "picture");
    SPACELENS_REQUIRE(entry != nullptr);
    SPACELENS_REQUIRE(entry->contentIdentification.has_value());
    SPACELENS_REQUIRE(entry->contentIdentification->status ==
                      ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(entry->contentIdentification->type ==
                      ZaloContentType::Jpeg);
    SPACELENS_REQUIRE(entry->contentIdentification->wrapper);
    SPACELENS_REQUIRE_EQ(entry->contentIdentification->payloadOffset,
                         static_cast<ByteSize>(prefix.size()));
    SPACELENS_REQUIRE_EQ(entry->contentIdentification->payloadLength,
                         static_cast<ByteSize>(jpeg.size()));

    const ZaloExactCopyMatch* match =
        findExactCopyMatch(report.exactCopy, entry->entryId);
    SPACELENS_REQUIRE(match != nullptr);
    SPACELENS_REQUIRE_EQ(match->proofMethod, "sha256_payload_vs_full");
    SPACELENS_REQUIRE_EQ(match->payloadKind, "validated_payload");
    SPACELENS_REQUIRE_EQ(match->matchedBytes,
                         static_cast<ByteSize>(jpeg.size()));

    requirePrivacySafeExactCopy(report.exactCopy, {source, external}, {} ,
                                {*sourceBefore});
    const auto sourceAfter = queryFileIdentity(source.wstring());
    SPACELENS_REQUIRE(sourceAfter.has_value());
    SPACELENS_REQUIRE(sameStableIdentity(*sourceBefore, *sourceAfter));
    SPACELENS_REQUIRE(readBytes(source) == sourceBytesBefore);
}

SPACELENS_TEST(Zalo_exact_copy_collapses_external_hard_links_and_skips_same_identity)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "cache" / "source.bin";
    const fs::path comparison = fixture.root() / "comparison";
    const fs::path external = comparison / "copy.bin";
    const fs::path externalAlias = comparison / "copy-alias.bin";
    const fs::path sameIdentity = comparison / "source-alias.bin";
    const std::vector<std::uint8_t> bytes(4096U, 0x4bU);
    writeBytes(source, bytes);
    writeBytes(external, bytes);
    if (!::CreateHardLinkW(externalAlias.wstring().c_str(),
                           external.wstring().c_str(), nullptr)) {
        std::cout << "[ SKIP ] "
                     "Zalo_exact_copy_collapses_external_hard_links_and_skips_same_identity"
                     " — external hard-link capability unavailable\n";
        return;
    }
    if (!::CreateHardLinkW(sameIdentity.wstring().c_str(),
                           source.wstring().c_str(), nullptr)) {
        std::cout << "[ SKIP ] "
                     "Zalo_exact_copy_collapses_external_hard_links_and_skips_same_identity"
                     " — cross-scope hard-link capability unavailable\n";
        return;
    }

    const auto sourceBefore = queryFileIdentity(source.wstring());
    SPACELENS_REQUIRE(sourceBefore.has_value());
    const auto sourceBytesBefore = readBytes(source);

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {comparison.wstring()};
    const auto report = inspectZaloStorage(options);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonFilesEnumerated, 3ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.uniqueComparisonFiles, 2ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonHardLinkAliasesCollapsed,
                         1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonFilesHashed, 2ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.skippedSameIdentity, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.matches.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.zaloHardLinkAliasesCollapsed, 0ULL);

    const ZaloEntry* entry = findEntry(report.accounts.front(), "cache");
    SPACELENS_REQUIRE(entry != nullptr);
    const ZaloExactCopyMatch* match =
        findExactCopyMatch(report.exactCopy, entry->entryId);
    SPACELENS_REQUIRE(match != nullptr);
    SPACELENS_REQUIRE_EQ(match->zaloHardLinkAliasCount, 0ULL);
    SPACELENS_REQUIRE_EQ(match->comparisonHardLinkAliasCount, 1ULL);
    SPACELENS_REQUIRE_EQ(match->matchedBytes,
                         static_cast<ByteSize>(bytes.size()));
    requirePrivacySafeExactCopy(report.exactCopy,
                                {source, external, externalAlias, sameIdentity},
                                {}, {*sourceBefore});

    const auto sourceAfter = queryFileIdentity(source.wstring());
    SPACELENS_REQUIRE(sourceAfter.has_value());
    SPACELENS_REQUIRE(sameStableIdentity(*sourceBefore, *sourceAfter));
    SPACELENS_REQUIRE(readBytes(source) == sourceBytesBefore);
}

SPACELENS_TEST(Zalo_exact_copy_overlapping_scopes_do_not_report_hard_link_aliases)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "cache" / "source.bin";
    const fs::path comparison = fixture.root() / "comparison";
    const fs::path nested = comparison / "nested";
    const fs::path external = nested / "copy.bin";
    const std::vector<std::uint8_t> bytes(2048U, 0x73U);
    writeBytes(source, bytes);
    writeBytes(external, bytes);

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {comparison.wstring(), nested.wstring()};
    const auto report = inspectZaloStorage(options);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonPathsRequested, 2ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonFilesEnumerated, 2ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.uniqueComparisonFiles, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonFilesHashed, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonHardLinkAliasesCollapsed,
                         0ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonOverlappingPathsCollapsed,
                         1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.matches.size(), 1ULL);

    const ZaloEntry* entry = findEntry(report.accounts.front(), "cache");
    SPACELENS_REQUIRE(entry != nullptr);
    const ZaloExactCopyMatch* match =
        findExactCopyMatch(report.exactCopy, entry->entryId);
    SPACELENS_REQUIRE(match != nullptr);
    SPACELENS_REQUIRE_EQ(match->comparisonHardLinkAliasCount, 0ULL);
    requirePrivacySafeExactCopy(report.exactCopy,
                                {source, comparison, nested, external}, {});
}

SPACELENS_TEST(Zalo_exact_copy_skips_reparse_points_and_reports_partial_scope)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "cache" / "source.bin";
    const fs::path comparison = fixture.root() / "comparison";
    const fs::path directCopy = comparison / "direct-copy.bin";
    const fs::path outside = fixture.root() / "outside";
    const fs::path reparseTarget = outside / "hidden-copy.bin";
    const fs::path reparseDirectory = comparison / "reparse-directory";
    const std::vector<std::uint8_t> bytes(1024U, 0x6dU);
    writeBytes(source, bytes);
    writeBytes(directCopy, bytes);
    writeBytes(reparseTarget, bytes);
    if (!createUnprivilegedSymlink(reparseDirectory, outside, true)) {
        std::cout << "[ SKIP ] "
                     "Zalo_exact_copy_skips_reparse_points_and_reports_partial_scope"
                     " — directory reparse fixture capability unavailable\n";
        return;
    }

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {comparison.wstring()};
    const auto report = inspectZaloStorage(options);
    const bool removed =
        ::RemoveDirectoryW(reparseDirectory.wstring().c_str()) != FALSE;
    SPACELENS_REQUIRE(removed);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonFilesEnumerated, 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.matches.size(), 1ULL);
    SPACELENS_REQUIRE(report.exactCopy.skippedReparsePoints >= 1ULL);
    SPACELENS_REQUIRE(report.exactCopy.detail.find("unavailable") !=
                      std::string::npos);
    requirePrivacySafeExactCopy(report.exactCopy,
                                {source, comparison, reparseTarget}, {});
}

SPACELENS_TEST(Zalo_exact_copy_intermediate_reparse_is_typed_separately)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "cache" / "source.bin";
    const fs::path outside = fixture.root() / "outside";
    const fs::path reparseDirectory = fixture.root() / "comparison-link";
    const fs::path reparseTarget = outside / "hidden-copy.bin";
    const std::vector<std::uint8_t> bytes(512U, 0x4aU);
    writeBytes(source, bytes);
    writeBytes(reparseTarget, bytes);
    if (!createUnprivilegedSymlink(reparseDirectory, outside, true)) {
        std::cout << "[ SKIP ] "
                     "Zalo_exact_copy_intermediate_reparse_is_typed_separately"
                     " — directory reparse fixture capability unavailable\\n";
        return;
    }

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {
        (reparseDirectory / "hidden-copy.bin").wstring()};
    const auto report = inspectZaloStorage(options);
    const bool removed =
        ::RemoveDirectoryW(reparseDirectory.wstring().c_str()) != FALSE;
    SPACELENS_REQUIRE(removed);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonFilesEnumerated, 0ULL);
    SPACELENS_REQUIRE(report.exactCopy.skippedReparsePoints >= 1ULL);
    SPACELENS_REQUIRE_EQ(report.exactCopy.skippedInaccessible, 0ULL);
    SPACELENS_REQUIRE(report.exactCopy.matches.empty());
    requirePrivacySafeExactCopy(report.exactCopy,
                                {source, reparseDirectory, reparseTarget}, {});
}

SPACELENS_TEST(Zalo_exact_copy_missing_scope_is_typed_partial_and_privacy_safe)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "cache" / "source.bin";
    const fs::path missing = fixture.root() / "missing-private-comparison-scope";
    const std::vector<std::uint8_t> bytes(512U, 0x21U);
    writeBytes(source, bytes);

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {missing.wstring()};
    const auto report = inspectZaloStorage(options);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.enabled);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Partial);
    SPACELENS_REQUIRE_EQ(report.exactCopy.comparisonPathsRequested, 1ULL);
    SPACELENS_REQUIRE(report.exactCopy.skippedInaccessible >= 1ULL);
    SPACELENS_REQUIRE(report.exactCopy.matches.empty());
    requirePrivacySafeExactCopy(report.exactCopy, {source, missing},
                                "missing-private-comparison-scope");
}

SPACELENS_TEST(Zalo_exact_copy_pre_cancel_publishes_no_comparison_result)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source = root / "cache" / "source.bin";
    const fs::path external = fixture.root() / "comparison" / "copy.bin";
    writeBytes(source, std::vector<std::uint8_t>(512U, 0x09U));
    writeBytes(external, std::vector<std::uint8_t>(512U, 0x09U));

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {external.wstring()};
    std::stop_source stop;
    stop.request_stop();
    const auto report = inspectZaloStorage(options, stop.get_token());

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Cancelled);
    SPACELENS_REQUIRE(report.cancelled());
    SPACELENS_REQUIRE(!report.exactCopy.enabled);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Disabled);
    SPACELENS_REQUIRE(report.exactCopy.matches.empty());
    requirePrivacySafeExactCopy(report.exactCopy, {source, external}, {});
}

SPACELENS_TEST(Zalo_exact_copy_json_omits_paths_digests_identities_and_private_text)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path source =
        root / "account-private-identifier-7" / "cache" /
        "private-message-SECRET-TEXT.bin";
    const fs::path external =
        fixture.root() / "comparison-private-account" /
        "external-private-message.bin";
    constexpr std::string_view privateText =
        "PRIVATE_MESSAGE_DO_NOT_PUBLISH_9f2a";
    const auto bytes = std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(privateText.data()),
        reinterpret_cast<const std::uint8_t*>(privateText.data()) +
            privateText.size());
    writeBytes(source, bytes);
    writeBytes(external, bytes);
    const auto sourceIdentity = queryFileIdentity(source.wstring());
    const auto externalIdentity = queryFileIdentity(external.wstring());
    SPACELENS_REQUIRE(sourceIdentity.has_value());
    SPACELENS_REQUIRE(externalIdentity.has_value());

    ZaloDiscoveryOptions options = explicitOptions(root);
    options.comparisonPaths = {external.wstring()};
    const auto report = inspectZaloStorage(options);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE(report.exactCopy.status == ZaloExactCopyStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.exactCopy.matches.size(), 1ULL);
    requirePrivacySafeExactCopy(report.exactCopy, {root, source, external},
                                privateText,
                                {*sourceIdentity, *externalIdentity});
}

SPACELENS_TEST(Zalo_human_identity_and_last_write_ticks_populated_on_entries)
{
    TempFixture fixture;
    const fs::path root = fixture.root() / "ZaloDownloads";
    const fs::path photo = root / "picture" / "photo.jpg";
    const fs::path randomFile = root / "file" / "unknown_blob.bin";

    // JPEG
    const std::vector<std::uint8_t> jpeg = {
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x20,
        0x00, 0x20, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
        0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x3f,
        0x00, 0x11, 0x22, 0xff, 0x00, 0x33, 0xff, 0xd9};
    writeBytes(photo, jpeg);

    // Random blob
    writeBytes(randomFile, std::vector<std::uint8_t>(256, 0x42));

    ZaloDiscoveryOptions options = explicitOptions(root);
    const auto report = inspectZaloStorage(options);

    SPACELENS_REQUIRE(report.status == ZaloStorageStatus::Complete);
    SPACELENS_REQUIRE_EQ(report.accounts.size(), 1ULL);
    const auto& account = report.accounts.front();

    const ZaloEntry* photoEntry = findEntry(account, "picture", ZaloEntryKind::File);
    SPACELENS_REQUIRE(photoEntry != nullptr);
    SPACELENS_REQUIRE(photoEntry->lastWriteTicks > 0ULL);
    SPACELENS_REQUIRE(photoEntry->humanIdentity.has_value());
    SPACELENS_REQUIRE(photoEntry->contentIdentification.has_value());
    SPACELENS_REQUIRE(photoEntry->contentIdentification->type == ZaloContentType::Jpeg);
    SPACELENS_REQUIRE(photoEntry->humanIdentity->previewKind == ZaloPreviewKind::Image);
    SPACELENS_REQUIRE(photoEntry->humanIdentity->previewAvailable);
    SPACELENS_REQUIRE(photoEntry->humanIdentity->contentSummary.find("JPEG") != std::string::npos);

    const ZaloEntry* randomEntry = findEntry(account, "file", ZaloEntryKind::File);
    SPACELENS_REQUIRE(randomEntry != nullptr);
    SPACELENS_REQUIRE(randomEntry->lastWriteTicks > 0ULL);
    SPACELENS_REQUIRE(randomEntry->humanIdentity.has_value());
    SPACELENS_REQUIRE(randomEntry->humanIdentity->previewKind == ZaloPreviewKind::None);
    SPACELENS_REQUIRE(!randomEntry->humanIdentity->previewAvailable);
}
