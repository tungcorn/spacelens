#include "platform/windows/FileContentHasher.hpp"

#include "core/CleanupReview.hpp"
#include "core/HashCache.hpp"
#include "platform/windows/UsnJournal.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <winioctl.h>

#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace spacelens {
namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept
        : m_handle(handle)
    {
    }

    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : m_handle(other.m_handle)
    {
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (valid()) {
            ::CloseHandle(m_handle);
        }
        m_handle = handle;
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

class UniqueHash {
public:
    UniqueHash() = default;
    explicit UniqueHash(BCRYPT_HASH_HANDLE handle) noexcept
        : m_handle(handle)
    {
    }

    ~UniqueHash() { reset(); }

    UniqueHash(const UniqueHash&) = delete;
    UniqueHash& operator=(const UniqueHash&) = delete;

    UniqueHash(UniqueHash&& other) noexcept
        : m_handle(other.m_handle)
    {
        other.m_handle = nullptr;
    }

    UniqueHash& operator=(UniqueHash&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr; }
    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return m_handle; }

    void reset() noexcept
    {
        if (m_handle != nullptr) {
            ::BCryptDestroyHash(m_handle);
            m_handle = nullptr;
        }
    }

private:
    BCRYPT_HASH_HANDLE m_handle = nullptr;
};

std::uint64_t fileTimeToU64(const FILETIME& ft)
{
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

std::array<std::uint8_t, 16> copyFileId128(const FILE_ID_128& id)
{
    std::array<std::uint8_t, 16> out{};
    std::memcpy(out.data(), id.Identifier, out.size());
    return out;
}

bool cancelled(const ContentHashRequest& request)
{
    return static_cast<bool>(request.cancelled) && request.cancelled();
}

ContentHashResult fail(DuplicateFileStatus status,
                       std::uint32_t nativeError,
                       std::string detail)
{
    ContentHashResult result;
    result.status = status;
    result.nativeError = nativeError;
    result.detail = std::move(detail);
    return result;
}

bool hashBytes(BCRYPT_HASH_HANDLE hash,
               const void* data,
               std::size_t size,
               ContentHashResult& error)
{
    if (size == 0) {
        return true;
    }
    const NTSTATUS status = ::BCryptHashData(
        hash, static_cast<PUCHAR>(const_cast<void*>(data)),
        static_cast<ULONG>(size), 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = fail(DuplicateFileStatus::ReadError, static_cast<std::uint32_t>(status),
                     "BCryptHashData failed");
        return false;
    }
    return true;
}

bool readAndHash(HANDLE handle,
                 ByteSize offset,
                 ByteSize length,
                 BCRYPT_HASH_HANDLE hash,
                 std::vector<std::uint8_t>& buffer,
                 ByteSize& bytesRead,
                 const ContentHashRequest& request,
                 ContentHashResult& error)
{
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(handle, pos, nullptr, FILE_BEGIN)) {
        error = fail(DuplicateFileStatus::ReadError, ::GetLastError(),
                     "SetFilePointerEx failed");
        return false;
    }

    ByteSize remaining = length;
    while (remaining > 0) {
        if (cancelled(request)) {
            error = fail(DuplicateFileStatus::Cancelled, 0, "cancelled");
            return false;
        }
        const DWORD chunk =
            remaining > buffer.size() ? static_cast<DWORD>(buffer.size())
                                      : static_cast<DWORD>(remaining);
        DWORD read = 0;
        if (!::ReadFile(handle, buffer.data(), chunk, &read, nullptr)) {
            error = fail(DuplicateFileStatus::ReadError, ::GetLastError(),
                         "ReadFile failed");
            return false;
        }
        if (read == 0) {
            error = fail(DuplicateFileStatus::ReadError, ERROR_HANDLE_EOF,
                         "Unexpected end of file");
            return false;
        }
        if (!hashBytes(hash, buffer.data(), read, error)) {
            return false;
        }
        remaining -= read;
        bytesRead += read;
    }
    return true;
}

bool readFileUsn(HANDLE handle, std::int64_t& usnOut)
{
    READ_FILE_USN_DATA input{};
    input.MinMajorVersion = 2;
    input.MaxMajorVersion = 3;
    alignas(8) std::uint8_t buffer[1024]{};
    DWORD bytes = 0;
    if (!::DeviceIoControl(handle, FSCTL_READ_FILE_USN_DATA, &input,
                           sizeof(input), buffer, sizeof(buffer), &bytes,
                           nullptr)) {
        return false;
    }
    if (bytes < 8) {
        return false;
    }
    struct UsnHeader {
        DWORD RecordLength;
        WORD MajorVersion;
        WORD MinorVersion;
    };
    const auto* header = reinterpret_cast<const UsnHeader*>(buffer);
    if (header->MajorVersion == 2 && bytes >= sizeof(USN_RECORD_V2)) {
        usnOut = reinterpret_cast<const USN_RECORD_V2*>(buffer)->Usn;
        return usnOut != 0;
    }
    if (header->MajorVersion == 3 && bytes >= sizeof(USN_RECORD_V3)) {
        usnOut = reinterpret_cast<const USN_RECORD_V3*>(buffer)->Usn;
        return usnOut != 0;
    }
    return false;
}

