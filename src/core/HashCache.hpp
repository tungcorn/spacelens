#pragma once

#include "core/Duplicates.hpp"
#include "core/index/Sqlite.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

inline constexpr int kHashCacheSchemaVersion = 1;
inline constexpr int kHashAlgorithmSha256 = 1;
inline constexpr int kHashEvidenceVersion = 1;

enum class HashCacheDisposition {
    Disabled,
    Reusable,
    MustRehash,
    Invalid
};

[[nodiscard]] const char* toString(HashCacheDisposition disposition) noexcept;

struct HashCacheRow {
    CleanupIdentity identity{};
    ByteSize logicalSize = 0;
    FileTimeTicks changeTime = 0;
    std::int64_t fileUsn = 0;
    std::uint64_t journalId = 0;
    int algorithm = 0;
    int evidenceVersion = 0;
    std::vector<std::uint8_t> digest;
};

struct HashCacheLookup {
    HashCacheDisposition disposition = HashCacheDisposition::MustRehash;
    std::array<std::uint8_t, 32> digest{};
    std::string detail;
};

/// True only when every evidence-v1 field needed for a correct reuse is present.
/// FileIndex64, missing USN/ChangeTime, or a zero FileId is never persistable.
[[nodiscard]] bool isHashCachePersistable(const ContentHashEvidence& evidence) noexcept;

/// Pure evaluator. FALSE HIT is a defect; insufficient evidence → MustRehash.
[[nodiscard]] HashCacheDisposition evaluateHashCacheRow(
    const ContentHashEvidence& live, const HashCacheRow& stored) noexcept;

/// Derived SHA-256 cache. Dedicated file (never state.db). Path is never a key.
/// Open/read/write failures degrade to hashing; they never fail a scan.
class HashCacheStore {
public:
    HashCacheStore() = default;
    ~HashCacheStore() = default;

    HashCacheStore(const HashCacheStore&) = delete;
    HashCacheStore& operator=(const HashCacheStore&) = delete;
    HashCacheStore(HashCacheStore&&) noexcept = default;
    HashCacheStore& operator=(HashCacheStore&&) noexcept = default;

    /// Empty path → disabled. Newer/corrupt schema → unavailable (hash again).
    [[nodiscard]] static HashCacheStore tryOpen(const std::wstring& path);

    [[nodiscard]] bool available() const noexcept { return m_available; }
    [[nodiscard]] const std::wstring& path() const noexcept { return m_path; }

    [[nodiscard]] HashCacheLookup lookup(const ContentHashEvidence& live);
    bool store(const ContentHashEvidence& live,
               const std::array<std::uint8_t, 32>& digest);

private:
    bool openExisting();
    void dropCorruptRow(std::uint64_t volumeSerial,
                        const std::array<std::uint8_t, 16>& fileId) noexcept;
    void touchLastUsed(std::uint64_t volumeSerial,
                       const std::array<std::uint8_t, 16>& fileId) noexcept;

    SqliteDb m_db;
    std::wstring m_path;
    bool m_available = false;
};

}  // namespace spacelens
