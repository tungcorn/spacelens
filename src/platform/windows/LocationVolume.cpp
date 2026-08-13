#include "platform/windows/LocationVolume.hpp"

#include "platform/windows/VolumeHandle.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace spacelens {

LocationVolumeEvidence WindowsVolumeIdentityReader::read(
    const std::wstring& path) const
{
    LocationVolumeEvidence out;
    VolumeIdentity identity;
    if (!resolveVolumeIdentity(path, identity)) {
        return out;
    }
    out.rootPath = identity.rootPath;
    if (identity.serialNumber != 0) {
        out.available = true;
        out.serial = identity.serialNumber;
    }

    std::wstring mount = identity.rootPath;
    if (!mount.empty() && mount.back() != L'\\' && mount.back() != L'/') {
        mount.push_back(L'\\');
    }
    wchar_t guid[MAX_PATH]{};
    if (!mount.empty() &&
        ::GetVolumeNameForVolumeMountPointW(mount.c_str(), guid, MAX_PATH)) {
        out.guid = guid;
        if (out.serial != 0) {
            out.available = true;
        }
    }
    return out;
}

}  // namespace spacelens
