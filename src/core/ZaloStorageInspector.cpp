#include "core/ZaloStorageInspector.hpp"

#include "core/CleanupReview.hpp"
#include "core/Json.hpp"
#include "core/JsonValue.hpp"
#include "core/ZaloContentIdentifier.hpp"
#include "platform/windows/FileContentHasher.hpp"
#include "platform/windows/FileIdentity.hpp"
#include "platform/windows/ReadOnlyPayload.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace spacelens {
namespace {

constexpr std::wstring_view kZaloDownloadsName = L"ZaloDownloads";
constexpr std::wstring_view kReceivedFilesName = L"Zalo Received Files";
constexpr std::size_t kDirectoryBufferBytes = 64U * 1024U;
constexpr std::size_t kMaximumDirectoryBufferBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxTraversalDepth = 256U;
constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

class UniqueHandle final {
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

struct Cancellation {
    std::stop_token explicitToken;
    std::stop_token optionToken;

    [[nodiscard]] bool requested() const noexcept
    {
        return explicitToken.stop_requested() || optionToken.stop_requested();
    }
};

struct PostScanCancellation final {
};

struct DirectorySortCancelled final {
};

void throwIfPostScanCancelled(const Cancellation& cancellation)
{
    if (cancellation.requested()) {
        throw PostScanCancellation{};
    }
}

enum class EvidenceState {
    Consistent,
    Unknown,
    Inconsistent,
    Changed
};

enum class DirectoryReadStatus {
    Ok,
    AccessDenied,
    NotFound,
    Changed,
    Cancelled,
    Error
};

struct RawDirectoryEntry {
    std::wstring name;
    std::uint32_t attributes = 0;
    std::uint64_t listedFileId = 0;
    bool listedFileIdKnown = false;
    bool directory = false;
    bool reparse = false;
    std::optional<ByteSize> listedLogicalBytes;
};

struct RawDirectoryListing {
    DirectoryReadStatus status = DirectoryReadStatus::Error;
    std::vector<RawDirectoryEntry> entries;
    std::size_t unsafeEntryCount = 0;
};

struct AccountPath {
    std::wstring path;
    std::wstring sortKey;
    std::wstring finalPath;
    std::wstring finalSortKey;
    CleanupIdentity expectedIdentity{};
    bool expectedIdentityKnown = false;
};

struct RetainedPathComponent {
    std::wstring path;
    std::wstring resolvedFinalPath;
    FileIdentity observation{};
    UniqueHandle handle;
};

struct ValidatedWindowsPath {
    std::wstring requestedPath;
    std::wstring resolvedFinalPath;
    FileIdentity finalObservation{};
    std::vector<RetainedPathComponent> parentComponents;
    UniqueHandle finalHandle;
    bool finalDirectory = false;
    bool finalReadData = false;
};

struct InternalRoot {
    std::wstring path;
    std::wstring sortKey;
    std::string rootAlias;
    std::vector<AccountPath> accounts;
};

struct InternalDiscovery {
    ZaloDiscoveryReport report;
    std::vector<InternalRoot> roots;
    bool hadAccessDenied = false;
    bool hadInvalidRoot = false;
    bool hadPartial = false;
    bool configUnavailable = false;
    bool cancelled = false;
};

struct InternalObservation {
    std::size_t entryIndex = kNoIndex;
    CleanupIdentity identity{};
    bool identityKnown = false;
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    std::uint32_t filesystemLinks = 0;
    bool linkCountKnown = false;
    ByteSize logicalBytes = 0;
    bool logicalKnown = false;
    ByteSize pathVisibleLogicalBytes = 0;
    bool pathVisibleLogicalKnown = false;
    EvidenceState consistency = EvidenceState::Unknown;
};

struct InternalEntry {
    std::wstring nativePath;
    std::wstring nativeRelative;
    std::wstring categoryKey;
    ZaloEntryKind kind = ZaloEntryKind::File;
    ByteSize logicalBytes = 0;
    bool logicalKnown = false;
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    bool identityKnown = false;
    bool reparsePoint = false;
    bool contentSkipped = false;
    std::optional<FileIdentity> contentIdentity;
    std::wstring contentCanonicalPath;
    std::optional<ZaloContentResult> contentIdentification;
    EvidenceState consistency = EvidenceState::Unknown;
    std::size_t observationIndex = kNoIndex;

    std::uint32_t filesystemLinks = 0;
    bool linkCountKnown = false;
    std::uint32_t observedPathCount = 0;
    std::optional<ByteSize> singlePathReleaseBytes;
    std::optional<ByteSize> allObservedPathReleaseBytes;
    bool hardLinkAlias = false;
    std::uint64_t lastWriteTicks = 0;
    std::string reportEntryId;
};

struct InternalAccount {
    AccountPath path;
    std::string rootAlias;
    std::string accountAlias;
    ZaloAccountReport report;
    std::vector<InternalEntry> entries;
    std::vector<InternalObservation> observations;
    bool cancelled = false;
};

struct ObservationRef {
    InternalObservation* observation = nullptr;
    InternalEntry* entry = nullptr;
    InternalAccount* account = nullptr;
};

struct IdentityKey {
    CleanupIdentitySource source = CleanupIdentitySource::Unavailable;
    std::uint64_t volumeSerial = 0;
    std::array<std::uint8_t, 16> fileId128{};
    std::uint64_t fileIndex64 = 0;

    [[nodiscard]] bool operator==(const IdentityKey& other) const noexcept
    {
        return source == other.source && volumeSerial == other.volumeSerial &&
               fileId128 == other.fileId128 && fileIndex64 == other.fileIndex64;
    }
};

struct IdentityKeyHash {
    [[nodiscard]] std::size_t operator()(const IdentityKey& key) const noexcept
    {
        std::size_t value = static_cast<std::size_t>(key.source);
        value ^= static_cast<std::size_t>(key.volumeSerial * 1315423911ULL);
        value ^= static_cast<std::size_t>(key.fileIndex64 * 2654435761ULL);
        for (const std::uint8_t byte : key.fileId128) {
            value = (value * 16777619U) ^ byte;
        }
        return value;
    }
};

CleanupIdentity identityFromFile(const FileIdentity& identity)
{
    if (identity.fileId128Known) {
        return makeFileId128Identity(identity.volumeSerial, identity.fileId128);
    }
    if (identity.fileIndex64Known) {
        return makeFileIndex64FallbackIdentity(identity.volumeSerial,
                                               identity.fileId);
    }
    return {};
}

IdentityKey makeIdentityKey(const CleanupIdentity& identity)
{
    IdentityKey key;
    key.source = identity.source;
    key.volumeSerial = identity.volumeSerial;
    key.fileId128 = identity.fileId128;
    key.fileIndex64 = identity.fileIndex64;
    return key;
}

struct IdentityGroup {
    CleanupIdentity identity{};
    std::vector<ObservationRef*> refs;
    std::optional<ByteSize> allocatedBytes;
    ByteSize logicalBytes = 0;
    bool logicalKnown = false;
    bool allocationKnown = false;
    std::uint32_t filesystemLinks = 0;
    bool linkCountKnown = false;
    bool inconsistent = false;
    bool changed = false;
    bool unknown = false;
};

enum class CandidateResolutionStatus {
    Unresolved,
    Resolved,
    AccessDenied,
    Changed,
    Error,
    Cancelled
};

enum class WindowsPathValidationStatus {
    Valid,
    AccessDenied,
    Changed,
    Reparse,
    Error,
    Cancelled
};

enum class RootShapeStatus {
    NoMatch,
    Matched,
    AccessDenied,
    Changed,
    Error,
    Cancelled
};

struct RootCandidate {
    std::wstring path;
    std::wstring sortKey;
    bool defaultCandidate = false;
    bool userFacingRoot = false;
    bool explicitCandidate = false;
    CandidateResolutionStatus resolution = CandidateResolutionStatus::Unresolved;
    ValidatedWindowsPath resolvedPath;
};

struct RootShapeResult {
    RootShapeStatus status = RootShapeStatus::NoMatch;
    std::vector<AccountPath> accounts;
    bool partialEvidence = false;
    bool accessDeniedEvidence = false;
};

std::wstring joinNative(std::wstring_view parent, std::wstring_view child)
{
    if (parent.empty()) {
        return std::wstring(child);
    }
    if (child.empty()) {
        return std::wstring(parent);
    }
    std::wstring result(parent);
    if (result.back() != L'\\' && result.back() != L'/') {
        result.push_back(L'\\');
    }
    if (child.front() == L'\\' || child.front() == L'/') {
        result.append(child.substr(1));
    } else {
        result.append(child);
    }
    return result;
}

std::wstring lowerCopy(std::wstring_view value)
{
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        result.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return result;
}

int compareInsensitive(std::wstring_view left, std::wstring_view right)
{
    const std::size_t count = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const wchar_t a = static_cast<wchar_t>(std::towlower(left[i]));
        const wchar_t b = static_cast<wchar_t>(std::towlower(right[i]));
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
    }
    if (left.size() < right.size()) {
        return -1;
    }
    if (left.size() > right.size()) {
        return 1;
    }
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

bool equalInsensitive(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (static_cast<wchar_t>(std::towlower(left[i])) !=
            static_cast<wchar_t>(std::towlower(right[i]))) {
            return false;
        }
    }
    return true;
}

bool isReservedDeviceBase(std::wstring_view name)
{
    const std::size_t dot = name.find(L'.');
    std::wstring_view base = name.substr(0, dot);
    while (!base.empty() && (base.back() == L'.' || base.back() == L' ')) {
        base.remove_suffix(1);
    }
    if (equalInsensitive(base, L"CON") || equalInsensitive(base, L"PRN") ||
        equalInsensitive(base, L"AUX") || equalInsensitive(base, L"NUL") ||
        equalInsensitive(base, L"CLOCK$") ||
        equalInsensitive(base, L"CONIN$") ||
        equalInsensitive(base, L"CONOUT$")) {
        return true;
    }
    if (base.size() == 4 &&
        (equalInsensitive(base.substr(0, 3), L"COM") ||
         equalInsensitive(base.substr(0, 3), L"LPT"))) {
        const wchar_t suffix = base[3];
        return (suffix >= L'1' && suffix <= L'9') || suffix == L'¹' ||
               suffix == L'²' || suffix == L'³';
    }
    return false;
}

bool isSafeLeaf(std::wstring_view name)
{
    if (name.empty() || name == L"." || name == L".." ||
        name.back() == L'.' || name.back() == L' ') {
        return false;
    }
    for (const wchar_t ch : name) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'<' ||
            ch == L'>' || ch == L'"' || ch == L'|' || ch == L'?' ||
            ch == L'*' || ch < 0x20) {
            return false;
        }
    }
    return !isReservedDeviceBase(name);
}

bool appendNativeRelative(std::wstring_view parent, std::wstring_view leaf,
                          std::wstring& out)
{
    if (!isSafeLeaf(leaf)) {
        return false;
    }
    out.assign(parent);
    if (!out.empty()) {
        out.push_back(L'\\');
    }
    out.append(leaf);
    return true;
}

std::wstring lastNativeComponent(std::wstring_view path)
{
    std::size_t end = path.size();
    while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) {
        --end;
    }
    const std::size_t start = path.substr(0, end).find_last_of(L"\\/");
    return std::wstring(path.substr(start == std::wstring_view::npos ? 0 : start + 1,
                                    end - (start == std::wstring_view::npos ? 0 : start + 1)));
}

bool isNamespaceSeparator(wchar_t ch) noexcept
{
    return ch == L'\\' || ch == L'/';
}

bool hasRejectedNamespacePrefix(std::wstring_view path) noexcept
{
    if (path.size() >= 4U && isNamespaceSeparator(path[0]) &&
        isNamespaceSeparator(path[1]) &&
        (path[2] == L'?' || path[2] == L'.') &&
        isNamespaceSeparator(path[3])) {
        return true;
    }
    if (path.size() >= 4U && isNamespaceSeparator(path[0]) &&
        path[1] == L'?' && path[2] == L'?' &&
        isNamespaceSeparator(path[3])) {
        return true;
    }
    return path.size() >= 5U && isNamespaceSeparator(path[0]) &&
           isNamespaceSeparator(path[1]) && path[2] == L'?' &&
           path[3] == L'?' && isNamespaceSeparator(path[4]);
}

bool isAbsoluteNativePath(std::wstring_view path)
{
    if (path.find(L'\0') != std::wstring_view::npos ||
        hasRejectedNamespacePrefix(path)) {
        // Device/extended namespaces preserve syntax that ordinary Win32 opens
        // later normalize differently. V1 rejects them rather than risk opening
        // a different object during recursive traversal. Check both slash
        // directions before parsing ordinary drive/UNC syntax.
        return false;
    }

    std::size_t position = 0;
    bool unc = false;
    if (path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) {
        position = 3;
    } else if (path.size() >= 2 &&
               ((path[0] == L'\\' && path[1] == L'\\') ||
                (path[0] == L'/' && path[1] == L'/'))) {
        position = 2;
        unc = true;
    } else {
        return false;
    }

    std::size_t componentCount = 0;
    while (position < path.size()) {
        while (position < path.size() &&
               (path[position] == L'\\' || path[position] == L'/')) {
            ++position;
        }
        if (position >= path.size()) {
            break;
        }
        const std::size_t end = path.find_first_of(L"\\/", position);
        const std::size_t length =
            (end == std::wstring_view::npos ? path.size() : end) - position;
        if (!isSafeLeaf(path.substr(position, length))) {
            return false;
        }
        ++componentCount;
        if (end == std::wstring_view::npos) {
            break;
        }
        position = end + 1U;
    }
    return !unc || componentCount >= 2U;
}

std::wstring canonicalSortPath(const std::wstring& path)
{
    std::wstring canonical = canonicalWin32Path(path);
    if (canonical.empty()) {
        canonical = normalizePathForPolicy(path);
    }
    return lowerCopy(canonical);
}

std::wstring knownFolderPath(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, 0, nullptr, &path)) || path == nullptr) {
        return {};
    }
    std::wstring result(path);
    ::CoTaskMemFree(path);
    return result;
}

UniqueHandle openNoFollow(const std::wstring& path, bool directory,
                          bool readData = false)
{
    if (path.empty()) {
        return {};
    }
    DWORD access = FILE_READ_ATTRIBUTES;
    if (directory) {
        access |= FILE_LIST_DIRECTORY;
    } else if (readData) {
        access |= FILE_READ_DATA;
    }
    const HANDLE handle = ::CreateFileW(
        path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    return UniqueHandle(handle);
}

bool isAccessError(DWORD error) noexcept
{
    return error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD;
}

bool isTransientUnavailableError(DWORD error) noexcept
{
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}

DirectoryReadStatus mapDirectoryError(DWORD error) noexcept
{
    if (isAccessError(error)) {
        return DirectoryReadStatus::AccessDenied;
    }
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_INVALID_NAME) {
        return DirectoryReadStatus::NotFound;
    }
    return DirectoryReadStatus::Error;
}

bool sameIdentity(const FileIdentity& left, const FileIdentity& right) noexcept
{
    return left.fileId128Known == right.fileId128Known &&
           left.fileIndex64Known == right.fileIndex64Known &&
           left.fileId128 == right.fileId128 && left.fileId == right.fileId &&
           left.volumeSerial == right.volumeSerial &&
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
           left.attributes == right.attributes;
}

// Component binding proves that the same filesystem object still occupies a
// path. Directory size/allocation/link count and write time are deliberately
// excluded: unrelated activity can legitimately change metadata on shared
// ancestors such as the system TEMP directory while retained handles remain
// bound to the same object.
bool samePathObject(const FileIdentity& left,
                    const FileIdentity& right) noexcept
{
    if (left.isDirectory != right.isDirectory) {
        return false;
    }
    if (left.fileId128Known && right.fileId128Known) {
        return left.volumeSerial == right.volumeSerial &&
               left.fileId128 == right.fileId128;
    }
    return left.fileIndex64Known && right.fileIndex64Known &&
           left.volumeSerial == right.volumeSerial &&
           left.fileId == right.fileId;
}

EvidenceState evidenceState(const FileIdentity& identity)
{
    if (!identity.observationConsistent) {
        return EvidenceState::Inconsistent;
    }
    if (!identity.identityKnown || !identity.logicalSizeKnown ||
        !identity.linkCountKnown) {
        return EvidenceState::Unknown;
    }
    return EvidenceState::Consistent;
}

std::wstring finalPathFromHandle(HANDLE handle)
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = ::GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED);
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            std::wstring path(buffer.data(), length);
            return canonicalWin32Path(path);
        }
        if (buffer.size() >= 32768U) {
            return {};
        }
        buffer.resize(buffer.size() * 2U);
    }
}

std::vector<std::wstring> absolutePathPrefixes(std::wstring_view path)
{
    if (!isAbsoluteNativePath(path)) {
        return {};
    }
    const std::wstring normalized = normalizePathForPolicy(path);
    if (normalized.empty()) {
        return {};
    }

    std::vector<std::wstring> prefixes;
    std::size_t position = 0;
    std::wstring current;
    if (normalized.size() >= 3U && std::iswalpha(normalized[0]) &&
        normalized[1] == L':' && normalized[2] == L'\\') {
        current = normalized.substr(0, 3U);
        prefixes.push_back(current);
        position = 3U;
    } else if (normalized.size() >= 2U && normalized[0] == L'\\' &&
               normalized[1] == L'\\') {
        position = 2U;
        const auto nextComponent = [&normalized](std::size_t& cursor) {
            while (cursor < normalized.size() && normalized[cursor] == L'\\') {
                ++cursor;
            }
            if (cursor >= normalized.size()) {
                return std::wstring{};
            }
            const std::size_t end = normalized.find(L'\\', cursor);
            const std::size_t length =
                (end == std::wstring::npos ? normalized.size() : end) - cursor;
            std::wstring component = normalized.substr(cursor, length);
            cursor = end == std::wstring::npos ? normalized.size() : end;
            return component;
        };
        const std::wstring server = nextComponent(position);
        const std::wstring share = nextComponent(position);
        if (!isSafeLeaf(server) || !isSafeLeaf(share)) {
            return {};
        }
        current = L"\\\\" + server + L"\\" + share;
        prefixes.push_back(current);
    } else {
        return {};
    }

    while (position < normalized.size()) {
        while (position < normalized.size() && normalized[position] == L'\\') {
            ++position;
        }
        if (position >= normalized.size()) {
            break;
        }
        const std::size_t end = normalized.find(L'\\', position);
        const std::size_t length =
            (end == std::wstring::npos ? normalized.size() : end) - position;
        const std::wstring component = normalized.substr(position, length);
        if (!isSafeLeaf(component)) {
            return {};
        }
        current = joinNative(current, component);
        prefixes.push_back(current);
        position = end == std::wstring::npos ? normalized.size() : end;
    }
    return prefixes;
}