void applyCacheEvidence(HANDLE handle, ContentHashResult& observed)
{
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                       sizeof(basic))) {
        observed.changeTime =
            static_cast<FileTimeTicks>(basic.ChangeTime.QuadPart);
    }
    std::int64_t usn = 0;
    if (readFileUsn(handle, usn)) {
        observed.fileUsn = usn;
    }
}

ContentHashResult inspectHandle(HANDLE handle)
{
    FILE_ID_INFO idInfo{};
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL haveId = ::GetFileInformationByHandleEx(handle, FileIdInfo, &idInfo,
                                                       sizeof(idInfo));
    const BOOL haveInfo = ::GetFileInformationByHandle(handle, &info);
    if (!haveInfo) {
        return fail(DuplicateFileStatus::ReadError, ::GetLastError(),
                    "GetFileInformationByHandle failed");
    }

    ContentHashResult observed;
    observed.attributes = info.dwFileAttributes;
    ULARGE_INTEGER size{};
    size.HighPart = info.nFileSizeHigh;
    size.LowPart = info.nFileSizeLow;
    observed.logicalSize = size.QuadPart;
    observed.lastWrite = fileTimeToU64(info.ftLastWriteTime);
    applyCacheEvidence(handle, observed);

    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        observed.status = DuplicateFileStatus::ReparsePoint;
        observed.detail = "Reparse point; content was not read";
        return observed;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        observed.status = DuplicateFileStatus::NotRegularFile;
        observed.detail = "Not a regular file";
        return observed;
    }

    if (haveId) {
        observed.identity =
            makeFileId128Identity(idInfo.VolumeSerialNumber,
                                  copyFileId128(idInfo.FileId));
    } else {
        ULARGE_INTEGER index{};
        index.HighPart = info.nFileIndexHigh;
        index.LowPart = info.nFileIndexLow;
        observed.identity = makeFileIndex64FallbackIdentity(
            info.dwVolumeSerialNumber, index.QuadPart);
    }
    observed.status = DuplicateFileStatus::Verified;
    return observed;
}

}  // namespace

WindowsFileContentHasher::WindowsFileContentHasher()
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const NTSTATUS status = ::BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (BCRYPT_SUCCESS(status)) {
        m_algorithm = algorithm;
    }
}

WindowsFileContentHasher::~WindowsFileContentHasher()
{
    if (m_algorithm != nullptr) {
        ::BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(m_algorithm),
                                       0);
        m_algorithm = nullptr;
    }
}

