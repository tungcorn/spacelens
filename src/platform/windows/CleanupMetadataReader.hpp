#pragma once

#include "core/CleanupRevalidation.hpp"

namespace spacelens {

/// Metadata-only Win32 reader. Opens with the same no-follow flags as
/// queryFileIdentity() and prefers FILE_ID_INFO over the 64-bit fallback.
class WindowsCleanupMetadataReader final : public ICleanupMetadataReader {
public:
    [[nodiscard]] CleanupMetadataProbe read(const std::wstring& path) override;
};

}  // namespace spacelens