UniqueHandle openPathComponent(std::wstring_view path, bool directory,
                               bool readData, bool listDirectory)
{
    if (path.empty()) {
        return {};
    }
    DWORD access = FILE_READ_ATTRIBUTES;
    if (directory && listDirectory) {
        access |= FILE_LIST_DIRECTORY;
    }
    if (!directory && readData) {
        access |= FILE_READ_DATA;
    }
    const std::wstring nativePath(path);
    const HANDLE handle = ::CreateFileW(
        nativePath.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    return UniqueHandle(handle);
}

std::optional<FileIdentity> stablePathObservation(HANDLE handle)
{
    const auto first = queryFileIdentityFromHandle(handle);
    const auto second = queryFileIdentityFromHandle(handle);
    if (!first || !second ||
        !isIdentityAvailable(identityFromFile(*first)) ||
        !isIdentityAvailable(identityFromFile(*second)) ||
        !samePathObject(*first, *second)) {
        return std::nullopt;
    }
    return second;
}

WindowsPathValidationStatus mapPathOpenFailure(DWORD error) noexcept
{
    if (isAccessError(error)) {
        return WindowsPathValidationStatus::AccessDenied;
    }
    if (isTransientUnavailableError(error)) {
        return WindowsPathValidationStatus::Changed;
    }
    return WindowsPathValidationStatus::Error;
}

WindowsPathValidationStatus revalidatePathComponent(
    HANDLE retainedHandle, const FileIdentity& expectedObservation,
    std::wstring_view requestedPath, std::wstring_view expectedFinalPath,
    bool expectDirectory, bool readData, bool listDirectory,
    const Cancellation& cancellation)
{
    if (cancellation.requested()) {
        return WindowsPathValidationStatus::Cancelled;
    }
    if (retainedHandle == nullptr || retainedHandle == INVALID_HANDLE_VALUE) {
        return WindowsPathValidationStatus::Changed;
    }

    const auto retainedObservation = stablePathObservation(retainedHandle);
    if (retainedObservation &&
        (retainedObservation->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return WindowsPathValidationStatus::Reparse;
    }
    if (!retainedObservation ||
        !samePathObject(expectedObservation, *retainedObservation)) {
        return WindowsPathValidationStatus::Changed;
    }
    const std::wstring retainedFinalPath = finalPathFromHandle(retainedHandle);
    if (retainedFinalPath.empty() || retainedFinalPath != expectedFinalPath) {
        return WindowsPathValidationStatus::Changed;
    }

    UniqueHandle reopened = openPathComponent(
        requestedPath, expectDirectory, readData, listDirectory);
    if (!reopened.valid()) {
        return mapPathOpenFailure(::GetLastError());
    }
    const auto reopenedObservation = stablePathObservation(reopened.get());
    if (!reopenedObservation) {
        return WindowsPathValidationStatus::Changed;
    }
    if ((reopenedObservation->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return WindowsPathValidationStatus::Reparse;
    }
    if (reopenedObservation->isDirectory != expectDirectory) {
        return WindowsPathValidationStatus::Error;
    }
    const std::wstring reopenedFinalPath = finalPathFromHandle(reopened.get());
    if (reopenedFinalPath.empty() || reopenedFinalPath != expectedFinalPath) {
        return WindowsPathValidationStatus::Changed;
    }
    if (!samePathObject(expectedObservation, *reopenedObservation)) {
        return WindowsPathValidationStatus::Changed;
    }
    return WindowsPathValidationStatus::Valid;
}

WindowsPathValidationStatus revalidateWindowsPath(
    const ValidatedWindowsPath& path, const Cancellation& cancellation)
{
    for (const auto& component : path.parentComponents) {
        const WindowsPathValidationStatus status = revalidatePathComponent(
            component.handle.get(), component.observation, component.path,
            component.resolvedFinalPath, true, false, false, cancellation);
        if (status != WindowsPathValidationStatus::Valid) {
            return status;
        }
    }
    return revalidatePathComponent(
        path.finalHandle.get(), path.finalObservation, path.requestedPath,
        path.resolvedFinalPath, path.finalDirectory, path.finalReadData,
        path.finalDirectory, cancellation);
}

WindowsPathValidationStatus validateWindowsPath(
    std::wstring_view input, bool finalDirectory, bool finalReadData,
    const Cancellation& cancellation, ValidatedWindowsPath& result)
{
    result = {};
    if (cancellation.requested()) {
        return WindowsPathValidationStatus::Cancelled;
    }
    const std::vector<std::wstring> prefixes = absolutePathPrefixes(input);
    if (prefixes.empty()) {
        return WindowsPathValidationStatus::Error;
    }

    result.requestedPath = normalizePathForPolicy(input);
    result.finalDirectory = finalDirectory;
    result.finalReadData = finalReadData;
    result.parentComponents.reserve(prefixes.size() - 1U);
    for (std::size_t index = 0; index < prefixes.size(); ++index) {
        if (cancellation.requested()) {
            result = {};
            return WindowsPathValidationStatus::Cancelled;
        }
        const bool isFinal = index + 1U == prefixes.size();
        const bool expectDirectory = !isFinal || finalDirectory;
        UniqueHandle handle = openPathComponent(
            prefixes[index], expectDirectory, isFinal && finalReadData,
            isFinal && finalDirectory);
        if (!handle.valid()) {
            const WindowsPathValidationStatus status =
                mapPathOpenFailure(::GetLastError());
            result = {};
            return status;
        }
        const auto observation = stablePathObservation(handle.get());
        if (!observation) {
            result = {};
            return WindowsPathValidationStatus::Changed;
        }
        if ((observation->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            result = {};
            return WindowsPathValidationStatus::Reparse;
        }
        if (observation->isDirectory != expectDirectory) {
            result = {};
            return WindowsPathValidationStatus::Error;
        }
        const std::wstring finalPath = finalPathFromHandle(handle.get());
        const std::wstring secondFinalPath = finalPathFromHandle(handle.get());
        if (finalPath.empty() || secondFinalPath.empty() ||
            finalPath != secondFinalPath) {
            result = {};
            return WindowsPathValidationStatus::Changed;
        }

        if (isFinal) {
            result.finalObservation = *observation;
            result.resolvedFinalPath = finalPath;
            result.finalHandle = std::move(handle);
        } else {
            RetainedPathComponent component;
            component.path = prefixes[index];
            component.resolvedFinalPath = finalPath;
            component.observation = *observation;
            component.handle = std::move(handle);
            result.parentComponents.push_back(std::move(component));
        }
    }

    const WindowsPathValidationStatus status =
        revalidateWindowsPath(result, cancellation);
    if (status != WindowsPathValidationStatus::Valid) {
        result = {};
    }
    return status;
}

bool directoryMatchesExpected(HANDLE directory,
                               const FileIdentity& expectedIdentity,
                               std::wstring_view expectedFinalPath)
{
    if (directory == nullptr || directory == INVALID_HANDLE_VALUE ||
        expectedFinalPath.empty() || !expectedIdentity.observationConsistent ||
        !isIdentityAvailable(identityFromFile(expectedIdentity))) {
        return false;
    }
    const auto current = queryFileIdentityFromHandle(directory);
    if (!current || !current->observationConsistent ||
        !isIdentityAvailable(identityFromFile(*current)) ||
        !sameIdentity(expectedIdentity, *current)) {
        return false;
    }
    const std::wstring currentFinalPath = finalPathFromHandle(directory);
    // Both values are already handle-resolved long canonical paths. Keep case
    // significant here: on a case-sensitive directory, a case-only rename or
    // replacement must invalidate the retained binding. 8.3 aliases were
    // accepted earlier, during initial canonicalization.
    return !currentFinalPath.empty() && currentFinalPath == expectedFinalPath;
}

bool openedObjectMatchesListing(const RawDirectoryEntry& listed,
                                const FileIdentity& opened,
                                const FileIdentity& parent) noexcept
{
    return listed.listedFileIdKnown && opened.fileIndex64Known &&
           listed.listedFileId == opened.fileId && parent.volumeSerial != 0 &&
           opened.volumeSerial == parent.volumeSerial;
}

RawDirectoryListing listImmediateChildren(HANDLE directory,
                                          const FileIdentity& before,
                                          const Cancellation& cancellation)
{
    RawDirectoryListing result;
    if (directory == nullptr || directory == INVALID_HANDLE_VALUE) {
        return result;
    }
    if (cancellation.requested()) {
        result.status = DirectoryReadStatus::Cancelled;
        return result;
    }

    const auto initial = queryFileIdentityFromHandle(directory);
    if (!initial || !initial->observationConsistent ||
        !sameIdentity(before, *initial)) {
        result.status = DirectoryReadStatus::Changed;
        return result;
    }

    std::vector<std::byte> buffer(kDirectoryBufferBytes);
    for (;;) {
        if (cancellation.requested()) {
            result.status = DirectoryReadStatus::Cancelled;
            result.entries.clear();
            return result;
        }
        const BOOL ok = ::GetFileInformationByHandleEx(
            directory, FileIdBothDirectoryInfo, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (ok == FALSE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_NO_MORE_FILES) {
                break;
            }
            if (error == ERROR_INSUFFICIENT_BUFFER &&
                buffer.size() < kMaximumDirectoryBufferBytes) {
                buffer.resize(buffer.size() * 2U);
                continue;
            }
            result.status = mapDirectoryError(error);
            result.entries.clear();
            return result;
        }

        std::size_t offset = 0;
        for (;;) {
            if (cancellation.requested()) {
                result.status = DirectoryReadStatus::Cancelled;
                result.entries.clear();
                return result;
            }
            if (offset + offsetof(FILE_ID_BOTH_DIR_INFO, FileName) >
                buffer.size()) {
                result.status = DirectoryReadStatus::Error;
                result.entries.clear();
                return result;
            }
            const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
                buffer.data() + offset);
            if (info->FileNameLength % sizeof(wchar_t) != 0 ||
                info->FileNameLength >
                    buffer.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) {
                result.status = DirectoryReadStatus::Error;
                result.entries.clear();
                return result;
            }
            const std::wstring name(
                info->FileName,
                info->FileNameLength / static_cast<DWORD>(sizeof(wchar_t)));
            if (name == L"." || name == L"..") {
                // FILE_ID_BOTH_DIR_INFO may include navigation entries. They
                // are expected enumeration metadata, not unsafe user leaves.
            } else if (!isSafeLeaf(name)) {
                ++result.unsafeEntryCount;
            } else {
                RawDirectoryEntry entry;
                entry.name = name;
                entry.attributes = info->FileAttributes;
                entry.listedFileId =
                    static_cast<std::uint64_t>(info->FileId.QuadPart);
                entry.listedFileIdKnown = entry.listedFileId != 0;
                entry.directory =
                    (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                entry.reparse =
                    (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                if (!entry.directory && !entry.reparse &&
                    info->EndOfFile.QuadPart >= 0) {
                    entry.listedLogicalBytes = static_cast<ByteSize>(
                        info->EndOfFile.QuadPart);
                }
                result.entries.push_back(std::move(entry));
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            if (info->NextEntryOffset < offsetof(FILE_ID_BOTH_DIR_INFO, FileName) ||
                info->NextEntryOffset > buffer.size() - offset) {
                result.status = DirectoryReadStatus::Error;
                result.entries.clear();
                return result;
            }
            offset += info->NextEntryOffset;
        }
    }

    const auto after = queryFileIdentityFromHandle(directory);
    if (!after || !after->observationConsistent ||
        !sameIdentity(before, *after)) {
        result.status = DirectoryReadStatus::Changed;
        result.entries.clear();
        return result;
    }
    try {
        std::sort(
            result.entries.begin(), result.entries.end(),
            [&cancellation](const RawDirectoryEntry& left,
                            const RawDirectoryEntry& right) {
                if (cancellation.requested()) {
                    throw DirectorySortCancelled{};
                }
                const int insensitive = compareInsensitive(left.name, right.name);
                if (insensitive != 0) {
                    return insensitive < 0;
                }
                return left.name < right.name;
            });
    } catch (const DirectorySortCancelled&) {
        result.status = DirectoryReadStatus::Cancelled;
        result.entries.clear();
        return result;
    }
    if (cancellation.requested()) {
        result.status = DirectoryReadStatus::Cancelled;
        result.entries.clear();
        return result;
    }
    result.status = DirectoryReadStatus::Ok;
    return result;
}

bool isGenericNonAccountDirectory(std::wstring_view name)
{
    static constexpr std::array<std::wstring_view, 14> names = {
        L"Chromium",       L"Chrome",      L"Edge",       L"Firefox",
        L"Config",         L"config",      L"Logs",        L"logs",
        L"Log",            L"log",         L"Profile",     L"Profiles",
        L"Local Storage",  L"IndexedDB"};
    for (const auto candidate : names) {
        if (equalInsensitive(name, candidate)) {
            return true;
        }
    }
    return false;
}

void addAccountPath(std::vector<AccountPath>& accounts, std::wstring path)
{
    std::wstring normalized = normalizePathForPolicy(path);
    const std::wstring key = canonicalSortPath(normalized);
    if (normalized.empty() || key.empty()) {
        return;
    }
    accounts.push_back({std::move(normalized), key});
}

RootShapeResult discoverRootShape(const RootCandidate& candidate,
                                  bool discoverAccounts,
                                  const Cancellation& cancellation)
{
    RootShapeResult result;
    if (cancellation.requested()) {
        result.status = RootShapeStatus::Cancelled;
        return result;
    }

    switch (candidate.resolution) {
    case CandidateResolutionStatus::AccessDenied:
        result.status = RootShapeStatus::AccessDenied;
        return result;
    case CandidateResolutionStatus::Changed:
        result.status = RootShapeStatus::Changed;
        return result;
    case CandidateResolutionStatus::Error:
    case CandidateResolutionStatus::Unresolved:
        result.status = RootShapeStatus::Error;
        return result;
    case CandidateResolutionStatus::Cancelled:
        result.status = RootShapeStatus::Cancelled;
        return result;
    case CandidateResolutionStatus::Resolved:
        break;
    }
    if (!candidate.resolvedPath.finalHandle.valid()) {
        result.status = RootShapeStatus::Changed;
        return result;
    }

    const WindowsPathValidationStatus validationStatus =
        revalidateWindowsPath(candidate.resolvedPath, cancellation);
    switch (validationStatus) {
    case WindowsPathValidationStatus::Valid:
        break;
    case WindowsPathValidationStatus::AccessDenied:
        result.status = RootShapeStatus::AccessDenied;
        return result;
    case WindowsPathValidationStatus::Changed:
        result.status = RootShapeStatus::Changed;
        return result;
    case WindowsPathValidationStatus::Reparse:
    case WindowsPathValidationStatus::Error:
        result.status = RootShapeStatus::Error;
        return result;
    case WindowsPathValidationStatus::Cancelled:
        result.status = RootShapeStatus::Cancelled;
        return result;
    }

    const auto rootIdentity = stablePathObservation(
        candidate.resolvedPath.finalHandle.get());
    if (!rootIdentity ||
        !samePathObject(candidate.resolvedPath.finalObservation, *rootIdentity) ||
        !rootIdentity->isDirectory ||
        (rootIdentity->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.status = RootShapeStatus::Changed;
        return result;
    }
    const std::wstring rootFinalPath =
        finalPathFromHandle(candidate.resolvedPath.finalHandle.get());
    if (rootFinalPath.empty() || rootFinalPath != candidate.path ||
        rootFinalPath != candidate.resolvedPath.resolvedFinalPath) {
        result.status = RootShapeStatus::Changed;
        return result;
    }

    const std::wstring& basePath = candidate.resolvedPath.resolvedFinalPath;
    const std::wstring leaf = lastNativeComponent(basePath);
    if (equalInsensitive(leaf, kZaloDownloadsName) ||
        (candidate.defaultCandidate && candidate.userFacingRoot &&
         equalInsensitive(leaf, kReceivedFilesName))) {
        addAccountPath(result.accounts, basePath);
        result.status = RootShapeStatus::Matched;
        return result;
    }

    const RawDirectoryListing rootListing = listImmediateChildren(
        candidate.resolvedPath.finalHandle.get(), *rootIdentity, cancellation);
    if (rootListing.status == DirectoryReadStatus::Cancelled) {
        result.status = RootShapeStatus::Cancelled;
        return result;
    }
    if (rootListing.status == DirectoryReadStatus::AccessDenied) {
        result.status = RootShapeStatus::AccessDenied;
        return result;
    }
    if (rootListing.status == DirectoryReadStatus::Changed) {
        result.status = RootShapeStatus::Changed;
        return result;
    }
    if (rootListing.status != DirectoryReadStatus::Ok) {
        result.status = RootShapeStatus::Error;
        return result;
    }
    if (rootListing.unsafeEntryCount != 0) {
        result.partialEvidence = true;
    }

    for (const auto& entry : rootListing.entries) {
        if (entry.directory && !entry.reparse &&
            equalInsensitive(entry.name, kZaloDownloadsName)) {
            addAccountPath(result.accounts, joinNative(candidate.path, entry.name));
            result.status = RootShapeStatus::Matched;
            return result;
        }
    }

    if (discoverAccounts && !candidate.userFacingRoot &&
        !candidate.path.empty()) {
        // A media root is only expanded one level. Unknown children are never
        // themselves treated as accounts.
        for (const auto& entry : rootListing.entries) {
            if (cancellation.requested()) {
                result.status = RootShapeStatus::Cancelled;
                return result;
            }
            if (!entry.directory || entry.reparse ||
                isGenericNonAccountDirectory(entry.name)) {
                continue;
            }
            const std::wstring childPath = joinNative(candidate.path, entry.name);
            UniqueHandle child = openNoFollow(childPath, true);
            if (!child.valid()) {
                result.partialEvidence = true;
                if (isAccessError(::GetLastError())) {
                    result.accessDeniedEvidence = true;
                }
                continue;
            }
            const auto childIdentity = queryFileIdentityFromHandle(child.get());
            if (!childIdentity || !childIdentity->observationConsistent ||
                !childIdentity->isDirectory ||
                (childIdentity->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                result.partialEvidence = true;
                continue;
            }
            const RawDirectoryListing childListing = listImmediateChildren(
                child.get(), *childIdentity, cancellation);
            if (childListing.status == DirectoryReadStatus::Cancelled) {
                result.status = RootShapeStatus::Cancelled;
                return result;
            }
            if (childListing.status == DirectoryReadStatus::Changed) {
                result.partialEvidence = true;
                continue;
            }
            if (childListing.status == DirectoryReadStatus::AccessDenied) {
                result.partialEvidence = true;
                result.accessDeniedEvidence = true;
                continue;
            }
            if (childListing.status != DirectoryReadStatus::Ok) {
                result.partialEvidence = true;
                continue;
            }
            if (childListing.unsafeEntryCount != 0) {
                result.partialEvidence = true;
            }
            for (const auto& nested : childListing.entries) {
                if (nested.directory && !nested.reparse &&
                    equalInsensitive(nested.name, kZaloDownloadsName)) {
                    addAccountPath(result.accounts,
                                   joinNative(childPath, nested.name));
                    break;
                }
            }
        }
    }

    if (!result.accounts.empty()) {
        result.status = RootShapeStatus::Matched;
    } else if (result.accessDeniedEvidence) {
        result.status = RootShapeStatus::AccessDenied;
    } else if (result.partialEvidence) {
        result.status = RootShapeStatus::Changed;
    } else {
        result.status = RootShapeStatus::NoMatch;
    }
    return result;
}

void addRootCandidate(std::vector<RootCandidate>& candidates, std::wstring path,
                      bool defaultCandidate, bool userFacingRoot,
                      bool explicitCandidate = false)
{
    if (path.empty() || !isAbsoluteNativePath(path)) {
        return;
    }
    std::wstring normalized = normalizePathForPolicy(path);
    const std::wstring key = canonicalSortPath(normalized);
    if (normalized.empty() || key.empty()) {
        return;
    }
    for (auto& existing : candidates) {
        if (existing.path == normalized) {
            existing.defaultCandidate = existing.defaultCandidate || defaultCandidate;
            existing.userFacingRoot = existing.userFacingRoot || userFacingRoot;
            existing.explicitCandidate =
                existing.explicitCandidate || explicitCandidate;
            return;
        }
    }
    RootCandidate candidate;
    candidate.path = std::move(normalized);
    candidate.sortKey = key;
    candidate.defaultCandidate = defaultCandidate;
    candidate.userFacingRoot = userFacingRoot;
    candidate.explicitCandidate = explicitCandidate;
    candidates.push_back(std::move(candidate));
}

CandidateResolutionStatus resolveRootCandidate(RootCandidate& candidate,
                                                const Cancellation& cancellation)
{
    candidate.resolvedPath = {};
    ValidatedWindowsPath validated;
    const WindowsPathValidationStatus status = validateWindowsPath(
        candidate.path, true, false, cancellation, validated);
    switch (status) {
    case WindowsPathValidationStatus::Valid:
        break;
    case WindowsPathValidationStatus::AccessDenied:
        candidate.resolution = CandidateResolutionStatus::AccessDenied;
        return candidate.resolution;
    case WindowsPathValidationStatus::Changed:
        candidate.resolution = CandidateResolutionStatus::Changed;
        return candidate.resolution;
    case WindowsPathValidationStatus::Cancelled:
        candidate.resolution = CandidateResolutionStatus::Cancelled;
        return candidate.resolution;
    case WindowsPathValidationStatus::Reparse:
    case WindowsPathValidationStatus::Error:
        candidate.resolution = CandidateResolutionStatus::Error;
        return candidate.resolution;
    }

    const CleanupIdentity cleanupIdentity =
        identityFromFile(validated.finalObservation);
    if (!isIdentityAvailable(cleanupIdentity) ||
        validated.resolvedFinalPath.empty()) {
        candidate.resolution = CandidateResolutionStatus::Changed;
        return candidate.resolution;
    }

    // Use the handle-resolved long path as the internal representative. This
    // keeps supported-shape classification deterministic for case and 8.3
    // aliases while the retained component handles preserve no-follow evidence.
    candidate.path = validated.resolvedFinalPath;
    candidate.sortKey = lowerCopy(candidate.path);
    candidate.resolvedPath = std::move(validated);
    candidate.resolution = CandidateResolutionStatus::Resolved;
    return candidate.resolution;
}

bool resolveAndMergeRootCandidates(std::vector<RootCandidate>& candidates,
                                    const Cancellation& cancellation)
{
    if (cancellation.requested()) {
        return false;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const RootCandidate& left, const RootCandidate& right) {
                  if (left.sortKey != right.sortKey) {
                      return left.sortKey < right.sortKey;
                  }
                  return left.path < right.path;
              });

    std::vector<RootCandidate> merged;
    merged.reserve(candidates.size());
    std::unordered_map<IdentityKey, std::size_t, IdentityKeyHash> owners;
    owners.reserve(candidates.size());
    for (auto& candidate : candidates) {
        if (cancellation.requested()) {
            return false;
        }
        if (resolveRootCandidate(candidate, cancellation) ==
            CandidateResolutionStatus::Cancelled) {
            return false;
        }
        if (candidate.resolution == CandidateResolutionStatus::Resolved) {
            const IdentityKey key = makeIdentityKey(
                identityFromFile(candidate.resolvedPath.finalObservation));
            const auto [it, inserted] = owners.emplace(key, merged.size());
            if (!inserted) {
                RootCandidate& owner = merged[it->second];
                owner.defaultCandidate =
                    owner.defaultCandidate || candidate.defaultCandidate;
                owner.userFacingRoot =
                    owner.userFacingRoot || candidate.userFacingRoot;
                owner.explicitCandidate =
                    owner.explicitCandidate || candidate.explicitCandidate;
                continue;
            }
        }
        merged.push_back(std::move(candidate));
    }
    if (cancellation.requested()) {
        return false;
    }
    candidates = std::move(merged);
    return true;
}

const std::vector<ZaloConfigKeyPath>& productionConfigKeyPaths() noexcept
{
    // Deliberately empty until external evidence establishes exact production
    // keys. Test callers may inject an explicit allowlist through options.
    static const std::vector<ZaloConfigKeyPath> paths;
    return paths;
}

std::vector<ZaloConfigKeyPath> effectiveConfigKeyPaths(
    const ZaloDiscoveryOptions& options)
{
    std::vector<ZaloConfigKeyPath> result;
    if (!options.configJsonKeyPaths.empty()) {
        result = options.configJsonKeyPaths;
    } else if (!options.configKeyPaths.empty()) {
        for (const auto& dotted : options.configKeyPaths) {
            ZaloConfigKeyPath path;
            std::size_t start = 0;
            while (start <= dotted.size()) {
                const std::size_t dot = dotted.find('.', start);
                const std::size_t end = dot == std::string::npos ? dotted.size() : dot;
                if (end == start) {
                    path.clear();
                    break;
                }
                path.emplace_back(dotted.substr(start, end - start));
                if (dot == std::string::npos) {
                    break;
                }
                start = dot + 1;
            }
            if (!path.empty()) {
                result.push_back(std::move(path));
            }
        }
    } else {
        result = productionConfigKeyPaths();
    }
    return result;
}

bool readExactConfig(const std::wstring& path, std::string& out,
                     const Cancellation& cancellation)
{
    ValidatedWindowsPath validated;
    if (validateWindowsPath(path, false, true, cancellation, validated) !=
        WindowsPathValidationStatus::Valid) {
        return false;
    }

    const FileIdentity& before = validated.finalObservation;
    if (!before.observationConsistent || before.isDirectory ||
        !before.logicalSizeKnown || before.sizeBytes > kZaloMaxConfigBytes) {
        return false;
    }

    const std::size_t size = static_cast<std::size_t>(before.sizeBytes);
    out.assign(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        if (cancellation.requested()) {
            out.clear();
            return false;
        }
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(size - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (::ReadFile(validated.finalHandle.get(), out.data() + offset, request,
                       &read, nullptr) == FALSE ||
            read == 0) {
            out.clear();
            return false;
        }
        offset += read;
    }

    if (revalidateWindowsPath(validated, cancellation) !=
        WindowsPathValidationStatus::Valid) {
        out.clear();
        return false;
    }
    const auto after = stablePathObservation(validated.finalHandle.get());
    if (!after || !after->observationConsistent || after->isDirectory ||
        !after->logicalSizeKnown || !sameIdentity(before, *after)) {
        out.clear();
        return false;
    }
    return true;
}

const json::JsonValue* valueAtKeyPath(const json::JsonValue& root,
                                      const ZaloConfigKeyPath& path)
{
    const json::JsonValue* value = &root;
    for (const auto& component : path) {
        if (component.empty() || value == nullptr || !value->isObject()) {
            return nullptr;
        }
        value = value->get(component);
    }
    return value;
}

void addConfigCandidates(const ZaloDiscoveryOptions& options,
                         std::vector<RootCandidate>& candidates,
                         InternalDiscovery& result,
                         const Cancellation& cancellation)
{
    if (options.configPath.empty()) {
        return;
    }
    const std::vector<ZaloConfigKeyPath> keyPaths =
        effectiveConfigKeyPaths(options);
    if (keyPaths.empty()) {
        result.configUnavailable = true;
        return;
    }
    if (cancellation.requested()) {
        result.cancelled = true;
        return;
    }
    std::string document;
    if (!readExactConfig(options.configPath, document, cancellation)) {
        if (cancellation.requested()) {
            result.cancelled = true;
        } else {
            result.configUnavailable = true;
        }
        return;
    }
    const auto parsed = json::parseJson(document);
    if (!parsed.ok || !parsed.value.isObject()) {
        result.configUnavailable = true;
        return;
    }
    for (const auto& keyPath : keyPaths) {
        if (cancellation.requested()) {
            result.cancelled = true;
            return;
        }
        const json::JsonValue* value = valueAtKeyPath(parsed.value, keyPath);
        if (value == nullptr || !value->isString()) {
            continue;
        }
        const std::wstring path = wideFromUtf8(value->string);
        if (!isAbsoluteNativePath(path)) {
            continue;
        }
        UniqueHandle directory = openNoFollow(path, true);
        if (!directory.valid()) {
            result.configUnavailable = true;
            result.hadPartial = true;
            if (isAccessError(::GetLastError())) {
                result.hadAccessDenied = true;
            }
            continue;
        }
        const auto identity = queryFileIdentityFromHandle(directory.get());
        if (!identity || !identity->observationConsistent ||
            !identity->isDirectory ||
            (identity->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            result.configUnavailable = true;
            result.hadPartial = true;
            continue;
        }
        addRootCandidate(candidates, path, false, false);
    }
}

InternalDiscovery discoverInternal(const ZaloDiscoveryOptions& options,
                                    const Cancellation& cancellation)
{
    InternalDiscovery result;
    std::vector<RootCandidate> candidates;
    candidates.reserve(options.explicitRoots.size() + 3U);

    for (const auto& path : options.explicitRoots) {
        if (cancellation.requested()) {
            result.cancelled = true;
            break;
        }
        if (path.empty() || !isAbsoluteNativePath(path)) {
            ++result.report.rejectedRootCount;
            result.hadInvalidRoot = true;
            continue;
        }
        addRootCandidate(candidates, path, false, false, true);
    }

    if (!result.cancelled && options.includeDefaultRoots) {
        const std::wstring roaming =
            options.roamingAppDataRoot.empty()
                ? knownFolderPath(FOLDERID_RoamingAppData)
                : options.roamingAppDataRoot;
        const std::wstring documents =
            options.documentsRoot.empty() ? knownFolderPath(FOLDERID_Documents)
                                          : options.documentsRoot;
        if (!documents.empty()) {
            addRootCandidate(candidates,
                             joinNative(documents, kReceivedFilesName), true,
                             true);
        }
        if (!roaming.empty()) {
            addRootCandidate(candidates,
                             joinNative(joinNative(roaming, L"ZaloData"), L"media"),
                             true, false);
        }

        // Check standard relocated Zalo media locations on available fixed/removable drives
        // only when default roots are unconfigured (not overridden by tests or custom options)
        if (options.roamingAppDataRoot.empty() && options.localAppDataRoot.empty()) {
            const DWORD drivesMask = ::GetLogicalDrives();
            for (wchar_t driveLetter = L'C'; driveLetter <= L'Z'; ++driveLetter) {
                const int driveIndex = driveLetter - L'A';
                if ((drivesMask & (1 << driveIndex)) == 0) {
                    continue;
                }
                const std::wstring driveRoot = std::wstring(1, driveLetter) + L":\\";
                const UINT driveType = ::GetDriveTypeW(driveRoot.c_str());
                if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE) {
                    continue;
                }
                addRootCandidate(candidates, joinNative(driveRoot, L"Zalo Data\\media"), true, false);
                addRootCandidate(candidates, joinNative(driveRoot, L"ZaloData\\media"), true, false);
            }
        }
    }

    if (!result.cancelled) {
        addConfigCandidates(options, candidates, result, cancellation);
    }

    if (cancellation.requested()) {
        result.cancelled = true;
    }
    if (!result.cancelled &&
        !resolveAndMergeRootCandidates(candidates, cancellation)) {
        result.cancelled = true;
    }
    if (cancellation.requested()) {
        result.cancelled = true;
    }
    if (!result.cancelled) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const RootCandidate& left, const RootCandidate& right) {
                      if (left.sortKey != right.sortKey) {
                          return left.sortKey < right.sortKey;
                      }
                      return left.path < right.path;
                  });
        for (const auto& candidate : candidates) {
            if (cancellation.requested()) {
                result.cancelled = true;
                break;
            }
            const RootShapeResult shape =
                discoverRootShape(candidate, options.discoverAccounts, cancellation);
            switch (shape.status) {
            case RootShapeStatus::Matched: {
                InternalRoot root;
                root.path = candidate.path;
                root.sortKey = candidate.sortKey;
                root.accounts = shape.accounts;
                if (cancellation.requested()) {
                    result.cancelled = true;
                    break;
                }
                std::sort(root.accounts.begin(), root.accounts.end(),
                          [](const AccountPath& left, const AccountPath& right) {
                              if (left.sortKey != right.sortKey) {
                                  return left.sortKey < right.sortKey;
                              }
                              return left.path < right.path;
                          });
                result.roots.push_back(std::move(root));
                if (shape.partialEvidence) {
                    result.hadPartial = true;
                }
                if (shape.accessDeniedEvidence) {
                    result.hadAccessDenied = true;
                }
                break;
            }
            case RootShapeStatus::AccessDenied:
                result.hadAccessDenied = true;
                if (candidate.explicitCandidate || !candidate.defaultCandidate) {
                    ++result.report.rejectedRootCount;
                }
                result.hadPartial = true;
                break;
            case RootShapeStatus::Changed:
                result.hadPartial = true;
                if (candidate.explicitCandidate || !candidate.defaultCandidate) {
                    ++result.report.rejectedRootCount;
                }
                break;
            case RootShapeStatus::Error:
                if (candidate.explicitCandidate || !candidate.defaultCandidate) {
                    ++result.report.rejectedRootCount;
                    result.hadInvalidRoot = true;
                }
                break;
            case RootShapeStatus::Cancelled:
                result.cancelled = true;
                break;
            case RootShapeStatus::NoMatch:
                if (candidate.explicitCandidate || !candidate.defaultCandidate) {
                    ++result.report.rejectedRootCount;
                    result.hadInvalidRoot = true;
                }
                break;
            }
            if (result.cancelled) {
                break;
            }
        }
    }

    // Sort roots before deduplicating accounts so overlapping explicit roots
    // have deterministic ownership independent of input order.
    if (cancellation.requested()) {
        result.cancelled = true;
    }
    if (!result.cancelled) {
        std::sort(result.roots.begin(), result.roots.end(),
                  [](const InternalRoot& left, const InternalRoot& right) {
                      if (left.sortKey != right.sortKey) {
                          return left.sortKey < right.sortKey;
                      }
                      return left.path < right.path;
                  });
    }
    struct ResolvedAccountCandidate {
        std::size_t rootIndex = 0;
        AccountPath account;
        std::optional<IdentityKey> identity;
    };
    std::vector<ResolvedAccountCandidate> resolvedAccounts;
    for (std::size_t rootIndex = 0;
         rootIndex < result.roots.size() && !result.cancelled; ++rootIndex) {
        if (cancellation.requested()) {
            result.cancelled = true;
            break;
        }
        for (const auto& account : result.roots[rootIndex].accounts) {
            if (cancellation.requested()) {
                result.cancelled = true;
                break;
            }
            ValidatedWindowsPath validated;
            const WindowsPathValidationStatus validationStatus =
                validateWindowsPath(account.path, true, false, cancellation,
                                    validated);
            if (validationStatus == WindowsPathValidationStatus::Cancelled) {
                result.cancelled = true;
                break;
            }
            if (validationStatus != WindowsPathValidationStatus::Valid) {
                result.hadPartial = true;
                if (validationStatus ==
                    WindowsPathValidationStatus::AccessDenied) {
                    result.hadAccessDenied = true;
                }
                continue;
            }
            const FileIdentity& identity = validated.finalObservation;
            if (!identity.isDirectory ||
                (identity.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                !identity.observationConsistent) {
                result.hadPartial = true;
                continue;
            }
            if (cancellation.requested()) {
                result.cancelled = true;
                break;
            }
            const CleanupIdentity expectedIdentity = identityFromFile(identity);
            if (!isIdentityAvailable(expectedIdentity)) {
                result.hadPartial = true;
                continue;
            }
            const std::wstring& finalPath = validated.resolvedFinalPath;
            const std::wstring finalKey = lowerCopy(finalPath);
            if (finalPath.empty() || finalKey.empty()) {
                result.hadPartial = true;
                continue;
            }

            ResolvedAccountCandidate resolved;
            resolved.rootIndex = rootIndex;
            resolved.account = account;
            // Carry the handle-resolved long path forward. Later inspection
            // revalidates every component again, so a supported account cannot
            // be reached through an intermediate junction, symlink, case alias,
            // or 8.3 spelling that changes after discovery.
            resolved.account.path = finalPath;
            resolved.account.sortKey = finalKey;
            resolved.account.finalPath = finalPath;
            resolved.account.finalSortKey = finalKey;
            resolved.account.expectedIdentity = expectedIdentity;
            resolved.account.expectedIdentityKnown = true;
            resolved.identity = makeIdentityKey(expectedIdentity);
            resolvedAccounts.push_back(std::move(resolved));
        }
    }

    // Resolve every account first, then select ancestor roots before descendants.
    // This makes ownership independent of candidate spelling and input order.
    if (cancellation.requested()) {
        result.cancelled = true;
    }
    if (!result.cancelled) {
        std::sort(resolvedAccounts.begin(), resolvedAccounts.end(),
              [](const ResolvedAccountCandidate& left,
                 const ResolvedAccountCandidate& right) {
                  if (left.account.finalSortKey.size() !=
                      right.account.finalSortKey.size()) {
                      return left.account.finalSortKey.size() <
                             right.account.finalSortKey.size();
                  }
                  if (left.account.finalSortKey != right.account.finalSortKey) {
                      return left.account.finalSortKey < right.account.finalSortKey;
                  }
                  if (left.account.finalPath != right.account.finalPath) {
                      return left.account.finalPath < right.account.finalPath;
                  }
                  if (left.rootIndex != right.rootIndex) {
                      return left.rootIndex < right.rootIndex;
                  }
                  return left.account.path < right.account.path;
              });
    std::vector<std::vector<AccountPath>> accountsByRoot(result.roots.size());
    std::unordered_set<std::wstring> acceptedFinalPaths;
    std::unordered_set<IdentityKey, IdentityKeyHash> acceptedIdentities;
    acceptedFinalPaths.reserve(resolvedAccounts.size());
    acceptedIdentities.reserve(resolvedAccounts.size());
    for (auto& candidate : resolvedAccounts) {
        if (cancellation.requested()) {
            result.cancelled = true;
            break;
        }
        const bool duplicateIdentity =
            candidate.identity.has_value() &&
            acceptedIdentities.find(*candidate.identity) !=
                acceptedIdentities.end();
        bool overlapsResolvedAncestor =
            acceptedFinalPaths.find(candidate.account.finalPath) !=
            acceptedFinalPaths.end();
        std::size_t separator = 0;
        while (!overlapsResolvedAncestor &&
               (separator = candidate.account.finalPath.find_first_of(
                    L"\\/", separator)) != std::wstring::npos) {
            if (separator != 0) {
                const std::wstring prefix =
                    candidate.account.finalPath.substr(0, separator);
                overlapsResolvedAncestor =
                    acceptedFinalPaths.find(prefix) != acceptedFinalPaths.end();
                if (!overlapsResolvedAncestor) {
                    std::wstring prefixWithSeparator = prefix;
                    prefixWithSeparator.push_back(
                        candidate.account.finalPath[separator]);
                    overlapsResolvedAncestor =
                        acceptedFinalPaths.find(prefixWithSeparator) !=
                        acceptedFinalPaths.end();
                }
            }
            separator += 1U;
        }
        if (duplicateIdentity || overlapsResolvedAncestor) {
            continue;
        }
        acceptedFinalPaths.emplace(candidate.account.finalPath);
        if (candidate.identity.has_value()) {
            acceptedIdentities.emplace(*candidate.identity);
        }
        accountsByRoot[candidate.rootIndex].push_back(
            std::move(candidate.account));
    }
    if (cancellation.requested()) {
        result.cancelled = true;
    }
    if (!result.cancelled) {
        for (std::size_t rootIndex = 0; rootIndex < result.roots.size();
             ++rootIndex) {
            result.roots[rootIndex].accounts =
                std::move(accountsByRoot[rootIndex]);
        }
        result.roots.erase(
            std::remove_if(result.roots.begin(), result.roots.end(),
                           [](const InternalRoot& root) {
                               return root.accounts.empty();
                           }),
            result.roots.end());
    }

    if (cancellation.requested()) {
        result.cancelled = true;
    }
    std::uint64_t rootNumber = 1;
    std::uint64_t accountNumber = 1;
    for (auto& root : result.roots) {
        if (cancellation.requested()) {
            result.cancelled = true;
            break;
        }
        root.rootAlias = std::string("root-") +
                         (rootNumber < 10 ? "0" : "") +
                         std::to_string(rootNumber++);
        if (cancellation.requested()) {
            result.cancelled = true;
            break;
        }
        std::sort(root.accounts.begin(), root.accounts.end(),
                  [](const AccountPath& left, const AccountPath& right) {
                      if (left.sortKey != right.sortKey) {
                          return left.sortKey < right.sortKey;
                      }
                      return left.path < right.path;
                  });
        if (cancellation.requested()) {
            result.cancelled = true;
            break;
        }
        ZaloRootSummary summary;
        summary.rootAlias = root.rootAlias;
        for (const auto& account : root.accounts) {
            if (cancellation.requested()) {
                result.cancelled = true;
                break;
            }
            (void)account;
            summary.accountAliases.push_back(
                std::string("account-") +
                (accountNumber < 10 ? "0" : "") +
                std::to_string(accountNumber++));
        }
        if (result.cancelled) {
            break;
        }
        result.report.roots.push_back(std::move(summary));
    }
    }

    if (result.cancelled || cancellation.requested()) {
        result.cancelled = true;
        result.report.roots.clear();
        result.report.status = ZaloStorageStatus::Cancelled;
        result.report.detail = "Zalo discovery cancelled";
    } else if (result.configUnavailable) {
        result.report.status = ZaloStorageStatus::ConfigUnavailable;
        result.report.detail =
            "Configuration discovery was unavailable or not allowlisted";
    } else if (result.roots.empty()) {
        if (result.hadAccessDenied) {
            result.report.status = ZaloStorageStatus::AccessDenied;
            result.report.detail = "No readable Zalo root was found";
        } else if (result.hadPartial) {
            result.report.status = ZaloStorageStatus::Partial;
            result.report.detail = "Zalo root evidence was unavailable";
        } else if (result.hadInvalidRoot) {
            result.report.status = ZaloStorageStatus::InvalidRoot;
            result.report.detail = "No exact supported Zalo shape was found";
        } else {
            result.report.status = ZaloStorageStatus::NoRoots;
            result.report.detail = "No exact supported Zalo root was found";
        }
    } else if (result.hadPartial || result.report.rejectedRootCount > 0) {
        result.report.status = ZaloStorageStatus::Partial;
        result.report.detail = "Some Zalo root evidence was unavailable";
    } else {
        result.report.status = ZaloStorageStatus::Complete;
        result.report.detail = "Zalo roots discovered";
    }
    return result;
}

std::size_t appendEntry(InternalAccount& account, InternalEntry entry)
{
    const std::size_t index = account.entries.size();
    account.entries.push_back(std::move(entry));
    return index;
}

void appendObservation(InternalAccount& account, std::size_t entryIndex,
                       const FileIdentity* first, EvidenceState state,
                       bool changed,
                       const std::optional<ByteSize>& listedLogicalBytes)
{
    InternalObservation observation;
    observation.entryIndex = entryIndex;
    observation.consistency = state;
    if (listedLogicalBytes.has_value()) {
        observation.pathVisibleLogicalBytes = *listedLogicalBytes;
        observation.pathVisibleLogicalKnown = true;
    }
    if (first != nullptr && first->logicalSizeKnown &&
        listedLogicalBytes.has_value() && first->sizeBytes != *listedLogicalBytes) {
        // The directory snapshot and the subsequently opened object disagree.
        // Keep the snapshot in namespace totals, but exclude this changing path
        // from exact identity/allocation accounting.
        changed = true;
        observation.consistency = EvidenceState::Changed;
        account.report.complete = false;
    }
    if (first != nullptr) {
        observation.logicalBytes = first->sizeBytes;
        observation.logicalKnown = first->logicalSizeKnown && !changed;
        observation.filesystemLinks = first->numberOfLinks;
        observation.linkCountKnown = first->linkCountKnown && !changed;
        observation.allocatedBytes = first->allocatedBytes;
        observation.allocationKnown = first->allocationKnown && !changed &&
                                      first->allocatedBytes.has_value();
        if (first->identityKnown) {
            observation.identity = identityFromFile(*first);
            observation.identityKnown = isIdentityAvailable(observation.identity);
        }
    }
    account.entries[entryIndex].observationIndex = account.observations.size();
    account.entries[entryIndex].logicalBytes =
        observation.pathVisibleLogicalKnown ? observation.pathVisibleLogicalBytes
                                            : observation.logicalBytes;
    account.entries[entryIndex].logicalKnown = observation.pathVisibleLogicalKnown ||
                                               observation.logicalKnown;
    account.entries[entryIndex].allocatedBytes = observation.allocatedBytes;
    account.entries[entryIndex].allocationKnown = observation.allocationKnown;
    account.entries[entryIndex].identityKnown = observation.identityKnown;
    account.entries[entryIndex].consistency = observation.consistency;
    account.entries[entryIndex].filesystemLinks = observation.filesystemLinks;
    account.entries[entryIndex].linkCountKnown = observation.linkCountKnown;
    account.observations.push_back(std::move(observation));
}

void markRangeChanged(InternalAccount& account, std::size_t start)
{
    for (std::size_t i = start; i < account.entries.size(); ++i) {
        account.entries[i].consistency = EvidenceState::Changed;
        if (account.entries[i].observationIndex != kNoIndex) {
            account.observations[account.entries[i].observationIndex].consistency =
                EvidenceState::Changed;
        }
    }
}

EvidenceState compareFileObservation(const FileIdentity& first,
                                     const std::optional<FileIdentity>& after,
                                     bool& changed)
{
    changed = !after.has_value() || !sameIdentity(first, *after);
    if (changed) {
        return EvidenceState::Changed;
    }
    if (!first.observationConsistent || !after->observationConsistent) {
        return EvidenceState::Inconsistent;
    }
    return evidenceState(first);
}

void appendUnreadableFile(InternalAccount& account, const std::wstring& path,
                          const std::wstring& relative,
                          const std::wstring& category,
                          const std::optional<ByteSize>& listedLogicalBytes,
                          DWORD error)
{
    InternalEntry entry;
    entry.nativePath = path;
    entry.nativeRelative = relative;
    entry.categoryKey = category;
    entry.kind = ZaloEntryKind::File;
    entry.consistency = EvidenceState::Unknown;
    const std::size_t index = appendEntry(account, std::move(entry));
    appendObservation(account, index, nullptr, EvidenceState::Unknown, false,
                      listedLogicalBytes);
    account.report.complete = false;
    if (isAccessError(error)) {
        ++account.report.accessDenied;
    } else {
        ++account.report.otherErrors;
    }
}

void scanDirectory(InternalAccount& account, UniqueHandle directory,
                   const FileIdentity& before,
                   const std::wstring& expectedFinalPath,
                   const std::wstring& nativePath,
                   const std::wstring& nativeRelative,
                   const std::wstring& categoryKey, std::size_t directoryEntryIndex,
                   std::size_t depth, const Cancellation& cancellation)
{
    if (cancellation.requested()) {
        account.cancelled = true;
        return;
    }
    if (depth > kMaxTraversalDepth) {
        account.report.complete = false;
        ++account.report.otherErrors;
        return;
    }

    const std::size_t subtreeStart =
        directoryEntryIndex == kNoIndex ? account.entries.size() : directoryEntryIndex;
    ++account.report.directoriesVisited;
    if (!directoryMatchesExpected(directory.get(), before, expectedFinalPath)) {
        account.report.complete = false;
        ++account.report.otherErrors;
        ++account.report.unsafeEntriesSkipped;
        markRangeChanged(account, subtreeStart);
        return;
    }
    if (!before.observationConsistent) {
        account.report.complete = false;
    }

    const RawDirectoryListing listing =
        listImmediateChildren(directory.get(), before, cancellation);
    if (listing.status != DirectoryReadStatus::Ok) {
        account.report.complete = false;
        switch (listing.status) {
        case DirectoryReadStatus::AccessDenied:
            ++account.report.accessDenied;
            break;
        case DirectoryReadStatus::Changed:
            ++account.report.otherErrors;
            markRangeChanged(account, subtreeStart);
            break;
        case DirectoryReadStatus::Cancelled:
            account.cancelled = true;
            break;
        case DirectoryReadStatus::NotFound:
        case DirectoryReadStatus::Error:
        case DirectoryReadStatus::Ok:
            ++account.report.otherErrors;
            break;
        }
        return;
    }
    if (listing.unsafeEntryCount != 0) {
        account.report.complete = false;
        account.report.unsafeEntriesSkipped += listing.unsafeEntryCount;
    }

    for (const auto& raw : listing.entries) {
        if (cancellation.requested()) {
            account.cancelled = true;
            return;
        }
        if (!directoryMatchesExpected(directory.get(), before, expectedFinalPath)) {
            account.report.complete = false;
            ++account.report.otherErrors;
            ++account.report.unsafeEntriesSkipped;
            markRangeChanged(account, subtreeStart);
            return;
        }

        std::wstring relative;
        if (!appendNativeRelative(nativeRelative, raw.name, relative)) {
            ++account.report.unsafeEntriesSkipped;
            account.report.complete = false;
            continue;
        }
        const std::wstring childPath = joinNative(nativePath, raw.name);
        const std::wstring expectedChildFinalPath =
            joinNative(expectedFinalPath, raw.name);

        if (raw.directory) {
            InternalEntry entry;
            entry.nativePath = childPath;
            entry.nativeRelative = relative;
            entry.categoryKey = nativeRelative.empty() ? raw.name : categoryKey;
            entry.reparsePoint = raw.reparse;
            entry.contentSkipped = raw.reparse;
            entry.kind = raw.reparse ? ZaloEntryKind::ReparseDirectory
                                     : ZaloEntryKind::Directory;
            if (raw.reparse) {
                entry.consistency = EvidenceState::Unknown;
                appendEntry(account, std::move(entry));
                ++account.report.reparsePointsSkipped;
                account.report.complete = false;
                continue;
            }
            if (!raw.listedFileIdKnown) {
                entry.consistency = EvidenceState::Unknown;
                appendEntry(account, std::move(entry));
                ++account.report.unsafeEntriesSkipped;
                account.report.complete = false;
                continue;
            }

            UniqueHandle child = openNoFollow(expectedChildFinalPath, true);
            if (!child.valid()) {
                const DWORD error = ::GetLastError();
                entry.consistency = EvidenceState::Unknown;
                appendEntry(account, std::move(entry));
                account.report.complete = false;
                if (isAccessError(error)) {
                    ++account.report.accessDenied;
                } else {
                    ++account.report.otherErrors;
                }
                continue;
            }
            const std::wstring childFinalPath = finalPathFromHandle(child.get());
            if (childFinalPath.empty() || childFinalPath != expectedChildFinalPath) {
                entry.consistency = EvidenceState::Changed;
                appendEntry(account, std::move(entry));
                ++account.report.unsafeEntriesSkipped;
                account.report.complete = false;
                continue;
            }
            const auto childIdentity = queryFileIdentityFromHandle(child.get());
            if (!childIdentity) {
                entry.consistency = EvidenceState::Unknown;
                appendEntry(account, std::move(entry));
                ++account.report.otherErrors;
                account.report.complete = false;
                continue;
            }
            if (!openedObjectMatchesListing(raw, *childIdentity, before)) {
                entry.consistency = EvidenceState::Changed;
                appendEntry(account, std::move(entry));
                ++account.report.unsafeEntriesSkipped;
                account.report.complete = false;
                continue;
            }
            if (!directoryMatchesExpected(directory.get(), before,
                                          expectedFinalPath)) {
                entry.consistency = EvidenceState::Changed;
                appendEntry(account, std::move(entry));
                ++account.report.otherErrors;
                ++account.report.unsafeEntriesSkipped;
                account.report.complete = false;
                markRangeChanged(account, subtreeStart);
                return;
            }
            if (!childIdentity->isDirectory ||
                (childIdentity->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                entry.kind = ZaloEntryKind::ReparseDirectory;
                entry.reparsePoint = true;
                entry.contentSkipped = true;
                entry.consistency = EvidenceState::Inconsistent;
                appendEntry(account, std::move(entry));
                ++account.report.reparsePointsSkipped;
                account.report.complete = false;
                continue;
            }

            const EvidenceState state = evidenceState(*childIdentity);
            const std::wstring childCategory = entry.categoryKey;
            const std::size_t childEntryIndex = appendEntry(account, std::move(entry));
            InternalEntry& stored = account.entries[childEntryIndex];
            stored.consistency = state;
            stored.logicalBytes = childIdentity->sizeBytes;
            stored.logicalKnown = childIdentity->logicalSizeKnown;
            stored.allocatedBytes = childIdentity->allocatedBytes;
            stored.allocationKnown = childIdentity->allocationKnown &&
                                     childIdentity->allocatedBytes.has_value();
            stored.identityKnown = childIdentity->identityKnown;
            stored.lastWriteTicks = childIdentity->lastWriteTicks;
            stored.filesystemLinks = childIdentity->numberOfLinks;
            stored.linkCountKnown = childIdentity->linkCountKnown;
            scanDirectory(account, std::move(child), *childIdentity,
                          childFinalPath, childPath, relative, childCategory,
                          childEntryIndex, depth + 1U, cancellation);
            if (account.cancelled) {
                return;
            }
            if (!directoryMatchesExpected(directory.get(), before,
                                          expectedFinalPath)) {
                account.report.complete = false;
                ++account.report.otherErrors;
                ++account.report.unsafeEntriesSkipped;
                markRangeChanged(account, subtreeStart);
                return;
            }
            continue;
        }

        InternalEntry entry;
        entry.nativePath = childPath;
        entry.nativeRelative = relative;
        entry.categoryKey = nativeRelative.empty() ? raw.name : categoryKey;
        entry.reparsePoint = raw.reparse;
        entry.contentSkipped = raw.reparse;
        entry.kind = raw.reparse ? ZaloEntryKind::ReparseFile : ZaloEntryKind::File;
        if (raw.reparse) {
            entry.consistency = EvidenceState::Unknown;
            appendEntry(account, std::move(entry));
            ++account.report.reparsePointsSkipped;
            account.report.complete = false;
            continue;
        }
        if (!raw.listedFileIdKnown) {
            const std::size_t entryIndex = appendEntry(account, std::move(entry));
            appendObservation(account, entryIndex, nullptr,
                              EvidenceState::Unknown, false,
                              raw.listedLogicalBytes);
            ++account.report.unsafeEntriesSkipped;
            account.report.complete = false;
            continue;
        }

        UniqueHandle child = openNoFollow(expectedChildFinalPath, false);
        if (!child.valid()) {
            const DWORD error = ::GetLastError();
            appendUnreadableFile(account, childPath, relative, entry.categoryKey,
                                 raw.listedLogicalBytes, error);
            continue;
        }
        const std::wstring childFinalPath = finalPathFromHandle(child.get());
        if (childFinalPath.empty() || childFinalPath != expectedChildFinalPath) {
            entry.consistency = EvidenceState::Changed;
            const std::size_t entryIndex = appendEntry(account, std::move(entry));
            appendObservation(account, entryIndex, nullptr,
                              EvidenceState::Changed, true,
                              raw.listedLogicalBytes);
            ++account.report.unsafeEntriesSkipped;
            account.report.complete = false;
            continue;
        }
        const auto first = queryFileIdentityFromHandle(child.get());
        if (!first) {
            const std::size_t entryIndex = appendEntry(account, std::move(entry));
            appendObservation(account, entryIndex, nullptr, EvidenceState::Unknown,
                              false, raw.listedLogicalBytes);
            account.report.complete = false;
            ++account.report.otherErrors;
            continue;
        }
        if (!openedObjectMatchesListing(raw, *first, before)) {
            const std::size_t entryIndex = appendEntry(account, std::move(entry));
            appendObservation(account, entryIndex, nullptr, EvidenceState::Changed,
                              true, raw.listedLogicalBytes);
            account.report.complete = false;
            ++account.report.unsafeEntriesSkipped;
            continue;
        }
        if (!directoryMatchesExpected(directory.get(), before,
                                      expectedFinalPath)) {
            const std::size_t entryIndex = appendEntry(account, std::move(entry));
            appendObservation(account, entryIndex, nullptr, EvidenceState::Changed,
                              true, raw.listedLogicalBytes);
            account.report.complete = false;
            ++account.report.otherErrors;
            ++account.report.unsafeEntriesSkipped;
            markRangeChanged(account, subtreeStart);
            return;
        }
        if (first->isDirectory ||
            (first->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            entry.consistency = EvidenceState::Inconsistent;
            entry.contentSkipped =
                (first->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            const std::size_t entryIndex = appendEntry(account, std::move(entry));
            appendObservation(account, entryIndex, &*first,
                              EvidenceState::Inconsistent, false,
                              raw.listedLogicalBytes);
            account.report.complete = false;
            ++account.report.otherErrors;
            continue;
        }
        const auto after = queryFileIdentityFromHandle(child.get());
        if (!directoryMatchesExpected(directory.get(), before,
                                      expectedFinalPath)) {
            const std::size_t entryIndex = appendEntry(account, std::move(entry));
            appendObservation(account, entryIndex, nullptr, EvidenceState::Changed,
                              true, raw.listedLogicalBytes);
            account.report.complete = false;
            ++account.report.otherErrors;
            ++account.report.unsafeEntriesSkipped;
            markRangeChanged(account, subtreeStart);
            return;
        }
        bool changed = false;
        const EvidenceState state = compareFileObservation(*first, after, changed);
        const std::size_t entryIndex = appendEntry(account, std::move(entry));
        appendObservation(account, entryIndex, &*first, state, changed,
                          raw.listedLogicalBytes);
        if (state == EvidenceState::Consistent && !changed && after.has_value()) {
            InternalEntry& stored = account.entries[entryIndex];
            stored.contentIdentity = *after;
            stored.contentCanonicalPath = childFinalPath;
            stored.lastWriteTicks = after->lastWriteTicks;
        }
        if (state != EvidenceState::Consistent) {
            account.report.complete = false;
        }
    }

    if (cancellation.requested()) {
        account.cancelled = true;
        return;
    }
    if (!directoryMatchesExpected(directory.get(), before, expectedFinalPath)) {
        account.report.complete = false;
        ++account.report.otherErrors;
        ++account.report.unsafeEntriesSkipped;
        markRangeChanged(account, subtreeStart);
    }
}

void scanAccount(InternalAccount& account, const Cancellation& cancellation)
{
    if (cancellation.requested()) {
        account.cancelled = true;
        return;
    }
    if (!account.path.expectedIdentityKnown ||
        !isIdentityAvailable(account.path.expectedIdentity) ||
        account.path.finalPath.empty() || account.path.finalSortKey.empty()) {
        account.report.complete = false;
        ++account.report.otherErrors;
        return;
    }

    ValidatedWindowsPath validated;
    const WindowsPathValidationStatus validationStatus = validateWindowsPath(
        account.path.path, true, false, cancellation, validated);
    if (validationStatus == WindowsPathValidationStatus::Cancelled) {
        account.cancelled = true;
        return;
    }
    if (validationStatus != WindowsPathValidationStatus::Valid) {
        account.report.complete = false;
        if (validationStatus == WindowsPathValidationStatus::AccessDenied) {
            ++account.report.accessDenied;
        } else {
            ++account.report.otherErrors;
            if (validationStatus == WindowsPathValidationStatus::Reparse ||
                validationStatus == WindowsPathValidationStatus::Error) {
                ++account.report.unsafeEntriesSkipped;
            }
        }
        return;
    }
    const FileIdentity& identity = validated.finalObservation;
    const CleanupIdentity actualIdentity = identityFromFile(identity);
    const std::wstring& finalPath = validated.resolvedFinalPath;
    const std::wstring finalKey = lowerCopy(finalPath);
    if (!identity.isDirectory ||
        (identity.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !identity.observationConsistent ||
        !isIdentityAvailable(actualIdentity) ||
        !identitiesEqual(actualIdentity, account.path.expectedIdentity) ||
        finalPath.empty() || finalPath != account.path.finalPath ||
        finalKey != account.path.finalSortKey) {
        account.report.complete = false;
        ++account.report.unsafeEntriesSkipped;
        return;
    }
    scanDirectory(account, std::move(validated.finalHandle), identity, finalPath,
                  account.path.path, {}, {}, kNoIndex, 0, cancellation);
}

void addSaturatingTracked(ByteSize& total, ByteSize value, bool& overflow)
{
    overflow = addSaturating(total, value) || overflow;
}

HardLinkCoverage groupCoverage(const IdentityGroup& group)
{
    if (group.changed || group.inconsistent || !group.linkCountKnown ||
        group.filesystemLinks == 0 || group.refs.empty() ||
        group.refs.size() > group.filesystemLinks) {
        return HardLinkCoverage::Unknown;
    }
    if (group.refs.size() < group.filesystemLinks) {
        return HardLinkCoverage::Incomplete;
    }
    return HardLinkCoverage::Complete;
}

void applyGroupEntryEvidence(IdentityGroup& group,
                              const Cancellation& cancellation)
{
    throwIfPostScanCancelled(cancellation);
    std::vector<ObservationRef*> ordered = group.refs;
    std::sort(ordered.begin(), ordered.end(),
              [&cancellation](const ObservationRef* left,
                              const ObservationRef* right) {
                  throwIfPostScanCancelled(cancellation);
                  const std::wstring leftKey =
                      left->account->path.sortKey + L"\\" +
                      left->entry->nativeRelative;
                  const std::wstring rightKey =
                      right->account->path.sortKey + L"\\" +
                      right->entry->nativeRelative;
                  return compareInsensitive(leftKey, rightKey) < 0;
              });
    throwIfPostScanCancelled(cancellation);
    const HardLinkCoverage coverage = groupCoverage(group);
    const bool stable = !group.changed && !group.inconsistent;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        throwIfPostScanCancelled(cancellation);
        ObservationRef* ref = ordered[i];
        ref->entry->observedPathCount = stable
                                            ? static_cast<std::uint32_t>(ordered.size())
                                            : 0;
        ref->entry->filesystemLinks =
            group.linkCountKnown ? group.filesystemLinks : 0;
        ref->entry->linkCountKnown = group.linkCountKnown;
        ref->entry->hardLinkAlias = i > 0;
        ref->entry->singlePathReleaseBytes.reset();
        ref->entry->allObservedPathReleaseBytes.reset();
        if (!stable || !group.linkCountKnown) {
            continue;
        }
        if (group.filesystemLinks > 1) {
            ref->entry->singlePathReleaseBytes = 0;
        } else if (group.filesystemLinks == 1 &&
                   group.allocationKnown && group.allocatedBytes.has_value()) {
            ref->entry->singlePathReleaseBytes = group.allocatedBytes;
        }
        if (coverage == HardLinkCoverage::Incomplete) {
            ref->entry->allObservedPathReleaseBytes = 0;
        } else if (coverage == HardLinkCoverage::Complete &&
                   group.allocationKnown && group.allocatedBytes.has_value()) {
            ref->entry->allObservedPathReleaseBytes = group.allocatedBytes;
        }
    }
}

std::optional<ZaloAccountingSummary> summarizeAccounting(
    std::vector<ObservationRef>& refs, bool mutateEntries, bool traversalComplete,
    const Cancellation& cancellation)
{
    if (cancellation.requested()) {
        return std::nullopt;
    }
    try {
        ZaloAccountingSummary result;
    result.pathCount = refs.size();
    if (refs.empty()) {
        if (traversalComplete) {
            result.uniqueAllocatedBytes = 0;
            result.allObservedPathReleaseBytes = 0;
            result.allocationKnown = true;
            result.hardLinkAliasKnown = true;
            result.hardLinkCoverage = HardLinkCoverage::Complete;
        } else {
            result.pathVisibleLogicalKnown = false;
            result.uniqueLogicalKnown = false;
            result.partialKnownUniqueAllocatedBytes = 0;
            result.allocationKnown = false;
            result.hardLinkAliasKnown = false;
            result.hardLinkCoverage = HardLinkCoverage::Unknown;
        }
        return result;
    }

    std::vector<IdentityGroup> groups;
    groups.reserve(refs.size());
    std::unordered_map<IdentityKey, std::size_t, IdentityKeyHash> knownGroups;
    knownGroups.reserve(refs.size());
    bool allIdentityKnown = true;
    bool allStable = true;
    bool allAllocationKnown = true;
    bool allLogicalKnown = true;
    bool allReleaseKnown = true;
    bool sawIncomplete = false;
    bool sawUnknownCoverage = false;
    bool sawComplete = false;
    ByteSize partialAllocation = 0;
    bool partialAllocationAny = false;
    ByteSize partialRelease = 0;
    bool partialReleaseAny = false;

    for (ObservationRef& ref : refs) {
        throwIfPostScanCancelled(cancellation);
        InternalObservation& observation = *ref.observation;
        const bool pathStable = observation.consistency != EvidenceState::Changed &&
                                observation.consistency != EvidenceState::Inconsistent;
        if (observation.pathVisibleLogicalKnown) {
            addSaturatingTracked(result.pathVisibleLogicalBytes,
                                 observation.pathVisibleLogicalBytes,
                                 result.logicalOverflow);
        } else if (observation.logicalKnown && pathStable) {
            addSaturatingTracked(result.pathVisibleLogicalBytes,
                                 observation.logicalBytes,
                                 result.logicalOverflow);
        } else {
            result.pathVisibleLogicalKnown = false;
            ++result.unknownLogicalCount;
        }
        if (observation.consistency == EvidenceState::Inconsistent) {
            ++result.inconsistentEvidenceCount;
            allStable = false;
        } else if (observation.consistency == EvidenceState::Changed) {
            ++result.changedEvidenceCount;
            allStable = false;
        }

        if (!observation.identityKnown) {
            ++result.unknownIdentityCount;
            allIdentityKnown = false;
            result.uniqueLogicalKnown = false;
            result.hardLinkAliasKnown = false;
            allLogicalKnown = false;
            allReleaseKnown = false;
            sawUnknownCoverage = true;
            if (!observation.allocationKnown) {
                ++result.unknownAllocationCount;
            }
            continue;
        }

        const IdentityKey key = makeIdentityKey(observation.identity);
        auto [it, inserted] = knownGroups.emplace(key, groups.size());
        if (inserted) {
            IdentityGroup group;
            group.identity = observation.identity;
            group.refs.push_back(&ref);
            group.logicalKnown = observation.logicalKnown;
            group.logicalBytes = observation.logicalBytes;
            group.allocationKnown = observation.allocationKnown &&
                                    observation.allocatedBytes.has_value();
            group.allocatedBytes = observation.allocatedBytes;
            group.filesystemLinks = observation.filesystemLinks;
            group.linkCountKnown = observation.linkCountKnown;
            group.changed = observation.consistency == EvidenceState::Changed;
            group.inconsistent = observation.consistency == EvidenceState::Inconsistent;
            groups.push_back(std::move(group));
        } else {
            IdentityGroup& group = groups[it->second];
            group.refs.push_back(&ref);
            if (group.logicalKnown && observation.logicalKnown) {
                if (group.logicalBytes != observation.logicalBytes) {
                    group.inconsistent = true;
                }
            } else {
                group.logicalKnown = false;
            }
            const bool allocationKnown =
                observation.allocationKnown && observation.allocatedBytes.has_value();
            if (group.allocationKnown && allocationKnown) {
                if (group.allocatedBytes != observation.allocatedBytes) {
                    group.inconsistent = true;
                }
            } else {
                group.allocationKnown = false;
                group.allocatedBytes.reset();
            }
            if (group.linkCountKnown && observation.linkCountKnown) {
                if (group.filesystemLinks != observation.filesystemLinks) {
                    group.inconsistent = true;
                }
            } else {
                group.linkCountKnown = false;
            }
            group.changed = group.changed ||
                            observation.consistency == EvidenceState::Changed;
            group.inconsistent = group.inconsistent ||
                                 observation.consistency == EvidenceState::Inconsistent;
        }
    }

    for (IdentityGroup& group : groups) {
        throwIfPostScanCancelled(cancellation);
        if (group.logicalKnown && !group.changed && !group.inconsistent) {
            addSaturatingTracked(result.partialKnownUniqueLogicalBytes,
                                 group.logicalBytes, result.logicalOverflow);
        } else {
            allLogicalKnown = false;
        }
        if (group.allocationKnown && group.allocatedBytes.has_value() &&
            !group.changed && !group.inconsistent) {
            partialAllocationAny = true;
            addSaturatingTracked(partialAllocation, *group.allocatedBytes,
                                 result.allocationOverflow);
        } else {
            ++result.unknownAllocationCount;
            allAllocationKnown = false;
        }
        if (group.changed) {
            allStable = false;
        }
        if (group.inconsistent) {
            allStable = false;
        }
        ++result.uniqueIdentityCount;

        const HardLinkCoverage coverage = groupCoverage(group);
        if (coverage == HardLinkCoverage::Complete) {
            sawComplete = true;
        } else if (coverage == HardLinkCoverage::Incomplete) {
            sawIncomplete = true;
        } else {
            sawUnknownCoverage = true;
        }

        if (mutateEntries) {
            applyGroupEntryEvidence(group, cancellation);
        }
        std::vector<ObservationRef*> ordered = group.refs;
        std::sort(ordered.begin(), ordered.end(),
                  [&cancellation](const ObservationRef* left,
                                  const ObservationRef* right) {
                      throwIfPostScanCancelled(cancellation);
                      const std::wstring leftKey =
                          left->account->path.sortKey + L"\\" +
                          left->entry->nativeRelative;
                      const std::wstring rightKey =
                          right->account->path.sortKey + L"\\" +
                          right->entry->nativeRelative;
                      return compareInsensitive(leftKey, rightKey) < 0;
                  });
        throwIfPostScanCancelled(cancellation);
        for (std::size_t i = 1; i < ordered.size(); ++i) {
            throwIfPostScanCancelled(cancellation);
            if (ordered[i]->observation->logicalKnown &&
                ordered[i]->observation->consistency != EvidenceState::Changed &&
                ordered[i]->observation->consistency != EvidenceState::Inconsistent) {
                ++result.hardLinkAliasPathCount;
                addSaturatingTracked(result.hardLinkAliasBytes,
                                     ordered[i]->observation->logicalBytes,
                                     result.logicalOverflow);
            } else {
                result.hardLinkAliasKnown = false;
            }
        }

        const bool stableGroup = !group.changed && !group.inconsistent;
        if (stableGroup && coverage == HardLinkCoverage::Incomplete) {
            // At least one filesystem link remains outside this observed set,
            // so removing every observed path releases exactly zero allocation.
            partialReleaseAny = true;
        } else if (stableGroup && coverage == HardLinkCoverage::Complete &&
                   group.allocationKnown && group.allocatedBytes.has_value()) {
            partialReleaseAny = true;
            addSaturatingTracked(partialRelease, *group.allocatedBytes,
                                 result.releaseOverflow);
        } else {
            allReleaseKnown = false;
        }
    }

    if (allIdentityKnown && allStable && allLogicalKnown) {
        result.uniqueLogicalKnown = true;
        result.uniqueLogicalBytes = result.partialKnownUniqueLogicalBytes;
    } else {
        result.uniqueLogicalKnown = false;
    }
    if (partialAllocationAny && !(allIdentityKnown && allStable && allAllocationKnown)) {
        result.partialKnownUniqueAllocatedBytes = partialAllocation;
    }
    if (allIdentityKnown && allStable && allAllocationKnown &&
        !result.allocationOverflow) {
        result.uniqueAllocatedBytes = partialAllocation;
        result.allocationKnown = true;
    } else {
        result.allocationKnown = false;
    }
    if (allIdentityKnown && allStable && allReleaseKnown &&
        !result.releaseOverflow) {
        result.allObservedPathReleaseBytes = partialRelease;
    }
    if (partialReleaseAny && !result.allObservedPathReleaseBytes.has_value()) {
        result.partialKnownReleaseBytes = partialRelease;
    }
    if (!allIdentityKnown || !allStable) {
        result.hardLinkAliasKnown = false;
    }

    if (!sawComplete && !sawIncomplete && !sawUnknownCoverage) {
        result.hardLinkCoverage = HardLinkCoverage::Complete;
    } else if (sawUnknownCoverage) {
        result.hardLinkCoverage = HardLinkCoverage::Unknown;
    } else if (sawIncomplete) {
        result.hardLinkCoverage = HardLinkCoverage::Incomplete;
    } else {
        result.hardLinkCoverage = HardLinkCoverage::Complete;
    }

    if (!traversalComplete) {
        result.pathVisibleLogicalKnown = false;
        result.uniqueLogicalKnown = false;
        result.uniqueLogicalBytes = 0;
        if (result.uniqueAllocatedBytes.has_value()) {
            result.partialKnownUniqueAllocatedBytes =
                result.uniqueAllocatedBytes;
            result.uniqueAllocatedBytes.reset();
        }
        result.allocationKnown = false;
        if (result.allObservedPathReleaseBytes.has_value()) {
            result.partialKnownReleaseBytes =
                *result.allObservedPathReleaseBytes;
            result.allObservedPathReleaseBytes.reset();
        }
        result.hardLinkAliasKnown = false;
        result.hardLinkCoverage = HardLinkCoverage::Unknown;
    }
    return result;
    } catch (const PostScanCancellation&) {
        return std::nullopt;
    }
}

ZaloContentResult contentReadErrorResult()
{
    ZaloContentResult result;
    result.status = ZaloContentStatus::ReadError;
    result.description = "Payload read failed during content identification";
    return result;
}

bool payloadOpenInvalidates(PayloadOpenStatus status) noexcept
{
    switch (status) {
    case PayloadOpenStatus::Missing:
    case PayloadOpenStatus::NotRegularFile:
    case PayloadOpenStatus::ReparsePoint:
    case PayloadOpenStatus::IdentityChanged:
    case PayloadOpenStatus::PathChanged:
    case PayloadOpenStatus::Inconsistent:
        return true;
    case PayloadOpenStatus::Opened:
    case PayloadOpenStatus::Cancelled:
    case PayloadOpenStatus::AccessDenied:
    case PayloadOpenStatus::Error:
        return false;
    }
    return false;
}

bool payloadRevalidationInvalidates(PayloadRevalidationStatus status) noexcept
{
    switch (status) {
    case PayloadRevalidationStatus::IdentityChanged:
    case PayloadRevalidationStatus::PathChanged:
    case PayloadRevalidationStatus::NotRegularFile:
    case PayloadRevalidationStatus::ReparsePoint:
    case PayloadRevalidationStatus::Inconsistent:
        return true;
    case PayloadRevalidationStatus::Valid:
    case PayloadRevalidationStatus::Cancelled:
    case PayloadRevalidationStatus::Error:
        return false;
    }
    return false;
}

bool contentBindingReady(const ObservationRef& ref) noexcept
{
    if (ref.observation == nullptr || ref.entry == nullptr ||
        ref.entry->kind != ZaloEntryKind::File || ref.entry->reparsePoint ||
        ref.entry->contentSkipped ||
        ref.entry->consistency != EvidenceState::Consistent ||
        ref.observation->consistency != EvidenceState::Consistent ||
        !ref.observation->identityKnown ||
        !isStrongIdentity(ref.observation->identity) ||
        !ref.entry->contentIdentity.has_value() ||
        ref.entry->contentCanonicalPath.empty()) {
        return false;
    }

    const FileIdentity& identity = *ref.entry->contentIdentity;
    if (!identity.identityKnown || identity.isDirectory ||
        (identity.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !identity.logicalSizeKnown || !identity.basicMetadataKnown ||
        !identity.linkCountKnown || !identity.observationConsistent ||
        identity.allocatedBytes.has_value() != identity.allocationKnown) {
        return false;
    }
    const CleanupIdentity contentIdentity = identityFromFile(identity);
    return isStrongIdentity(contentIdentity) &&
           identitiesEqual(contentIdentity, ref.observation->identity);
}

void markContentGroupChanged(IdentityGroup& group)
{
    for (ObservationRef* ref : group.refs) {
        if (ref == nullptr || ref->entry == nullptr ||
            ref->observation == nullptr || ref->account == nullptr) {
            continue;
        }
        ref->entry->consistency = EvidenceState::Changed;
        ref->observation->consistency = EvidenceState::Changed;
        ref->entry->contentIdentification.reset();
        ref->account->report.complete = false;
    }
}

bool identifyContentGroups(std::vector<ObservationRef>& refs,
                          const Cancellation& cancellation)
{
    try {
        std::vector<IdentityGroup> groups;
        groups.reserve(refs.size());
        std::unordered_map<IdentityKey, std::size_t, IdentityKeyHash> groupIndexes;
        groupIndexes.reserve(refs.size());

        for (ObservationRef& ref : refs) {
            throwIfPostScanCancelled(cancellation);
            if (ref.observation == nullptr || !ref.observation->identityKnown ||
                !isStrongIdentity(ref.observation->identity)) {
                continue;
            }
            const IdentityKey key = makeIdentityKey(ref.observation->identity);
            const auto [it, inserted] = groupIndexes.emplace(key, groups.size());
            if (inserted) {
                IdentityGroup group;
                group.identity = ref.observation->identity;
                group.refs.push_back(&ref);
                groups.push_back(std::move(group));
            } else {
                groups[it->second].refs.push_back(&ref);
            }
        }

        const PayloadCancellation payloadCancellation =
            [&cancellation]() noexcept { return cancellation.requested(); };
        for (IdentityGroup& group : groups) {
            throwIfPostScanCancelled(cancellation);
            ObservationRef* candidate = nullptr;
            for (ObservationRef* ref : group.refs) {
                throwIfPostScanCancelled(cancellation);
                if (ref != nullptr && contentBindingReady(*ref)) {
                    candidate = ref;
                    break;
                }
            }
            if (candidate == nullptr) {
                continue;
            }

            PayloadOpenResult opened = ReadOnlyPayload::open(
                candidate->entry->nativePath, *candidate->entry->contentIdentity,
                candidate->entry->contentCanonicalPath, payloadCancellation);
            if (opened.status == PayloadOpenStatus::Cancelled ||
                cancellation.requested()) {
                throw PostScanCancellation{};
            }
            if (!opened.ok()) {
                if (payloadOpenInvalidates(opened.status)) {
                    markContentGroupChanged(group);
                } else {
                    const ZaloContentResult readError = contentReadErrorResult();
                    for (ObservationRef* ref : group.refs) {
                        throwIfPostScanCancelled(cancellation);
                        if (ref != nullptr && contentBindingReady(*ref)) {
                            ref->entry->contentIdentification = readError;
                        }
                    }
                }
                continue;
            }

            ZaloContentResult identified = identifyZaloContent(
                opened.payload->view(), payloadCancellation);
            if (identified.status == ZaloContentStatus::Cancelled ||
                cancellation.requested()) {
                throw PostScanCancellation{};
            }
            if (identified.status == ZaloContentStatus::Changed) {
                markContentGroupChanged(group);
                continue;
            }

            std::optional<ZaloSemanticMetadata> semanticMetadata;
            bool semanticChanged = false;
            if (identified.identified()) {
                ZaloSemanticMetadata extracted = extractZaloSemanticMetadata(
                    opened.payload->view(), identified, payloadCancellation);
                if (extracted.status == ZaloSemanticMetadataStatus::Cancelled ||
                    cancellation.requested()) {
                    throw PostScanCancellation{};
                }
                semanticChanged =
                    extracted.status == ZaloSemanticMetadataStatus::Changed;
                semanticMetadata = std::move(extracted);
            }

            const PayloadRevalidationStatus revalidation =
                opened.payload->revalidate(payloadCancellation);
            if (revalidation == PayloadRevalidationStatus::Cancelled ||
                cancellation.requested()) {
                throw PostScanCancellation{};
            }
            if (payloadRevalidationInvalidates(revalidation) || semanticChanged) {
                markContentGroupChanged(group);
                continue;
            }
            if (revalidation == PayloadRevalidationStatus::Error) {
                // A failed stability observation is not proof of mutation. Do
                // not discard otherwise valid physical accounting, but never
                // publish a recognized result whose post-read stability could
                // not be established.
                const ZaloContentResult readError = contentReadErrorResult();
                for (ObservationRef* ref : group.refs) {
                    throwIfPostScanCancelled(cancellation);
                    if (ref != nullptr && contentBindingReady(*ref)) {
                        ref->entry->contentIdentification = readError;
                    }
                }
                continue;
            }

            if (semanticMetadata.has_value()) {
                identified.semanticMetadata = std::move(*semanticMetadata);
            }
            std::string catAlias;
            ByteSize logBytes = 0;
            if (!group.refs.empty() && group.refs.front() != nullptr &&
                group.refs.front()->entry != nullptr) {
                catAlias = utf8FromWide(group.refs.front()->entry->categoryKey);
                logBytes = group.refs.front()->entry->logicalBytes;
            }
            if (identified.identified()) {
                identified.humanIdentity = buildHumanIdentity(
                    identified, "", logBytes, catAlias);
            }
            for (ObservationRef* ref : group.refs) {
                throwIfPostScanCancelled(cancellation);
                if (ref != nullptr && contentBindingReady(*ref)) {
                    ref->entry->contentIdentification = identified;
                }
            }
        }
        return true;
    } catch (const PostScanCancellation&) {
        return false;
    }
}

const char* knownCategoryAlias(std::wstring_view category) noexcept
{
    static constexpr std::array<std::pair<std::wstring_view, const char*>, 10>
        known = {{{L"cache", "cache"},
                  {L"file", "file"},
                  {L"fileNoise", "file-noise"},
                  {L"fileThumb", "file-thumb"},
                  {L"picture", "picture"},
                  {L"resource", "resource"},
                  {L"richThumb", "rich-thumb"},
                  {L"video", "video"},
                  {L"voice", "voice"},
                  {L"zinstant", "zinstant"}}};
    for (const auto& [name, alias] : known) {
        if (equalInsensitive(category, name)) {
            return alias;
        }
    }
    return nullptr;
}

std::string otherCategoryAlias(std::size_t number)
{
    std::ostringstream out;
    out << "other-";
    if (number < 10) {
        out << '0';
    }
    out << number;
    return out.str();
}

std::string categoryAliasFor(
    const std::wstring& category,
    const std::unordered_map<std::wstring, std::size_t>& numbers)
{
    if (const char* known = knownCategoryAlias(category); known != nullptr) {
        return known;
    }
    const auto it = numbers.find(lowerCopy(category));
    if (it == numbers.end()) {
        return "other-00";
    }
    return otherCategoryAlias(it->second);
}

std::string entryIdFor(std::size_t number)
{
    std::ostringstream out;
    out << "entry-";
    if (number < 1000000U) {
        out.width(6);
        out.fill('0');
    }
    out << number;
    return out.str();
}

ZaloEntry makePublicEntry(const InternalEntry& internal, std::string id,
                          std::string categoryAlias)
{
    ZaloEntry entry;
    entry.entryId = std::move(id);
    entry.categoryAlias = std::move(categoryAlias);
    entry.kind = internal.kind;
    entry.logicalBytes = internal.logicalBytes;
    entry.allocatedBytes = internal.allocatedBytes;
    entry.allocationKnown = internal.allocationKnown;
    entry.reparsePoint = internal.reparsePoint;
    entry.contentSkipped = internal.contentSkipped;
    entry.contentIdentification = internal.contentIdentification;
    entry.identityKnown = internal.identityKnown;
    entry.hardLinkAlias = internal.hardLinkAlias;
    entry.filesystemLinkCount = internal.filesystemLinks;
    entry.observedPathCount = internal.observedPathCount;
    entry.singlePathReleaseBytes = internal.singlePathReleaseBytes;
    entry.allObservedPathReleaseBytes = internal.allObservedPathReleaseBytes;
    entry.lastWriteTicks = internal.lastWriteTicks;
    if (internal.contentIdentification.has_value() &&
        internal.contentIdentification->humanIdentity.has_value()) {
        entry.humanIdentity = internal.contentIdentification->humanIdentity;
    } else {
        entry.humanIdentity = buildHumanIdentity(
            internal.contentIdentification.value_or(ZaloContentResult{}),
            "", internal.logicalBytes, entry.categoryAlias);
    }
    switch (internal.consistency) {
    case EvidenceState::Consistent:
        entry.consistency = ZaloEntryConsistency::Consistent;
        break;
    case EvidenceState::Unknown:
        entry.consistency = ZaloEntryConsistency::Unknown;
        break;
    case EvidenceState::Inconsistent:
        entry.consistency = ZaloEntryConsistency::Inconsistent;
        break;
    case EvidenceState::Changed:
        entry.consistency = ZaloEntryConsistency::Changed;
        break;
    }
    return entry;
}

bool finalizePublicReports(std::vector<InternalAccount>& accounts,
                           ZaloStorageReport& result,
                           const Cancellation& cancellation)
{
    try {
        struct EntryRef {
            InternalAccount* account = nullptr;
            InternalEntry* entry = nullptr;
        };

        std::unordered_map<InternalAccount*,
                           std::unordered_map<std::wstring, std::size_t>>
            categoryNumbers;
        for (auto& account : accounts) {
            throwIfPostScanCancelled(cancellation);
            std::vector<std::wstring> unknown;
            for (const auto& entry : account.entries) {
                throwIfPostScanCancelled(cancellation);
                if (knownCategoryAlias(entry.categoryKey) == nullptr) {
                    const std::wstring key = lowerCopy(entry.categoryKey);
                    if (std::find(unknown.begin(), unknown.end(), key) ==
                        unknown.end()) {
                        unknown.push_back(key);
                    }
                }
            }
            std::sort(unknown.begin(), unknown.end(),
                      [&cancellation](const std::wstring& left,
                                     const std::wstring& right) {
                          throwIfPostScanCancelled(cancellation);
                          return compareInsensitive(left, right) < 0;
                      });
            throwIfPostScanCancelled(cancellation);
            auto& numbers = categoryNumbers[&account];
            for (std::size_t i = 0; i < unknown.size(); ++i) {
                throwIfPostScanCancelled(cancellation);
                numbers.emplace(unknown[i], i + 1U);
            }
        }

        std::vector<EntryRef> entries;
        for (auto& account : accounts) {
            throwIfPostScanCancelled(cancellation);
            for (auto& entry : account.entries) {
                throwIfPostScanCancelled(cancellation);
                entries.push_back({&account, &entry});
            }
        }
        std::sort(entries.begin(), entries.end(),
                  [&cancellation](const EntryRef& left,
                                  const EntryRef& right) {
                      throwIfPostScanCancelled(cancellation);
                      const std::wstring leftKey =
                          left.account->path.sortKey + L"\\" +
                          left.entry->nativeRelative;
                      const std::wstring rightKey =
                          right.account->path.sortKey + L"\\" +
                          right.entry->nativeRelative;
                      const int compare = compareInsensitive(leftKey, rightKey);
                      if (compare != 0) {
                          return compare < 0;
                      }
                      return static_cast<int>(left.entry->kind) <
                             static_cast<int>(right.entry->kind);
                  });
        throwIfPostScanCancelled(cancellation);

        std::unordered_map<InternalAccount*, std::vector<ZaloEntry>> publicEntries;
        std::size_t number = 1;
        for (const EntryRef& ref : entries) {
            throwIfPostScanCancelled(cancellation);
            const auto& numbers = categoryNumbers[ref.account];
            const std::string category =
                categoryAliasFor(ref.entry->categoryKey, numbers);
            const std::string reportEntryId = entryIdFor(number++);
            ref.entry->reportEntryId = reportEntryId;
            publicEntries[ref.account].push_back(
                makePublicEntry(*ref.entry, reportEntryId, category));
        }

        result.accounts.clear();
        result.accounts.reserve(accounts.size());
        for (auto& account : accounts) {
            throwIfPostScanCancelled(cancellation);
            account.report.entries = std::move(publicEntries[&account]);
            result.accounts.push_back(std::move(account.report));
        }
    } catch (const PostScanCancellation&) {
        return false;
    }
    return true;
}

constexpr ByteSize kZaloExactCopyMaxBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kZaloExactCopyChunkBytes = 64U * 1024U;

struct ComparisonFileInternal {
    std::wstring path;
    std::wstring sortKey;
    std::size_t scopeIndex = 0U;
    FileIdentity identity{};
    CleanupIdentity cleanupIdentity{};
    std::uint64_t hardLinkAliasCount = 0U;
    std::string comparisonFileId;
    std::string scopeAlias;
};

struct ComparisonDirectoryInternal {
    std::wstring path;
    std::wstring sortKey;
    std::size_t scopeIndex = 0U;
    FileIdentity expectedIdentity{};
    std::wstring expectedFinalPath;
};

struct ZaloCopyCandidateInternal {
    InternalEntry* entry = nullptr;
    CleanupIdentity cleanupIdentity{};
    std::size_t aliasCount = 0U;
    ByteSize bytes = 0U;
    bool wrapped = false;
};

struct DigestResultInternal {
    bool ok = false;
    bool cancelled = false;
    bool changed = false;
    std::array<std::uint8_t, 32> digest{};
};

DigestResultInternal hashPayloadView(const PayloadView& payload,
                                     PayloadCancellation cancellation)
{
    DigestResultInternal result;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> object;
    do {
        if (cancellation && cancellation()) {
            result.cancelled = true;
            break;
        }
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                        nullptr, 0U) < 0) {
            break;
        }
        ULONG objectBytes = 0U;
        ULONG resultBytes = 0U;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectBytes),
                              sizeof(objectBytes), &resultBytes, 0U) < 0 ||
            objectBytes == 0U) {
            break;
        }
        object.resize(objectBytes);
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
                             nullptr, 0U, 0U) < 0) {
            break;
        }
        std::vector<std::uint8_t> buffer(kZaloExactCopyChunkBytes);
        ByteSize offset = 0U;
        while (offset < payload.size()) {
            if (cancellation && cancellation()) {
                result.cancelled = true;
                break;
            }
            const ByteSize remaining = payload.size() - offset;
            const std::size_t requested = static_cast<std::size_t>(
                std::min<ByteSize>(remaining, buffer.size()));
            const PayloadReadResult read = payload.readAt(
                offset, buffer.data(), requested, cancellation);
            if (!read.ok() || read.bytesRead != requested) {
                result.cancelled = read.status == PayloadReadStatus::Cancelled;
                result.changed = read.status == PayloadReadStatus::IdentityChanged ||
                                 read.status == PayloadReadStatus::PathChanged;
                break;
            }
            if (BCryptHashData(hash, buffer.data(),
                               static_cast<ULONG>(requested), 0U) < 0) {
                break;
            }
            offset += requested;
        }
        if (result.cancelled || result.changed || offset != payload.size()) {
            break;
        }
        if (BCryptFinishHash(hash, result.digest.data(),
                             static_cast<ULONG>(result.digest.size()), 0U) < 0) {
            break;
        }
        result.ok = true;
    } while (false);
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0U);
    }
    return result;
}

bool isReparseFile(const FileIdentity& identity) noexcept
{
    return (identity.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}

bool collectComparisonFilesForRoot(
    const std::wstring& root, std::size_t scopeIndex,
    std::vector<ComparisonFileInternal>& files, ZaloExactCopyReport& report,
    const Cancellation& cancellation, bool& partial)
{
    const auto noteValidationFailure =
        [&report, &partial](WindowsPathValidationStatus status) {
            partial = true;
            if (status == WindowsPathValidationStatus::Reparse) {
                ++report.skippedReparsePoints;
            } else if (status == WindowsPathValidationStatus::Changed) {
                ++report.skippedUnstable;
            } else {
                ++report.skippedInaccessible;
            }
        };

    const auto parentBindingMatches =
        [](const ValidatedWindowsPath& child,
           const FileIdentity& expectedParent,
           std::wstring_view expectedParentPath) {
            if (child.parentComponents.empty()) {
                return false;
            }
            const RetainedPathComponent& parent = child.parentComponents.back();
            return samePathObject(expectedParent, parent.observation) &&
                   parent.resolvedFinalPath == expectedParentPath;
        };

    // Probe only to classify a final reparse point. Actual traversal below uses
    // component-by-component validation so an intermediate junction cannot be
    // followed while resolving an explicitly authorized comparison path.
    if (const auto probe = queryFileIdentity(root);
        probe.has_value() && isReparseFile(*probe)) {
        ++report.skippedReparsePoints;
        partial = true;
        return true;
    }

    ValidatedWindowsPath rootDirectory;
    const WindowsPathValidationStatus directoryStatus = validateWindowsPath(
        root, true, false, cancellation, rootDirectory);
    if (directoryStatus == WindowsPathValidationStatus::Cancelled) {
        return false;
    }
    if (directoryStatus == WindowsPathValidationStatus::Reparse) {
        noteValidationFailure(directoryStatus);
        return true;
    }

    if (directoryStatus == WindowsPathValidationStatus::Valid) {
        if (!rootDirectory.finalObservation.isDirectory ||
            isReparseFile(rootDirectory.finalObservation)) {
            ++report.skippedReparsePoints;
            partial = true;
            return true;
        }

        std::vector<ComparisonDirectoryInternal> pending;
        pending.push_back({rootDirectory.resolvedFinalPath,
                           lowerCopy(rootDirectory.resolvedFinalPath), scopeIndex,
                           rootDirectory.finalObservation,
                           rootDirectory.resolvedFinalPath});

        while (!pending.empty()) {
            if (cancellation.requested()) {
                return false;
            }
            ComparisonDirectoryInternal queued = std::move(pending.back());
            pending.pop_back();

            ValidatedWindowsPath directory;
            const WindowsPathValidationStatus status = validateWindowsPath(
                queued.path, true, false, cancellation, directory);
            if (status == WindowsPathValidationStatus::Cancelled) {
                return false;
            }
            if (status != WindowsPathValidationStatus::Valid) {
                noteValidationFailure(status);
                continue;
            }
            if (!samePathObject(queued.expectedIdentity,
                                directory.finalObservation) ||
                directory.resolvedFinalPath != queued.expectedFinalPath ||
                isReparseFile(directory.finalObservation)) {
                ++report.skippedUnstable;
                partial = true;
                continue;
            }

            const RawDirectoryListing listing = listImmediateChildren(
                directory.finalHandle.get(), directory.finalObservation,
                cancellation);
            if (listing.status == DirectoryReadStatus::Cancelled) {
                return false;
            }
            if (listing.status != DirectoryReadStatus::Ok) {
                partial = true;
                if (listing.status == DirectoryReadStatus::Changed) {
                    ++report.skippedUnstable;
                } else {
                    ++report.skippedInaccessible;
                }
                continue;
            }
            if (listing.unsafeEntryCount != 0U) {
                partial = true;
            }

            for (const RawDirectoryEntry& listed : listing.entries) {
                if (cancellation.requested()) {
                    return false;
                }
                const std::wstring child =
                    joinNative(directory.resolvedFinalPath, listed.name);
                if (listed.reparse) {
                    ++report.skippedReparsePoints;
                    partial = true;
                    continue;
                }

                if (listed.directory) {
                    ValidatedWindowsPath childDirectory;
                    const WindowsPathValidationStatus childStatus =
                        validateWindowsPath(child, true, false, cancellation,
                                            childDirectory);
                    if (childStatus == WindowsPathValidationStatus::Cancelled) {
                        return false;
                    }
                    if (childStatus != WindowsPathValidationStatus::Valid) {
                        noteValidationFailure(childStatus);
                        continue;
                    }
                    if (!parentBindingMatches(childDirectory,
                                               directory.finalObservation,
                                               directory.resolvedFinalPath) ||
                        !openedObjectMatchesListing(
                            listed, childDirectory.finalObservation,
                            directory.finalObservation) ||
                        isReparseFile(childDirectory.finalObservation)) {
                        ++report.skippedUnstable;
                        partial = true;
                        continue;
                    }
                    pending.push_back({childDirectory.resolvedFinalPath,
                                       lowerCopy(childDirectory.resolvedFinalPath),
                                       scopeIndex, childDirectory.finalObservation,
                                       childDirectory.resolvedFinalPath});
                    continue;
                }

                ++report.comparisonFilesEnumerated;
                ValidatedWindowsPath childFile;
                const WindowsPathValidationStatus childStatus =
                    validateWindowsPath(child, false, true, cancellation,
                                        childFile);
                if (childStatus == WindowsPathValidationStatus::Cancelled) {
                    return false;
                }
                if (childStatus != WindowsPathValidationStatus::Valid) {
                    noteValidationFailure(childStatus);
                    continue;
                }
                if (!parentBindingMatches(childFile, directory.finalObservation,
                                           directory.resolvedFinalPath) ||
                    !openedObjectMatchesListing(
                        listed, childFile.finalObservation,
                        directory.finalObservation) ||
                    childFile.finalObservation.isDirectory ||
                    isReparseFile(childFile.finalObservation)) {
                    if (isReparseFile(childFile.finalObservation)) {
                        ++report.skippedReparsePoints;
                    } else {
                        ++report.skippedUnstable;
                    }
                    partial = true;
                    continue;
                }
                if (!childFile.finalObservation.logicalSizeKnown) {
                    ++report.skippedUnsupported;
                    partial = true;
                    continue;
                }
                files.push_back({child, lowerCopy(child), scopeIndex,
                                 childFile.finalObservation,
                                 identityFromFile(childFile.finalObservation), 0U,
                                 {}, {}});
            }
        }
        return true;
    }

    // A comparison scope may name one file rather than a directory. The first
    // directory validation intentionally fails for that shape; validate the
    // file with read access and the same component no-follow policy.
    ValidatedWindowsPath rootFile;
    const WindowsPathValidationStatus fileStatus = validateWindowsPath(
        root, false, true, cancellation, rootFile);
    if (fileStatus == WindowsPathValidationStatus::Cancelled) {
        return false;
    }
    if (fileStatus != WindowsPathValidationStatus::Valid) {
        noteValidationFailure(fileStatus);
        return true;
    }
    if (rootFile.finalObservation.isDirectory ||
        isReparseFile(rootFile.finalObservation)) {
        if (isReparseFile(rootFile.finalObservation)) {
            ++report.skippedReparsePoints;
        } else {
            ++report.skippedUnstable;
        }
        partial = true;
        return true;
    }
    if (!rootFile.finalObservation.logicalSizeKnown) {
        ++report.skippedUnsupported;
        partial = true;
        return true;
    }
    ++report.comparisonFilesEnumerated;
    files.push_back({root, lowerCopy(root), scopeIndex,
                     rootFile.finalObservation,
                     identityFromFile(rootFile.finalObservation), 0U, {}, {}});
    return true;
}

DigestResultInternal hashZaloEntry(const ZaloCopyCandidateInternal& candidate,
                                   const Cancellation& cancellation)
{
    if (candidate.entry == nullptr) {
        return {};
    }
    const InternalEntry& entry = *candidate.entry;
    const PayloadCancellation payloadCancellation =
        [&cancellation]() noexcept { return cancellation.requested(); };
    if (candidate.wrapped && entry.contentIdentification.has_value() &&
        entry.contentIdentity.has_value()) {
        const std::wstring canonical = entry.contentCanonicalPath.empty()
                                           ? canonicalWin32Path(entry.nativePath)
                                           : entry.contentCanonicalPath;
        const auto opened = ReadOnlyPayload::open(
            entry.nativePath, *entry.contentIdentity, canonical,
            payloadCancellation);
        if (!opened.ok()) {
            DigestResultInternal result;
            result.cancelled = opened.status == PayloadOpenStatus::Cancelled;
            result.changed = opened.status == PayloadOpenStatus::IdentityChanged ||
                             opened.status == PayloadOpenStatus::PathChanged;
            return result;
        }
        const auto& identification = *entry.contentIdentification;
        const auto slice = opened.payload->view().trySlice(
            identification.payloadOffset, identification.payloadLength);
        if (!slice.has_value()) {
            return {};
        }
        DigestResultInternal result = hashPayloadView(*slice, payloadCancellation);
        if (result.ok) {
            const PayloadRevalidationStatus revalidation =
                opened.payload->revalidate(payloadCancellation);
            result.cancelled = revalidation == PayloadRevalidationStatus::Cancelled;
            result.changed = revalidation == PayloadRevalidationStatus::IdentityChanged ||
                             revalidation == PayloadRevalidationStatus::PathChanged ||
                             revalidation == PayloadRevalidationStatus::Inconsistent;
            if (result.cancelled || result.changed) {
                result.ok = false;
            }
        }
        return result;
    }

    FileIdentity identity{};
    if (entry.contentIdentity.has_value()) {
        identity = *entry.contentIdentity;
    } else {
        const auto queried = queryFileIdentity(entry.nativePath);
        if (!queried.has_value()) {
            return {};
        }
        identity = *queried;
    }
    WindowsFileContentHasher hasher;
    ContentHashRequest request;
    request.path = entry.nativePath;
    request.expectedSize = identity.sizeBytes;
    request.expectedIdentity = identityFromFile(identity);
    request.expectedLastWrite = identity.lastWriteTicks;
    request.kind = ContentHashKind::Full;
    request.cancelled = payloadCancellation;
    const ContentHashResult hashed = hasher.hash(request);
    DigestResultInternal result;
    result.ok = hashed.status == DuplicateFileStatus::Verified;
    result.cancelled = hashed.status == DuplicateFileStatus::Cancelled;
    result.changed = hashed.status == DuplicateFileStatus::ChangedDuringRead ||
                     hashed.status == DuplicateFileStatus::IdentityChanged ||
                     hashed.status == DuplicateFileStatus::SizeChanged;
    result.digest = hashed.digest;
    return result;
}

void performZaloExactCopyComparison(
    std::vector<InternalAccount>& accounts,
    const std::vector<std::wstring>& requestedPaths, ZaloStorageReport& result,
    const Cancellation& cancellation)
{
    ZaloExactCopyReport& report = result.exactCopy;
    report.enabled = !requestedPaths.empty();
    report.comparisonPathsRequested = requestedPaths.size();
    if (requestedPaths.empty()) {
        report.status = ZaloExactCopyStatus::Disabled;
        return;
    }

    struct ScopeRoot {
        std::wstring path;
        std::wstring sortKey;
    };
    std::vector<ScopeRoot> scopes;
    for (const std::wstring& requested : requestedPaths) {
        const std::wstring canonical = canonicalWin32Path(requested);
        if (canonical.empty()) {
            report.skippedInaccessible++;
            continue;
        }
        scopes.push_back({canonical, lowerCopy(canonical)});
    }
    std::sort(scopes.begin(), scopes.end(),
              [](const ScopeRoot& left, const ScopeRoot& right) {
                  if (left.sortKey != right.sortKey) {
                      return left.sortKey < right.sortKey;
                  }
                  // Keep the exact canonical spelling as a deterministic tie
                  // breaker. Case-distinct paths can be separate namespaces on
                  // a case-sensitive directory and must not be silently merged.
                  return left.path < right.path;
              });
    scopes.erase(std::unique(scopes.begin(), scopes.end(),
                             [](const ScopeRoot& left, const ScopeRoot& right) {
                                 return left.path == right.path;
                             }),
                 scopes.end());
    for (std::size_t i = 0; i < scopes.size(); ++i) {
        std::ostringstream alias;
        alias << "comparison-scope-";
        alias.width(2);
        alias.fill('0');
        alias << i + 1U;
        report.comparisonScopeAliases.push_back(alias.str());
    }

    bool partial = report.skippedInaccessible != 0U;
    std::vector<ComparisonFileInternal> discoveredFiles;
    for (std::size_t i = 0; i < scopes.size(); ++i) {
        if (!collectComparisonFilesForRoot(scopes[i].path, i, discoveredFiles,
                                           report, cancellation, partial)) {
            report.status = ZaloExactCopyStatus::Cancelled;
            report.detail = "Zalo exact-copy comparison cancelled";
            return;
        }
    }

    try {
        std::sort(
            discoveredFiles.begin(), discoveredFiles.end(),
            [&cancellation](const ComparisonFileInternal& left,
                            const ComparisonFileInternal& right) {
                if (cancellation.requested()) {
                    throw PostScanCancellation{};
                }
                if (left.sortKey != right.sortKey) {
                    return left.sortKey < right.sortKey;
                }
                if (left.path != right.path) {
                    return left.path < right.path;
                }
                return left.scopeIndex < right.scopeIndex;
            });
    } catch (const PostScanCancellation&) {
        report.status = ZaloExactCopyStatus::Cancelled;
        report.detail = "Zalo exact-copy comparison cancelled";
        return;
    }
    if (cancellation.requested()) {
        report.status = ZaloExactCopyStatus::Cancelled;
        report.detail = "Zalo exact-copy comparison cancelled";
        return;
    }
    std::unordered_map<IdentityKey, std::size_t, IdentityKeyHash> uniqueByIdentity;
    std::vector<ComparisonFileInternal> uniqueFiles;
    for (auto& file : discoveredFiles) {
        const IdentityKey key = makeIdentityKey(file.cleanupIdentity);
        if (key.source == CleanupIdentitySource::Unavailable) {
            ++report.skippedInaccessible;
            partial = true;
            continue;
        }
        const auto [it, inserted] = uniqueByIdentity.emplace(key, uniqueFiles.size());
        if (inserted) {
            file.scopeAlias = report.comparisonScopeAliases[file.scopeIndex];
            uniqueFiles.push_back(std::move(file));
        } else if (file.path == uniqueFiles[it->second].path) {
            // The same namespace path was reached through overlapping explicit
            // scopes. This is scope overlap, not a filesystem hard-link alias.
            ++report.comparisonOverlappingPathsCollapsed;
        } else {
            ++uniqueFiles[it->second].hardLinkAliasCount;
            ++report.comparisonHardLinkAliasesCollapsed;
        }
    }
    report.uniqueComparisonFiles = uniqueFiles.size();
    for (std::size_t i = 0; i < uniqueFiles.size(); ++i) {
        std::ostringstream id;
        id << "comparison-file-";
        id.width(6);
        id.fill('0');
        id << i + 1U;
        uniqueFiles[i].comparisonFileId = id.str();
    }

    WindowsFileContentHasher externalHasher;
    std::map<ByteSize, std::vector<std::size_t>> externalBySize;
    std::map<std::array<std::uint8_t, 32>, std::vector<std::size_t>> externalByDigest;
    for (std::size_t i = 0; i < uniqueFiles.size(); ++i) {
        if (cancellation.requested()) {
            report.status = ZaloExactCopyStatus::Cancelled;
            report.detail = "Zalo exact-copy comparison cancelled";
            return;
        }
        ComparisonFileInternal& file = uniqueFiles[i];
        if (!file.identity.logicalSizeKnown ||
            file.identity.sizeBytes > kZaloExactCopyMaxBytes) {
            ++report.skippedOversized;
            partial = true;
            continue;
        }
        externalBySize[file.identity.sizeBytes].push_back(i);
        ContentHashRequest request;
        request.path = file.path;
        request.expectedSize = file.identity.sizeBytes;
        request.expectedIdentity = file.cleanupIdentity;
        request.expectedLastWrite = file.identity.lastWriteTicks;
        request.kind = ContentHashKind::Full;
        request.cancelled = [&cancellation]() noexcept {
            return cancellation.requested();
        };
        const ContentHashResult hashed = externalHasher.hash(request);
        if (hashed.status == DuplicateFileStatus::Cancelled) {
            report.status = ZaloExactCopyStatus::Cancelled;
            report.detail = "Zalo exact-copy comparison cancelled";
            return;
        }
        if (hashed.status != DuplicateFileStatus::Verified) {
            ++report.skippedUnstable;
            partial = true;
            continue;
        }
        ++report.comparisonFilesHashed;
        externalByDigest[hashed.digest].push_back(i);
    }

    std::unordered_map<IdentityKey, ZaloCopyCandidateInternal, IdentityKeyHash>
        sourceByIdentity;
    for (auto& account : accounts) {
        for (auto& entry : account.entries) {
            if (cancellation.requested()) {
                report.status = ZaloExactCopyStatus::Cancelled;
                report.detail = "Zalo exact-copy comparison cancelled";
                return;
            }
            if (entry.kind != ZaloEntryKind::File || entry.reparsePoint ||
                entry.consistency != EvidenceState::Consistent ||
                !entry.identityKnown || !entry.logicalKnown ||
                entry.reportEntryId.empty()) {
                continue;
            }
            CleanupIdentity identity{};
            if (entry.contentIdentity.has_value()) {
                identity = identityFromFile(*entry.contentIdentity);
            }
            if (!isIdentityAvailable(identity)) {
                const auto queried = queryFileIdentity(entry.nativePath);
                if (queried.has_value()) {
                    identity = identityFromFile(*queried);
                }
            }
            if (!isIdentityAvailable(identity)) {
                ++report.skippedUnsupported;
                partial = true;
                continue;
            }
            const IdentityKey key = makeIdentityKey(identity);
            auto it = sourceByIdentity.find(key);
            if (it == sourceByIdentity.end()) {
                const bool wrapped = entry.contentIdentification.has_value() &&
                                     entry.contentIdentification->identified() &&
                                     entry.contentIdentification->wrapper &&
                                     entry.contentIdentification->payloadLength != 0U;
                sourceByIdentity.emplace(
                    key, ZaloCopyCandidateInternal{&entry, identity, 1U,
                                                   wrapped ? entry.contentIdentification->payloadLength
                                                           : entry.logicalBytes,
                                                   wrapped});
            } else {
                ++it->second.aliasCount;
            }
        }
    }
    report.zaloEntriesConsidered = sourceByIdentity.size();
    for (auto& [key, candidate] : sourceByIdentity) {
        (void)key;
        if (cancellation.requested()) {
            report.status = ZaloExactCopyStatus::Cancelled;
            report.detail = "Zalo exact-copy comparison cancelled";
            return;
        }
        report.zaloHardLinkAliasesCollapsed += candidate.aliasCount > 0U
                                                    ? candidate.aliasCount - 1U
                                                    : 0U;
        if (candidate.bytes > kZaloExactCopyMaxBytes ||
            externalBySize.find(candidate.bytes) == externalBySize.end()) {
            if (candidate.bytes > kZaloExactCopyMaxBytes) {
                ++report.skippedOversized;
                partial = true;
            }
            continue;
        }
        const DigestResultInternal hashed = hashZaloEntry(candidate, cancellation);
        if (hashed.cancelled || cancellation.requested()) {
            report.status = ZaloExactCopyStatus::Cancelled;
            report.detail = "Zalo exact-copy comparison cancelled";
            return;
        }
        if (hashed.changed) {
            ++report.skippedUnstable;
            partial = true;
            continue;
        }
        if (!hashed.ok) {
            ++report.skippedUnsupported;
            partial = true;
            continue;
        }
        const auto matches = externalByDigest.find(hashed.digest);
        if (matches == externalByDigest.end()) {
            continue;
        }
        for (const std::size_t externalIndex : matches->second) {
            const ComparisonFileInternal& file = uniqueFiles[externalIndex];
            if (!identitiesEqual(candidate.cleanupIdentity, file.cleanupIdentity)) {
                ZaloExactCopyMatch match;
                match.zaloEntryId = candidate.entry->reportEntryId;
                match.comparisonFileId = file.comparisonFileId;
                match.comparisonScopeAlias = file.scopeAlias;
                match.payloadKind = candidate.wrapped ? "validated_payload" : "full_file";
                match.proofMethod = candidate.wrapped
                                        ? "sha256_payload_vs_full"
                                        : "full_sha256";
                match.matchedBytes = candidate.bytes;
                match.zaloHardLinkAliasCount = candidate.aliasCount > 0U
                                                    ? candidate.aliasCount - 1U
                                                    : 0U;
                match.comparisonHardLinkAliasCount = file.hardLinkAliasCount;
                report.matches.push_back(std::move(match));
            } else {
                ++report.skippedSameIdentity;
            }
        }
    }
    std::sort(report.matches.begin(), report.matches.end(),
              [](const ZaloExactCopyMatch& left, const ZaloExactCopyMatch& right) {
                  if (left.zaloEntryId != right.zaloEntryId) {
                      return left.zaloEntryId < right.zaloEntryId;
                  }
                  return left.comparisonFileId < right.comparisonFileId;
              });
    report.status = partial ? ZaloExactCopyStatus::Partial
                            : ZaloExactCopyStatus::Complete;
    report.detail = partial
                        ? "Zalo exact-copy comparison completed with unavailable evidence"
                        : "Zalo exact-copy comparison completed";
}

void resetCancelledInspectionResult(ZaloStorageReport& result)
{
    result.status = ZaloStorageStatus::Cancelled;
    result.detail = "Zalo inspection cancelled";
    result.discovery = {};
    result.discovery.status = ZaloStorageStatus::Cancelled;
    result.discovery.detail = "Zalo inspection cancelled";
    result.roots.clear();
    result.accounts.clear();
    result.accounting = {};
    result.exactCopy = {};
}

std::string accountAliasAt(const ZaloDiscoveryReport& discovery,
                           std::size_t rootIndex, std::size_t accountIndex)
{
    if (rootIndex >= discovery.roots.size() ||
        accountIndex >= discovery.roots[rootIndex].accountAliases.size()) {
        return {};
    }
    return discovery.roots[rootIndex].accountAliases[accountIndex];
}

}  // namespace

const char* toString(ZaloStorageStatus status) noexcept
{
    switch (status) {
    case ZaloStorageStatus::Complete:
        return "Complete";
    case ZaloStorageStatus::Partial:
        return "Partial";
    case ZaloStorageStatus::Cancelled:
        return "Cancelled";
    case ZaloStorageStatus::ConfigUnavailable:
        return "ConfigUnavailable";
    case ZaloStorageStatus::NoRoots:
        return "NoRoots";
    case ZaloStorageStatus::InvalidRoot:
        return "InvalidRoot";
    case ZaloStorageStatus::AccessDenied:
        return "AccessDenied";
    case ZaloStorageStatus::Error:
        return "Error";
    }
    return "Error";
}

const char* toString(ZaloExactCopyStatus status) noexcept
{
    switch (status) {
    case ZaloExactCopyStatus::Disabled:
        return "Disabled";
    case ZaloExactCopyStatus::Complete:
        return "Complete";
    case ZaloExactCopyStatus::Partial:
        return "Partial";
    case ZaloExactCopyStatus::Cancelled:
        return "Cancelled";
    }
    return "Disabled";
}

std::string ZaloExactCopyReport::toJson() const
{
    std::ostringstream out;
    out << "{\"enabled\":" << jsonBool(enabled)
        << ",\"status\":" << jsonString(toString(status))
        << ",\"detail\":" << jsonString(detail)
        << ",\"proof_method\":" << jsonString(proofMethod)
        << ",\"comparison_paths_requested\":"
        << jsonUInt(comparisonPathsRequested)
        << ",\"comparison_files_enumerated\":"
        << jsonUInt(comparisonFilesEnumerated)
        << ",\"comparison_files_hashed\":"
        << jsonUInt(comparisonFilesHashed)
        << ",\"unique_comparison_files\":"
        << jsonUInt(uniqueComparisonFiles)
        << ",\"comparison_hard_link_aliases_collapsed\":"
        << jsonUInt(comparisonHardLinkAliasesCollapsed)
        << ",\"comparison_overlapping_paths_collapsed\":"
        << jsonUInt(comparisonOverlappingPathsCollapsed)
        << ",\"zalo_entries_considered\":"
        << jsonUInt(zaloEntriesConsidered)
        << ",\"zalo_hard_link_aliases_collapsed\":"
        << jsonUInt(zaloHardLinkAliasesCollapsed)
        << ",\"skipped_reparse_points\":"
        << jsonUInt(skippedReparsePoints)
        << ",\"skipped_inaccessible\":" << jsonUInt(skippedInaccessible)
        << ",\"skipped_unstable\":" << jsonUInt(skippedUnstable)
        << ",\"skipped_oversized\":" << jsonUInt(skippedOversized)
        << ",\"skipped_unsupported\":" << jsonUInt(skippedUnsupported)
        << ",\"skipped_same_identity\":" << jsonUInt(skippedSameIdentity)
        << ",\"scope_aliases\":[";
    for (std::size_t i = 0; i < comparisonScopeAliases.size(); ++i) {
        if (i != 0U) {
            out << ',';
        }
        out << jsonString(comparisonScopeAliases[i]);
    }
    out << "],\"matches\":[";
    for (std::size_t i = 0; i < matches.size(); ++i) {
        if (i != 0U) {
            out << ',';
        }
        const auto& match = matches[i];
        out << "{\"zalo_entry_id\":" << jsonString(match.zaloEntryId)
            << ",\"comparison_file_id\":"
            << jsonString(match.comparisonFileId)
            << ",\"comparison_scope_alias\":"
            << jsonString(match.comparisonScopeAlias)
            << ",\"proof_method\":" << jsonString(match.proofMethod)
            << ",\"payload_kind\":" << jsonString(match.payloadKind)
            << ",\"matched_bytes\":" << jsonUInt(match.matchedBytes)
            << ",\"zalo_hard_link_alias_count\":"
            << jsonUInt(match.zaloHardLinkAliasCount)
            << ",\"comparison_hard_link_alias_count\":"
            << jsonUInt(match.comparisonHardLinkAliasCount) << "}";
    }
    out << "]}";
    return out.str();
}

const char* toString(ZaloEntryKind kind) noexcept
{
    switch (kind) {
    case ZaloEntryKind::File:
        return "File";
    case ZaloEntryKind::Directory:
        return "Directory";
    case ZaloEntryKind::ReparseFile:
        return "ReparseFile";
    case ZaloEntryKind::ReparseDirectory:
        return "ReparseDirectory";
    }
    return "File";
}

const char* toString(ZaloEntryConsistency consistency) noexcept
{
    switch (consistency) {
    case ZaloEntryConsistency::Consistent:
        return "Consistent";
    case ZaloEntryConsistency::Unknown:
        return "Unknown";
    case ZaloEntryConsistency::Inconsistent:
        return "Inconsistent";
    case ZaloEntryConsistency::Changed:
        return "Changed";
    }
    return "Unknown";
}

ZaloDiscoveryReport discoverZaloRoots(const ZaloDiscoveryOptions& options)
{
    return discoverZaloRoots(options, {});
}

ZaloDiscoveryReport discoverZaloRoots(const ZaloDiscoveryOptions& options,
                                      std::stop_token stop)
{
    const Cancellation cancellation{stop, options.stopToken};
    return discoverInternal(options, cancellation).report;
}

ZaloStorageReport inspectZaloStorage(const ZaloInspectionOptions& options)
{
    return inspectZaloStorage(options, {});
}

ZaloStorageReport inspectZaloStorage(const ZaloInspectionOptions& options,
                                     std::stop_token stop)
{
    const Cancellation cancellation{stop, options.stopToken};
    InternalDiscovery discovered = discoverInternal(options, cancellation);
    ZaloStorageReport result;
    result.discovery = discovered.report;
    result.status = discovered.report.status;
    result.detail = discovered.report.detail;
    result.roots = discovered.report.roots;

    if (discovered.cancelled || cancellation.requested()) {
        resetCancelledInspectionResult(result);
        return result;
    }

    std::vector<InternalAccount> accounts;
    std::size_t accountCount = 0;
    for (const auto& root : discovered.roots) {
        if (cancellation.requested()) {
            resetCancelledInspectionResult(result);
            return result;
        }
        accountCount += root.accounts.size();
    }
    if (cancellation.requested()) {
        resetCancelledInspectionResult(result);
        return result;
    }
    accounts.reserve(accountCount);
    for (std::size_t rootIndex = 0; rootIndex < discovered.roots.size();
         ++rootIndex) {
        if (cancellation.requested()) {
            resetCancelledInspectionResult(result);
            return result;
        }
        const InternalRoot& root = discovered.roots[rootIndex];
        for (std::size_t accountIndex = 0;
             accountIndex < root.accounts.size(); ++accountIndex) {
            if (cancellation.requested()) {
                resetCancelledInspectionResult(result);
                return result;
            }
            InternalAccount account;
            account.path = root.accounts[accountIndex];
            account.rootAlias = root.rootAlias;
            account.accountAlias =
                accountAliasAt(discovered.report, rootIndex, accountIndex);
            account.report.rootAlias = account.rootAlias;
            account.report.accountAlias = account.accountAlias;
            scanAccount(account, cancellation);
            if (account.cancelled || cancellation.requested()) {
                resetCancelledInspectionResult(result);
                return result;
            }
            accounts.push_back(std::move(account));
        }
    }

    std::vector<ObservationRef> allRefs;
    for (auto& account : accounts) {
        if (cancellation.requested()) {
            resetCancelledInspectionResult(result);
            return result;
        }
        for (auto& observation : account.observations) {
            if (cancellation.requested()) {
                resetCancelledInspectionResult(result);
                return result;
            }
            allRefs.push_back({&observation,
                               &account.entries[observation.entryIndex],
                               &account});
        }
    }
    if (!identifyContentGroups(allRefs, cancellation) ||
        cancellation.requested()) {
        resetCancelledInspectionResult(result);
        return result;
    }

    bool incomplete = false;
    for (const auto& account : accounts) {
        if (cancellation.requested()) {
            resetCancelledInspectionResult(result);
            return result;
        }
        incomplete = incomplete || !account.report.complete;
    }
    const bool overallTraversalComplete =
        !incomplete && discovered.report.status == ZaloStorageStatus::Complete;
    const auto globalAccounting = summarizeAccounting(
        allRefs, true, overallTraversalComplete, cancellation);
    if (!globalAccounting.has_value()) {
        resetCancelledInspectionResult(result);
        return result;
    }
    result.accounting = *globalAccounting;

    for (auto& account : accounts) {
        if (cancellation.requested()) {
            resetCancelledInspectionResult(result);
            return result;
        }
        std::vector<ObservationRef> refs;
        refs.reserve(account.observations.size());
        for (auto& observation : account.observations) {
            if (cancellation.requested()) {
                resetCancelledInspectionResult(result);
                return result;
            }
            refs.push_back({&observation,
                            &account.entries[observation.entryIndex],
                            &account});
        }
        const auto accounting = summarizeAccounting(
            refs, false, account.report.complete, cancellation);
        if (!accounting.has_value()) {
            resetCancelledInspectionResult(result);
            return result;
        }
        account.report.accounting = *accounting;
    }

    if (cancellation.requested()) {
        resetCancelledInspectionResult(result);
        return result;
    }
    if (!finalizePublicReports(accounts, result, cancellation) ||
        cancellation.requested()) {
        resetCancelledInspectionResult(result);
        return result;
    }
    performZaloExactCopyComparison(accounts, options.comparisonPaths, result,
                                   cancellation);
    if (cancellation.requested()) {
        resetCancelledInspectionResult(result);
        return result;
    }

    if (discovered.report.status == ZaloStorageStatus::ConfigUnavailable) {
        result.status = ZaloStorageStatus::ConfigUnavailable;
        result.detail = discovered.report.detail;
    } else if (incomplete || discovered.report.status == ZaloStorageStatus::Partial) {
        result.status = ZaloStorageStatus::Partial;
        result.detail = "Zalo inspection completed with unavailable evidence";
    } else if (result.accounts.empty()) {
        result.status = discovered.report.status;
    } else {
        result.status = ZaloStorageStatus::Complete;
        result.detail = "Zalo inspection completed";
    }
    return result;
}

ZaloDiscoveryReport ZaloStorageInspector::discover(
    const ZaloDiscoveryOptions& options) const
{
    return discoverZaloRoots(options);
}

ZaloDiscoveryReport ZaloStorageInspector::discover(
    const ZaloDiscoveryOptions& options, std::stop_token stop) const
{
    return discoverZaloRoots(options, stop);
}

ZaloStorageReport ZaloStorageInspector::inspect(
    const ZaloInspectionOptions& options) const
{
    return inspectZaloStorage(options);
}

ZaloStorageReport ZaloStorageInspector::inspect(
    const ZaloInspectionOptions& options, std::stop_token stop) const
{
    return inspectZaloStorage(options, stop);
}

}  // namespace spacelens
