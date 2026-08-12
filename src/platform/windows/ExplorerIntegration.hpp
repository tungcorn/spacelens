#pragma once

#include <string>

namespace spacelens {

/// Opens a folder in Windows Explorer. Returns false on failure.
[[nodiscard]] bool openInExplorer(const std::wstring& path);

/// Opens Explorer with the file selected when possible.
[[nodiscard]] bool revealInExplorer(const std::wstring& path);

/// Opens a file or folder with the Windows default association (ShellExecute "open").
/// Paths are passed as structured API arguments — never via a shell command string.
[[nodiscard]] bool openWithDefaultApp(const std::wstring& path);

/// Opens the parent directory of a file, or the directory itself.
[[nodiscard]] bool openParentFolder(const std::wstring& path);

}  // namespace spacelens
