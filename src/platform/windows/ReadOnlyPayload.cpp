#include "platform/windows/ReadOnlyPayload.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace spacelens {
namespace {

bool cancelled(const PayloadCancellation& cancellation) noexcept
{
    if (!cancellation) {
        return false;
    }
    try {
        return cancellation();
    } catch (...) {
        // Cancellation is caller-controlled. Fail closed instead of allowing a
        // throwing callback to escape through read-only inspection code.
        return true;
    }
}

class ScopedHandle final {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) noexcept
        : m_handle(handle)
    {
    }

    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : m_handle(other.m_handle)
    {
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept
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

    HANDLE release() noexcept
    {
        const HANDLE result = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return result;
    }

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

std::wstring finalPathFromHandle(HANDLE handle)
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::vector<wchar_t> buffer(512U);
    for (;;) {
        const DWORD length = ::GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED);
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return canonicalWin32Path(
                std::wstring_view(buffer.data(), length));
        }
        if (buffer.size() >= 32768U) {
            return {};
        }
        buffer.resize(buffer.size() * 2U);
    }
}

bool sameObjectIdentity(const FileIdentity& expected,
                        const FileIdentity& actual) noexcept
{
    if (!expected.identityKnown || !actual.identityKnown ||
        expected.isDirectory || actual.isDirectory) {
        return false;
    }
    if (expected.fileId128Known && actual.fileId128Known) {
        if (expected.volumeSerial != actual.volumeSerial ||
            expected.fileId128 != actual.fileId128) {
            return false;
        }
    } else if (expected.fileIndex64Known && actual.fileIndex64Known) {
        if (expected.volumeSerial != actual.volumeSerial ||
            expected.fileId != actual.fileId) {
            return false;
        }
    } else {
        return false;
    }

    if (expected.logicalSizeKnown && actual.logicalSizeKnown &&
        expected.sizeBytes != actual.sizeBytes) {
        return false;
    }
    return true;
}

bool payloadStabilityObservationComplete(
    const FileIdentity& observation) noexcept
{
    return observation.identityKnown && observation.logicalSizeKnown &&
           observation.basicMetadataKnown && observation.linkCountKnown &&
           observation.observationConsistent;
}

bool payloadObservationComplete(const FileIdentity& observation) noexcept
{
    return payloadStabilityObservationComplete(observation) &&
           observation.allocatedBytes.has_value() ==
               observation.allocationKnown;
}

// Compare every retained file observation except LastAccessTime. Windows may
// update that field merely by opening or reading the retained handle; the
// remaining fields are stable evidence for this read-only payload binding.
bool sameStableObservation(const FileIdentity& expected,
                           const FileIdentity& actual) noexcept
{
    return expected.fileId == actual.fileId &&
           expected.volumeSerial == actual.volumeSerial &&
           expected.fileId128 == actual.fileId128 &&
           expected.fileId128Known == actual.fileId128Known &&
           expected.fileIndex64Known == actual.fileIndex64Known &&
           expected.identityKnown == actual.identityKnown &&
           expected.isDirectory == actual.isDirectory &&
           expected.sizeBytes == actual.sizeBytes &&
           expected.logicalSizeKnown == actual.logicalSizeKnown &&
           expected.allocatedBytes == actual.allocatedBytes &&
           expected.allocationKnown == actual.allocationKnown &&
           expected.numberOfLinks == actual.numberOfLinks &&
           expected.linkCountKnown == actual.linkCountKnown &&
           expected.sparse == actual.sparse &&
           expected.compressed == actual.compressed &&
           expected.creationTimeTicks == actual.creationTimeTicks &&
           expected.changeTimeTicks == actual.changeTimeTicks &&
           expected.lastWriteTicks == actual.lastWriteTicks &&
           expected.basicMetadataKnown == actual.basicMetadataKnown &&
           expected.attributes == actual.attributes &&
           expected.observationConsistent == actual.observationConsistent;
}