ContentHashResult WindowsFileContentHasher::hash(const ContentHashRequest& request)
{
    if (request.path.empty()) {
        return fail(DuplicateFileStatus::ReadError, 0, "Empty path");
    }
    if (m_algorithm == nullptr) {
        return fail(DuplicateFileStatus::Unsupported, 0,
                    "BCrypt SHA-256 is unavailable");
    }
    if (cancelled(request)) {
        return fail(DuplicateFileStatus::Cancelled, 0, "cancelled");
    }

    UniqueHandle handle(::CreateFileW(
        request.path.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle.valid()) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_INVALID_NAME) {
            return fail(DuplicateFileStatus::Missing, err, "Path not found");
        }
        if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
            return fail(DuplicateFileStatus::AccessDenied, err, "Access denied");
        }
        return fail(DuplicateFileStatus::ReadError, err, "CreateFileW failed");
    }

    ContentHashResult before = inspectHandle(handle.get());
    if (before.status != DuplicateFileStatus::Verified) {
        return before;
    }

    if (request.expectedSize != 0 && before.logicalSize != request.expectedSize) {
        before.status = DuplicateFileStatus::SizeChanged;
        before.detail = "Logical size changed before hashing";
        return before;
    }
    if (isIdentityAvailable(request.expectedIdentity) &&
        !identitiesEqual(request.expectedIdentity, before.identity)) {
        before.status = DuplicateFileStatus::IdentityChanged;
        before.detail = "Path no longer refers to the expected file identity";
        return before;
    }

    BCRYPT_HASH_HANDLE rawHash = nullptr;
    const NTSTATUS created = ::BCryptCreateHash(
        static_cast<BCRYPT_ALG_HANDLE>(m_algorithm), &rawHash, nullptr, 0,
        nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(created) || rawHash == nullptr) {
        return fail(DuplicateFileStatus::ReadError,
                    static_cast<std::uint32_t>(created), "BCryptCreateHash failed");
    }
    UniqueHash hash(rawHash);

    std::vector<std::uint8_t> buffer(kDuplicateReadBufferBytes);
    ContentHashResult error;
    ByteSize bytesRead = 0;

    if (request.kind == ContentHashKind::Sample &&
        before.logicalSize > kDuplicateSampleThresholdBytes) {
        const ByteSize chunk = kDuplicateSampleChunkBytes;
        const ByteSize size = before.logicalSize;
        const std::uint64_t sizeLe = size;
        if (!hashBytes(hash.get(), &sizeLe, sizeof(sizeLe), error)) {
            return error;
        }
        const ByteSize mid = (size - chunk) / 2U;
        const ByteSize last = size - chunk;
        if (!readAndHash(handle.get(), 0, chunk, hash.get(), buffer, bytesRead,
                         request, error) ||
            !readAndHash(handle.get(), mid, chunk, hash.get(), buffer, bytesRead,
                         request, error) ||
            !readAndHash(handle.get(), last, chunk, hash.get(), buffer, bytesRead,
                         request, error)) {
            return error;
        }
    } else {
        if (!readAndHash(handle.get(), 0, before.logicalSize, hash.get(), buffer,
                         bytesRead, request, error)) {
            return error;
        }
    }

    ContentHashResult after = inspectHandle(handle.get());
    if (after.status == DuplicateFileStatus::ReparsePoint ||
        after.status == DuplicateFileStatus::NotRegularFile) {
        return after;
    }
    if (after.status != DuplicateFileStatus::Verified) {
        return after;
    }
    if (after.logicalSize != before.logicalSize ||
        after.lastWrite != before.lastWrite ||
        (before.changeTime != 0 && after.changeTime != 0 &&
         before.changeTime != after.changeTime) ||
        (before.fileUsn != 0 && after.fileUsn != 0 &&
         before.fileUsn != after.fileUsn)) {
        after.status = DuplicateFileStatus::ChangedDuringRead;
        after.detail = "File metadata changed while hashing";
        after.bytesRead = bytesRead;
        return after;
    }
    if (isIdentityAvailable(before.identity) &&
        isIdentityAvailable(after.identity) &&
        !identitiesEqual(before.identity, after.identity)) {
        after.status = DuplicateFileStatus::IdentityChanged;
        after.detail = "File identity changed while hashing";
        after.bytesRead = bytesRead;
        return after;
    }

    // Path replacement check: the same path must still name this object.
    UniqueHandle pathAgain(::CreateFileW(
        request.path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!pathAgain.valid()) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return fail(DuplicateFileStatus::Missing, err,
                        "Path disappeared during hashing");
        }
    } else {
        ContentHashResult pathNow = inspectHandle(pathAgain.get());
        if (pathNow.status == DuplicateFileStatus::Verified &&
            isIdentityAvailable(before.identity) &&
            isIdentityAvailable(pathNow.identity) &&
            !identitiesEqual(before.identity, pathNow.identity)) {
            pathNow.status = DuplicateFileStatus::IdentityChanged;
            pathNow.detail = "Path now refers to a different file identity";
            pathNow.bytesRead = bytesRead;
            return pathNow;
        }
    }

    std::array<std::uint8_t, 32> digest{};
    const NTSTATUS finished = ::BCryptFinishHash(
        hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (!BCRYPT_SUCCESS(finished)) {
        return fail(DuplicateFileStatus::ReadError,
                    static_cast<std::uint32_t>(finished), "BCryptFinishHash failed");
    }

    after.status = DuplicateFileStatus::Verified;
    after.digest = digest;
    after.bytesRead = bytesRead;
    after.identity = before.identity;
    after.changeTime = after.changeTime != 0 ? after.changeTime : before.changeTime;
    after.fileUsn = after.fileUsn != 0 ? after.fileUsn : before.fileUsn;
    if (after.identity.source == CleanupIdentitySource::FileId128) {
        after.journalId = journalIdFor(request.path, after.identity.volumeSerial);
    }
    // Sample fingerprints must never be cacheable as a full digest.
    after.persistable = request.kind == ContentHashKind::Full &&
                        isHashCachePersistable(evidenceFrom(after));
    return after;
}

ContentHashEvidence WindowsFileContentHasher::probe(const std::wstring& path)
{
    ContentHashEvidence evidence;
    if (path.empty()) {
        evidence.detail = "Empty path";
        return evidence;
    }

    UniqueHandle handle(::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!handle.valid()) {
        evidence.detail = "CreateFileW failed";
        return evidence;
    }

    const ContentHashResult observed = inspectHandle(handle.get());
    evidence = evidenceFrom(observed);
    if (observed.status == DuplicateFileStatus::Verified &&
        observed.identity.source == CleanupIdentitySource::FileId128) {
        evidence.journalId =
            journalIdFor(path, observed.identity.volumeSerial);
    }
    evidence.persistable = isHashCachePersistable(evidence);
    return evidence;
}

std::uint64_t WindowsFileContentHasher::journalIdFor(const std::wstring& path,
                                                     std::uint64_t volumeSerial)
{
    if (volumeSerial == 0) {
        return 0;
    }
    const auto it = m_journalByVolume.find(volumeSerial);
    if (it != m_journalByVolume.end()) {
        return it->second;
    }
    std::uint64_t journalId = 0;
    UsnJournalReader reader;
    if (UsnJournalReader::tryOpen(path, reader) == UsnCapability::Supported) {
        UsnJournalState state{};
        if (reader.query(state) == UsnCapability::Supported) {
            journalId = state.journalId;
        }
    }
    m_journalByVolume.emplace(volumeSerial, journalId);
    return journalId;
}

}  // namespace spacelens
