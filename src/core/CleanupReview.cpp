#include "core/CleanupReview.hpp"

#include "core/SizeFormatter.hpp"

#include <cwctype>
#include <sstream>

namespace spacelens {
namespace {

bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* toString(ItemKind kind) noexcept
{
    switch (kind) {
    case ItemKind::File:
        return "File";
    case ItemKind::Directory:
        return "Directory";
    case ItemKind::ReparseDirectory:
        return "ReparseDirectory";
    }
    return "File";
}

std::wstring CleanupReview::normalizeKey(std::wstring_view path)
{
    std::wstring out(path);
    for (wchar_t& ch : out) {
        if (ch == L'/') {
            ch = L'\\';
        }
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    while (out.size() > 3 && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

std::vector<CleanupCandidate>::iterator CleanupReview::findIt(
    std::wstring_view path)
{
    const std::wstring key = normalizeKey(path);
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (normalizeKey(it->path) == key) {
            return it;
        }
    }
    return m_items.end();
}

std::vector<CleanupCandidate>::const_iterator CleanupReview::findIt(
    std::wstring_view path) const
{
    const std::wstring key = normalizeKey(path);
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (normalizeKey(it->path) == key) {
            return it;
        }
    }
    return m_items.end();
}

std::uint64_t CleanupReview::add(CleanupCandidate candidate)
{
    if (candidate.path.empty()) {
        return 0;
    }
    auto it = findIt(candidate.path);
    if (it != m_items.end()) {
        // Duplicate path: refresh snapshot metadata, keep stable id.
        const auto id = it->id;
        candidate.id = id;
        *it = std::move(candidate);
        return id;
    }
    candidate.id = m_nextId++;
    m_items.push_back(std::move(candidate));
    return m_items.back().id;
}

bool CleanupReview::removeById(std::uint64_t id)
{
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (it->id == id) {
            m_items.erase(it);
            return true;
        }
    }
    return false;
}

bool CleanupReview::removeByPath(std::wstring_view path)
{
    auto it = findIt(path);
    if (it == m_items.end()) {
        return false;
    }
    m_items.erase(it);
    return true;
}

void CleanupReview::clear() noexcept
{
    m_items.clear();
}

bool CleanupReview::containsPath(std::wstring_view path) const
{
    return findIt(path) != m_items.end();
}

std::optional<CleanupCandidate> CleanupReview::findById(std::uint64_t id) const
{
    for (const auto& item : m_items) {
        if (item.id == id) {
            return item;
        }
    }
    return std::nullopt;
}

std::optional<CleanupCandidate> CleanupReview::findByPath(
    std::wstring_view path) const
{
    auto it = findIt(path);
    if (it == m_items.end()) {
        return std::nullopt;
    }
    return *it;
}

ByteSize CleanupReview::totalLogicalSize() const noexcept
{
    ByteSize total = 0;
    for (const auto& item : m_items) {
        total += item.sizeAtSelection;
    }
    return total;
}

std::string CleanupReview::copyReport() const
{
    std::ostringstream os;
    os << "SpaceLens Cleanup Review (planning only — no deletions)\n";
    os << "Selected: " << m_items.size() << " items\n";
    os << "Total logical size: " << SizeFormatter::format(totalLogicalSize())
       << "\n\n";
    for (const auto& item : m_items) {
        // Path may be wide; print as UTF-8-ish best-effort ASCII for report.
        std::string path;
        path.reserve(item.path.size());
        for (wchar_t ch : item.path) {
            path.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        os << "- [" << toString(item.kind) << "] " << path << "\n";
        os << "  size: " << SizeFormatter::format(item.sizeAtSelection) << "\n";
        os << "  classification: " << toString(item.classification.category)
           << " (" << toString(item.classification.confidence) << ")\n";
        if (!item.classification.reason.empty()) {
            os << "  reason: " << item.classification.reason << "\n";
        }
        if (!item.reasonAdded.empty()) {
            os << "  added: " << item.reasonAdded << "\n";
        }
        os << "\n";
    }
    os << "Note: This report is not authorization to delete or move files.\n";
    return os.str();
}

}  // namespace spacelens
