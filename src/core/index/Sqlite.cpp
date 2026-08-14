#include "core/index/Sqlite.hpp"

#include "sqlite3.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <limits>
#include <sstream>
#include <vector>

namespace spacelens {
namespace {

std::string wideToUtf8(std::wstring_view wide)
{
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
        nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

std::string errorFrom(sqlite3* db, int rc, std::string_view context)
{
    std::ostringstream os;
    os << context << " (rc=" << rc << ")";
    if (db != nullptr) {
        const char* msg = sqlite3_errmsg(db);
        if (msg != nullptr && msg[0] != '\0') {
            os << ": " << msg;
        }
    }
    return os.str();
}

}  // namespace

SqliteDb::SqliteDb(const std::wstring& pathUtf16, int flags)
{
    const std::string pathUtf8 = wideToUtf8(pathUtf16);
    if (pathUtf8.empty() && !pathUtf16.empty()) {
        throw SqliteError("failed to convert database path to UTF-8");
    }
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(pathUtf8.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK || db == nullptr) {
        if (db != nullptr) {
            const std::string msg = errorFrom(db, rc, "sqlite3_open_v2");
            sqlite3_close(db);
            throw SqliteError(msg);
        }
        throw SqliteError(errorFrom(nullptr, rc, "sqlite3_open_v2"));
    }
    m_db = db;
    sqlite3_busy_timeout(m_db, 5000);
    try {
        exec("PRAGMA foreign_keys=ON;");
        if ((flags & SqliteOpen::ReadOnly) != 0) {
            exec("PRAGMA query_only=ON;");
        }
    } catch (...) {
        close();
        throw;
    }
}

SqliteDb::~SqliteDb()
{
    close();
}

SqliteDb::SqliteDb(SqliteDb&& other) noexcept
    : m_db(other.m_db)
{
    other.m_db = nullptr;
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept
{
    if (this != &other) {
        close();
        m_db = other.m_db;
        other.m_db = nullptr;
    }
    return *this;
}

void SqliteDb::close() noexcept
{
    if (m_db != nullptr) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void SqliteDb::exec(std::string_view sql)
{
    if (m_db == nullptr) {
        throw SqliteError("database is not open");
    }
    char* err = nullptr;
    const int rc =
        sqlite3_exec(m_db, std::string(sql).c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : errorFrom(m_db, rc, "sqlite3_exec");
        if (err) {
            sqlite3_free(err);
        }
        throw SqliteError(msg);
    }
}

std::int64_t SqliteDb::lastInsertRowId() const
{
    return m_db ? static_cast<std::int64_t>(sqlite3_last_insert_rowid(m_db)) : 0;
}

int SqliteDb::changes() const
{
    return m_db ? sqlite3_changes(m_db) : 0;
}

SqliteStmt::SqliteStmt(SqliteDb& db, std::string_view sql)
    : m_db(db.handle())
{
    if (m_db == nullptr) {
        throw SqliteError("prepare: database is not open");
    }
    const int rc = sqlite3_prepare_v2(m_db, std::string(sql).c_str(),
                                      static_cast<int>(sql.size()), &m_stmt,
                                      nullptr);
    if (rc != SQLITE_OK || m_stmt == nullptr) {
        throw SqliteError(errorFrom(m_db, rc, "sqlite3_prepare_v2"));
    }
}

SqliteStmt::~SqliteStmt()
{
    finalize();
}

SqliteStmt::SqliteStmt(SqliteStmt&& other) noexcept
    : m_stmt(other.m_stmt)
    , m_db(other.m_db)
{
    other.m_stmt = nullptr;
    other.m_db = nullptr;
}

SqliteStmt& SqliteStmt::operator=(SqliteStmt&& other) noexcept
{
    if (this != &other) {
        finalize();
        m_stmt = other.m_stmt;
        m_db = other.m_db;
        other.m_stmt = nullptr;
        other.m_db = nullptr;
    }
    return *this;
}

void SqliteStmt::finalize() noexcept
{
    if (m_stmt != nullptr) {
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
    }
    m_db = nullptr;
}

void SqliteStmt::reset()
{
    if (m_stmt) {
        sqlite3_reset(m_stmt);
    }
}

void SqliteStmt::clearBindings()
{
    if (m_stmt) {
        sqlite3_clear_bindings(m_stmt);
    }
}

void SqliteStmt::bindNull(int index)
{
    const int rc = sqlite3_bind_null(m_stmt, index);
    if (rc != SQLITE_OK) {
        throw SqliteError(errorFrom(m_db, rc, "bindNull"));
    }
}

void SqliteStmt::bindInt64(int index, std::int64_t value)
{
    const int rc = sqlite3_bind_int64(m_stmt, index, value);
    if (rc != SQLITE_OK) {
        throw SqliteError(errorFrom(m_db, rc, "bindInt64"));
    }
}

void SqliteStmt::bindText(int index, std::string_view value)
{
    const int rc = sqlite3_bind_text(m_stmt, index, value.data(),
                                     static_cast<int>(value.size()),
                                     SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw SqliteError(errorFrom(m_db, rc, "bindText"));
    }
}

void SqliteStmt::bindText16(int index, std::wstring_view value)
{
    const int rc = sqlite3_bind_text16(
        m_stmt, index, value.data(),
        static_cast<int>(value.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw SqliteError(errorFrom(m_db, rc, "bindText16"));
    }
}

void SqliteStmt::bindBlob(int index, const void* data, std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw SqliteError("bindBlob: blob too large");
    }
    const int rc = sqlite3_bind_blob(m_stmt, index, data, static_cast<int>(size),
                                     SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw SqliteError(errorFrom(m_db, rc, "bindBlob"));
    }
}

bool SqliteStmt::step()
{
    const int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw SqliteError(errorFrom(m_db, rc, "sqlite3_step"));
}

void SqliteStmt::stepDone()
{
    if (step()) {
        throw SqliteError("sqlite3_step expected DONE but got ROW");
    }
}

std::int64_t SqliteStmt::columnInt64(int index) const
{
    return static_cast<std::int64_t>(sqlite3_column_int64(m_stmt, index));
}

std::string SqliteStmt::columnText(int index) const
{
    const unsigned char* text = sqlite3_column_text(m_stmt, index);
    if (text == nullptr) {
        return {};
    }
    return reinterpret_cast<const char*>(text);
}

std::wstring SqliteStmt::columnText16(int index) const
{
    const void* text = sqlite3_column_text16(m_stmt, index);
    if (text == nullptr) {
        return {};
    }
    const int bytes = sqlite3_column_bytes16(m_stmt, index);
    if (bytes <= 0) {
        return {};
    }
    return std::wstring(static_cast<const wchar_t*>(text),
                        static_cast<std::size_t>(bytes / sizeof(wchar_t)));
}

std::vector<std::uint8_t> SqliteStmt::columnBlob(int index) const
{
    const void* data = sqlite3_column_blob(m_stmt, index);
    const int bytes = sqlite3_column_bytes(m_stmt, index);
    if (data == nullptr || bytes <= 0) {
        return {};
    }
    const auto* begin = static_cast<const std::uint8_t*>(data);
    return std::vector<std::uint8_t>(begin, begin + bytes);
}

SqliteTxn::SqliteTxn(SqliteDb& db)
    : m_db(&db)
{
    m_db->exec("BEGIN IMMEDIATE;");
}

SqliteTxn::~SqliteTxn()
{
    if (!m_done && m_db != nullptr && m_db->isOpen()) {
        try {
            m_db->exec("ROLLBACK;");
        } catch (...) {
        }
    }
}

void SqliteTxn::commit()
{
    if (m_done || m_db == nullptr) {
        return;
    }
    m_db->exec("COMMIT;");
    m_done = true;
}

void SqliteTxn::rollback()
{
    if (m_done || m_db == nullptr) {
        return;
    }
    m_db->exec("ROLLBACK;");
    m_done = true;
}

}  // namespace spacelens