// Repeated content reads need identity and mutation evidence, not another
// potentially expensive named-stream allocation inventory. Allocation remains
// part of the full open-time observation above.
bool samePayloadObservation(const FileIdentity& expected,
                            const FileIdentity& actual) noexcept
{
    return expected.fileId == actual.fileId &&
           expected.volumeSerial == actual.volumeSerial &&
           expected.fileId128 == actual.fileId128 &&
           expected.fileId128Known == actual.fileId128Known &&
           expected.fileIndex64Known == actual.fileIndex64Known &&
           expected.identityKnown == actual.identityKnown &&
           expected.isDirectory == actual.isDirectory &&
           expected.sizeBytes == actual.sizeBytes &&
           expected.logicalSizeKnown == actual.logicalSizeKnown &&
           expected.numberOfLinks == actual.numberOfLinks &&
           expected.linkCountKnown == actual.linkCountKnown &&
           expected.sparse == actual.sparse &&
           expected.compressed == actual.compressed &&
           expected.creationTimeTicks == actual.creationTimeTicks &&
           expected.changeTimeTicks == actual.changeTimeTicks &&
           expected.lastWriteTicks == actual.lastWriteTicks &&
           expected.basicMetadataKnown == actual.basicMetadataKnown &&
           expected.attributes == actual.attributes &&
           expected.observationConsistent == actual.observationConsistent;
}

PayloadOpenStatus mapOpenError(DWORD error) noexcept
{
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_INVALID_NAME) {
        return PayloadOpenStatus::Missing;
    }
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
        error == ERROR_LOCK_VIOLATION || error == ERROR_PRIVILEGE_NOT_HELD) {
        return PayloadOpenStatus::AccessDenied;
    }
    return PayloadOpenStatus::Error;
}

PayloadRevalidationStatus revalidateState(
    const PayloadView::State& state, const PayloadCancellation& cancellation);

}  // namespace

struct PayloadView::State {
    HANDLE handle = INVALID_HANDLE_VALUE;
    FileIdentity expected{};
    FileIdentity opened{};
    std::wstring expectedCanonicalPath;
    ByteSize size = 0;

    ~State()
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
        }
    }
};

struct ReadOnlyPayloadFactory final {
    static std::unique_ptr<ReadOnlyPayload> make(
        std::shared_ptr<PayloadView::State> state)
    {
        return std::unique_ptr<ReadOnlyPayload>(
            new ReadOnlyPayload(std::move(state)));
    }
};

namespace {

PayloadReadStatus mapReadRevalidation(PayloadRevalidationStatus status) noexcept
{
    switch (status) {
    case PayloadRevalidationStatus::Valid:
        return PayloadReadStatus::Ok;
    case PayloadRevalidationStatus::Cancelled:
        return PayloadReadStatus::Cancelled;
    case PayloadRevalidationStatus::IdentityChanged:
    case PayloadRevalidationStatus::Inconsistent:
    case PayloadRevalidationStatus::NotRegularFile:
    case PayloadRevalidationStatus::ReparsePoint:
        return PayloadReadStatus::IdentityChanged;
    case PayloadRevalidationStatus::PathChanged:
        return PayloadReadStatus::PathChanged;
    case PayloadRevalidationStatus::Error:
        return PayloadReadStatus::ReadError;
    }
    return PayloadReadStatus::ReadError;
}

PayloadReadResult preferChangedState(
    PayloadReadResult result, const PayloadView::State& state,
    const PayloadCancellation& cancellation)
{
    const PayloadReadStatus revalidated =
        mapReadRevalidation(revalidateState(state, cancellation));
    if (revalidated == PayloadReadStatus::Cancelled) {
        result.status = revalidated;
        result.nativeError = ERROR_OPERATION_ABORTED;
    } else if (revalidated == PayloadReadStatus::IdentityChanged ||
               revalidated == PayloadReadStatus::PathChanged) {
        result.status = revalidated;
        result.nativeError = ERROR_INVALID_DATA;
    }
    return result;
}

PayloadReadResult readState(const std::shared_ptr<PayloadView::State>& state,
                            ByteSize absoluteOffset, void* destination,
                            std::size_t length,
                            const PayloadCancellation& cancellation)
{
    PayloadReadResult result;
    if (state == nullptr) {
        result.status = PayloadReadStatus::Bounds;
        return result;
    }
    if (cancelled(cancellation)) {
        result.status = PayloadReadStatus::Cancelled;
        return result;
    }
    if (static_cast<std::uintmax_t>(length) >
        static_cast<std::uintmax_t>(std::numeric_limits<ByteSize>::max())) {
        result.status = PayloadReadStatus::Bounds;
        result.nativeError = ERROR_ARITHMETIC_OVERFLOW;
        return result;
    }
    const ByteSize readLength = static_cast<ByteSize>(length);
    if (absoluteOffset > std::numeric_limits<ByteSize>::max() - readLength) {
        result.status = PayloadReadStatus::Bounds;
        result.nativeError = ERROR_ARITHMETIC_OVERFLOW;
        return result;
    }
    if (length != 0U && destination == nullptr) {
        result.status = PayloadReadStatus::ReadError;
        result.nativeError = ERROR_INVALID_PARAMETER;
        return result;
    }
    if (length == 0U) {
        const PayloadRevalidationStatus status =
            revalidateState(*state, cancellation);
        result.status = mapReadRevalidation(status);
        if (result.status != PayloadReadStatus::Ok &&
            result.status != PayloadReadStatus::Cancelled &&
            result.nativeError == 0U) {
            result.nativeError = ERROR_INVALID_DATA;
        }
        return result;
    }

    ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        result.status = PayloadReadStatus::ReadError;
        result.nativeError = ::GetLastError();
        return result;
    }

