#include "app/IndexSession.hpp"

#include <QMetaObject>

namespace spacelens {
namespace {

std::wstring toWide(const QString& s)
{
    return s.toStdWString();
}

}  // namespace

IndexSession::IndexSession(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<spacelens::IndexBuildState>("spacelens::IndexBuildState");
    qRegisterMetaType<spacelens::IndexRefreshOutcome>(
        "spacelens::IndexRefreshOutcome");
}

IndexSession::~IndexSession()
{
    cancel();
    joinWorker();
}

bool IndexSession::isRunning() const
{
    std::lock_guard lock(m_mutex);
    return m_running;
}

IndexSession::JobKind IndexSession::currentJob() const
{
    std::lock_guard lock(m_mutex);
    return m_job;
}

bool IndexSession::startBuild(const QString& rootPath)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
    }
    joinWorker();
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
        m_running = true;
        m_job = JobKind::Build;
        m_buildResult.reset();
        m_refreshResult.reset();
    }

    const std::wstring root = toWide(rootPath);
    m_worker = std::jthread([this, root](std::stop_token stop) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                emit statusMessage(QStringLiteral("Building index…"));
            },
            Qt::QueuedConnection);

        IndexBuildResult result = buildIndexForRoot(root, stop);

        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result)]() mutable {
                onBuildDone(std::move(result));
            },
            Qt::QueuedConnection);
    });
    return true;
}

bool IndexSession::startRefresh(const QString& rootPath)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
    }
    joinWorker();
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
        m_running = true;
        m_job = JobKind::Refresh;
        m_buildResult.reset();
        m_refreshResult.reset();
    }

    const std::wstring root = toWide(rootPath);
    m_worker = std::jthread([this, root](std::stop_token stop) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                emit statusMessage(QStringLiteral("Refreshing index (USN)…"));
            },
            Qt::QueuedConnection);

        IndexRefreshResult result = refreshIndex(root, stop);

        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result)]() mutable {
                onRefreshDone(std::move(result));
            },
            Qt::QueuedConnection);
    });
    return true;
}

void IndexSession::cancel()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
    }
}

std::optional<IndexBuildResult> IndexSession::takeBuildResult()
{
    std::lock_guard lock(m_mutex);
    auto out = std::move(m_buildResult);
    m_buildResult.reset();
    return out;
}

std::optional<IndexRefreshResult> IndexSession::takeRefreshResult()
{
    std::lock_guard lock(m_mutex);
    auto out = std::move(m_refreshResult);
    m_refreshResult.reset();
    return out;
}

void IndexSession::joinWorker()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

void IndexSession::onBuildDone(IndexBuildResult result)
{
    const IndexBuildState state = result.state;
    {
        std::lock_guard lock(m_mutex);
        m_buildResult = std::move(result);
        m_running = false;
        m_job = JobKind::None;
    }
    emit buildFinished(state);
}

void IndexSession::onRefreshDone(IndexRefreshResult result)
{
    const IndexRefreshOutcome outcome = result.outcome;
    {
        std::lock_guard lock(m_mutex);
        m_refreshResult = std::move(result);
        m_running = false;
        m_job = JobKind::None;
    }
    emit refreshFinished(outcome);
}

}  // namespace spacelens
