#include "platform/windows/ExplorerIntegration.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>

#include <string>

namespace spacelens {
namespace {

bool shellOpen(const wchar_t* verb, const std::wstring& file, const wchar_t* params)
{
    if (file.empty()) {
        return false;
    }
    const HINSTANCE result = ::ShellExecuteW(
        nullptr, verb, file.c_str(), params, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
}

std::wstring parentPathOf(const std::wstring& path)
{
    if (path.empty()) {
        return {};
    }
    std::size_t end = path.size();
    while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) {
        --end;
    }
    if (end == 0) {
        return {};
    }
    const auto pos = path.find_last_of(L"\\/", end - 1);
    if (pos == std::wstring::npos) {
        return {};
    }
    // Keep drive root "C:\"
    if (pos == 2 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, pos);
}

}  // namespace

bool openInExplorer(const std::wstring& path)
{
    return shellOpen(L"explore", path, nullptr);
}

bool revealInExplorer(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    // explorer /select,"path" — path is a single ShellExecute parameter, not cmd.exe.
    std::wstring params = L"/select,\"";
    params += path;
    params += L"\"";
    return shellOpen(L"open", L"explorer.exe", params.c_str());
}

bool openWithDefaultApp(const std::wstring& path)
{
    return shellOpen(L"open", path, nullptr);
}

bool openParentFolder(const std::wstring& path)
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES &&
        (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return openInExplorer(path);
    }
    const std::wstring parent = parentPathOf(path);
    if (parent.empty()) {
        return false;
    }
    return openInExplorer(parent);
}

}  // namespace spacelens