    const std::size_t maximumChunk =
        std::min<std::size_t>(1024U * 1024U,
                              static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
    std::size_t completed = 0;
    while (completed < length) {
        if (cancelled(cancellation)) {
            result.status = PayloadReadStatus::Cancelled;
            result.bytesRead = completed;
            return result;
        }

        const std::size_t requestSize =
            std::min(maximumChunk, length - completed);
        const ByteSize chunkOffset =
            absoluteOffset + static_cast<ByteSize>(completed);
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(chunkOffset & 0xffffffffULL);
        overlapped.OffsetHigh = static_cast<DWORD>(chunkOffset >> 32U);
        overlapped.hEvent = event.get();
        (void)::ResetEvent(event.get());

        DWORD transferred = 0;
        BOOL ok = ::ReadFile(
            state->handle,
            static_cast<std::uint8_t*>(destination) + completed,
            static_cast<DWORD>(requestSize), &transferred, &overlapped);
        if (ok == FALSE) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_IO_PENDING) {
                result.status = PayloadReadStatus::ReadError;
                result.nativeError = error;
                result.bytesRead = completed;
                return preferChangedState(std::move(result), *state,
                                          cancellation);
            }
        }
        if (ok == FALSE) {
            for (;;) {
                if (cancelled(cancellation)) {
                    (void)::CancelIoEx(state->handle, &overlapped);
                    (void)::GetOverlappedResult(state->handle, &overlapped,
                                                &transferred, TRUE);
                    result.status = PayloadReadStatus::Cancelled;
                    result.bytesRead = completed;
                    return result;
                }
                const DWORD wait = ::WaitForSingleObject(event.get(), 10U);
                if (wait == WAIT_OBJECT_0) {
                    break;
                }
                if (wait == WAIT_FAILED) {
                    const DWORD error = ::GetLastError();
                    (void)::CancelIoEx(state->handle, &overlapped);
                    (void)::GetOverlappedResult(state->handle, &overlapped,
                                                &transferred, TRUE);
                    result.status = PayloadReadStatus::ReadError;
                    result.nativeError = error;
                    result.bytesRead = completed;
                    return preferChangedState(std::move(result), *state,
                                              cancellation);
                }
            }
            if (!::GetOverlappedResult(state->handle, &overlapped, &transferred,
                                       FALSE)) {
                const DWORD error = ::GetLastError();
                if (error == ERROR_OPERATION_ABORTED &&
                    cancelled(cancellation)) {
                    result.status = PayloadReadStatus::Cancelled;
                } else {
                    result.status = PayloadReadStatus::ReadError;
                    result.nativeError = error;
                }
                result.bytesRead = completed;
                return preferChangedState(std::move(result), *state,
                                          cancellation);
            }
        }

