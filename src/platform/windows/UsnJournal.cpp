#include "platform/windows/UsnJournal.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winioctl.h>

#include <cstring>
#include <vector>

namespace {
// Portable header — USN_RECORD_COMMON_HEADER is not always defined on older SDKs.
struct UsnRecordHeader {
    DWORD RecordLength;
    WORD MajorVersion;
    WORD MinorVersion;
};
}  // namespace

namespace spacelens {

const char* toString(UsnCapability cap) noexcept
{
    switch (cap) {
    case UsnCapability::Supported:
        return "supported";
    case UsnCapability::UnsupportedFilesystem:
        return "unsupported_filesystem";
    case UsnCapability::AccessDenied:
        return "access_denied";
    case UsnCapability::JournalNotActive:
        return "journal_not_active";
    case UsnCapability::JournalChanged:
        return "journal_changed";
    case UsnCapability::HistoryLost:
        return "history_lost";
    case UsnCapability::InvalidPath:
        return "invalid_path";
    case UsnCapability::Error:
        return "error";
    }
    return "error";
}

UsnCapability UsnJournalReader::tryOpen(const std::wstring& anyPath,
                                        UsnJournalReader& out)
{
    out = UsnJournalReader{};
    if (anyPath.empty()) {
        out.m_cap = UsnCapability::InvalidPath;
        return out.m_cap;
    }

    if (!resolveVolumeIdentity(anyPath, out.m_volume)) {
        out.m_cap = UsnCapability::InvalidPath;
        return out.m_cap;
    }
    if (!out.m_volume.isNtfs) {
        out.m_cap = UsnCapability::UnsupportedFilesystem;
        return out.m_cap;
    }

    out.m_handle = openVolumeReadOnly(out.m_volume);
    if (!out.m_handle.valid()) {
        const DWORD err = ::GetLastError();
        // Volume open almost always fails as ACCESS_DENIED without backup/admin.
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_INVALID_NAME) {
            out.m_cap = UsnCapability::InvalidPath;
        } else {
            out.m_cap = UsnCapability::AccessDenied;
        }
        return out.m_cap;
    }

    UsnJournalState state{};
    const UsnCapability q = out.query(state);
    // A handle that opens but cannot run USN IOCTLs is an access problem on NTFS,
    // not a non-NTFS volume (identity already confirmed NTFS above).
    if (q == UsnCapability::UnsupportedFilesystem && out.m_volume.isNtfs) {
        out.m_cap = UsnCapability::AccessDenied;
        out.m_handle.reset();
        return out.m_cap;
    }
    out.m_cap = q;
    return out.m_cap;
}

UsnCapability UsnJournalReader::query(UsnJournalState& outState) const
{
    outState = {};
    if (!m_handle.valid()) {
        return UsnCapability::Error;
    }

    USN_JOURNAL_DATA_V0 data{};
    DWORD bytes = 0;
    if (!::DeviceIoControl(m_handle.get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                           &data, sizeof(data), &bytes, nullptr)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_JOURNAL_NOT_ACTIVE) {
            return UsnCapability::JournalNotActive;
        }
        if (err == ERROR_ACCESS_DENIED || err == ERROR_PRIVILEGE_NOT_HELD) {
            return UsnCapability::AccessDenied;
        }
        // On NTFS these usually mean the volume handle lacks rights for USN
        // IOCTLs (e.g. zero-access CreateFile succeeded). Only treat as
        // unsupported filesystem when the volume is not NTFS.
        if (err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED) {
            return m_volume.isNtfs ? UsnCapability::AccessDenied
                                  : UsnCapability::UnsupportedFilesystem;
        }
        return UsnCapability::Error;
    }

    outState.journalId = data.UsnJournalID;
    outState.firstUsn = static_cast<std::uint64_t>(data.FirstUsn);
    outState.nextUsn = static_cast<std::uint64_t>(data.NextUsn);
    outState.lowestValidUsn = static_cast<std::uint64_t>(data.LowestValidUsn);
    outState.maxUsn = static_cast<std::uint64_t>(data.MaxUsn);
    outState.maxSize = data.MaximumSize;
    return UsnCapability::Supported;
}

