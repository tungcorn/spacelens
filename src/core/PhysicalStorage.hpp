#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spacelens {

/// Whether every filesystem hard link of an identity is accounted for.
enum class HardLinkCoverage {
    Complete,
    Incomplete,
    Unknown
};

[[nodiscard]] const char* toString(HardLinkCoverage coverage) noexcept;
[[nodiscard]] HardLinkCoverage parseHardLinkCoverage(std::string_view text) noexcept;

/// Volume + 64-bit file reference used by the index and live probes.
struct StorageIdentity {
    std::uint64_t volumeSerial = 0;
    std::uint64_t fileId = 0;

    [[nodiscard]] bool valid() const noexcept { return fileId != 0; }
};

[[nodiscard]] inline bool operator==(const StorageIdentity& a,
                                     const StorageIdentity& b) noexcept
{
    return a.volumeSerial == b.volumeSerial && a.fileId == b.fileId;
}

struct StorageIdentityHash {
    [[nodiscard]] std::size_t operator()(const StorageIdentity& id) const noexcept
    {
        return static_cast<std::size_t>(id.volumeSerial * 1315423911ull) ^
               static_cast<std::size_t>(id.fileId);
    }
};

/// One observed path of a filesystem identity.
struct IdentityLinkSample {
    std::wstring path;
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    std::uint32_t filesystemLinks = 0;
    bool sparse = false;
    bool compressed = false;
};

struct IdentityAllocation {
    StorageIdentity identity;
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    std::uint32_t filesystemLinks = 0;
    std::uint32_t observedInIndex = 0;
    std::uint32_t observedInCandidate = 0;
    bool sparse = false;
    bool compressed = false;
};

/// Coverage of one identity relative to a candidate (directory or file set).
[[nodiscard]] HardLinkCoverage classifyHardLinkCoverage(
    std::uint32_t filesystemLinks,
    std::uint32_t observedInIndex,
    std::uint32_t observedInCandidate,
    bool identityKnown) noexcept;

/// Unique physical bytes across identities. Incomplete hard links never
/// contribute to exactReclaimBytes. Unknown allocation is never replaced
/// with logical size.
struct UniqueAllocation {
    std::optional<ByteSize> uniqueAllocatedBytes;
    std::optional<ByteSize> exactReclaimBytes;
    bool allAllocationKnown = true;
    HardLinkCoverage coverage = HardLinkCoverage::Unknown;
    std::uint64_t identityCount = 0;
    std::uint64_t exactIdentityCount = 0;
    std::uint64_t incompleteIdentityCount = 0;
    std::uint64_t unknownIdentityCount = 0;
};

[[nodiscard]] UniqueAllocation summarizeIdentities(
    const std::vector<IdentityAllocation>& items);

/// Saturating add that records overflow.
[[nodiscard]] bool addSaturating(ByteSize& total, ByteSize extra) noexcept;

/// Case-insensitive component-boundary ancestry (or equality).
[[nodiscard]] bool pathIsUnderNormalized(std::wstring_view path,
                                         std::wstring_view ancestor);

}  // namespace spacelens