        if (transferred == 0U) {
            result.status = PayloadReadStatus::EndOfFile;
            result.nativeError = ERROR_HANDLE_EOF;
            result.bytesRead = completed;
            return preferChangedState(std::move(result), *state,
                                      cancellation);
        }
        if (transferred > requestSize) {
            result.status = PayloadReadStatus::ReadError;
            result.nativeError = ERROR_INVALID_DATA;
            result.bytesRead = completed;
            return result;
        }
        completed += transferred;
    }

    result.bytesRead = completed;
    const PayloadRevalidationStatus status =
        revalidateState(*state, cancellation);
    result.status = mapReadRevalidation(status);
    if (result.status != PayloadReadStatus::Ok &&
        result.nativeError == 0U) {
        result.nativeError = ERROR_INVALID_DATA;
    }
    return result;
}

}  // namespace

namespace {

PayloadRevalidationStatus revalidateState(
    const PayloadView::State& state, const PayloadCancellation& cancellation)
{
    if (cancelled(cancellation)) {
        return PayloadRevalidationStatus::Cancelled;
    }
    if (state.handle == nullptr || state.handle == INVALID_HANDLE_VALUE) {
        return PayloadRevalidationStatus::Error;
    }
    if (!payloadObservationComplete(state.expected) ||
        !payloadObservationComplete(state.opened) ||
        !sameStableObservation(state.expected, state.opened)) {
        return PayloadRevalidationStatus::Inconsistent;
    }

    const auto observed = queryFileIdentityFromHandleLightweight(state.handle);
    if (!observed) {
        return PayloadRevalidationStatus::Error;
    }
    if (cancelled(cancellation)) {
        return PayloadRevalidationStatus::Cancelled;
    }
    if ((observed->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return PayloadRevalidationStatus::ReparsePoint;
    }
    if (observed->isDirectory) {
        return PayloadRevalidationStatus::NotRegularFile;
    }
    if (!payloadStabilityObservationComplete(*observed)) {
        return PayloadRevalidationStatus::Inconsistent;
    }
    if (!sameObjectIdentity(state.expected, *observed) ||
        !sameObjectIdentity(state.opened, *observed) ||
        !samePayloadObservation(state.expected, *observed) ||
        !samePayloadObservation(state.opened, *observed)) {
        return PayloadRevalidationStatus::IdentityChanged;
    }
    if (observed->sizeBytes != state.size) {
        return PayloadRevalidationStatus::IdentityChanged;
    }

    const std::wstring finalPath = finalPathFromHandle(state.handle);
    if (cancelled(cancellation)) {
        return PayloadRevalidationStatus::Cancelled;
    }
    if (finalPath.empty() || finalPath != state.expectedCanonicalPath) {
        return PayloadRevalidationStatus::PathChanged;
    }
    return PayloadRevalidationStatus::Valid;
}

PayloadOpenResult openPayload(const std::wstring& path,
                              const FileIdentity& expectedIdentity,
                              std::wstring_view expectedCanonicalPath,
                              const PayloadCancellation& cancellation)
{
    PayloadOpenResult result;
    if (cancelled(cancellation)) {
        result.status = PayloadOpenStatus::Cancelled;
        return result;
    }
    if (path.empty() || path.find(L'\0') != std::wstring::npos ||
        expectedCanonicalPath.find(L'\0') != std::wstring_view::npos) {
        result.status = PayloadOpenStatus::Error;
        result.nativeError = ERROR_INVALID_NAME;
        return result;
    }
    if (!expectedIdentity.identityKnown || expectedIdentity.isDirectory) {
        result.status = PayloadOpenStatus::IdentityChanged;
        result.nativeError = ERROR_INVALID_DATA;
        return result;
    }
    if (!payloadObservationComplete(expectedIdentity)) {
        result.status = PayloadOpenStatus::Inconsistent;
        result.nativeError = ERROR_INVALID_DATA;
        return result;
    }

    std::wstring expectedPath(expectedCanonicalPath);
    if (expectedPath.empty()) {
        expectedPath = canonicalWin32Path(path);
    } else {
        expectedPath = canonicalWin32Path(expectedPath);
    }
    if (expectedPath.empty()) {
        result.status = PayloadOpenStatus::PathChanged;
        result.nativeError = ERROR_INVALID_NAME;
        return result;
    }

    ScopedHandle handle(::CreateFileW(
        path.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_RANDOM_ACCESS,
        nullptr));
    if (!handle.valid()) {
        result.nativeError = ::GetLastError();
        result.status = mapOpenError(result.nativeError);
        return result;
    }
    if (cancelled(cancellation)) {
        result.status = PayloadOpenStatus::Cancelled;
        return result;
    }

    const auto observed = queryFileIdentityFromHandle(handle.get());
    if (!observed) {
        result.status = PayloadOpenStatus::Error;
        result.nativeError = ERROR_INVALID_DATA;
        return result;
    }
    if (cancelled(cancellation)) {
        result.status = PayloadOpenStatus::Cancelled;
        return result;
    }
    if ((observed->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.status = PayloadOpenStatus::ReparsePoint;
        result.nativeError = ERROR_REPARSE_TAG_INVALID;
        return result;
    }
    if (observed->isDirectory) {
        result.status = PayloadOpenStatus::NotRegularFile;
        result.nativeError = ERROR_DIRECTORY;
        return result;
    }
    if (!payloadObservationComplete(*observed)) {
        result.status = PayloadOpenStatus::Inconsistent;
        result.nativeError = ERROR_INVALID_DATA;
        return result;
    }
    if (!sameObjectIdentity(expectedIdentity, *observed) ||
        !sameStableObservation(expectedIdentity, *observed)) {
        result.status = PayloadOpenStatus::IdentityChanged;
        result.nativeError = ERROR_INVALID_DATA;
        return result;
    }
    const std::wstring actualPath = finalPathFromHandle(handle.get());
    if (cancelled(cancellation)) {
        result.status = PayloadOpenStatus::Cancelled;
        return result;
    }
    if (actualPath.empty() || actualPath != expectedPath) {
        result.status = PayloadOpenStatus::PathChanged;
        result.nativeError = ERROR_INVALID_DATA;
        return result;
    }

    auto state = std::make_shared<PayloadView::State>();
    state->handle = handle.release();
    state->expected = expectedIdentity;
    state->opened = *observed;
    state->expectedCanonicalPath = actualPath;
    state->size = observed->sizeBytes;

    const PayloadRevalidationStatus validation =
        revalidateState(*state, cancellation);
    if (validation != PayloadRevalidationStatus::Valid) {
        switch (validation) {
        case PayloadRevalidationStatus::Cancelled:
            result.status = PayloadOpenStatus::Cancelled;
            break;
        case PayloadRevalidationStatus::IdentityChanged:
            result.status = PayloadOpenStatus::IdentityChanged;
            break;
        case PayloadRevalidationStatus::PathChanged:
            result.status = PayloadOpenStatus::PathChanged;
            break;
        case PayloadRevalidationStatus::NotRegularFile:
            result.status = PayloadOpenStatus::NotRegularFile;
            break;
        case PayloadRevalidationStatus::ReparsePoint:
            result.status = PayloadOpenStatus::ReparsePoint;
            break;
        case PayloadRevalidationStatus::Inconsistent:
            result.status = PayloadOpenStatus::Inconsistent;
            break;
        case PayloadRevalidationStatus::Error:
            result.status = PayloadOpenStatus::Error;
            break;
        case PayloadRevalidationStatus::Valid:
            break;
        }
        result.nativeError = ERROR_INVALID_DATA;
        return result;
    }

    result.status = PayloadOpenStatus::Opened;
    result.payload = ReadOnlyPayloadFactory::make(std::move(state));
    return result;
}

}  // namespace

PayloadView::PayloadView(std::shared_ptr<State> state, ByteSize offset,
                         ByteSize length) noexcept
    : m_state(std::move(state)), m_offset(offset), m_length(length)
{
}

bool PayloadView::valid() const noexcept
{
    return m_state != nullptr && m_offset <= m_state->size &&
           m_length <= m_state->size - m_offset;
}

ByteSize PayloadView::size() const noexcept
{
    return valid() ? m_length : 0;
}

PayloadView PayloadView::slice(ByteSize offset, ByteSize length) const
{
    try {
        if (!valid() || offset > m_length || length > m_length - offset) {
            return {};
        }
        return PayloadView(m_state, m_offset + offset, length);
    } catch (...) {
        return {};
    }
}

std::optional<PayloadView> PayloadView::trySlice(ByteSize offset,
                                                 ByteSize length) const
{
    PayloadView result = slice(offset, length);
    if (!result.valid()) {
        return std::nullopt;
    }
    return result;
}

PayloadReadResult PayloadView::readAt(ByteSize offset, void* buffer,
                                      std::size_t length,
                                      PayloadCancellation cancellation) const
{
    try {
        PayloadReadResult result;
        if (!valid() || offset > m_length ||
            static_cast<std::uintmax_t>(length) >
                static_cast<std::uintmax_t>(m_length - offset)) {
            result.status = PayloadReadStatus::Bounds;
            result.nativeError = ERROR_INVALID_PARAMETER;
            return result;
        }
        if (length != 0U && buffer == nullptr) {
            result.status = PayloadReadStatus::ReadError;
            result.nativeError = ERROR_INVALID_PARAMETER;
            return result;
        }
        if (m_offset > std::numeric_limits<ByteSize>::max() - offset) {
            result.status = PayloadReadStatus::Bounds;
            result.nativeError = ERROR_ARITHMETIC_OVERFLOW;
            return result;
        }
        return readState(m_state, m_offset + offset, buffer, length,
                         std::move(cancellation));
    } catch (...) {
        return PayloadReadResult{PayloadReadStatus::ReadError, 0U,
                                 ERROR_NOT_ENOUGH_MEMORY};
    }
}

PayloadReadResult PayloadView::readAt(ByteSize offset, void* buffer,
                                      std::size_t length,
                                      std::stop_token stop) const
{
    return readAt(offset, buffer, length,
                  [stop]() noexcept { return stop.stop_requested(); });
}

ReadOnlyPayload::~ReadOnlyPayload() = default;

ReadOnlyPayload::ReadOnlyPayload(std::shared_ptr<PayloadView::State> state) noexcept
    : m_state(std::move(state))
{
}

PayloadOpenResult ReadOnlyPayload::open(
    const std::wstring& path, const FileIdentity& expectedIdentity,
    std::wstring_view expectedCanonicalPath, PayloadCancellation cancellation)
{
    try {
        return openPayload(path, expectedIdentity, expectedCanonicalPath,
                           std::move(cancellation));
    } catch (...) {
        PayloadOpenResult result;
        result.status = PayloadOpenStatus::Error;
        result.nativeError = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    }
}

PayloadOpenResult ReadOnlyPayload::open(
    const std::wstring& path, const FileIdentity& expectedIdentity,
    std::wstring_view expectedCanonicalPath, std::stop_token stop)
{
    try {
        return open(path, expectedIdentity, expectedCanonicalPath,
                    [stop]() noexcept { return stop.stop_requested(); });
    } catch (...) {
        PayloadOpenResult result;
        result.status = PayloadOpenStatus::Error;
        result.nativeError = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    }
}

PayloadOpenResult ReadOnlyPayload::open(
    const std::wstring& path, const FileIdentity& expectedIdentity,
    PayloadCancellation cancellation)
{
    try {
        return openPayload(path, expectedIdentity, {}, std::move(cancellation));
    } catch (...) {
        PayloadOpenResult result;
        result.status = PayloadOpenStatus::Error;
        result.nativeError = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    }
}

PayloadOpenResult ReadOnlyPayload::open(
    const std::wstring& path, const FileIdentity& expectedIdentity,
    std::stop_token stop)
{
    try {
        return open(path, expectedIdentity,
                    [stop]() noexcept { return stop.stop_requested(); });
    } catch (...) {
        PayloadOpenResult result;
        result.status = PayloadOpenStatus::Error;
        result.nativeError = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    }
}

bool ReadOnlyPayload::valid() const noexcept
{
    return m_state != nullptr && m_state->handle != INVALID_HANDLE_VALUE &&
           m_state->handle != nullptr;
}

ByteSize ReadOnlyPayload::size() const noexcept
{
    return valid() ? m_state->size : 0;
}

PayloadView ReadOnlyPayload::view() const
{
    try {
        return valid() ? PayloadView(m_state, 0, m_state->size) : PayloadView{};
    } catch (...) {
        return {};
    }
}

PayloadView ReadOnlyPayload::slice(ByteSize offset, ByteSize length) const
{
    try {
        return view().slice(offset, length);
    } catch (...) {
        return {};
    }
}

PayloadReadResult ReadOnlyPayload::readAt(ByteSize offset, void* buffer,
                                          std::size_t length,
                                          PayloadCancellation cancellation) const
{
    return view().readAt(offset, buffer, length, std::move(cancellation));
}

PayloadReadResult ReadOnlyPayload::readAt(ByteSize offset, void* buffer,
                                          std::size_t length,
                                          std::stop_token stop) const
{
    return view().readAt(offset, buffer, length, stop);
}

PayloadRevalidationStatus ReadOnlyPayload::revalidate(
    PayloadCancellation cancellation) const
{
    try {
        if (!valid()) {
            return PayloadRevalidationStatus::Error;
        }
        return revalidateState(*m_state, std::move(cancellation));
    } catch (...) {
        return PayloadRevalidationStatus::Error;
    }
}

PayloadRevalidationStatus ReadOnlyPayload::revalidate(std::stop_token stop) const
{
    return revalidate([stop]() noexcept { return stop.stop_requested(); });
}

const char* toString(PayloadOpenStatus status) noexcept
{
    switch (status) {
    case PayloadOpenStatus::Opened:
        return "Opened";
    case PayloadOpenStatus::Cancelled:
        return "Cancelled";
    case PayloadOpenStatus::Missing:
        return "Missing";
    case PayloadOpenStatus::AccessDenied:
        return "AccessDenied";
    case PayloadOpenStatus::NotRegularFile:
        return "NotRegularFile";
    case PayloadOpenStatus::ReparsePoint:
        return "ReparsePoint";
    case PayloadOpenStatus::IdentityChanged:
        return "IdentityChanged";
    case PayloadOpenStatus::PathChanged:
        return "PathChanged";
    case PayloadOpenStatus::Inconsistent:
        return "Inconsistent";
    case PayloadOpenStatus::Error:
        return "Error";
    }
    return "Error";
}

const char* toString(PayloadReadStatus status) noexcept
{
    switch (status) {
    case PayloadReadStatus::Ok:
        return "Ok";
    case PayloadReadStatus::Cancelled:
        return "Cancelled";
    case PayloadReadStatus::Bounds:
        return "Bounds";
    case PayloadReadStatus::EndOfFile:
        return "EndOfFile";
    case PayloadReadStatus::IdentityChanged:
        return "IdentityChanged";
    case PayloadReadStatus::PathChanged:
        return "PathChanged";
    case PayloadReadStatus::ReadError:
        return "ReadError";
    }
    return "ReadError";
}

const char* toString(PayloadRevalidationStatus status) noexcept
{
    switch (status) {
    case PayloadRevalidationStatus::Valid:
        return "Valid";
    case PayloadRevalidationStatus::Cancelled:
        return "Cancelled";
    case PayloadRevalidationStatus::IdentityChanged:
        return "IdentityChanged";
    case PayloadRevalidationStatus::PathChanged:
        return "PathChanged";
    case PayloadRevalidationStatus::NotRegularFile:
        return "NotRegularFile";
    case PayloadRevalidationStatus::ReparsePoint:
        return "ReparsePoint";
    case PayloadRevalidationStatus::Inconsistent:
        return "Inconsistent";
    case PayloadRevalidationStatus::Error:
        return "Error";
    }
    return "Error";
}

}  // namespace spacelens
