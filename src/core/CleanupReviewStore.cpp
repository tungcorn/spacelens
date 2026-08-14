#include "core/CleanupReviewStore.hpp"

#include "core/Classification.hpp"
#include "core/Json.hpp"
#include "core/index/IndexPaths.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <sstream>
#include <utility>

namespace spacelens {
namespace {

constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS meta (
  key TEXT PRIMARY KEY NOT NULL,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS review_items (
  id INTEGER PRIMARY KEY NOT NULL,
  path TEXT NOT NULL,
  path_key TEXT NOT NULL,
  kind TEXT NOT NULL,
  identity_source TEXT NOT NULL,
  identity_volume INTEGER NOT NULL DEFAULT 0,
  identity_file_id_128 TEXT NOT NULL DEFAULT '',
  identity_file_index_64 INTEGER NOT NULL DEFAULT 0,
  object_available INTEGER NOT NULL DEFAULT 0,
  object_kind TEXT NOT NULL,
  size_scope TEXT NOT NULL,
  logical_size INTEGER NOT NULL DEFAULT 0,
  last_write_ticks INTEGER NOT NULL DEFAULT 0,
  last_access_ticks INTEGER NOT NULL DEFAULT 0,
  attributes INTEGER NOT NULL DEFAULT 0,
  hist_agg_available INTEGER NOT NULL DEFAULT 0,
  hist_agg_revalidated INTEGER NOT NULL DEFAULT 0,
  hist_agg_recursive_size INTEGER NOT NULL DEFAULT 0,
  hist_agg_newest_descendant_write INTEGER NOT NULL DEFAULT 0,
  captured_safety TEXT NOT NULL,
  captured_reclaimability TEXT NOT NULL,
  captured_candidate_strength TEXT NOT NULL,
  class_category TEXT NOT NULL DEFAULT 'Unknown',
  class_confidence TEXT NOT NULL DEFAULT 'Low',
  class_rule_id TEXT NOT NULL DEFAULT '',
  class_reason TEXT NOT NULL DEFAULT '',
  source TEXT NOT NULL DEFAULT 'live_scan',
  source_root TEXT NOT NULL DEFAULT '',
  index_age_ms INTEGER NOT NULL DEFAULT 0,
  index_indexed_at_iso TEXT NOT NULL DEFAULT '',
  added_at INTEGER NOT NULL DEFAULT 0,
  reason_added TEXT NOT NULL DEFAULT '',
  size_at_selection INTEGER NOT NULL DEFAULT 0,
  last_write_time INTEGER NOT NULL DEFAULT 0,
  legacy_attributes INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS review_validation (
  review_item_id INTEGER PRIMARY KEY NOT NULL,
  current_available INTEGER NOT NULL DEFAULT 0,
  current_exists INTEGER NOT NULL DEFAULT 1,
  current_object_available INTEGER NOT NULL DEFAULT 0,
  current_identity_source TEXT NOT NULL DEFAULT 'Unavailable',
  current_identity_volume INTEGER NOT NULL DEFAULT 0,
  current_identity_file_id_128 TEXT NOT NULL DEFAULT '',
  current_identity_file_index_64 INTEGER NOT NULL DEFAULT 0,
  current_object_kind TEXT NOT NULL DEFAULT 'File',
  current_size_scope TEXT NOT NULL DEFAULT 'Direct',
  current_logical_size INTEGER NOT NULL DEFAULT 0,
  current_last_write_ticks INTEGER NOT NULL DEFAULT 0,
  current_last_access_ticks INTEGER NOT NULL DEFAULT 0,
  current_attributes INTEGER NOT NULL DEFAULT 0,
  current_agg_available INTEGER NOT NULL DEFAULT 0,
  current_agg_revalidated INTEGER NOT NULL DEFAULT 0,
  current_agg_recursive_size INTEGER NOT NULL DEFAULT 0,
  current_agg_newest_descendant_write INTEGER NOT NULL DEFAULT 0,
  current_safety TEXT NOT NULL DEFAULT 'Unknown',
  primary_state TEXT NOT NULL DEFAULT 'NotValidated',
  reason_flags INTEGER NOT NULL DEFAULT 0,
  diffs_json TEXT NOT NULL DEFAULT '[]',
  object_identity_matched INTEGER NOT NULL DEFAULT 0,
  direct_metadata_unchanged INTEGER NOT NULL DEFAULT 0,
  recursive_evidence_revalidated INTEGER NOT NULL DEFAULT 0,
  checked_at INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY(review_item_id) REFERENCES review_items(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_review_items_path_key
  ON review_items(path_key);
)SQL";

struct ColumnSpec {
    const char* table;
    const char* column;
};

constexpr ColumnSpec kRequiredColumns[] = {
    {"meta", "key"},
    {"meta", "value"},
    {"review_items", "id"},
    {"review_items", "path"},
    {"review_items", "path_key"},
    {"review_items", "kind"},
    {"review_items", "identity_source"},
    {"review_items", "identity_volume"},
    {"review_items", "identity_file_id_128"},
    {"review_items", "identity_file_index_64"},
    {"review_items", "object_available"},
    {"review_items", "object_kind"},
    {"review_items", "size_scope"},
    {"review_items", "logical_size"},
    {"review_items", "last_write_ticks"},
    {"review_items", "last_access_ticks"},
    {"review_items", "attributes"},
    {"review_items", "hist_agg_available"},
    {"review_items", "hist_agg_revalidated"},
    {"review_items", "hist_agg_recursive_size"},
    {"review_items", "hist_agg_newest_descendant_write"},
    {"review_items", "captured_safety"},
    {"review_items", "captured_reclaimability"},
    {"review_items", "captured_candidate_strength"},
    {"review_items", "class_category"},
    {"review_items", "class_confidence"},
    {"review_items", "class_rule_id"},
    {"review_items", "class_reason"},
    {"review_items", "source"},
    {"review_items", "source_root"},
    {"review_items", "index_age_ms"},
    {"review_items", "index_indexed_at_iso"},
    {"review_items", "added_at"},
    {"review_items", "reason_added"},
    {"review_items", "size_at_selection"},
    {"review_items", "last_write_time"},
    {"review_items", "legacy_attributes"},
    {"review_validation", "review_item_id"},
    {"review_validation", "current_available"},
    {"review_validation", "current_exists"},
    {"review_validation", "current_object_available"},
    {"review_validation", "current_identity_source"},
    {"review_validation", "current_identity_volume"},
    {"review_validation", "current_identity_file_id_128"},
    {"review_validation", "current_identity_file_index_64"},
    {"review_validation", "current_object_kind"},
    {"review_validation", "current_size_scope"},
    {"review_validation", "current_logical_size"},
    {"review_validation", "current_last_write_ticks"},
    {"review_validation", "current_last_access_ticks"},
    {"review_validation", "current_attributes"},
    {"review_validation", "current_agg_available"},
    {"review_validation", "current_agg_revalidated"},
    {"review_validation", "current_agg_recursive_size"},
    {"review_validation", "current_agg_newest_descendant_write"},
    {"review_validation", "current_safety"},
    {"review_validation", "primary_state"},
    {"review_validation", "reason_flags"},
    {"review_validation", "diffs_json"},
    {"review_validation", "object_identity_matched"},
    {"review_validation", "direct_metadata_unchanged"},
    {"review_validation", "recursive_evidence_revalidated"},
    {"review_validation", "checked_at"},
};

enum class SchemaState {
    Empty,
    Ready,
    Newer,
    Malformed
};

CleanupReviewStatus makeStatus(CleanupReviewError error, std::string message)
{
    CleanupReviewStatus status;
    status.ok = error == CleanupReviewError::None;
    status.error = error;
    status.message = std::move(message);
    return status;
}

std::wstring parentDirectory(const std::wstring& path)
{
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return {};
    }
    return path.substr(0, pos);
}

bool tableExists(SqliteDb& db, const char* name)
{
    SqliteStmt stmt(
        db,
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1;");
    stmt.bindText(1, name);
    return stmt.step();
}

bool columnExists(SqliteDb& db, const char* table, const char* column)
{
    const std::string sql = std::string("PRAGMA table_info(") + table + ");";
    SqliteStmt stmt(db, sql);
    while (stmt.step()) {
        if (stmt.columnText(1) == column) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> userTables(SqliteDb& db)
{
    std::vector<std::string> names;
    SqliteStmt stmt(db,
                    "SELECT name FROM sqlite_master WHERE type = 'table' "
                    "AND name NOT LIKE 'sqlite_%' ORDER BY name;");
    while (stmt.step()) {
        names.push_back(stmt.columnText(0));
    }
    return names;
}

std::optional<int> readReviewSchemaVersion(SqliteDb& db)
{
    if (!tableExists(db, "meta")) {
        return std::nullopt;
    }
    SqliteStmt stmt(db, "SELECT value FROM meta WHERE key = ?1;");
    stmt.bindText(1, "review_schema_version");
    if (!stmt.step()) {
        return std::nullopt;
    }
    try {
        return std::stoi(stmt.columnText(0));
    } catch (...) {
        return std::nullopt;
    }
}

void upsertMeta(SqliteDb& db, std::string_view key, std::string_view value)
{
    SqliteStmt stmt(db,
                    "INSERT INTO meta(key, value) VALUES(?1, ?2) "
                    "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    stmt.bindText(1, key);
    stmt.bindText(2, value);
    stmt.stepDone();
}

std::uint64_t readNextId(SqliteDb& db)
{
    if (!tableExists(db, "meta")) {
        return 1;
    }
    SqliteStmt stmt(db, "SELECT value FROM meta WHERE key = ?1;");
    stmt.bindText(1, "review_next_id");
    if (!stmt.step()) {
        return 1;
    }
    try {
        const auto value = std::stoull(stmt.columnText(0));
        return value == 0 ? 1 : static_cast<std::uint64_t>(value);
    } catch (...) {
        return 1;
    }
}

SchemaState inspectSchema(SqliteDb& db)
{
    const auto tables = userTables(db);
    const bool hasMeta = tableExists(db, "meta");
    const bool hasItems = tableExists(db, "review_items");
    const bool hasValidation = tableExists(db, "review_validation");

    if (!hasMeta && !hasItems && !hasValidation) {
        return tables.empty() ? SchemaState::Empty : SchemaState::Malformed;
    }

    const auto version = readReviewSchemaVersion(db);
    if (!version.has_value()) {
        return SchemaState::Malformed;
    }
    if (*version > kReviewSchemaVersion) {
        return SchemaState::Newer;
    }
    if (*version < 1) {
        return SchemaState::Malformed;
    }
    if (!hasItems || !hasValidation) {
        return SchemaState::Malformed;
    }
    for (const auto& spec : kRequiredColumns) {
        if (!columnExists(db, spec.table, spec.column)) {
            return SchemaState::Malformed;
        }
    }
    return SchemaState::Ready;
}

std::string fileIdToHex(const std::array<std::uint8_t, 16>& id)
{
    std::string out;
    out.reserve(id.size() * 2);
    for (const auto byte : id) {
        char buffer[3]{};
        std::snprintf(buffer, sizeof(buffer), "%02x", byte);
        out += buffer;
    }
    return out;
}

std::array<std::uint8_t, 16> hexToFileId(std::string_view text)
{
    std::array<std::uint8_t, 16> id{};
    if (text.size() != 32) {
        return id;
    }
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < 16; ++i) {
        const int hi = nibble(text[i * 2]);
        const int lo = nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        id[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return id;
}

int hexDigit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string unescapeJsonString(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch != '\\' || i + 1 >= text.size()) {
            out.push_back(ch);
            continue;
        }
        const char next = text[++i];
        switch (next) {
        case '"':
        case '\\':
        case '/':
            out.push_back(next);
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u':
            if (i + 4 < text.size()) {
                int value = 0;
                bool ok = true;
                for (int n = 0; n < 4; ++n) {
                    const int digit = hexDigit(text[i + 1 + static_cast<std::size_t>(n)]);
                    if (digit < 0) {
                        ok = false;
                        break;
                    }
                    value = (value << 4) | digit;
                }
                if (ok) {
                    i += 4;
                    if (value < 0x80) {
                        out.push_back(static_cast<char>(value));
                    } else if (value < 0x800) {
                        out.push_back(static_cast<char>(0xc0 | (value >> 6)));
                        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
                    } else {
                        out.push_back(static_cast<char>(0xe0 | (value >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
                        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
                    }
                    break;
                }
            }
            out.push_back('u');
            break;
        default:
            out.push_back(next);
            break;
        }
    }
    return out;
}

void skipWs(std::string_view text, std::size_t& i)
{
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' ||
            text[i] == '\t')) {
        ++i;
    }
}

bool parseJsonString(std::string_view text, std::size_t& i, std::string& out)
{
    skipWs(text, i);
    if (i >= text.size() || text[i] != '"') {
        return false;
    }
    ++i;
    std::string raw;
    while (i < text.size()) {
        const char ch = text[i++];
        if (ch == '"') {
            out = unescapeJsonString(raw);
            return true;
        }
        raw.push_back(ch);
        if (ch == '\\' && i < text.size()) {
            raw.push_back(text[i++]);
        }
    }
    return false;
}

ItemKind parseItemKind(std::string_view text)
{
    if (text == "Directory") {
        return ItemKind::Directory;
    }
    if (text == "ReparseDirectory") {
        return ItemKind::ReparseDirectory;
    }
    return ItemKind::File;
}

CleanupIdentitySource parseIdentitySource(std::string_view text)
{
    if (text == "FileId128") {
        return CleanupIdentitySource::FileId128;
    }
    if (text == "FileIndex64Fallback") {
        return CleanupIdentitySource::FileIndex64Fallback;
    }
    return CleanupIdentitySource::Unavailable;
}

CleanupEvidenceScope parseScope(std::string_view text)
{
    return text == "Recursive" ? CleanupEvidenceScope::Recursive
                               : CleanupEvidenceScope::Direct;
}

LocationSafety parseSafety(std::string_view text)
{
    if (text == "Protected") {
        return LocationSafety::Protected;
    }
    if (text == "Sensitive") {
        return LocationSafety::Sensitive;
    }
    if (text == "Ordinary") {
        return LocationSafety::Ordinary;
    }
    return LocationSafety::Unknown;
}

Reclaimability parseReclaimability(std::string_view text)
{
    if (text == "LikelyRegenerable") {
        return Reclaimability::LikelyRegenerable;
    }
    if (text == "PossiblyRegenerable") {
        return Reclaimability::PossiblyRegenerable;
    }
    if (text == "NotApplicable") {
        return Reclaimability::NotApplicable;
    }
    return Reclaimability::Unknown;
}

CandidateStrength parseStrength(std::string_view text)
{
    if (text == "ReviewOnly") {
        return CandidateStrength::ReviewOnly;
    }
    if (text == "Moderate") {
        return CandidateStrength::Moderate;
    }
    if (text == "Strong") {
        return CandidateStrength::Strong;
    }
    return CandidateStrength::None;
}

Confidence parseConfidence(std::string_view text)
{
    if (text == "High") {
        return Confidence::High;
    }
    if (text == "Medium") {
        return Confidence::Medium;
    }
    return Confidence::Low;
}

CleanupValidationState parseValidationState(std::string_view text)
{
    if (text == "Unchanged") {
        return CleanupValidationState::Unchanged;
    }
    if (text == "Missing") {
        return CleanupValidationState::Missing;
    }
    if (text == "TypeChanged") {
        return CleanupValidationState::TypeChanged;
    }
    if (text == "IdentityChanged") {
        return CleanupValidationState::IdentityChanged;
    }
    if (text == "IdentityUnavailable") {
        return CleanupValidationState::IdentityUnavailable;
    }
    if (text == "Protected") {
        return CleanupValidationState::Protected;
    }
    if (text == "Changed") {
        return CleanupValidationState::Changed;
    }
    if (text == "DirectUnchangedRecursiveNotRevalidated") {
        return CleanupValidationState::DirectUnchangedRecursiveNotRevalidated;
    }
    if (text == "AccessDenied") {
        return CleanupValidationState::AccessDenied;
    }
    if (text == "ProbeError") {
        return CleanupValidationState::ProbeError;
    }
    return CleanupValidationState::NotValidated;
}

CleanupValidationDiffKind parseDiffKind(std::string_view text)
{
    if (text == "Type") {
        return CleanupValidationDiffKind::Type;
    }
    if (text == "Identity") {
        return CleanupValidationDiffKind::Identity;
    }
    if (text == "LastWriteTime") {
        return CleanupValidationDiffKind::LastWriteTime;
    }
    if (text == "LastAccessTime") {
        return CleanupValidationDiffKind::LastAccessTime;
    }
    if (text == "Attributes") {
        return CleanupValidationDiffKind::Attributes;
    }
    if (text == "Safety") {
        return CleanupValidationDiffKind::Safety;
    }
    if (text == "RecursiveLogicalSize") {
        return CleanupValidationDiffKind::RecursiveLogicalSize;
    }
    if (text == "RecursiveNewestDescendantWrite") {
        return CleanupValidationDiffKind::RecursiveNewestDescendantWrite;
    }
    return CleanupValidationDiffKind::LogicalSize;
}

std::string diffsToJson(const std::vector<CleanupValidationDiff>& diffs)
{
    std::ostringstream os;
    os << '[';
    for (std::size_t i = 0; i < diffs.size(); ++i) {
        if (i != 0) {
            os << ',';
        }
        os << "{\"kind\":" << jsonString(toString(diffs[i].kind))
           << ",\"captured\":" << jsonString(diffs[i].captured)
           << ",\"current\":" << jsonString(diffs[i].current) << "}";
    }
    os << ']';
    return os.str();
}

std::vector<CleanupValidationDiff> diffsFromJson(std::string_view text)
{
    std::vector<CleanupValidationDiff> out;
    std::size_t i = 0;
    skipWs(text, i);
    if (i >= text.size() || text[i] != '[') {
        return out;
    }
    ++i;
    skipWs(text, i);
    if (i < text.size() && text[i] == ']') {
        return out;
    }
    while (i < text.size()) {
        skipWs(text, i);
        if (i >= text.size() || text[i] != '{') {
            break;
        }
        ++i;
        CleanupValidationDiff diff;
        while (i < text.size() && text[i] != '}') {
            std::string key;
            if (!parseJsonString(text, i, key)) {
                return out;
            }
            skipWs(text, i);
            if (i >= text.size() || text[i] != ':') {
                return out;
            }
            ++i;
            std::string value;
            if (!parseJsonString(text, i, value)) {
                return out;
            }
            if (key == "kind") {
                diff.kind = parseDiffKind(value);
            } else if (key == "captured") {
                diff.captured = std::move(value);
            } else if (key == "current") {
                diff.current = std::move(value);
            }
            skipWs(text, i);
            if (i < text.size() && text[i] == ',') {
                ++i;
            }
        }
        if (i < text.size() && text[i] == '}') {
            ++i;
        }
        out.push_back(std::move(diff));
        skipWs(text, i);
        if (i < text.size() && text[i] == ',') {
            ++i;
            continue;
        }
        break;
    }
    return out;
}

CleanupIdentity readIdentity(std::string_view source,
                             std::uint64_t volume,
                             std::string_view fileIdHex,
                             std::uint64_t fileIndex)
{
    CleanupIdentity identity;
    identity.source = parseIdentitySource(source);
    identity.volumeSerial = volume;
    identity.fileId128 = hexToFileId(fileIdHex);
    identity.fileIndex64 = fileIndex;
    return identity;
}

void bindIdentity(SqliteStmt& stmt,
                  int sourceIndex,
                  const CleanupIdentity& identity)
{
    stmt.bindText(sourceIndex, toString(identity.source));
    stmt.bindInt64(sourceIndex + 1, static_cast<std::int64_t>(identity.volumeSerial));
    stmt.bindText(sourceIndex + 2, fileIdToHex(identity.fileId128));
    stmt.bindInt64(sourceIndex + 3, static_cast<std::int64_t>(identity.fileIndex64));
}

void insertItem(SqliteDb& db, const CleanupCandidate& item)
{
    SqliteStmt stmt(
        db,
        "INSERT INTO review_items("
        "id, path, path_key, kind, identity_source, identity_volume, "
        "identity_file_id_128, identity_file_index_64, object_available, "
        "object_kind, size_scope, logical_size, last_write_ticks, "
        "last_access_ticks, attributes, hist_agg_available, "
        "hist_agg_revalidated, hist_agg_recursive_size, "
        "hist_agg_newest_descendant_write, captured_safety, "
        "captured_reclaimability, captured_candidate_strength, "
        "class_category, class_confidence, class_rule_id, class_reason, "
        "source, source_root, index_age_ms, index_indexed_at_iso, added_at, "
        "reason_added, size_at_selection, last_write_time, legacy_attributes) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,"
        "?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,"
        "?34,?35);");
    stmt.bindInt64(1, static_cast<std::int64_t>(item.id));
    stmt.bindText16(2, item.path);
    stmt.bindText16(3, normalizeCleanupPath(item.path));
    stmt.bindText(4, toString(item.kind));
    bindIdentity(stmt, 5, item.objectEvidence.identity);
    stmt.bindInt64(9, item.objectEvidence.available ? 1 : 0);
    stmt.bindText(10, toString(item.objectEvidence.kind));
    stmt.bindText(11, toString(item.objectEvidence.sizeScope));
    stmt.bindInt64(12, static_cast<std::int64_t>(item.objectEvidence.logicalSize));
    stmt.bindInt64(13, static_cast<std::int64_t>(item.objectEvidence.lastWriteTime));
    stmt.bindInt64(14, static_cast<std::int64_t>(item.objectEvidence.lastAccessTime));
    stmt.bindInt64(15, static_cast<std::int64_t>(item.objectEvidence.attributes));
    stmt.bindInt64(16, item.historicalDirectoryAggregate.available ? 1 : 0);
    stmt.bindInt64(17, item.historicalDirectoryAggregate.revalidated ? 1 : 0);
    stmt.bindInt64(
        18,
        static_cast<std::int64_t>(
            item.historicalDirectoryAggregate.recursiveLogicalSize));
    stmt.bindInt64(
        19,
        static_cast<std::int64_t>(
            item.historicalDirectoryAggregate.newestDescendantWrite));
    stmt.bindText(20, toString(item.capturedSafety));
    stmt.bindText(21, toString(item.capturedReclaimability));
    stmt.bindText(22, toString(item.capturedCandidateStrength));
    stmt.bindText(23, toString(item.classification.category));
    stmt.bindText(24, toString(item.classification.confidence));
    stmt.bindText(25, item.classification.ruleId);
    stmt.bindText(26, item.classification.reason);
    stmt.bindText(27, item.source);
    stmt.bindText16(28, item.sourceRoot);
    stmt.bindInt64(29, static_cast<std::int64_t>(item.indexAgeMs));
    stmt.bindText(30, item.indexIndexedAtIso);
    stmt.bindInt64(31, static_cast<std::int64_t>(item.addedAt));
    stmt.bindText(32, item.reasonAdded);
    stmt.bindInt64(33, static_cast<std::int64_t>(item.sizeAtSelection));
    stmt.bindInt64(34, static_cast<std::int64_t>(item.lastWriteTime));
    stmt.bindInt64(35, static_cast<std::int64_t>(item.attributes));
    stmt.stepDone();
}

void insertValidation(SqliteDb& db, const CleanupCandidate& item)
{
    const auto& current = item.currentEvidence;
    SqliteStmt stmt(
        db,
        "INSERT INTO review_validation("
        "review_item_id, current_available, current_exists, "
        "current_object_available, current_identity_source, "
        "current_identity_volume, current_identity_file_id_128, "
        "current_identity_file_index_64, current_object_kind, "
        "current_size_scope, current_logical_size, current_last_write_ticks, "
        "current_last_access_ticks, current_attributes, current_agg_available, "
        "current_agg_revalidated, current_agg_recursive_size, "
        "current_agg_newest_descendant_write, current_safety, primary_state, "
        "reason_flags, diffs_json, object_identity_matched, "
        "direct_metadata_unchanged, recursive_evidence_revalidated, "
        "checked_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,"
        "?18,?19,?20,?21,?22,?23,?24,?25,?26);");
    stmt.bindInt64(1, static_cast<std::int64_t>(item.id));
    stmt.bindInt64(2, current.available ? 1 : 0);
    stmt.bindInt64(3, current.exists ? 1 : 0);
    stmt.bindInt64(4, current.objectEvidence.available ? 1 : 0);
    bindIdentity(stmt, 5, current.objectEvidence.identity);
    stmt.bindText(9, toString(current.objectEvidence.kind));
    stmt.bindText(10, toString(current.objectEvidence.sizeScope));
    stmt.bindInt64(11, static_cast<std::int64_t>(current.objectEvidence.logicalSize));
    stmt.bindInt64(12, static_cast<std::int64_t>(current.objectEvidence.lastWriteTime));
    stmt.bindInt64(13, static_cast<std::int64_t>(current.objectEvidence.lastAccessTime));
    stmt.bindInt64(14, static_cast<std::int64_t>(current.objectEvidence.attributes));
    stmt.bindInt64(15, current.directoryAggregate.available ? 1 : 0);
    stmt.bindInt64(16, current.directoryAggregate.revalidated ? 1 : 0);
    stmt.bindInt64(
        17,
        static_cast<std::int64_t>(current.directoryAggregate.recursiveLogicalSize));
    stmt.bindInt64(
        18,
        static_cast<std::int64_t>(current.directoryAggregate.newestDescendantWrite));
    stmt.bindText(19, toString(current.safety));
    stmt.bindText(20, toString(item.validation.state));
    stmt.bindInt64(21, static_cast<std::int64_t>(
                           static_cast<std::uint32_t>(item.validation.reasons)));
    stmt.bindText(22, diffsToJson(item.validation.diffs));
    stmt.bindInt64(23, item.validation.objectIdentityMatched ? 1 : 0);
    stmt.bindInt64(24, item.validation.directMetadataUnchanged ? 1 : 0);
    stmt.bindInt64(25, item.validation.recursiveEvidenceRevalidated ? 1 : 0);
    stmt.bindInt64(26, static_cast<std::int64_t>(item.validationCheckedAt));
    stmt.stepDone();
}

CleanupCandidate readItem(SqliteStmt& stmt)
{
    CleanupCandidate item;
    item.id = static_cast<std::uint64_t>(stmt.columnInt64(0));
    item.path = stmt.columnText16(1);
    item.kind = parseItemKind(stmt.columnText(3));
    item.objectEvidence.identity =
        readIdentity(stmt.columnText(4),
                     static_cast<std::uint64_t>(stmt.columnInt64(5)),
                     stmt.columnText(6),
                     static_cast<std::uint64_t>(stmt.columnInt64(7)));
    item.objectEvidence.available = stmt.columnInt64(8) != 0;
    item.objectEvidence.kind = parseItemKind(stmt.columnText(9));
    item.objectEvidence.sizeScope = parseScope(stmt.columnText(10));
    item.objectEvidence.logicalSize =
        static_cast<ByteSize>(stmt.columnInt64(11));
    item.objectEvidence.lastWriteTime =
        static_cast<FileTimeTicks>(stmt.columnInt64(12));
    item.objectEvidence.lastAccessTime =
        static_cast<FileTimeTicks>(stmt.columnInt64(13));
    item.objectEvidence.attributes =
        static_cast<std::uint32_t>(stmt.columnInt64(14));
    item.historicalDirectoryAggregate.available = stmt.columnInt64(15) != 0;
    item.historicalDirectoryAggregate.revalidated = stmt.columnInt64(16) != 0;
    item.historicalDirectoryAggregate.recursiveLogicalSize =
        static_cast<ByteSize>(stmt.columnInt64(17));
    item.historicalDirectoryAggregate.newestDescendantWrite =
        static_cast<FileTimeTicks>(stmt.columnInt64(18));
    item.capturedSafety = parseSafety(stmt.columnText(19));
    item.capturedReclaimability = parseReclaimability(stmt.columnText(20));
    item.capturedCandidateStrength = parseStrength(stmt.columnText(21));
    item.classification.category = parseStorageCategory(stmt.columnText(22));
    item.classification.confidence = parseConfidence(stmt.columnText(23));
    item.classification.ruleId = stmt.columnText(24);
    item.classification.reason = stmt.columnText(25);
    item.source = stmt.columnText(26);
    item.sourceRoot = stmt.columnText16(27);
    item.indexAgeMs = static_cast<std::uint64_t>(stmt.columnInt64(28));
    item.indexIndexedAtIso = stmt.columnText(29);
    item.addedAt = static_cast<FileTimeTicks>(stmt.columnInt64(30));
    item.reasonAdded = stmt.columnText(31);
    item.sizeAtSelection = static_cast<ByteSize>(stmt.columnInt64(32));
    item.lastWriteTime = static_cast<std::uint64_t>(stmt.columnInt64(33));
    item.attributes = static_cast<std::uint32_t>(stmt.columnInt64(34));
    return item;
}

void applyValidationRow(CleanupCandidate& item, SqliteStmt& stmt)
{
    auto& current = item.currentEvidence;
    current.available = stmt.columnInt64(1) != 0;
    current.exists = stmt.columnInt64(2) != 0;
    current.objectEvidence.available = stmt.columnInt64(3) != 0;
    current.objectEvidence.identity =
        readIdentity(stmt.columnText(4),
                     static_cast<std::uint64_t>(stmt.columnInt64(5)),
                     stmt.columnText(6),
                     static_cast<std::uint64_t>(stmt.columnInt64(7)));
    current.objectEvidence.kind = parseItemKind(stmt.columnText(8));
    current.objectEvidence.sizeScope = parseScope(stmt.columnText(9));
    current.objectEvidence.logicalSize =
        static_cast<ByteSize>(stmt.columnInt64(10));
    current.objectEvidence.lastWriteTime =
        static_cast<FileTimeTicks>(stmt.columnInt64(11));
    current.objectEvidence.lastAccessTime =
        static_cast<FileTimeTicks>(stmt.columnInt64(12));
    current.objectEvidence.attributes =
        static_cast<std::uint32_t>(stmt.columnInt64(13));
    current.directoryAggregate.available = stmt.columnInt64(14) != 0;
    current.directoryAggregate.revalidated = stmt.columnInt64(15) != 0;
    current.directoryAggregate.recursiveLogicalSize =
        static_cast<ByteSize>(stmt.columnInt64(16));
    current.directoryAggregate.newestDescendantWrite =
        static_cast<FileTimeTicks>(stmt.columnInt64(17));
    current.safety = parseSafety(stmt.columnText(18));
    item.validation.state = parseValidationState(stmt.columnText(19));
    if (item.validation.state == CleanupValidationState::AccessDenied) {
        current.observation = CleanupObservation::AccessDenied;
    } else if (item.validation.state == CleanupValidationState::ProbeError) {
        current.observation = CleanupObservation::ProbeError;
    } else if (current.available && !current.exists) {
        current.observation = CleanupObservation::Missing;
    } else if (current.available) {
        current.observation = CleanupObservation::Present;
    } else {
        current.observation = CleanupObservation::Unobserved;
    }
    item.validation.reasons =
        static_cast<CleanupValidationReason>(stmt.columnInt64(20));
    item.validation.diffs = diffsFromJson(stmt.columnText(21));
    item.validation.objectIdentityMatched = stmt.columnInt64(22) != 0;
    item.validation.directMetadataUnchanged = stmt.columnInt64(23) != 0;
    item.validation.recursiveEvidenceRevalidated = stmt.columnInt64(24) != 0;
    item.validationCheckedAt = static_cast<FileTimeTicks>(stmt.columnInt64(25));
}

CleanupReviewStatus mapWriteError(const SqliteError& ex)
{
    const std::string what = ex.what();
    const bool locked =
        what.find("locked") != std::string::npos ||
        what.find("busy") != std::string::npos ||
        what.find("SQLITE_BUSY") != std::string::npos ||
        what.find("SQLITE_LOCKED") != std::string::npos;
    if (locked) {
        return makeStatus(CleanupReviewError::LockFailed,
                          "Cleanup review database is locked.");
    }
    return makeStatus(CleanupReviewError::WriteFailed,
                      std::string("Failed to write cleanup review changes: ") +
                          what);
}

constexpr const char* kMaintenanceSql = R"SQL(
CREATE TABLE IF NOT EXISTS maintenance_operations (
  id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
  requested_at INTEGER NOT NULL DEFAULT 0,
  confirmed_at INTEGER NOT NULL DEFAULT 0,
  completed_at INTEGER NOT NULL DEFAULT 0,
  attempted INTEGER NOT NULL DEFAULT 0,
  recycled INTEGER NOT NULL DEFAULT 0,
  blocked INTEGER NOT NULL DEFAULT 0,
  cancelled INTEGER NOT NULL DEFAULT 0,
  failed INTEGER NOT NULL DEFAULT 0,
  recycled_logical_bytes INTEGER NOT NULL DEFAULT 0,
  unexpected_permanent_removal INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS maintenance_receipt_items (
  id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
  operation_id INTEGER NOT NULL,
  review_item_id INTEGER NOT NULL DEFAULT 0,
  path TEXT NOT NULL DEFAULT '',
  result TEXT NOT NULL,
  block_reason TEXT NOT NULL DEFAULT 'None',
  hresult INTEGER NOT NULL DEFAULT 0,
  native_error INTEGER NOT NULL DEFAULT 0,
  recycle_parsing_name TEXT NOT NULL DEFAULT '',
  detail TEXT NOT NULL DEFAULT '',
  identity_source TEXT NOT NULL DEFAULT 'Unavailable',
  identity_volume INTEGER NOT NULL DEFAULT 0,
  identity_file_id_128 TEXT NOT NULL DEFAULT ''
);
)SQL";

constexpr const char* kLocationSql = R"SQL(
CREATE TABLE IF NOT EXISTS ordinary_location_declarations (
  id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
  configured_path TEXT NOT NULL,
  path_key TEXT NOT NULL UNIQUE,
  created_at INTEGER NOT NULL DEFAULT 0,
  volume_serial INTEGER NOT NULL DEFAULT 0,
  volume_guid TEXT NOT NULL DEFAULT '',
  volume_root TEXT NOT NULL DEFAULT '',
  volume_available INTEGER NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'Invalid',
  detail TEXT NOT NULL DEFAULT ''
);
)SQL";

CleanupItemLifecycle parseLifecycle(std::string_view text)
{
    return text == "Recycled" ? CleanupItemLifecycle::Recycled
                              : CleanupItemLifecycle::Active;
}

void updateItemLifecycle(SqliteDb& db, const CleanupCandidate& item)
{
    if (!columnExists(db, "review_items", "lifecycle")) {
        return;
    }
    SqliteStmt stmt(db, "UPDATE review_items SET lifecycle = ?1 WHERE id = ?2;");
    stmt.bindText(1, toString(item.lifecycle));
    stmt.bindInt64(2, static_cast<std::int64_t>(item.id));
    stmt.stepDone();
}

void applyLifecycle(SqliteDb& db, std::vector<CleanupCandidate>& items)
{
    if (columnExists(db, "review_items", "lifecycle")) {
        SqliteStmt stmt(db, "SELECT id, lifecycle FROM review_items;");
        while (stmt.step()) {
            const auto id = static_cast<std::uint64_t>(stmt.columnInt64(0));
            const auto lifecycle = parseLifecycle(stmt.columnText(1));
            for (auto& item : items) {
                if (item.id == id) {
                    item.lifecycle = lifecycle;
                    break;
                }
            }
        }
    }
    if (!tableExists(db, "maintenance_receipt_items")) {
        return;
    }
    SqliteStmt stmt(
        db,
        "SELECT DISTINCT review_item_id FROM maintenance_receipt_items "
        "WHERE result = 'Recycled';");
    while (stmt.step()) {
        const auto id = static_cast<std::uint64_t>(stmt.columnInt64(0));
        for (auto& item : items) {
            if (item.id == id) {
                item.lifecycle = CleanupItemLifecycle::Recycled;
                break;
            }
        }
    }
}

}  // namespace

const char* toString(CleanupReviewError error) noexcept
{
    switch (error) {
    case CleanupReviewError::None:
        return "None";
    case CleanupReviewError::NotOpen:
        return "NotOpen";
    case CleanupReviewError::OpenFailed:
        return "OpenFailed";
    case CleanupReviewError::LockFailed:
        return "LockFailed";
    case CleanupReviewError::SchemaUnsupported:
        return "SchemaUnsupported";
    case CleanupReviewError::SchemaMalformed:
        return "SchemaMalformed";
    case CleanupReviewError::WriteFailed:
        return "WriteFailed";
    case CleanupReviewError::IoFailed:
        return "IoFailed";
    case CleanupReviewError::InvalidArgument:
        return "InvalidArgument";
    }
    return "None";
}

CleanupReviewStore::~CleanupReviewStore()
{
    close();
}

CleanupReviewStore::CleanupReviewStore(CleanupReviewStore&& other) noexcept
    : m_db(std::move(other.m_db))
    , m_path(std::move(other.m_path))
    , m_failNextWrite(other.m_failNextWrite)
    , m_failNextMaintenanceWrite(other.m_failNextMaintenanceWrite)
    , m_failNextMaintenanceRead(other.m_failNextMaintenanceRead)
{
    other.m_failNextWrite = false;
    other.m_failNextMaintenanceWrite = false;
    other.m_failNextMaintenanceRead = false;
}

CleanupReviewStore& CleanupReviewStore::operator=(
    CleanupReviewStore&& other) noexcept
{
    if (this != &other) {
        close();
        m_db = std::move(other.m_db);
        m_path = std::move(other.m_path);
        m_failNextWrite = other.m_failNextWrite;
        m_failNextMaintenanceWrite = other.m_failNextMaintenanceWrite;
        m_failNextMaintenanceRead = other.m_failNextMaintenanceRead;
        other.m_failNextWrite = false;
        other.m_failNextMaintenanceWrite = false;
        other.m_failNextMaintenanceRead = false;
    }
    return *this;
}

void CleanupReviewStore::close() noexcept
{
    m_db.close();
    m_path.clear();
    m_failNextWrite = false;
    m_failNextMaintenanceWrite = false;
    m_failNextMaintenanceRead = false;
}

bool CleanupReviewStore::isOpen() const noexcept
{
    return m_db.isOpen();
}

CleanupReviewStatus CleanupReviewStore::ensureSchema()
{
    try {
        const auto state = inspectSchema(m_db);
        if (state == SchemaState::Newer) {
            return makeStatus(
                CleanupReviewError::SchemaUnsupported,
                "Cleanup review database uses an unsupported newer schema.");
        }
        if (state == SchemaState::Malformed) {
            return makeStatus(
                CleanupReviewError::SchemaMalformed,
                "Cleanup review database schema is incomplete or malformed.");
        }
        if (state == SchemaState::Ready) {
            return {};
        }

        SqliteTxn txn(m_db);
        m_db.exec(kSchemaSql);
        upsertMeta(m_db, "review_schema_version",
                   std::to_string(kReviewSchemaVersion));
        upsertMeta(m_db, "review_next_id", "1");
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return makeStatus(
            CleanupReviewError::OpenFailed,
            std::string("Failed to open cleanup review database: ") + ex.what());
    }
}

CleanupReviewStatus CleanupReviewStore::open(const std::wstring& dbPath)
{
    close();
    if (dbPath.empty()) {
        return makeStatus(CleanupReviewError::OpenFailed,
                          "Cleanup review database path is empty.");
    }
    const auto parent = parentDirectory(dbPath);
    if (!parent.empty() && !ensureDirectory(parent)) {
        return makeStatus(CleanupReviewError::IoFailed,
                          "Failed to create cleanup review database directory.");
    }
    try {
        m_db = SqliteDb(dbPath, SqliteOpen::ReadWrite | SqliteOpen::Create);
    } catch (const SqliteError& ex) {
        return makeStatus(
            CleanupReviewError::OpenFailed,
            std::string("Failed to open cleanup review database: ") + ex.what());
    }

    auto schema = ensureSchema();
    if (!schema.ok) {
        m_db.close();
        return schema;
    }
    auto maintenance = ensureMaintenanceSchema();
    if (!maintenance.ok) {
        m_db.close();
        return maintenance;
    }
    auto locations = ensureLocationSchema();
    if (!locations.ok) {
        m_db.close();
        return locations;
    }
    m_path = dbPath;
    return {};
}

CleanupReviewStatus CleanupReviewStore::load(CleanupReview& out)
{
    if (!isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    try {
        // Deferred read transaction so items, validation, and nextId
        // come from one snapshot if another process commits mid-load.
        m_db.exec("BEGIN;");
        std::vector<CleanupCandidate> items;
        {
            SqliteStmt stmt(
                m_db,
                "SELECT id, path, path_key, kind, identity_source, "
                "identity_volume, identity_file_id_128, identity_file_index_64, "
                "object_available, object_kind, size_scope, logical_size, "
                "last_write_ticks, last_access_ticks, attributes, "
                "hist_agg_available, hist_agg_revalidated, "
                "hist_agg_recursive_size, hist_agg_newest_descendant_write, "
                "captured_safety, captured_reclaimability, "
                "captured_candidate_strength, class_category, class_confidence, "
                "class_rule_id, class_reason, source, source_root, index_age_ms, "
                "index_indexed_at_iso, added_at, reason_added, "
                "size_at_selection, last_write_time, legacy_attributes "
                "FROM review_items ORDER BY id;");
            while (stmt.step()) {
                items.push_back(readItem(stmt));
            }
        }
        {
            SqliteStmt stmt(
                m_db,
                "SELECT review_item_id, current_available, current_exists, "
                "current_object_available, current_identity_source, "
                "current_identity_volume, current_identity_file_id_128, "
                "current_identity_file_index_64, current_object_kind, "
                "current_size_scope, current_logical_size, "
                "current_last_write_ticks, current_last_access_ticks, "
                "current_attributes, current_agg_available, "
                "current_agg_revalidated, current_agg_recursive_size, "
                "current_agg_newest_descendant_write, current_safety, "
                "primary_state, reason_flags, diffs_json, "
                "object_identity_matched, direct_metadata_unchanged, "
                "recursive_evidence_revalidated, checked_at "
                "FROM review_validation;");
            while (stmt.step()) {
                const auto id = static_cast<std::uint64_t>(stmt.columnInt64(0));
                for (auto& item : items) {
                    if (item.id == id) {
                        applyValidationRow(item, stmt);
                        break;
                    }
                }
            }
        }
        applyLifecycle(m_db, items);
        out.resetTo(std::move(items), readNextId(m_db));
        m_db.exec("COMMIT;");
        return {};
    } catch (const SqliteError& ex) {
        try {
            m_db.exec("ROLLBACK;");
        } catch (...) {
        }
        return makeStatus(
            CleanupReviewError::OpenFailed,
            std::string("Failed to load cleanup review database: ") + ex.what());
    }
}

CleanupReviewStatus CleanupReviewStore::save(const CleanupReview& review)
{
    if (!isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    if (m_failNextWrite) {
        m_failNextWrite = false;
        return makeStatus(CleanupReviewError::WriteFailed,
                          "Failed to write cleanup review changes.");
    }
    try {
        SqliteTxn txn(m_db);
        {
            SqliteStmt delVal(m_db, "DELETE FROM review_validation;");
            delVal.stepDone();
            SqliteStmt delItems(m_db, "DELETE FROM review_items;");
            delItems.stepDone();
        }
        for (const auto& item : review.items()) {
            insertItem(m_db, item);
            insertValidation(m_db, item);
            updateItemLifecycle(m_db, item);
        }
        upsertMeta(m_db, "review_next_id", std::to_string(review.nextId()));
        upsertMeta(m_db, "review_schema_version",
                   std::to_string(kReviewSchemaVersion));
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return mapWriteError(ex);
    }
}

namespace {

std::optional<int> readMaintenanceSchemaVersion(SqliteDb& db)
{
    if (!tableExists(db, "meta")) {
        return std::nullopt;
    }
    SqliteStmt stmt(db, "SELECT value FROM meta WHERE key = ?1;");
    stmt.bindText(1, "maintenance_schema_version");
    if (!stmt.step()) {
        return std::nullopt;
    }
    try {
        return std::stoi(stmt.columnText(0));
    } catch (...) {
        return std::nullopt;
    }
}

void upsertReceiptItem(SqliteDb& db,
                       std::uint64_t operationId,
                       const MaintenanceItemReceipt& item)
{
    {
        SqliteStmt update(
            db,
            "UPDATE maintenance_receipt_items SET path = ?3, result = ?4, "
            "block_reason = ?5, hresult = ?6, native_error = ?7, "
            "recycle_parsing_name = ?8, detail = ?9, identity_source = ?10, "
            "identity_volume = ?11, identity_file_id_128 = ?12 "
            "WHERE operation_id = ?1 AND review_item_id = ?2;");
        update.bindInt64(1, static_cast<std::int64_t>(operationId));
        update.bindInt64(2, static_cast<std::int64_t>(item.reviewId));
        update.bindText16(3, item.path);
        update.bindText(4, toString(item.result));
        update.bindText(5, toString(item.blockReason));
        update.bindInt64(6, item.hresult);
        update.bindInt64(7, static_cast<std::int64_t>(item.nativeError));
        update.bindText(8, item.recycleParsingName);
        update.bindText(9, item.detail);
        update.bindText(10, toString(item.expectedIdentity.source));
        update.bindInt64(
            11, static_cast<std::int64_t>(item.expectedIdentity.volumeSerial));
        update.bindText(12, fileIdToHex(item.expectedIdentity.fileId128));
        update.stepDone();
        if (db.changes() > 0) {
            return;
        }
    }
    SqliteStmt insert(
        db,
        "INSERT INTO maintenance_receipt_items("
        "operation_id, review_item_id, path, result, block_reason, "
        "hresult, native_error, recycle_parsing_name, detail, "
        "identity_source, identity_volume, identity_file_id_128) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);");
    insert.bindInt64(1, static_cast<std::int64_t>(operationId));
    insert.bindInt64(2, static_cast<std::int64_t>(item.reviewId));
    insert.bindText16(3, item.path);
    insert.bindText(4, toString(item.result));
    insert.bindText(5, toString(item.blockReason));
    insert.bindInt64(6, item.hresult);
    insert.bindInt64(7, static_cast<std::int64_t>(item.nativeError));
    insert.bindText(8, item.recycleParsingName);
    insert.bindText(9, item.detail);
    insert.bindText(10, toString(item.expectedIdentity.source));
    insert.bindInt64(
        11, static_cast<std::int64_t>(item.expectedIdentity.volumeSerial));
    insert.bindText(12, fileIdToHex(item.expectedIdentity.fileId128));
    insert.stepDone();
}

void recountOperation(SqliteDb& db, std::uint64_t operationId)
{
    std::uint64_t attempted = 0;
    std::uint64_t recycled = 0;
    std::uint64_t blocked = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;
    std::uint64_t uncertain = 0;
    bool unexpected = false;
    SqliteStmt items(
        db,
        "SELECT result FROM maintenance_receipt_items WHERE operation_id = ?1;");
    items.bindInt64(1, static_cast<std::int64_t>(operationId));
    while (items.step()) {
        const auto result = parseMaintenanceItemResult(items.columnText(0));
        switch (result) {
        case MaintenanceItemResult::Recycled:
            ++attempted;
            ++recycled;
            break;
        case MaintenanceItemResult::BlockedPreflight:
        case MaintenanceItemResult::BlockedFinalGuard:
            ++blocked;
            if (result == MaintenanceItemResult::BlockedFinalGuard) {
                ++attempted;
            }
            break;
        case MaintenanceItemResult::Cancelled:
            ++cancelled;
            break;
        case MaintenanceItemResult::NotAttempted:
            break;
        case MaintenanceItemResult::Attempting:
            ++attempted;
            break;
        case MaintenanceItemResult::Uncertain:
            ++attempted;
            ++uncertain;
            break;
        case MaintenanceItemResult::UnexpectedPermanentRemoval:
            ++attempted;
            ++failed;
            unexpected = true;
            break;
        default:
            ++attempted;
            ++failed;
            break;
        }
    }

    const char* sql =
        columnExists(db, "maintenance_operations", "uncertain")
            ? "UPDATE maintenance_operations SET attempted = ?2, recycled = ?3, "
              "blocked = ?4, cancelled = ?5, failed = ?6, "
              "unexpected_permanent_removal = ?7, uncertain = ?8 "
              "WHERE id = ?1;"
            : "UPDATE maintenance_operations SET attempted = ?2, recycled = ?3, "
              "blocked = ?4, cancelled = ?5, failed = ?6, "
              "unexpected_permanent_removal = ?7 WHERE id = ?1;";
    SqliteStmt update(db, sql);
    update.bindInt64(1, static_cast<std::int64_t>(operationId));
    update.bindInt64(2, static_cast<std::int64_t>(attempted));
    update.bindInt64(3, static_cast<std::int64_t>(recycled));
    update.bindInt64(4, static_cast<std::int64_t>(blocked));
    update.bindInt64(5, static_cast<std::int64_t>(cancelled));
    update.bindInt64(6, static_cast<std::int64_t>(failed));
    update.bindInt64(7, unexpected ? 1 : 0);
    if (columnExists(db, "maintenance_operations", "uncertain")) {
        update.bindInt64(8, static_cast<std::int64_t>(uncertain));
    }
    update.stepDone();
}

MaintenanceOperationStatus statusFromItems(SqliteDb& db,
                                           std::uint64_t operationId)
{
    bool unexpected = false;
    bool uncertain = false;
    bool cancelled = false;
    bool attempting = false;
    SqliteStmt items(
        db,
        "SELECT result FROM maintenance_receipt_items WHERE operation_id = ?1;");
    items.bindInt64(1, static_cast<std::int64_t>(operationId));
    while (items.step()) {
        const auto result = parseMaintenanceItemResult(items.columnText(0));
        if (result == MaintenanceItemResult::UnexpectedPermanentRemoval) {
            unexpected = true;
        } else if (result == MaintenanceItemResult::Uncertain ||
                   result == MaintenanceItemResult::Attempting) {
            uncertain = true;
        } else if (result == MaintenanceItemResult::Cancelled) {
            cancelled = true;
        }
        if (result == MaintenanceItemResult::Attempting) {
            attempting = true;
        }
    }
    (void)attempting;
    if (unexpected) {
        return MaintenanceOperationStatus::HardStopped;
    }
    if (uncertain) {
        return MaintenanceOperationStatus::Uncertain;
    }
    if (cancelled) {
        return MaintenanceOperationStatus::Cancelled;
    }
    return MaintenanceOperationStatus::Completed;
}

}  // namespace

bool CleanupReviewStore::consumeMaintenanceWriteFailure()
{
    if (!m_failNextMaintenanceWrite) {
        return false;
    }
    m_failNextMaintenanceWrite = false;
    return true;
}

CleanupReviewStatus CleanupReviewStore::ensureMaintenanceSchema()
{
    try {
        const auto version = readMaintenanceSchemaVersion(m_db);
        if (version.has_value() && *version > kMaintenanceSchemaVersion) {
            return makeStatus(
                CleanupReviewError::SchemaUnsupported,
                "Cleanup review database uses an unsupported newer "
                "maintenance schema.");
        }

        SqliteTxn txn(m_db);
        m_db.exec(kMaintenanceSql);
        if (!columnExists(m_db, "review_items", "lifecycle")) {
            m_db.exec(
                "ALTER TABLE review_items ADD COLUMN lifecycle TEXT NOT NULL "
                "DEFAULT 'Active';");
        }
        if (!columnExists(m_db, "maintenance_operations", "status")) {
            m_db.exec(
                "ALTER TABLE maintenance_operations ADD COLUMN status TEXT "
                "NOT NULL DEFAULT 'Completed';");
        }
        if (!columnExists(m_db, "maintenance_operations", "uncertain")) {
            m_db.exec(
                "ALTER TABLE maintenance_operations ADD COLUMN uncertain "
                "INTEGER NOT NULL DEFAULT 0;");
        }
        if (!columnExists(m_db, "maintenance_operations", "selected_count")) {
            m_db.exec(
                "ALTER TABLE maintenance_operations ADD COLUMN selected_count "
                "INTEGER NOT NULL DEFAULT 0;");
        }
        if (!columnExists(m_db, "maintenance_operations", "eligible_count")) {
            m_db.exec(
                "ALTER TABLE maintenance_operations ADD COLUMN eligible_count "
                "INTEGER NOT NULL DEFAULT 0;");
        }
        if (!columnExists(m_db, "maintenance_operations",
                          "selected_logical_bytes")) {
            m_db.exec(
                "ALTER TABLE maintenance_operations ADD COLUMN "
                "selected_logical_bytes INTEGER NOT NULL DEFAULT 0;");
        }
        if (!columnExists(m_db, "maintenance_operations",
                          "eligible_logical_bytes")) {
            m_db.exec(
                "ALTER TABLE maintenance_operations ADD COLUMN "
                "eligible_logical_bytes INTEGER NOT NULL DEFAULT 0;");
        }
        upsertMeta(m_db, "maintenance_schema_version",
                   std::to_string(kMaintenanceSchemaVersion));
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return makeStatus(
            CleanupReviewError::OpenFailed,
            std::string("Failed to prepare maintenance tables: ") + ex.what());
    }
}

CleanupReviewStatus CleanupReviewStore::saveMaintenanceReceipt(
    MaintenanceReceipt& receipt)
{
    if (!isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    if (consumeMaintenanceWriteFailure()) {
        return makeStatus(CleanupReviewError::WriteFailed,
                          "Failed to persist the Recycle Bin receipt.");
    }
    try {
        SqliteTxn txn(m_db);
        {
            SqliteStmt stmt(
                m_db,
                "INSERT INTO maintenance_operations("
                "requested_at, confirmed_at, completed_at, attempted, recycled, "
                "blocked, cancelled, failed, recycled_logical_bytes, "
                "unexpected_permanent_removal, status, uncertain, "
                "selected_count, eligible_count, selected_logical_bytes, "
                "eligible_logical_bytes) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);");
            stmt.bindInt64(1, static_cast<std::int64_t>(receipt.requestedAt));
            stmt.bindInt64(2, static_cast<std::int64_t>(receipt.confirmedAt));
            stmt.bindInt64(3, static_cast<std::int64_t>(receipt.completedAt));
            stmt.bindInt64(4, static_cast<std::int64_t>(receipt.attempted));
            stmt.bindInt64(5, static_cast<std::int64_t>(receipt.recycled));
            stmt.bindInt64(6, static_cast<std::int64_t>(receipt.blocked));
            stmt.bindInt64(7, static_cast<std::int64_t>(receipt.cancelled));
            stmt.bindInt64(8, static_cast<std::int64_t>(receipt.failed));
            stmt.bindInt64(
                9, static_cast<std::int64_t>(receipt.recycledLogicalBytes));
            stmt.bindInt64(10, receipt.unexpectedPermanentRemoval ? 1 : 0);
            stmt.bindText(11, toString(receipt.status));
            stmt.bindInt64(12, static_cast<std::int64_t>(receipt.uncertain));
            stmt.bindInt64(13, static_cast<std::int64_t>(receipt.selectedCount));
            stmt.bindInt64(14, static_cast<std::int64_t>(receipt.eligibleCount));
            stmt.bindInt64(
                15, static_cast<std::int64_t>(receipt.selectedLogicalBytes));
            stmt.bindInt64(
                16, static_cast<std::int64_t>(receipt.eligibleLogicalBytes));
            stmt.stepDone();
        }
        receipt.operationId =
            static_cast<std::uint64_t>(m_db.lastInsertRowId());
        for (const auto& item : receipt.items) {
            SqliteStmt stmt(
                m_db,
                "INSERT INTO maintenance_receipt_items("
                "operation_id, review_item_id, path, result, block_reason, "
                "hresult, native_error, recycle_parsing_name, detail, "
                "identity_source, identity_volume, identity_file_id_128) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);");
            stmt.bindInt64(1, static_cast<std::int64_t>(receipt.operationId));
            stmt.bindInt64(2, static_cast<std::int64_t>(item.reviewId));
            stmt.bindText16(3, item.path);
            stmt.bindText(4, toString(item.result));
            stmt.bindText(5, toString(item.blockReason));
            stmt.bindInt64(6, item.hresult);
            stmt.bindInt64(7, static_cast<std::int64_t>(item.nativeError));
            stmt.bindText(8, item.recycleParsingName);
            stmt.bindText(9, item.detail);
            stmt.bindText(10, toString(item.expectedIdentity.source));
            stmt.bindInt64(
                11, static_cast<std::int64_t>(item.expectedIdentity.volumeSerial));
            stmt.bindText(12, fileIdToHex(item.expectedIdentity.fileId128));
            stmt.stepDone();
        }
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return mapWriteError(ex);
    }
}

CleanupReviewStatus CleanupReviewStore::beginMaintenanceOperation(
    MaintenanceReceipt& receipt)
{
    if (!isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    if (consumeMaintenanceWriteFailure()) {
        return makeStatus(CleanupReviewError::WriteFailed,
                          "Failed to begin the Recycle Bin operation.");
    }
    try {
        receipt.status = MaintenanceOperationStatus::Executing;
        SqliteTxn txn(m_db);
        SqliteStmt stmt(
            m_db,
            "INSERT INTO maintenance_operations("
            "requested_at, confirmed_at, completed_at, attempted, recycled, "
            "blocked, cancelled, failed, recycled_logical_bytes, "
            "unexpected_permanent_removal, status, uncertain, "
            "selected_count, eligible_count, selected_logical_bytes, "
            "eligible_logical_bytes) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);");
        stmt.bindInt64(1, static_cast<std::int64_t>(receipt.requestedAt));
        stmt.bindInt64(2, static_cast<std::int64_t>(receipt.confirmedAt));
        stmt.bindInt64(3, static_cast<std::int64_t>(receipt.completedAt));
        stmt.bindInt64(4, static_cast<std::int64_t>(receipt.attempted));
        stmt.bindInt64(5, static_cast<std::int64_t>(receipt.recycled));
        stmt.bindInt64(6, static_cast<std::int64_t>(receipt.blocked));
        stmt.bindInt64(7, static_cast<std::int64_t>(receipt.cancelled));
        stmt.bindInt64(8, static_cast<std::int64_t>(receipt.failed));
        stmt.bindInt64(9, static_cast<std::int64_t>(receipt.recycledLogicalBytes));
        stmt.bindInt64(10, receipt.unexpectedPermanentRemoval ? 1 : 0);
        stmt.bindText(11, toString(receipt.status));
        stmt.bindInt64(12, static_cast<std::int64_t>(receipt.uncertain));
        stmt.bindInt64(13, static_cast<std::int64_t>(receipt.selectedCount));
        stmt.bindInt64(14, static_cast<std::int64_t>(receipt.eligibleCount));
        stmt.bindInt64(15, static_cast<std::int64_t>(receipt.selectedLogicalBytes));
        stmt.bindInt64(16, static_cast<std::int64_t>(receipt.eligibleLogicalBytes));
        stmt.stepDone();
        receipt.operationId = static_cast<std::uint64_t>(m_db.lastInsertRowId());
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return mapWriteError(ex);
    }
}

CleanupReviewStatus CleanupReviewStore::checkpointMaintenanceItem(
    std::uint64_t operationId,
    const MaintenanceItemReceipt& item,
    ByteSize recycledLogicalBytes)
{
    if (!isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    if (consumeMaintenanceWriteFailure()) {
        return makeStatus(CleanupReviewError::WriteFailed,
                          "Failed to persist a Recycle Bin checkpoint.");
    }
    try {
        SqliteTxn txn(m_db);
        upsertReceiptItem(m_db, operationId, item);
        if (item.result == MaintenanceItemResult::Recycled &&
            columnExists(m_db, "review_items", "lifecycle") &&
            item.reviewId != 0) {
            SqliteStmt life(
                m_db, "UPDATE review_items SET lifecycle = ?1 WHERE id = ?2;");
            life.bindText(1, "Recycled");
            life.bindInt64(2, static_cast<std::int64_t>(item.reviewId));
            life.stepDone();
        }
        if (item.result == MaintenanceItemResult::Recycled &&
            recycledLogicalBytes > 0) {
            SqliteStmt bytes(
                m_db,
                "UPDATE maintenance_operations SET recycled_logical_bytes = "
                "recycled_logical_bytes + ?2 WHERE id = ?1;");
            bytes.bindInt64(1, static_cast<std::int64_t>(operationId));
            bytes.bindInt64(2, static_cast<std::int64_t>(recycledLogicalBytes));
            bytes.stepDone();
        }
        recountOperation(m_db, operationId);
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return mapWriteError(ex);
    }
}

CleanupReviewStatus CleanupReviewStore::completeMaintenanceOperation(
    const MaintenanceReceipt& receipt)
{
    if (!isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    if (consumeMaintenanceWriteFailure()) {
        return makeStatus(CleanupReviewError::WriteFailed,
                          "Failed to complete the Recycle Bin operation.");
    }
    try {
        SqliteTxn txn(m_db);
        SqliteStmt stmt(
            m_db,
            "UPDATE maintenance_operations SET requested_at = ?2, "
            "confirmed_at = ?3, completed_at = ?4, attempted = ?5, "
            "recycled = ?6, blocked = ?7, cancelled = ?8, failed = ?9, "
            "recycled_logical_bytes = ?10, unexpected_permanent_removal = ?11, "
            "status = ?12, uncertain = ?13, selected_count = ?14, "
            "eligible_count = ?15, selected_logical_bytes = ?16, "
            "eligible_logical_bytes = ?17 WHERE id = ?1;");
        stmt.bindInt64(1, static_cast<std::int64_t>(receipt.operationId));
        stmt.bindInt64(2, static_cast<std::int64_t>(receipt.requestedAt));
        stmt.bindInt64(3, static_cast<std::int64_t>(receipt.confirmedAt));
        stmt.bindInt64(4, static_cast<std::int64_t>(receipt.completedAt));
        stmt.bindInt64(5, static_cast<std::int64_t>(receipt.attempted));
        stmt.bindInt64(6, static_cast<std::int64_t>(receipt.recycled));
        stmt.bindInt64(7, static_cast<std::int64_t>(receipt.blocked));
        stmt.bindInt64(8, static_cast<std::int64_t>(receipt.cancelled));
        stmt.bindInt64(9, static_cast<std::int64_t>(receipt.failed));
        stmt.bindInt64(10, static_cast<std::int64_t>(receipt.recycledLogicalBytes));
        stmt.bindInt64(11, receipt.unexpectedPermanentRemoval ? 1 : 0);
        stmt.bindText(12, toString(receipt.status));
        stmt.bindInt64(13, static_cast<std::int64_t>(receipt.uncertain));
        stmt.bindInt64(14, static_cast<std::int64_t>(receipt.selectedCount));
        stmt.bindInt64(15, static_cast<std::int64_t>(receipt.eligibleCount));
        stmt.bindInt64(16, static_cast<std::int64_t>(receipt.selectedLogicalBytes));
        stmt.bindInt64(17, static_cast<std::int64_t>(receipt.eligibleLogicalBytes));
        stmt.stepDone();
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return mapWriteError(ex);
    }
}

CleanupReviewStatus CleanupReviewStore::reconcileIncompleteMaintenance()
{
    if (!isOpen() || !tableExists(m_db, "maintenance_operations")) {
        return {};
    }
    try {
        SqliteTxn txn(m_db);
        if (tableExists(m_db, "maintenance_receipt_items")) {
            SqliteStmt attempting(
                m_db,
                "UPDATE maintenance_receipt_items SET result = 'Uncertain', "
                "detail = CASE WHEN detail = '' OR detail = 'Recycle in progress' "
                "THEN 'Interrupted while recycling; outcome is Uncertain' "
                "ELSE detail END WHERE result = 'Attempting';");
            attempting.stepDone();
        }
        if (columnExists(m_db, "maintenance_operations", "status")) {
            std::vector<std::uint64_t> executing;
            {
                SqliteStmt ops(
                    m_db,
                    "SELECT id FROM maintenance_operations "
                    "WHERE status = 'Executing';");
                while (ops.step()) {
                    executing.push_back(
                        static_cast<std::uint64_t>(ops.columnInt64(0)));
                }
            }
            for (const auto id : executing) {
                recountOperation(m_db, id);
                const auto status = statusFromItems(m_db, id);
                SqliteStmt update(
                    m_db,
                    "UPDATE maintenance_operations SET status = ?2 WHERE id = ?1;");
                update.bindInt64(1, static_cast<std::int64_t>(id));
                update.bindText(2, toString(status));
                update.stepDone();
            }
        }
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return mapWriteError(ex);
    }
}

CleanupReviewStatus CleanupReviewStore::loadMaintenanceReceipts(
    std::vector<MaintenanceReceipt>& out)
{
    out.clear();
    if (!isOpen() || !tableExists(m_db, "maintenance_operations")) {
        return {};
    }
    if (m_failNextMaintenanceRead) {
        m_failNextMaintenanceRead = false;
        return makeStatus(CleanupReviewError::IoFailed,
                          "Failed to read Recycle Bin history.");
    }
    try {
        const bool hasStatus =
            columnExists(m_db, "maintenance_operations", "status");
        const bool hasUncertain =
            columnExists(m_db, "maintenance_operations", "uncertain");
        const bool hasSelected =
            columnExists(m_db, "maintenance_operations", "selected_count");
        SqliteStmt stmt(
            m_db,
            hasStatus
                ? "SELECT id, requested_at, confirmed_at, completed_at, "
                  "attempted, recycled, blocked, cancelled, failed, "
                  "recycled_logical_bytes, unexpected_permanent_removal, "
                  "status, uncertain, selected_count, eligible_count, "
                  "selected_logical_bytes, eligible_logical_bytes "
                  "FROM maintenance_operations ORDER BY id;"
                : "SELECT id, requested_at, confirmed_at, completed_at, "
                  "attempted, recycled, blocked, cancelled, failed, "
                  "recycled_logical_bytes, unexpected_permanent_removal "
                  "FROM maintenance_operations ORDER BY id;");
        while (stmt.step()) {
            MaintenanceReceipt receipt;
            receipt.operationId =
                static_cast<std::uint64_t>(stmt.columnInt64(0));
            receipt.requestedAt =
                static_cast<FileTimeTicks>(stmt.columnInt64(1));
            receipt.confirmedAt =
                static_cast<FileTimeTicks>(stmt.columnInt64(2));
            receipt.completedAt =
                static_cast<FileTimeTicks>(stmt.columnInt64(3));
            receipt.attempted =
                static_cast<std::uint64_t>(stmt.columnInt64(4));
            receipt.recycled = static_cast<std::uint64_t>(stmt.columnInt64(5));
            receipt.blocked = static_cast<std::uint64_t>(stmt.columnInt64(6));
            receipt.cancelled = static_cast<std::uint64_t>(stmt.columnInt64(7));
            receipt.failed = static_cast<std::uint64_t>(stmt.columnInt64(8));
            receipt.recycledLogicalBytes =
                static_cast<ByteSize>(stmt.columnInt64(9));
            receipt.unexpectedPermanentRemoval = stmt.columnInt64(10) != 0;
            if (hasStatus) {
                receipt.status =
                    parseMaintenanceOperationStatus(stmt.columnText(11));
                if (hasUncertain) {
                    receipt.uncertain =
                        static_cast<std::uint64_t>(stmt.columnInt64(12));
                }
                if (hasSelected) {
                    receipt.selectedCount =
                        static_cast<std::uint64_t>(stmt.columnInt64(13));
                    receipt.eligibleCount =
                        static_cast<std::uint64_t>(stmt.columnInt64(14));
                    receipt.selectedLogicalBytes =
                        static_cast<ByteSize>(stmt.columnInt64(15));
                    receipt.eligibleLogicalBytes =
                        static_cast<ByteSize>(stmt.columnInt64(16));
                }
            }
            out.push_back(std::move(receipt));
        }
        SqliteStmt items(
            m_db,
            "SELECT operation_id, review_item_id, path, result, block_reason, "
            "hresult, native_error, recycle_parsing_name, detail, "
            "identity_source, identity_volume, identity_file_id_128 "
            "FROM maintenance_receipt_items ORDER BY id;");
        while (items.step()) {
            const auto operationId =
                static_cast<std::uint64_t>(items.columnInt64(0));
            MaintenanceItemReceipt row;
            row.reviewId = static_cast<std::uint64_t>(items.columnInt64(1));
            row.path = items.columnText16(2);
            row.result = parseMaintenanceItemResult(items.columnText(3));
            row.blockReason = parseMaintenanceBlockReason(items.columnText(4));
            row.hresult = static_cast<std::int32_t>(items.columnInt64(5));
            row.nativeError = static_cast<std::uint32_t>(items.columnInt64(6));
            row.recycleParsingName = items.columnText(7);
            row.detail = items.columnText(8);
            row.expectedIdentity.source = parseIdentitySource(items.columnText(9));
            row.expectedIdentity.volumeSerial =
                static_cast<std::uint64_t>(items.columnInt64(10));
            row.expectedIdentity.fileId128 = hexToFileId(items.columnText(11));
            for (auto& receipt : out) {
                if (receipt.operationId == operationId) {
                    receipt.items.push_back(std::move(row));
                    break;
                }
            }
        }
        return {};
    } catch (const SqliteError& ex) {
        out.clear();
        const auto status = mapWriteError(ex);
        if (status.error == CleanupReviewError::LockFailed) {
            return status;
        }
        return makeStatus(CleanupReviewError::IoFailed,
                          "Failed to read Recycle Bin history.");
    }
}

CleanupReviewStatus CleanupReviewController::open(const std::wstring& dbPath)
{
    CleanupReviewStore candidate;
    auto status = candidate.open(dbPath);
    if (!status.ok) {
        return status;
    }
    status = candidate.reconcileIncompleteMaintenance();
    if (!status.ok) {
        return status;
    }
    CleanupReview loaded;
    status = candidate.load(loaded);
    if (!status.ok) {
        return status;
    }
    m_store = std::move(candidate);
    m_review = std::move(loaded);
    return {};
}

CleanupReviewStatus CleanupReviewController::openDefault()
{
    return open(spaceLensReviewStatePath());
}

void CleanupReviewController::close() noexcept
{
    m_store.close();
}

template <typename Mutator>
CleanupReviewStatus CleanupReviewController::commit(Mutator&& mutator)
{
    if (!m_store.isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    CleanupReview draft = m_review;
    auto status = mutator(draft);
    if (!status.ok || !status.changed) {
        return status;
    }
    const auto saved = m_store.save(draft);
    if (!saved.ok) {
        return saved;
    }
    m_review = std::move(draft);
    return status;
}

CleanupReviewStatus CleanupReviewController::add(CleanupCandidate candidate)
{
    return addDetailed(std::move(candidate));
}

CleanupReviewStatus CleanupReviewController::addDetailed(
    CleanupCandidate candidate)
{
    if (m_mutationsBlocked) {
        return makeStatus(
            CleanupReviewError::InvalidArgument,
            "Cleanup review is being revalidated. Wait or cancel first.");
    }
    return commit([&](CleanupReview& draft) {
        CleanupReviewStatus status;
        status.add = draft.addDetailed(std::move(candidate));
        status.id = status.add.id;
        status.changed = status.add.accepted();
        return status;
    });
}

CleanupReviewStatus CleanupReviewController::removeById(std::uint64_t id)
{
    if (m_mutationsBlocked) {
        return makeStatus(
            CleanupReviewError::InvalidArgument,
            "Cleanup review is being revalidated. Wait or cancel first.");
    }
    return commit([&](CleanupReview& draft) {
        CleanupReviewStatus status;
        status.changed = draft.removeById(id);
        status.id = id;
        return status;
    });
}

CleanupReviewStatus CleanupReviewController::removeByPath(std::wstring_view path)
{
    if (m_mutationsBlocked) {
        return makeStatus(
            CleanupReviewError::InvalidArgument,
            "Cleanup review is being revalidated. Wait or cancel first.");
    }
    return commit([&](CleanupReview& draft) {
        CleanupReviewStatus status;
        status.changed = draft.removeByPath(path);
        return status;
    });
}

CleanupReviewStatus CleanupReviewController::clear()
{
    if (m_mutationsBlocked) {
        return makeStatus(
            CleanupReviewError::InvalidArgument,
            "Cleanup review is being revalidated. Wait or cancel first.");
    }
    return commit([](CleanupReview& draft) {
        CleanupReviewStatus status;
        status.changed = !draft.empty();
        draft.clear();
        return status;
    });
}

CleanupReviewStatus CleanupReviewController::refreshEvidence(std::uint64_t id)
{
    if (m_mutationsBlocked) {
        return makeStatus(
            CleanupReviewError::InvalidArgument,
            "Cleanup review is being revalidated. Wait or cancel first.");
    }
    return commit([&](CleanupReview& draft) {
        CleanupReviewStatus status;
        status.id = id;
        if (!draft.findById(id)) {
            status.ok = false;
            status.error = CleanupReviewError::InvalidArgument;
            status.message = "Cleanup review item was not found.";
            return status;
        }
        status.changed = draft.refreshEvidence(id);
        return status;
    });
}

CleanupReviewStatus CleanupReviewController::replaceValidation(
    std::uint64_t id,
    CleanupCurrentEvidence current,
    FileTimeTicks checkedAt)
{
    CleanupValidationReplacement update;
    update.id = id;
    update.current = std::move(current);
    update.checkedAt = checkedAt;
    return replaceValidationBatch({std::move(update)});
}

CleanupReviewStatus CleanupReviewController::replaceValidationBatch(
    const std::vector<CleanupValidationReplacement>& updates)
{
    return commit([&](CleanupReview& draft) {
        CleanupReviewStatus status;
        if (updates.empty()) {
            return status;
        }
        if (!draft.replaceValidationBatch(updates)) {
            status.ok = false;
            status.error = CleanupReviewError::InvalidArgument;
            status.message =
                "Cleanup review item was not found or no longer matches "
                "the revalidation snapshot.";
            return status;
        }
        status.changed = true;
        return status;
    });
}

CleanupReviewStatus CleanupReviewController::recordMaintenance(
    MaintenanceReceipt receipt)
{
    if (!m_store.isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    auto persist = m_store.saveMaintenanceReceipt(receipt);
    if (!persist.ok) {
        return persist;
    }
    persist.id = receipt.operationId;
    const auto lifecycle = commit([&](CleanupReview& draft) {
        CleanupReviewStatus status;
        status.id = receipt.operationId;
        for (const auto& item : receipt.items) {
            if (item.result == MaintenanceItemResult::Recycled &&
                draft.setLifecycle(item.reviewId,
                                   CleanupItemLifecycle::Recycled)) {
                status.changed = true;
            }
        }
        return status;
    });
    if (!lifecycle.ok) {
        return lifecycle;
    }
    persist.changed = lifecycle.changed;
    return persist;
}

CleanupReviewStatus CleanupReviewController::beginMaintenance(
    MaintenanceReceipt& receipt)
{
    if (!m_store.isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    return m_store.beginMaintenanceOperation(receipt);
}

CleanupReviewStatus CleanupReviewController::checkpointMaintenance(
    std::uint64_t operationId,
    const MaintenanceItemReceipt& item,
    ByteSize recycledLogicalBytes)
{
    if (!m_store.isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    auto status =
        m_store.checkpointMaintenanceItem(operationId, item, recycledLogicalBytes);
    if (!status.ok) {
        return status;
    }
    if (item.result == MaintenanceItemResult::Recycled) {
        (void)m_review.setLifecycle(item.reviewId, CleanupItemLifecycle::Recycled);
    }
    return status;
}

CleanupReviewStatus CleanupReviewController::completeMaintenance(
    const MaintenanceReceipt& receipt)
{
    if (!m_store.isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    return m_store.completeMaintenanceOperation(receipt);
}

CleanupReviewStatus CleanupReviewController::loadMaintenanceReceipts(
    std::vector<MaintenanceReceipt>& out)
{
    return m_store.loadMaintenanceReceipts(out);
}

std::vector<MaintenanceReceipt> CleanupReviewController::maintenanceReceipts()
{
    std::vector<MaintenanceReceipt> out;
    (void)m_store.loadMaintenanceReceipts(out);
    return out;
}

OrdinaryLocationStatus parseLocationStatus(std::string_view text)
{
    if (text == "Active") {
        return OrdinaryLocationStatus::Active;
    }
    if (text == "VolumeMismatch") {
        return OrdinaryLocationStatus::VolumeMismatch;
    }
    if (text == "VolumeUnavailable") {
        return OrdinaryLocationStatus::VolumeUnavailable;
    }
    if (text == "PathUnavailable") {
        return OrdinaryLocationStatus::PathUnavailable;
    }
    return OrdinaryLocationStatus::Invalid;
}

OrdinaryLocationDeclaration readLocationRow(SqliteStmt& stmt)
{
    OrdinaryLocationDeclaration row;
    row.id = static_cast<std::uint64_t>(stmt.columnInt64(0));
    row.configuredPath = stmt.columnText16(1);
    row.normalizedPathKey = stmt.columnText16(2);
    row.createdAt = static_cast<FileTimeTicks>(stmt.columnInt64(3));
    row.volume.serial = static_cast<std::uint32_t>(stmt.columnInt64(4));
    row.volume.guid = stmt.columnText16(5);
    row.volume.rootPath = stmt.columnText16(6);
    row.volume.available = stmt.columnInt64(7) != 0;
    row.status = parseLocationStatus(stmt.columnText(8));
    row.detail = stmt.columnText(9);
    return row;
}

std::uint64_t readLocationGeneration(SqliteDb& db)
{
    SqliteStmt stmt(db, "SELECT value FROM meta WHERE key = ?1;");
    stmt.bindText(1, "location_declaration_generation");
    if (!stmt.step()) {
        return 0;
    }
    try {
        return std::stoull(stmt.columnText(0));
    } catch (...) {
        return 0;
    }
}

CleanupReviewStatus CleanupReviewStore::ensureLocationSchema()
{
    try {
        SqliteTxn txn(m_db);
        m_db.exec(kLocationSql);
        upsertMeta(m_db, "location_schema_version",
                   std::to_string(kLocationSchemaVersion));
        SqliteStmt existing(m_db,
                            "SELECT value FROM meta WHERE key = ?1;");
        existing.bindText(1, "location_declaration_generation");
        if (!existing.step()) {
            upsertMeta(m_db, "location_declaration_generation", "0");
        }
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return makeStatus(
            CleanupReviewError::OpenFailed,
            std::string("Failed to prepare location declaration tables: ") +
                ex.what());
    }
}

OrdinaryLocationAddOutcome CleanupReviewStore::addOrdinaryLocation(
    OrdinaryLocationDeclaration declaration)
{
    OrdinaryLocationAddOutcome out;
    if (!isOpen()) {
        out.result = OrdinaryLocationAddResult::Error;
        out.message = "Cleanup review database is not open.";
        return out;
    }
    if (declaration.normalizedPathKey.empty()) {
        out.result = OrdinaryLocationAddResult::InvalidPath;
        out.message = "Declaration path key is empty.";
        return out;
    }
    try {
        {
            SqliteStmt existing(
                m_db,
                "SELECT id FROM ordinary_location_declarations "
                "WHERE path_key = ?1;");
            existing.bindText16(1, declaration.normalizedPathKey);
            if (existing.step()) {
                out.result = OrdinaryLocationAddResult::AlreadyExists;
                out.declaration = declaration;
                out.declaration.id =
                    static_cast<std::uint64_t>(existing.columnInt64(0));
                out.message = "This ordinary location is already declared.";
                return out;
            }
        }
        SqliteTxn txn(m_db);
        {
            SqliteStmt stmt(
                m_db,
                "INSERT INTO ordinary_location_declarations("
                "configured_path, path_key, created_at, volume_serial, "
                "volume_guid, volume_root, volume_available, status, detail) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);");
            stmt.bindText16(1, declaration.configuredPath);
            stmt.bindText16(2, declaration.normalizedPathKey);
            stmt.bindInt64(3, static_cast<std::int64_t>(declaration.createdAt));
            stmt.bindInt64(4, static_cast<std::int64_t>(declaration.volume.serial));
            stmt.bindText16(5, declaration.volume.guid);
            stmt.bindText16(6, declaration.volume.rootPath);
            stmt.bindInt64(7, declaration.volume.available ? 1 : 0);
            stmt.bindText(8, toString(declaration.status));
            stmt.bindText(9, declaration.detail);
            stmt.stepDone();
        }
        declaration.id =
            static_cast<std::uint64_t>(m_db.lastInsertRowId());
        const auto nextGen = readLocationGeneration(m_db) + 1;
        upsertMeta(m_db, "location_declaration_generation",
                   std::to_string(nextGen));
        txn.commit();
        out.result = OrdinaryLocationAddResult::Added;
        out.declaration = std::move(declaration);
        return out;
    } catch (const SqliteError& ex) {
        out.result = OrdinaryLocationAddResult::Error;
        out.message = ex.what();
        return out;
    }
}

CleanupReviewStatus CleanupReviewStore::removeOrdinaryLocation(std::uint64_t id)
{
    if (!isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    try {
        SqliteTxn txn(m_db);
        SqliteStmt stmt(
            m_db, "DELETE FROM ordinary_location_declarations WHERE id = ?1;");
        stmt.bindInt64(1, static_cast<std::int64_t>(id));
        stmt.stepDone();
        if (m_db.changes() == 0) {
            txn.rollback();
            return makeStatus(CleanupReviewError::InvalidArgument,
                              "Ordinary location declaration was not found.");
        }
        const auto nextGen = readLocationGeneration(m_db) + 1;
        upsertMeta(m_db, "location_declaration_generation",
                   std::to_string(nextGen));
        txn.commit();
        CleanupReviewStatus status;
        status.changed = true;
        status.id = id;
        return status;
    } catch (const SqliteError& ex) {
        return mapWriteError(ex);
    }
}

std::vector<OrdinaryLocationDeclaration>
CleanupReviewStore::loadOrdinaryLocations()
{
    std::vector<OrdinaryLocationDeclaration> out;
    if (!isOpen() || !tableExists(m_db, "ordinary_location_declarations")) {
        return out;
    }
    try {
        SqliteStmt stmt(
            m_db,
            "SELECT id, configured_path, path_key, created_at, volume_serial, "
            "volume_guid, volume_root, volume_available, status, detail "
            "FROM ordinary_location_declarations ORDER BY id;");
        while (stmt.step()) {
            out.push_back(readLocationRow(stmt));
        }
    } catch (const SqliteError&) {
        return {};
    }
    return out;
}

OrdinaryLocationPolicy CleanupReviewStore::loadOrdinaryLocationPolicy()
{
    OrdinaryLocationPolicy policy;
    if (!isOpen()) {
        return policy;
    }
    try {
        policy.generation = readLocationGeneration(m_db);
    } catch (const SqliteError&) {
        policy.generation = 0;
    }
    policy.declarations = loadOrdinaryLocations();
    return policy;
}

CleanupReviewStatus CleanupReviewStore::saveOrdinaryLocationStatuses(
    const std::vector<OrdinaryLocationDeclaration>& declarations)
{
    if (!isOpen()) {
        return makeStatus(CleanupReviewError::NotOpen,
                          "Cleanup review database is not open.");
    }
    try {
        SqliteTxn txn(m_db);
        for (const auto& declaration : declarations) {
            if (declaration.normalizedPathKey.empty()) {
                txn.rollback();
                return makeStatus(CleanupReviewError::InvalidArgument,
                                  "Ordinary location path key is missing.");
            }
            SqliteStmt stmt(
                m_db,
                "UPDATE ordinary_location_declarations SET status = ?1, "
                "detail = ?2 WHERE id = ?3 OR path_key = ?4;");
            stmt.bindText(1, toString(declaration.status));
            stmt.bindText(2, declaration.detail);
            stmt.bindInt64(3, static_cast<std::int64_t>(declaration.id));
            stmt.bindText16(4, declaration.normalizedPathKey);
            stmt.stepDone();
            if (m_db.changes() == 0) {
                txn.rollback();
                return makeStatus(
                    CleanupReviewError::InvalidArgument,
                    "Ordinary location declaration was not found.");
            }
        }
        txn.commit();
        return {};
    } catch (const SqliteError& ex) {
        return mapWriteError(ex);
    }
}

OrdinaryLocationAddOutcome CleanupReviewController::addOrdinaryLocation(
    std::wstring path,
    ICleanupMetadataReader& rootProbe,
    IVolumeIdentityReader& volumes,
    FileTimeTicks createdAt)
{
    OrdinaryLocationAddOutcome out;
    if (m_mutationsBlocked) {
        out.result = OrdinaryLocationAddResult::Error;
        out.message =
            "Location declarations cannot change while review or "
            "maintenance is running.";
        return out;
    }
    if (!m_store.isOpen()) {
        out.result = OrdinaryLocationAddResult::Error;
        out.message = "Cleanup review database is not open.";
        return out;
    }
    const auto key = normalizeOrdinaryLocationPath(path);
    if (!key.empty()) {
        for (const auto& existing : m_store.loadOrdinaryLocations()) {
            if (existing.normalizedPathKey == key) {
                out.result = OrdinaryLocationAddResult::AlreadyExists;
                out.declaration = existing;
                out.message = "This ordinary location is already declared.";
                return out;
            }
        }
    }
    auto evaluated = evaluateOrdinaryLocationDeclaration(
        path, rootProbe, volumes, createdAt);
    if (evaluated.result != OrdinaryLocationAddResult::Added &&
        evaluated.result != OrdinaryLocationAddResult::VolumeUnavailable) {
        return evaluated;
    }
    auto persisted = m_store.addOrdinaryLocation(evaluated.declaration);
    if (persisted.result == OrdinaryLocationAddResult::AlreadyExists ||
        persisted.result == OrdinaryLocationAddResult::Error) {
        return persisted;
    }
    persisted.result = evaluated.result;
    persisted.message = evaluated.message;
    return persisted;
}

CleanupReviewStatus CleanupReviewController::removeOrdinaryLocation(
    std::uint64_t id)
{
    if (m_mutationsBlocked) {
        return makeStatus(
            CleanupReviewError::InvalidArgument,
            "Location declarations cannot change while review or "
            "maintenance is running.");
    }
    return m_store.removeOrdinaryLocation(id);
}

OrdinaryLocationPolicy CleanupReviewController::ordinaryLocationPolicy()
{
    return m_store.loadOrdinaryLocationPolicy();
}

OrdinaryLocationPolicy CleanupReviewController::refreshOrdinaryLocations(
    ICleanupMetadataReader& rootProbe,
    IVolumeIdentityReader& volumes)
{
    auto policy = m_store.loadOrdinaryLocationPolicy();
    refreshOrdinaryLocationPolicy(policy, rootProbe, volumes);
    if (const auto status =
            m_store.saveOrdinaryLocationStatuses(policy.declarations);
        !status.ok) {
        // Live classification still uses the refreshed snapshot.
    }
    return policy;
}

}  // namespace spacelens
