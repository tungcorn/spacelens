#pragma once

#include "platform/windows/VolumeHandle.hpp"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace spacelens {

/// Why USN incremental refresh cannot proceed (reason is agent-visible).
enum class UsnCapability {
    Supported,              // journal active; may refresh if checkpoint matches
    UnsupportedFilesystem,  // not NTFS
    AccessDenied,           // cannot open volume / DeviceIoControl denied
    JournalNotActive,       // no USN journal on volume (we never create it)
    JournalChanged,         // UsnJournalID mismatch vs checkpoint
    HistoryLost,            // start USN no longer in journal range
    InvalidPath,
    Error
};

[[nodiscard]] const char* toString(UsnCapability cap) noexcept;

struct UsnJournalState {
    std::uint64_t journalId = 0;
    std::uint64_t firstUsn = 0;
    std::uint64_t nextUsn = 0;
    std::uint64_t lowestValidUsn = 0;
    std::uint64_t maxUsn = 0;
    std::uint64_t maxSize = 0;
};

struct UsnChangeRecord {
    std::uint64_t usn = 0;
    std::uint64_t fileReferenceNumber = 0;
    std::uint64_t parentFileReferenceNumber = 0;
    std::uint32_t reason = 0;
    std::uint32_t fileAttributes = 0;
    std::uint64_t timeStampTicks = 0;  // FILETIME
    std::wstring fileName;
};

/// Read-only USN Change Journal access.
///
/// Safety: only FSCTL_QUERY_USN_JOURNAL and FSCTL_READ_USN_JOURNAL.
/// Never FSCTL_CREATE_USN_JOURNAL / DELETE / EXTEND / etc.
class UsnJournalReader {
public:
    UsnJournalReader() = default;

    /// Open volume for path and query journal presence.
    [[nodiscard]] static UsnCapability tryOpen(const std::wstring& anyPath,
                                               UsnJournalReader& out);

    [[nodiscard]] UsnCapability capability() const noexcept { return m_cap; }
    [[nodiscard]] const VolumeIdentity& volume() const noexcept { return m_volume; }
    [[nodiscard]] HANDLE volumeHandle() const noexcept { return m_handle.get(); }

    /// FSCTL_QUERY_USN_JOURNAL
    [[nodiscard]] UsnCapability query(UsnJournalState& out) const;

    /// Read records with USN >= startUsn (exclusive of already-applied boundary:
    /// callers typically pass the previously stored next_usn as the start).
    /// onRecord returns false to stop early.
    /// HistoryLost if startUsn is below LowestValidUsn / entry deleted.
    [[nodiscard]] UsnCapability readSince(
        std::uint64_t startUsn,
        const std::function<bool(const UsnChangeRecord&)>& onRecord,
        std::stop_token stop = {}) const;

private:
    UsnCapability m_cap = UsnCapability::InvalidPath;
    VolumeIdentity m_volume{};
    VolumeHandle m_handle{};
};

/// Reason bit helpers (values match winioctl.h USN_REASON_*).
namespace UsnReason {
inline constexpr std::uint32_t FileCreate = 0x00000100;
inline constexpr std::uint32_t FileDelete = 0x00000200;
inline constexpr std::uint32_t DataOverwrite = 0x00000001;
inline constexpr std::uint32_t DataExtend = 0x00000002;
inline constexpr std::uint32_t DataTruncation = 0x00000004;
inline constexpr std::uint32_t BasicInfoChange = 0x00008000;
inline constexpr std::uint32_t RenameOldName = 0x00001000;
inline constexpr std::uint32_t RenameNewName = 0x00002000;
inline constexpr std::uint32_t Close = 0x80000000;
inline constexpr std::uint32_t HardLinkChange = 0x00010000;
inline constexpr std::uint32_t SecurityChange = 0x00000800;
inline constexpr std::uint32_t EaChange = 0x00000400;
inline constexpr std::uint32_t IndexableChange = 0x00004000;
inline constexpr std::uint32_t ReparsePointChange = 0x00100000;
inline constexpr std::uint32_t StreamChange = 0x00200000;

/// Mask for FSCTL_READ_USN_JOURNAL — content + rename + basic metadata.
inline constexpr std::uint32_t DefaultMask =
    FileCreate | FileDelete | DataOverwrite | DataExtend | DataTruncation |
    BasicInfoChange | RenameOldName | RenameNewName | Close | HardLinkChange |
    ReparsePointChange | StreamChange;
}  // namespace UsnReason

}  // namespace spacelens
