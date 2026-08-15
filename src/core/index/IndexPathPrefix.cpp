#include "core/index/IndexPathPrefix.hpp"

namespace spacelens {

std::wstring escapeLikeWide(std::wstring_view text)
{
    std::wstring out;
    out.reserve(text.size() * 2);
    for (wchar_t ch : text) {
        if (ch == L'%' || ch == L'_' || ch == L'\\') {
            out.push_back(L'\\');
        }
        out.push_back(ch);
    }
    return out;
}

void bindIndexedPathPrefix(SqliteStmt& stmt, int& idx, std::wstring prefix)
{
    for (wchar_t& ch : prefix) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    while (prefix.size() > 3 &&
           (prefix.back() == L'\\' || prefix.back() == L'/')) {
        prefix.pop_back();
    }
    stmt.bindText16(idx++, prefix);
    std::wstring likeBase = prefix;
    while (!likeBase.empty() &&
           (likeBase.back() == L'\\' || likeBase.back() == L'/')) {
        likeBase.pop_back();
    }
    std::wstring likePat = escapeLikeWide(likeBase);
    likePat.push_back(L'\\');
    likePat.push_back(L'\\');
    likePat.push_back(L'%');
    stmt.bindText16(idx++, likePat);
}

}  // namespace spacelens
