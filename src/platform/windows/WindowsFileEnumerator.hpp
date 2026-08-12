#pragma once

#include "core/IFileEnumerator.hpp"

namespace spacelens {

/// Win32 wide-character directory enumerator using FindFirstFileExW.
/// Does not follow directory reparse points (classified as ReparseDirectory).
class WindowsFileEnumerator final : public IFileEnumerator {
public:
    [[nodiscard]] EnumerateResult enumerate(
        const std::wstring& directoryPath) override;
};

}  // namespace spacelens