UsnCapability UsnJournalReader::readSince(
    std::uint64_t startUsn,
    const std::function<bool(const UsnChangeRecord&)>& onRecord,
    std::uint64_t& outNextUsn,
    std::stop_token stop) const
{
    outNextUsn = startUsn;

    if (!m_handle.valid() || !onRecord) {
        return UsnCapability::Error;
    }

    UsnJournalState state{};
    const UsnCapability q = query(state);
    if (q != UsnCapability::Supported) {
        return q;
    }

    // If the caller's cursor is before what the journal still retains, history
    // is gone — never invent gaps.
    if (startUsn != 0 && startUsn < state.lowestValidUsn) {
        return UsnCapability::HistoryLost;
    }
    if (startUsn > state.nextUsn) {
        // Cursor from the future / wrong volume — treat as discontinuity.
        return UsnCapability::HistoryLost;
    }
    if (startUsn == state.nextUsn) {
        // Empty tail: already at tip. Not a discontinuity.
        outNextUsn = state.nextUsn;
        return UsnCapability::Supported;
    }

    // READ_USN_JOURNAL_DATA_V0: StartUsn is the first USN to return.
    READ_USN_JOURNAL_DATA_V0 readData{};
    readData.StartUsn = static_cast<USN>(startUsn);
    readData.ReasonMask = UsnReason::DefaultMask;
    readData.ReturnOnlyOnClose = FALSE;
    readData.Timeout = 0;
    readData.BytesToWaitFor = 0;
    readData.UsnJournalID = state.journalId;

    // 64 KiB buffer is typical; records are variable-length.
    std::vector<std::uint8_t> buffer(64 * 1024);

    // Re-query tip periodically so we can stop when caught up to a moving journal.
    // Initial tip is a lower bound; concurrent writers may advance it.
    std::uint64_t tipUsn = state.nextUsn;

    for (;;) {
        if (stop.stop_requested()) {
            return UsnCapability::Supported;
        }

        DWORD bytes = 0;
        if (!::DeviceIoControl(m_handle.get(), FSCTL_READ_USN_JOURNAL, &readData,
                               sizeof(readData), buffer.data(),
                               static_cast<DWORD>(buffer.size()), &bytes,
                               nullptr)) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_HANDLE_EOF) {
                // No more records currently available. Cursor stays at StartUsn
                // (or last continuation). Prefer live tip if we are at/after it.
                UsnJournalState live{};
                if (query(live) == UsnCapability::Supported) {
                    tipUsn = live.nextUsn;
                    if (outNextUsn < tipUsn &&
                        static_cast<std::uint64_t>(readData.StartUsn) >= tipUsn) {
                        outNextUsn = tipUsn;
                    } else if (static_cast<std::uint64_t>(readData.StartUsn) >=
                               tipUsn) {
                        outNextUsn = tipUsn;
                    } else {
                        // EOF with StartUsn still below tip is unusual; keep the
                        // driver-provided StartUsn as the safe continuation.
                        outNextUsn = static_cast<std::uint64_t>(readData.StartUsn);
                    }
                }
                return UsnCapability::Supported;
            }
            if (err == ERROR_JOURNAL_ENTRY_DELETED) {
                return UsnCapability::HistoryLost;
            }
            if (err == ERROR_JOURNAL_NOT_ACTIVE) {
                return UsnCapability::JournalNotActive;
            }
            if (err == ERROR_INVALID_PARAMETER) {
                // Often: StartUsn outside valid range / not a record boundary /
                // JournalID mismatch. Callers must only persist driver-issued
                // continuation USNs or journal NextUsn.
                return UsnCapability::HistoryLost;
            }
            if (err == ERROR_ACCESS_DENIED) {
                return UsnCapability::AccessDenied;
            }
            return UsnCapability::Error;
        }

        if (bytes < sizeof(USN)) {
            // Empty successful read — treat as caught up.
            UsnJournalState live{};
            if (query(live) == UsnCapability::Supported) {
                outNextUsn = live.nextUsn;
            }
            return UsnCapability::Supported;
        }

        // First 8 bytes: next USN to continue from (driver-authoritative).
        const USN nextStart = *reinterpret_cast<const USN*>(buffer.data());
        const std::uint64_t continuation =
            static_cast<std::uint64_t>(nextStart);
        DWORD offset = sizeof(USN);
        bool any = false;
        bool stopEarly = false;

        while (offset + sizeof(UsnRecordHeader) <= bytes) {
            if (stop.stop_requested()) {
                outNextUsn = continuation;
                return UsnCapability::Supported;
            }

            const auto* common =
                reinterpret_cast<const UsnRecordHeader*>(buffer.data() + offset);
            if (common->RecordLength == 0 ||
                offset + common->RecordLength > bytes) {
                break;
            }

            UsnChangeRecord rec{};
            if (common->MajorVersion == 2 &&
                common->RecordLength >= sizeof(USN_RECORD_V2)) {
                const auto* r =
                    reinterpret_cast<const USN_RECORD_V2*>(buffer.data() + offset);
                rec.usn = static_cast<std::uint64_t>(r->Usn);
                rec.fileReferenceNumber =
                    static_cast<std::uint64_t>(r->FileReferenceNumber);
                rec.parentFileReferenceNumber =
                    static_cast<std::uint64_t>(r->ParentFileReferenceNumber);
                rec.reason = r->Reason;
                rec.fileAttributes = r->FileAttributes;
                rec.timeStampTicks =
                    static_cast<std::uint64_t>(r->TimeStamp.QuadPart);
                if (r->FileNameLength > 0 &&
                    static_cast<DWORD>(r->FileNameOffset) + r->FileNameLength <=
                        r->RecordLength) {
                    const auto* nameChars = reinterpret_cast<const wchar_t*>(
                        reinterpret_cast<const std::uint8_t*>(r) +
                        r->FileNameOffset);
                    const std::size_t nameLen =
                        r->FileNameLength / sizeof(wchar_t);
                    rec.fileName.assign(nameChars, nameLen);
                }
            } else if (common->MajorVersion == 3 &&
                       common->RecordLength >= sizeof(USN_RECORD_V3)) {
                const auto* r =
                    reinterpret_cast<const USN_RECORD_V3*>(buffer.data() + offset);
                // V3 uses FILE_ID_128; on NTFS the low 8 bytes are the classic FRN.
                static_assert(sizeof(r->FileReferenceNumber) >= 8, "FILE_ID_128");
                std::memcpy(&rec.fileReferenceNumber, &r->FileReferenceNumber,
                            sizeof(std::uint64_t));
                std::memcpy(&rec.parentFileReferenceNumber,
                            &r->ParentFileReferenceNumber, sizeof(std::uint64_t));
                rec.usn = static_cast<std::uint64_t>(r->Usn);
                rec.reason = r->Reason;
                rec.fileAttributes = r->FileAttributes;
                rec.timeStampTicks =
                    static_cast<std::uint64_t>(r->TimeStamp.QuadPart);
                if (r->FileNameLength > 0 &&
                    static_cast<DWORD>(r->FileNameOffset) + r->FileNameLength <=
                        r->RecordLength) {
                    const auto* nameChars = reinterpret_cast<const wchar_t*>(
                        reinterpret_cast<const std::uint8_t*>(r) +
                        r->FileNameOffset);
                    const std::size_t nameLen =
                        r->FileNameLength / sizeof(wchar_t);
                    rec.fileName.assign(nameChars, nameLen);
                }
            } else {
                // Skip unknown versions safely; continuation USN still advances.
                offset += common->RecordLength;
                continue;
            }

            any = true;
            if (!onRecord(rec)) {
                stopEarly = true;
                break;
            }

            offset += common->RecordLength;
        }

        // Always adopt the driver continuation after a successful buffer parse.
        // This is the only safe value to feed back as StartUsn / checkpoint.
        outNextUsn = continuation;

        if (stopEarly) {
            return UsnCapability::Supported;
        }

        // Refresh tip — concurrent activity may have moved it.
        {
            UsnJournalState live{};
            if (query(live) == UsnCapability::Supported) {
                tipUsn = live.nextUsn;
            }
        }

        // No progress from the driver and no records → stop (empty / stuck).
        if (continuation <= static_cast<std::uint64_t>(readData.StartUsn) &&
            !any) {
            if (outNextUsn < tipUsn) {
                // Avoid spinning; if we cannot advance past StartUsn, surface as
                // caught-up only when already at tip.
                outNextUsn = static_cast<std::uint64_t>(readData.StartUsn);
            }
            return UsnCapability::Supported;
        }

        // Caught up to journal tip.
        if (continuation >= tipUsn) {
            outNextUsn = tipUsn;
            return UsnCapability::Supported;
        }

        // More journal remains. Continue even if this buffer had zero matching
        // records (ReasonMask / unknown versions) — previously we returned early
        // on !any and left the cursor mid-range incorrectly on next cycle when
        // combined with usn+1 math.
        readData.StartUsn = nextStart;
    }
}

}  // namespace spacelens
