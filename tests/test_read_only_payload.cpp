#include "TestRunner.hpp"

#include "platform/windows/FileIdentity.hpp"
#include "platform/windows/ReadOnlyPayload.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace spacelens;
namespace fs = std::filesystem;

static_assert(!std::is_copy_constructible_v<ReadOnlyPayload>);
static_assert(std::is_move_constructible_v<ReadOnlyPayload>);
static_assert(std::is_copy_constructible_v<PayloadView>);

namespace {

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : m_handle(handle)
    {
    }

    ~ScopedHandle()
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_handle);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

class TempFixture final {
public:
    TempFixture()
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_root = fs::temp_directory_path() / "spacelens_read_only_payload_tests" /
                 (std::to_string(::GetCurrentProcessId()) + "_" +
                  std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(m_root, ec);
        if (ec) {
            throw spacelens::test::Failure(
                "cannot create read-only payload fixture root");
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

void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        throw spacelens::test::Failure(
            "cannot create read-only payload fixture directory");
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw spacelens::test::Failure(
            "cannot create read-only payload fixture file");
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw spacelens::test::Failure(
            "cannot write read-only payload fixture file");
    }
}

void writeText(const fs::path& path, std::string_view text)
{
    writeBytes(path, std::vector<std::uint8_t>(
                         reinterpret_cast<const std::uint8_t*>(text.data()),
                         reinterpret_cast<const std::uint8_t*>(text.data()) +
                             text.size()));
}

FileIdentity identityFor(const fs::path& path)
{
    const auto identity = queryFileIdentity(path.wstring());
    if (!identity.has_value()) {
        throw spacelens::test::Failure(
            "cannot query read-only payload fixture identity");
    }
    return *identity;
}

std::wstring canonicalPathFor(const fs::path& path)
{
    const std::wstring canonical = canonicalWin32Path(path.wstring());
    if (canonical.empty()) {
        throw spacelens::test::Failure(
            "cannot canonicalize read-only payload fixture path");
    }
    return canonical;
}

PayloadOpenResult openFor(const fs::path& path)
{
    const FileIdentity identity = identityFor(path);
    return ReadOnlyPayload::open(path.wstring(), identity,
                                 canonicalPathFor(path));
}

bool sameFileIdAndSize(const FileIdentity& left, const FileIdentity& right)
{
    const bool sameId =
        left.volumeSerial == right.volumeSerial &&
        ((left.fileId128Known && right.fileId128Known)
             ? left.fileId128 == right.fileId128
             : (left.fileIndex64Known && right.fileIndex64Known &&
                left.fileId == right.fileId));
    return sameId && left.logicalSizeKnown && right.logicalSizeKnown &&
           left.sizeBytes == right.sizeBytes;
}

bool rewriteSameSize(const fs::path& path, const FileIdentity& baseline,
                     std::uint8_t value)
{
    const HANDLE raw = ::CreateFileW(
        path.wstring().c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    ScopedHandle handle(raw);
    if (!handle.valid()) {
        return false;
    }

    LARGE_INTEGER position{};
    if (::SetFilePointerEx(handle.get(), position, nullptr, FILE_BEGIN) == FALSE) {
        return false;
    }
    DWORD written = 0;
    if (::WriteFile(handle.get(), &value, 1U, &written, nullptr) == FALSE ||
        written != 1U || ::FlushFileBuffers(handle.get()) == FALSE) {
        return false;
    }

    constexpr std::uint64_t kTenSeconds = 10'000'000ULL;
    constexpr std::uint64_t kTimestampDelta = 6ULL * kTenSeconds;
    std::uint64_t timestamp = baseline.lastWriteTicks;
    if (timestamp <= std::numeric_limits<std::uint64_t>::max() -
                         kTimestampDelta) {
        timestamp += kTimestampDelta;
    } else if (timestamp > kTimestampDelta) {
        timestamp -= kTimestampDelta;
    } else {
        timestamp = 1;
    }
    FILETIME lastWrite{};
    lastWrite.dwLowDateTime = static_cast<DWORD>(timestamp & 0xffffffffULL);
    lastWrite.dwHighDateTime = static_cast<DWORD>(timestamp >> 32U);
    if (::SetFileTime(handle.get(), nullptr, nullptr, &lastWrite) == FALSE ||
        ::FlushFileBuffers(handle.get()) == FALSE) {
        return false;
    }
    return true;
}

}  // namespace

SPACELENS_TEST(ReadOnlyPayload_open_read_slice_and_shared_lifetime)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "payload.bin";
    writeText(path, "0123456789abcdef");

