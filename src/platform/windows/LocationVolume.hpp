#pragma once

#include "core/OrdinaryLocation.hpp"

namespace spacelens {

/// Volume serial plus optional volume GUID. Does not open the USN journal.
class WindowsVolumeIdentityReader final : public IVolumeIdentityReader {
public:
    [[nodiscard]] LocationVolumeEvidence read(
        const std::wstring& path) const override;
};

}  // namespace spacelens
