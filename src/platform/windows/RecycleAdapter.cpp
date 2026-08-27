#include "platform/windows/RecycleAdapter.hpp"
#include "core/Json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <shellapi.h>

#include <string>

namespace spacelens {
namespace {

class ComInit {
public:
    ComInit()
        : m_hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {
    }

    ~ComInit()
    {
        if (SUCCEEDED(m_hr) || m_hr == S_FALSE) {
            ::CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT hr() const noexcept { return m_hr; }

    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

private:
    HRESULT m_hr = E_FAIL;
};

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept
        : m_ptr(other.m_ptr)
    {
        other.m_ptr = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    T** put() noexcept
    {
        reset();
        return &m_ptr;
    }

    [[nodiscard]] T* get() const noexcept { return m_ptr; }
    T* operator->() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

    void reset() noexcept
    {
        if (m_ptr != nullptr) {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

private:
    T* m_ptr = nullptr;
};

std::string narrow(const std::wstring& wide)
{
    return utf8FromWide(wide);
}

std::wstring displayName(IShellItem* item, SIGDN sigdn)
{
    if (item == nullptr) {
        return {};
    }
    PWSTR name = nullptr;
    if (FAILED(item->GetDisplayName(sigdn, &name)) || name == nullptr) {
        return {};
    }
    std::wstring out(name);
    ::CoTaskMemFree(name);
    return out;
}

bool isLocalDrivePath(const std::wstring& path)
{
    return path.size() >= 3 &&
           ((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) &&
           path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

std::wstring driveRoot(const std::wstring& path)
{
    if (!isLocalDrivePath(path)) {
        return {};
    }
    std::wstring root(3, L'\\');
    root[0] = path[0];
    root[1] = L':';
    return root;
}

class RecycleSink final : public IFileOperationProgressSink {
public:
    HRESULT recycledHr = E_FAIL;
    bool sawPostDelete = false;
    bool newlyCreatedPresent = false;
    std::wstring newlyCreatedName;
    std::wstring sourceName;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(::InterlockedIncrement(&m_refs));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG value = ::InterlockedDecrement(&m_refs);
        if (value == 0) {
            delete this;
        }
        return static_cast<ULONG>(value);
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT,
                                             IShellItem*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*,
                                          LPCWSTR) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR,
                                           HRESULT, IShellItem*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*,
                                          LPCWSTR) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR,
                                           HRESULT, IShellItem*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem* item) override
    {
        sourceName = displayName(item, SIGDN_FILESYSPATH);
        if (sourceName.empty()) {
            sourceName = displayName(item, SIGDN_DESKTOPABSOLUTEPARSING);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT hrDelete,
                                             IShellItem* newlyCreated) override
    {
        sawPostDelete = true;
        recycledHr = hrDelete;
        newlyCreatedPresent = newlyCreated != nullptr;
        newlyCreatedName = displayName(newlyCreated, SIGDN_DESKTOPABSOLUTEPARSING);
        if (newlyCreatedName.empty()) {
            newlyCreatedName = displayName(newlyCreated, SIGDN_NORMALDISPLAY);
        }
        if (SUCCEEDED(hrDelete) && newlyCreated == nullptr) {
            return E_FAIL;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR,
                                          DWORD, HRESULT, IShellItem*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    LONG m_refs = 1;
};

}  // namespace

bool WindowsRecycleAdapter::volumeCanRecycle(const std::wstring& path,
                                             ByteSize logicalSize,
                                             std::string* detail) const
{
    if (!isLocalDrivePath(path)) {
        if (detail != nullptr) {
            *detail = "Recycle Bin requires a local drive-letter path";
        }
        return false;
    }
    const auto root = driveRoot(path);
    const UINT type = ::GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
        if (detail != nullptr) {
            *detail = "Volume type does not support a Recycle Bin";
        }
        return false;
    }

    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    const HRESULT hr = ::SHQueryRecycleBinW(root.c_str(), &info);
    if (FAILED(hr)) {
        if (detail != nullptr) {
            *detail = "SHQueryRecycleBin failed; recycle is not available";
        }
        return false;
    }
    (void)logicalSize;
    (void)info;
    return true;
}

MaintenanceItemReceipt WindowsRecycleAdapter::recycle(const MaintenancePlanItem& item)
{
    MaintenanceItemReceipt receipt;
    receipt.reviewId = item.reviewId;
    receipt.path = item.path;
    receipt.expectedIdentity = item.expectedIdentity;

    std::string recycleDetail;
    if (!volumeCanRecycle(item.path, item.logicalSize, &recycleDetail)) {
        receipt.result = MaintenanceItemResult::BlockedFinalGuard;
        receipt.blockReason = MaintenanceBlockReason::RecycleUnavailable;
        receipt.detail = recycleDetail;
        return receipt;
    }

    ComInit com;
    if (FAILED(com.hr()) && com.hr() != RPC_E_CHANGED_MODE) {
        receipt.result = MaintenanceItemResult::ShellError;
        receipt.hresult = static_cast<std::int32_t>(com.hr());
        receipt.detail = "CoInitializeEx failed";
        return receipt;
    }

    ComPtr<IFileOperation> operation;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(operation.put()));
    if (FAILED(hr) || !operation) {
        receipt.result = MaintenanceItemResult::ShellError;
        receipt.hresult = static_cast<std::int32_t>(hr);
        receipt.detail = "CoCreateInstance(CLSID_FileOperation) failed";
        return receipt;
    }

    const DWORD flags =
        FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD |
        FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE;
    hr = operation->SetOperationFlags(flags);
    if (FAILED(hr)) {
        receipt.result = MaintenanceItemResult::ShellError;
        receipt.hresult = static_cast<std::int32_t>(hr);
        receipt.detail = "SetOperationFlags failed";
        return receipt;
    }

    auto* sink = new RecycleSink();
    DWORD cookie = 0;
    hr = operation->Advise(sink, &cookie);
    if (FAILED(hr)) {
        sink->Release();
        receipt.result = MaintenanceItemResult::ShellError;
        receipt.hresult = static_cast<std::int32_t>(hr);
        receipt.detail = "IFileOperation::Advise failed";
        return receipt;
    }

    ComPtr<IShellItem> shellItem;
    hr = ::SHCreateItemFromParsingName(item.path.c_str(), nullptr,
                                       IID_PPV_ARGS(shellItem.put()));
    if (FAILED(hr) || !shellItem) {
        operation->Unadvise(cookie);
        sink->Release();
        receipt.result = MaintenanceItemResult::ShellError;
        receipt.hresult = static_cast<std::int32_t>(hr);
        receipt.detail = "SHCreateItemFromParsingName failed";
        return receipt;
    }

    hr = operation->DeleteItem(shellItem.get(), nullptr);
    if (FAILED(hr)) {
        operation->Unadvise(cookie);
        sink->Release();
        receipt.result = MaintenanceItemResult::ShellError;
        receipt.hresult = static_cast<std::int32_t>(hr);
        receipt.detail = "DeleteItem queue failed";
        return receipt;
    }

    hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    operation->GetAnyOperationsAborted(&aborted);
    operation->Unadvise(cookie);

    const bool sourceGone = ::GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES;

    const bool sawPostDelete = sink->sawPostDelete;
    const bool newlyCreatedPresent = sink->newlyCreatedPresent;
    const HRESULT recycledHr = sink->recycledHr;
    const std::wstring newlyCreatedName = sink->newlyCreatedName;
    sink->Release();
    sink = nullptr;

    if (aborted) {
        receipt.result = sourceGone && !newlyCreatedPresent
                             ? MaintenanceItemResult::UnexpectedPermanentRemoval
                             : MaintenanceItemResult::OperationAborted;
        receipt.hresult = static_cast<std::int32_t>(hr);
        receipt.detail = "GetAnyOperationsAborted was true";
        return receipt;
    }

    if (sawPostDelete && SUCCEEDED(recycledHr) && newlyCreatedPresent) {
        if (!sourceGone) {
            receipt.result = MaintenanceItemResult::UnknownResult;
            receipt.detail =
                "Shell reported a Recycle Bin item but the source path remains";
            receipt.recycleParsingName = narrow(newlyCreatedName);
            return receipt;
        }
        receipt.result = MaintenanceItemResult::Recycled;
        receipt.recycleParsingName = narrow(newlyCreatedName);
        receipt.hresult = static_cast<std::int32_t>(recycledHr);
        receipt.detail = "Recycled to Recycle Bin";
        return receipt;
    }

    if (sourceGone && (!newlyCreatedPresent || !sawPostDelete)) {
        receipt.result = MaintenanceItemResult::UnexpectedPermanentRemoval;
        receipt.hresult = static_cast<std::int32_t>(
            sawPostDelete ? recycledHr : hr);
        receipt.detail =
            "Source path is gone without Recycle Bin item evidence";
        return receipt;
    }

#ifndef COPYENGINE_E_REQUIRES_ELEVATION
    constexpr HRESULT kRequiresElevation = static_cast<HRESULT>(0x80270002L);
#else
    constexpr HRESULT kRequiresElevation = COPYENGINE_E_REQUIRES_ELEVATION;
#endif
    if (FAILED(hr) || FAILED(recycledHr)) {
        const HRESULT used = FAILED(recycledHr) ? recycledHr : hr;
        receipt.hresult = static_cast<std::int32_t>(used);
        if (used == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
            used == kRequiresElevation) {
            receipt.result = MaintenanceItemResult::AccessDenied;
            receipt.blockReason = used == kRequiresElevation
                                      ? MaintenanceBlockReason::RequiresElevation
                                      : MaintenanceBlockReason::AccessDenied;
            receipt.detail = "Recycle access denied or requires elevation";
            return receipt;
        }
        receipt.result = MaintenanceItemResult::ShellError;
        receipt.detail = "IFileOperation recycle failed";
        return receipt;
    }

    receipt.result = MaintenanceItemResult::UnknownResult;
    receipt.hresult = static_cast<std::int32_t>(hr);
    receipt.detail = "Recycle result was inconclusive";
    return receipt;
}

}  // namespace spacelens