    PayloadView retainedSlice;
    {
        auto opened = openFor(path);
        SPACELENS_REQUIRE(opened.ok());
        SPACELENS_REQUIRE(opened.payload->valid());
        SPACELENS_REQUIRE_EQ(opened.payload->size(), 16ULL);

        const PayloadView view = opened.payload->view();
        SPACELENS_REQUIRE(view.valid());
        SPACELENS_REQUIRE_EQ(view.size(), 16ULL);
        retainedSlice = view.slice(2, 8);
        SPACELENS_REQUIRE(retainedSlice.valid());
        SPACELENS_REQUIRE_EQ(retainedSlice.size(), 8ULL);

        const auto nested = retainedSlice.trySlice(2, 3);
        SPACELENS_REQUIRE(nested.has_value());
        std::array<std::uint8_t, 3> nestedBytes{};
        const auto nestedRead = nested->readAt(0, nestedBytes);
        SPACELENS_REQUIRE(nestedRead.ok());
        SPACELENS_REQUIRE_EQ(nestedRead.bytesRead, 3ULL);
        SPACELENS_REQUIRE(std::memcmp(nestedBytes.data(), "456", 3) == 0);

        SPACELENS_REQUIRE(!view.slice(17, 0).valid());
        SPACELENS_REQUIRE(!view.slice(15, 2).valid());
    }

    std::array<std::uint8_t, 4> bytes{};
    const auto read = retainedSlice.readAt(3, bytes);
    SPACELENS_REQUIRE(read.ok());
    SPACELENS_REQUIRE_EQ(read.bytesRead, 4ULL);
    SPACELENS_REQUIRE(std::memcmp(bytes.data(), "5678", 4) == 0);
}

SPACELENS_TEST(ReadOnlyPayload_bounds_zero_length_and_null_destination)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "bounds.bin";
    writeText(path, "bounded");
    auto opened = openFor(path);
    SPACELENS_REQUIRE(opened.ok());

    std::array<std::uint8_t, 1> byte{};
    auto result = opened.payload->readAt(opened.payload->size(), nullptr, 0);
    SPACELENS_REQUIRE(result.status == PayloadReadStatus::Ok);
    SPACELENS_REQUIRE_EQ(result.bytesRead, 0ULL);

    result = opened.payload->readAt(opened.payload->size() + 1, nullptr, 0);
    SPACELENS_REQUIRE(result.status == PayloadReadStatus::Bounds);
    result = opened.payload->readAt(opened.payload->size(), byte.data(), 1);
    SPACELENS_REQUIRE(result.status == PayloadReadStatus::Bounds);
    result = opened.payload->readAt(0, nullptr, 1);
    SPACELENS_REQUIRE(result.status == PayloadReadStatus::ReadError);
    result = opened.payload->readAt(std::numeric_limits<ByteSize>::max(),
                                    byte.data(), 1);
    SPACELENS_REQUIRE(result.status == PayloadReadStatus::Bounds);

    const PayloadView view = opened.payload->view();
    SPACELENS_REQUIRE(view.slice(view.size(), 0).valid());
    SPACELENS_REQUIRE(!view.trySlice(view.size() + 1, 0).has_value());
}

