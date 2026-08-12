#pragma once

// Thin RAII wrappers around the SQLite C API.
// Ownership: one SqliteDb owns one connection; statements and transactions
// must not outlive the db. V1 is single-threaded per connection (writer on
// the indexing thread; queries open a short-lived read connection).

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

struct sqlite3;
struct sqlite3_stmt;

namespace spacelens {

class SqliteError : public std::runtime_error {
public:
    explicit SqliteError(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

class SqliteDb {
public:
    SqliteDb() = default;
    explicit SqliteDb(const std::wstring& pathUtf16, int flags);
    ~SqliteDb();

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;
    SqliteDb(SqliteDb&& other) noexcept;
    SqliteDb& operator=(SqliteDb&& other) noexcept;

    [[nodiscard]] bool isOpen() const noexcept { return m_db != nullptr; }
    [[nodiscard]] sqlite3* handle() const noexcept { return m_db; }

    void exec(std::string_view sql);
    [[nodiscard]] std::int64_t lastInsertRowId() const;
    [[nodiscard]] int changes() const;

    void close() noexcept;

private:
    sqlite3* m_db = nullptr;
};

class SqliteStmt {
public:
    SqliteStmt() = default;
    SqliteStmt(SqliteDb& db, std::string_view sql);
    ~SqliteStmt();

    SqliteStmt(const SqliteStmt&) = delete;
    SqliteStmt& operator=(const SqliteStmt&) = delete;
    SqliteStmt(SqliteStmt&& other) noexcept;
    SqliteStmt& operator=(SqliteStmt&& other) noexcept;

    void reset();
    void clearBindings();
    void bindNull(int index);
    void bindInt64(int index, std::int64_t value);
    void bindText(int index, std::string_view value);
    void bindText16(int index, std::wstring_view value);

    /// Returns true when a row is available (SQLITE_ROW).
    [[nodiscard]] bool step();
    void stepDone();  // expects SQLITE_DONE

    [[nodiscard]] std::int64_t columnInt64(int index) const;
    [[nodiscard]] std::string columnText(int index) const;
    [[nodiscard]] std::wstring columnText16(int index) const;

    void finalize() noexcept;

private:
    sqlite3_stmt* m_stmt = nullptr;
    sqlite3* m_db = nullptr;
};

class SqliteTxn {
public:
    explicit SqliteTxn(SqliteDb& db);
    ~SqliteTxn();

    SqliteTxn(const SqliteTxn&) = delete;
    SqliteTxn& operator=(const SqliteTxn&) = delete;

    void commit();
    void rollback();

private:
    SqliteDb* m_db = nullptr;
    bool m_done = false;
};

/// Open flags for SqliteDb (subset of SQLite open flags).
namespace SqliteOpen {
inline constexpr int ReadOnly = 0x00000001;   // SQLITE_OPEN_READONLY
inline constexpr int ReadWrite = 0x00000002;  // SQLITE_OPEN_READWRITE
inline constexpr int Create = 0x00000004;     // SQLITE_OPEN_CREATE
inline constexpr int FullMutex = 0x00010000;  // SQLITE_OPEN_FULLMUTEX
}  // namespace SqliteOpen

}  // namespace spacelens
