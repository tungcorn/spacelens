#pragma once

#include "core/Maintenance.hpp"

#include <string>

namespace spacelens {

/// Recycle-Bin-only Shell adapter. Linked into the GUI (and adapter tests),
/// never into the CLI. Permanent delete is not a supported fallback.
class WindowsRecycleAdapter final : public IRecycleOperation {
public:
    [[nodiscard]] bool volumeCanRecycle(const std::wstring& path,
                                        ByteSize logicalSize,
                                        std::string* detail = nullptr) const;

    [[nodiscard]] MaintenanceItemReceipt recycle(
        const MaintenancePlanItem& item) override;
};

}  // namespace spacelens
