#pragma once

#include "core/Duplicates.hpp"

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

private:
    void* m_algorithm = nullptr;
};

}  // namespace spacelens
