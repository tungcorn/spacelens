#pragma once

#include "core/index/Sqlite.hpp"

#include <string>
#include <string_view>

namespace spacelens {

/// Component-safe indexed path-prefix SQL. Bind with bindIndexedPathPrefix.
/// Matches the path itself or any descendant; `D:\foo` does not match `D:\foobar`.
inline constexpr const char* kIndexedPathPrefixSql =
    " AND (path = ? COLLATE NOCASE OR path LIKE ? ESCAPE '\\' COLLATE NOCASE)";

/// Escape LIKE wildcards so a Windows path is a literal prefix match.
[[nodiscard]] std::wstring escapeLikeWide(std::wstring_view text);

/// Exact bind keeps a drive-root trailing slash (`D:\`). LIKE is built from
/// the slash-stripped base (`D:`) so ESCAPE '\' yields `D:\\%` and matches
/// `D:\Users\...`. Binding the unstripped `D:\` would LIKE-escape into
/// `D:\\\\%` and match nothing.
void bindIndexedPathPrefix(SqliteStmt& stmt, int& idx, std::wstring prefix);

}  // namespace spacelens
