#pragma once

#include "core/index/IndexBuilder.hpp"
#include "core/index/IndexRefresh.hpp"

#include <QObject>
#include <QString>

#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace spacelens {

/// Background full-index or USN refresh. Mirrors ScanSession: queued signals,
/// cooperative cancel, join on destroy. Never mutates analyzed filesystem.
class IndexSession final : public QObject {
    Q_OBJECT

public:
    enum class JobKind { None, Build, Refresh };

    explicit IndexSession(QObject* parent = nullptr);
    ~IndexSession() override;

    IndexSession(const IndexSession&) = delete;
    IndexSession& operator=(const IndexSession&) = delete;

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] JobKind currentJob() const;

    /// Full rebuild via buildIndexForRoot (scan + publish). False if busy.
    bool startBuild(const QString& rootPath);

    /// USN incremental refreshIndex. False if busy.
    bool startRefresh(const QString& rootPath);

    void cancel();

    std::optional<IndexBuildResult> takeBuildResult();
    std::optional<IndexRefreshResult> takeRefreshResult();

signals:
    void statusMessage(const QString& message);
    void buildFinished(spacelens::IndexBuildState state);
    void refreshFinished(spacelens::IndexRefreshOutcome outcome);

private:
    void joinWorker();
    void onBuildDone(IndexBuildResult result);
    void onRefreshDone(IndexRefreshResult result);

    std::jthread m_worker;
    mutable std::mutex m_mutex;
    bool m_running = false;
    JobKind m_job = JobKind::None;
    std::optional<IndexBuildResult> m_buildResult;
    std::optional<IndexRefreshResult> m_refreshResult;
};

}  // namespace spacelens

Q_DECLARE_METATYPE(spacelens::IndexBuildState)
Q_DECLARE_METATYPE(spacelens::IndexRefreshOutcome)
