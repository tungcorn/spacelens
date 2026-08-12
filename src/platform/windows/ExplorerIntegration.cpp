#include "platform/windows/ExplorerIntegration.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>

namespace spacelens {

bool openInExplorer(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    const HINSTANCE result = ::ShellExecuteW(
        nullptr, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
}

bool revealInExplorer(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    // explorer /select,"path"
    std::wstring params = L"/select,\"";
    params += path;
    params += L"\"";
    const HINSTANCE result = ::ShellExecuteW(
        nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
}

}  // namespace spacelens
