#pragma once

#include "core/Classification.hpp"
#include "core/Types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spacelens {

enum class ItemKind {
    File,
    Directory,
    ReparseDirectory
};

[[nodiscard]] const char* toString(ItemKind kind) noexcept;

/// Snapshot of an item selected for later human review (not deletion).
/// Stores enough metadata for future TOCTOU revalidation.
struct CleanupCandidate {
    std::uint64_t id = 0;
    std::wstring path;
    ItemKind kind = ItemKind::File;
    ByteSize sizeAtSelection = 0;
    std::uint64_t lastWriteTime = 0;   // FILETIME ticks; 0 unknown
    std::uint32_t attributes = 0;
    Classification classification{};
    std::string reasonAdded;
    /// "live_scan" (default) or "persistent_index" — planning metadata only.
    std::string source = "live_scan";
    /// Snapshot age when added from an index (0 if live / unknown).
    std::uint64_t indexAgeMs = 0;
    std::string indexIndexedAtIso;
};

/// In-memory planning queue. Value-type candidates; no filesystem mutation.
class CleanupReview {
public:
    /// Add candidate. Duplicate paths (case-insensitive) update metadata and
    /// return the existing id. Returns candidate id.
    std::uint64_t add(CleanupCandidate candidate);

    [[nodiscard]] bool removeById(std::uint64_t id);
    [[nodiscard]] bool removeByPath(std::wstring_view path);
    void clear() noexcept;

    [[nodiscard]] bool containsPath(std::wstring_view path) const;
    [[nodiscard]] std::optional<CleanupCandidate> findById(std::uint64_t id) const;
    [[nodiscard]] std::optional<CleanupCandidate> findByPath(
        std::wstring_view path) const;

    [[nodiscard]] const std::vector<CleanupCandidate>& items() const noexcept
    {
        return m_items;
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_items.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }
    [[nodiscard]] ByteSize totalLogicalSize() const noexcept;

    /// Multi-line human report for clipboard / export.
    [[nodiscard]] std::string copyReport() const;

private:
    [[nodiscard]] static std::wstring normalizeKey(std::wstring_view path);
    [[nodiscard]] std::vector<CleanupCandidate>::iterator findIt(
        std::wstring_view path);
    [[nodiscard]] std::vector<CleanupCandidate>::const_iterator findIt(
        std::wstring_view path) const;

    std::vector<CleanupCandidate> m_items;
    std::uint64_t m_nextId = 1;
};

}  // namespace spacelens