SPACELENS_TEST(ReadOnlyPayload_cancellation_is_typed_and_zero_length_aware)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "cancel.bin";
    writeText(path, "cancellable");
    const FileIdentity identity = identityFor(path);
    const std::wstring canonical = canonicalPathFor(path);

    std::stop_source openStop;
    openStop.request_stop();
    auto cancelledOpen = ReadOnlyPayload::open(path.wstring(), identity, canonical,
                                               openStop.get_token());
    SPACELENS_REQUIRE(cancelledOpen.status == PayloadOpenStatus::Cancelled);
    SPACELENS_REQUIRE(!cancelledOpen.payload);

    auto opened = ReadOnlyPayload::open(path.wstring(), identity, canonical);
    SPACELENS_REQUIRE(opened.ok());
    const PayloadCancellation cancellation = []() noexcept { return true; };
    std::array<std::uint8_t, 1> byte{};
    auto result = opened.payload->readAt(0, byte.data(), byte.size(),
                                         cancellation);
    SPACELENS_REQUIRE(result.status == PayloadReadStatus::Cancelled);
    SPACELENS_REQUIRE_EQ(result.bytesRead, 0ULL);
    result = opened.payload->readAt(opened.payload->size(), nullptr, 0,
                                    cancellation);
    SPACELENS_REQUIRE(result.status == PayloadReadStatus::Cancelled);
    SPACELENS_REQUIRE(opened.payload->revalidate(cancellation) ==
                      PayloadRevalidationStatus::Cancelled);
}

SPACELENS_TEST(ReadOnlyPayload_throwing_cancellation_fails_closed)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "throwing-cancel.bin";
    writeText(path, "throwing cancellation");
    auto opened = openFor(path);
    SPACELENS_REQUIRE(opened.ok());

    std::array<std::uint8_t, 1> byte{};
    const auto read = opened.payload->readAt(
        0U, byte.data(), byte.size(), []() -> bool { throw 17; });
    SPACELENS_REQUIRE(read.status == PayloadReadStatus::Cancelled);
    SPACELENS_REQUIRE(opened.payload->revalidate(
                           []() -> bool { throw 19; }) ==
                      PayloadRevalidationStatus::Cancelled);
}

SPACELENS_TEST(ReadOnlyPayload_exact_canonical_final_path_binding)
{
    TempFixture fixture;
    const fs::path actual = fixture.root() / "actual.bin";
    const fs::path wrong = fixture.root() / "wrong.bin";
    writeText(actual, "actual");
    writeText(wrong, "wrong");

    const FileIdentity identity = identityFor(actual);
    auto opened = ReadOnlyPayload::open(actual.wstring(), identity,
                                        canonicalPathFor(wrong));
    SPACELENS_REQUIRE(opened.status == PayloadOpenStatus::PathChanged);
    SPACELENS_REQUIRE(!opened.payload);
}

SPACELENS_TEST(ReadOnlyPayload_reparse_point_is_rejected)
{
    TempFixture fixture;
    const fs::path target = fixture.root() / "target.bin";
    const fs::path link = fixture.root() / "link.bin";
    writeText(target, "target");

    if (::CreateSymbolicLinkW(
            link.wstring().c_str(), target.wstring().c_str(),
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE) {
        std::cout << "[ SKIP ] ReadOnlyPayload_reparse_point_is_rejected — "
                     "symbolic-link fixture unavailable (error "
                  << ::GetLastError() << ")\n";
        return;
    }

    const FileIdentity identity = identityFor(link);
    auto opened = ReadOnlyPayload::open(link.wstring(), identity,
                                        canonicalPathFor(link));
    SPACELENS_REQUIRE(opened.status == PayloadOpenStatus::ReparsePoint);
    SPACELENS_REQUIRE(!opened.payload);
}

SPACELENS_TEST(ReadOnlyPayload_open_revalidation_rejects_same_id_same_size_change)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "open-change.bin";
    writeText(path, "same-size-content");
    const FileIdentity baseline = identityFor(path);
    SPACELENS_REQUIRE(rewriteSameSize(path, baseline, 'X'));
    const FileIdentity changed = identityFor(path);
    SPACELENS_REQUIRE(sameFileIdAndSize(baseline, changed));

    auto opened = ReadOnlyPayload::open(path.wstring(), baseline,
                                        canonicalPathFor(path));
    SPACELENS_REQUIRE(opened.status == PayloadOpenStatus::IdentityChanged);
    SPACELENS_REQUIRE(!opened.payload);
}

