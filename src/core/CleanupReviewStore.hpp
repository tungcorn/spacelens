#pragma once

#include "core/CleanupPlan.hpp"
#include "core/CleanupReview.hpp"
#include "core/index/Sqlite.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

/// Independent of kIndexSchemaVersion. Stored as meta.review_schema_version.
inline constexpr int kReviewSchemaVersion = 1;

enum class CleanupReviewError {
    None,
    NotOpen,
    OpenFailed,
    LockFailed,
    SchemaUnsupported,
    SchemaMalformed,
    WriteFailed,
    IoFailed,
    InvalidArgument
};

[[nodiscard]] const char* toString(CleanupReviewError error) noexcept;

struct CleanupReviewStatus {
    bool ok = true;
    CleanupReviewError error = CleanupReviewError::None;
    std::string message;
    CleanupAddOutcome add{};
    bool changed = false;
    std::uint64_t id = 0;

    explicit operator bool() const noexcept { return ok; }
};

/// SQLite persistence for Cleanup Review. Path is injected for tests; the
/// product default is spaceLensReviewStatePath() (%LOCALAPPDATA%\SpaceLens\state.db).
/// Never references indexes/*/index.db.
class CleanupReviewStore {
public:
    CleanupReviewStore() = default;
    ~CleanupReviewStore();

    CleanupReviewStore(const CleanupReviewStore&) = delete;
    CleanupReviewStore& operator=(const CleanupReviewStore&) = delete;
    CleanupReviewStore(CleanupReviewStore&& other) noexcept;
    CleanupReviewStore& operator=(CleanupReviewStore&& other) noexcept;

    [[nodiscard]] CleanupReviewStatus open(const std::wstring& dbPath);
    void close() noexcept;

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const std::wstring& path() const noexcept { return m_path; }

    [[nodiscard]] CleanupReviewStatus load(CleanupReview& out);
    [[nodiscard]] CleanupReviewStatus save(const CleanupReview& review);

    /// Test hook: next save() fails and rolls back without writing.
    void failNextWrite() noexcept { m_failNextWrite = true; }

private:
    [[nodiscard]] CleanupReviewStatus ensureSchema();

    SqliteDb m_db;
    std::wstring m_path;
    bool m_failNextWrite = false;
};

/// Durable facade: mutate a draft CleanupReview, persist the whole logical
/// operation in one transaction, then swap memory only after commit.
class CleanupReviewController {
public:
    CleanupReviewController() = default;

    CleanupReviewController(const CleanupReviewController&) = delete;
    CleanupReviewController& operator=(const CleanupReviewController&) = delete;
    CleanupReviewController(CleanupReviewController&&) noexcept = default;
    CleanupReviewController& operator=(CleanupReviewController&&) noexcept = default;

    [[nodiscard]] CleanupReviewStatus open(const std::wstring& dbPath);
    [[nodiscard]] CleanupReviewStatus openDefault();
    void close() noexcept;

    [[nodiscard]] bool isOpen() const noexcept { return m_store.isOpen(); }
    [[nodiscard]] const std::wstring& path() const noexcept { return m_store.path(); }

    [[nodiscard]] const CleanupReview& review() const noexcept { return m_review; }
    [[nodiscard]] std::string copyReport() const { return m_review.copyReport(); }
    [[nodiscard]] CleanupPlan buildCleanupPlan(
        const CleanupPlanOptions& options = {}) const
    {
        return spacelens::buildCleanupPlan(m_review, options);
    }

    [[nodiscard]] CleanupReviewStatus add(CleanupCandidate candidate);
    [[nodiscard]] CleanupReviewStatus addDetailed(CleanupCandidate candidate);
    [[nodiscard]] CleanupReviewStatus removeById(std::uint64_t id);
    [[nodiscard]] CleanupReviewStatus removeByPath(std::wstring_view path);
    [[nodiscard]] CleanupReviewStatus clear();
    [[nodiscard]] CleanupReviewStatus refreshEvidence(std::uint64_t id);
    [[nodiscard]] CleanupReviewStatus replaceValidation(
        std::uint64_t id,
        CleanupCurrentEvidence current,
        FileTimeTicks checkedAt = 0);
    [[nodiscard]] CleanupReviewStatus replaceValidationBatch(
        const std::vector<CleanupValidationReplacement>& updates);

    void failNextWrite() noexcept { m_store.failNextWrite(); }

private:
    template <typename Mutator>
    CleanupReviewStatus commit(Mutator&& mutator);

    CleanupReviewStore m_store;
    CleanupReview m_review;
};

}  // namespace spacelens
