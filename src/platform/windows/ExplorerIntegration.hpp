#pragma once

#include <string>

namespace spacelens {

/// Opens a path in Windows Explorer. Returns false on failure.
[[nodiscard]] bool openInExplorer(const std::wstring& path);

/// Opens Explorer with the file selected when possible.
[[nodiscard]] bool revealInExplorer(const std::wstring& path);

}  // namespace spacelens