SPACELENS_TEST(ReadOnlyPayload_post_read_revalidation_rejects_same_id_same_size_change)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "post-read-change.bin";
    writeText(path, "same-size-content");
    const FileIdentity baseline = identityFor(path);
    auto opened = ReadOnlyPayload::open(path.wstring(), baseline,
                                        canonicalPathFor(path));
    SPACELENS_REQUIRE(opened.ok());
    SPACELENS_REQUIRE(rewriteSameSize(path, baseline, 'Y'));
    const FileIdentity changed = identityFor(path);
    SPACELENS_REQUIRE(sameFileIdAndSize(baseline, changed));

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(opened.payload->size()));
    const auto read = opened.payload->readAt(0, bytes.data(), bytes.size());
    SPACELENS_REQUIRE(read.status == PayloadReadStatus::IdentityChanged);
    SPACELENS_REQUIRE_EQ(read.bytesRead, bytes.size());
}

SPACELENS_TEST(ReadOnlyPayload_unexpected_eof_prefers_identity_change)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "truncated-after-open.bin";
    std::vector<std::uint8_t> original(2U * 1024U * 1024U, 0x5aU);
    writeBytes(path, original);
    auto opened = openFor(path);
    SPACELENS_REQUIRE(opened.ok());

    writeText(path, "x");
    std::vector<std::uint8_t> destination(original.size());
    const auto read = opened.payload->readAt(
        0, destination.data(), destination.size());
    SPACELENS_REQUIRE(read.status == PayloadReadStatus::IdentityChanged);
    SPACELENS_REQUIRE(read.bytesRead < destination.size());
}

SPACELENS_TEST(ReadOnlyPayload_source_invariants_concurrent_destinations)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "parallel.bin";
    constexpr std::size_t kChunkBytes = 32U * 1024U;
    constexpr std::size_t kChunkCount = 8U;
    std::vector<std::uint8_t> source(kChunkBytes * kChunkCount);
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<std::uint8_t>((i * 37U + 11U) & 0xffU);
    }
    writeBytes(path, source);

    auto opened = openFor(path);
    SPACELENS_REQUIRE(opened.ok());
    std::atomic<std::size_t> ready = 0;
    std::atomic_bool start = false;
    std::atomic_bool failed = false;
    std::vector<std::jthread> workers;
    workers.reserve(kChunkCount);
    for (std::size_t worker = 0; worker < kChunkCount; ++worker) {
        workers.emplace_back([&, worker](std::stop_token stop) {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire) &&
                   !stop.stop_requested()) {
                std::this_thread::yield();
            }
            for (std::size_t iteration = 0;
                 iteration < 6U && !failed.load(std::memory_order_acquire) &&
                 !stop.stop_requested();
                 ++iteration) {
                const std::size_t offset = worker * kChunkBytes;
                std::vector<std::uint8_t> destination(kChunkBytes);
                const auto result = opened.payload->readAt(
                    static_cast<ByteSize>(offset), destination.data(),
                    destination.size());
                if (!result.ok() || result.bytesRead != destination.size() ||
                    std::memcmp(destination.data(), source.data() + offset,
                                destination.size()) != 0) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != kChunkCount) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    workers.clear();
    SPACELENS_REQUIRE(!failed.load(std::memory_order_acquire));
}
