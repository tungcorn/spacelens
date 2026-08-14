#pragma once

#include "core/Duplicates.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace spacelens {

/// Bounded SHA-256 reader. Opens without following the final reparse component,
/// keeps the handle for the whole read, and rejects metadata/identity changes.
class WindowsFileContentHasher final : public IFileContentHasher {
public:
    WindowsFileContentHasher();
    ~WindowsFileContentHasher() override;

    WindowsFileContentHasher(const WindowsFileContentHasher&) = delete;
    WindowsFileContentHasher& operator=(const WindowsFileContentHasher&) = delete;

    [[nodiscard]] ContentHashResult hash(const ContentHashRequest& request) override;
    [[nodiscard]] ContentHashEvidence probe(const std::wstring& path) override;

private:
    [[nodiscard]] std::uint64_t journalIdFor(const std::wstring& path,
                                             std::uint64_t volumeSerial);

    void* m_algorithm = nullptr;
    std::map<std::uint64_t, std::uint64_t> m_journalByVolume;
};

}  // namespace spacelens
