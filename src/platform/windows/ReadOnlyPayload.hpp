#pragma once

#include "platform/windows/FileIdentity.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

namespace spacelens {

using PayloadCancellation = std::function<bool()>;

enum class PayloadOpenStatus {
    Opened,
    Cancelled,
    Missing,
    AccessDenied,
    NotRegularFile,
    ReparsePoint,
    IdentityChanged,
    PathChanged,
    Inconsistent,
    Error
};

[[nodiscard]] const char* toString(PayloadOpenStatus status) noexcept;

enum class PayloadReadStatus {
    Ok,
    Cancelled,
    Bounds,
    EndOfFile,
    IdentityChanged,
    PathChanged,
    ReadError
};

[[nodiscard]] const char* toString(PayloadReadStatus status) noexcept;

struct PayloadReadResult {
    PayloadReadStatus status = PayloadReadStatus::ReadError;
    std::size_t bytesRead = 0;
    std::uint32_t nativeError = 0;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == PayloadReadStatus::Ok;
    }
};

enum class PayloadRevalidationStatus {
    Valid,
    Cancelled,
    IdentityChanged,
    PathChanged,
    NotRegularFile,
    ReparsePoint,
    Inconsistent,
    Error
};

[[nodiscard]] const char* toString(PayloadRevalidationStatus status) noexcept;

class ReadOnlyPayload;
struct ReadOnlyPayloadFactory;

struct PayloadOpenResult {
    PayloadOpenStatus status = PayloadOpenStatus::Error;
    std::uint32_t nativeError = 0;
    std::unique_ptr<ReadOnlyPayload> payload;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == PayloadOpenStatus::Opened && payload != nullptr;
    }
};

/// A bounded, read-only window over a retained payload handle. The view owns a
/// shared reference to the handle state, so it remains valid after its parent
/// ReadOnlyPayload value is moved or destroyed.
class PayloadView final {
public:
    PayloadView() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ByteSize size() const noexcept;

    /// Return an invalid view when offset or length falls outside this view.
    [[nodiscard]] PayloadView slice(ByteSize offset, ByteSize length) const;
    [[nodiscard]] std::optional<PayloadView> trySlice(
        ByteSize offset, ByteSize length) const;

    [[nodiscard]] PayloadReadResult readAt(
        ByteSize offset, void* buffer, std::size_t length,
        PayloadCancellation cancellation = {}) const;
    [[nodiscard]] PayloadReadResult readAt(
        ByteSize offset, void* buffer, std::size_t length,
        std::stop_token stop) const;
    [[nodiscard]] PayloadReadResult readAt(
        ByteSize offset, std::span<std::byte> buffer,
        PayloadCancellation cancellation = {}) const
    {
        return readAt(offset, buffer.data(), buffer.size(),
                      std::move(cancellation));
    }
    [[nodiscard]] PayloadReadResult readAt(
        ByteSize offset, std::span<std::uint8_t> buffer,
        PayloadCancellation cancellation = {}) const
    {
        return readAt(offset, buffer.data(), buffer.size(),
                      std::move(cancellation));
    }
    [[nodiscard]] PayloadReadResult readAt(
        ByteSize offset, std::span<std::byte> buffer,
        std::stop_token stop) const
    {
        return readAt(offset, buffer.data(), buffer.size(), stop);
    }
    [[nodiscard]] PayloadReadResult readAt(
        ByteSize offset, std::span<std::uint8_t> buffer,
        std::stop_token stop) const
    {
        return readAt(offset, buffer.data(), buffer.size(), stop);
    }

public:
    // Opaque retained-handle state. This declaration is public only so the
    // implementation can keep helper functions outside the class body.
    struct State;

private:
    PayloadView(std::shared_ptr<State> state, ByteSize offset,
                ByteSize length) noexcept;

    std::shared_ptr<State> m_state;
    ByteSize m_offset = 0;
    ByteSize m_length = 0;

    friend class ReadOnlyPayload;
};

/// A retained native read-only handle bound to one expected regular file.
/// The native path and handle never appear in content evidence or core models.
class ReadOnlyPayload final {
public:
    ReadOnlyPayload() = default;
    ~ReadOnlyPayload();

    ReadOnlyPayload(const ReadOnlyPayload&) = delete;
    ReadOnlyPayload& operator=(const ReadOnlyPayload&) = delete;
    ReadOnlyPayload(ReadOnlyPayload&&) noexcept = default;
    ReadOnlyPayload& operator=(ReadOnlyPayload&&) noexcept = default;

    [[nodiscard]] static PayloadOpenResult open(
        const std::wstring& path, const FileIdentity& expectedIdentity,
        std::wstring_view expectedCanonicalPath,
        PayloadCancellation cancellation = {});
    [[nodiscard]] static PayloadOpenResult open(
        const std::wstring& path, const FileIdentity& expectedIdentity,
        std::wstring_view expectedCanonicalPath, std::stop_token stop);
    [[nodiscard]] static PayloadOpenResult open(
        const std::wstring& path, const FileIdentity& expectedIdentity,
        PayloadCancellation cancellation = {});
    [[nodiscard]] static PayloadOpenResult open(
        const std::wstring& path, const FileIdentity& expectedIdentity,
        std::stop_token stop);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ByteSize size() const noexcept;
    [[nodiscard]] PayloadView view() const;
    [[nodiscard]] PayloadView slice(ByteSize offset, ByteSize length) const;

    [[nodiscard]] PayloadReadResult readAt(
        ByteSize offset, void* buffer, std::size_t length,
        PayloadCancellation cancellation = {}) const;
    [[nodiscard]] PayloadReadResult readAt(
        ByteSize offset, void* buffer, std::size_t length,
        std::stop_token stop) const;

    [[nodiscard]] PayloadRevalidationStatus revalidate(
        PayloadCancellation cancellation = {}) const;
    [[nodiscard]] PayloadRevalidationStatus revalidate(
        std::stop_token stop) const;

private:
    explicit ReadOnlyPayload(std::shared_ptr<PayloadView::State> state) noexcept;

    std::shared_ptr<PayloadView::State> m_state;

    friend struct ReadOnlyPayloadFactory;
};

using ReadOnlyPayloadResult = PayloadOpenResult;

}  // namespace spacelens
