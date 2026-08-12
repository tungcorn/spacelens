#pragma once

#include "core/IFileEnumerator.hpp"

#include <map>
#include <utility>

namespace spacelens::test {

/// In-memory enumerator for deterministic unit tests.
class FakeFileEnumerator final : public IFileEnumerator {
public:
    void setListing(std::wstring path, EnumerateResult result)
    {
        m_listings[std::move(path)] = std::move(result);
    }

    void setChildren(std::wstring path, std::vector<EnumeratedEntry> entries)
    {
        EnumerateResult result;
        result.status = EnumerateStatus::Ok;
        result.entries = std::move(entries);
        setListing(std::move(path), std::move(result));
    }

    [[nodiscard]] EnumerateResult enumerate(
        const std::wstring& directoryPath) override
    {
        const auto it = m_listings.find(directoryPath);
        if (it == m_listings.end()) {
            EnumerateResult missing;
            missing.status = EnumerateStatus::NotFound;
            missing.message = L"fake: path not registered";
            return missing;
        }
        return it->second;
    }

private:
    std::map<std::wstring, EnumerateResult> m_listings;
};

inline EnumeratedEntry makeFile(std::wstring name, ByteSize size)
{
    EnumeratedEntry e;
    e.name = std::move(name);
    e.kind = EntryKind::File;
    e.size = size;
    return e;
}

inline EnumeratedEntry makeDir(std::wstring name)
{
    EnumeratedEntry e;
    e.name = std::move(name);
    e.kind = EntryKind::Directory;
    return e;
}

inline EnumeratedEntry makeReparseDir(std::wstring name)
{
    EnumeratedEntry e;
    e.name = std::move(name);
    e.kind = EntryKind::ReparseDirectory;
    return e;
}

}  // namespace spacelens::test
