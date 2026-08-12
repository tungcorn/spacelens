#include "app/ScanSession.hpp"

#include "core/ScanEngine.hpp"

#include <QMetaObject>
#include <utility>

namespace spacelens {
namespace {

std::wstring toWide(const QString& s)
{
    return s.toStdWString();
}

}  // namespace

ScanSession::ScanSession(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<spacelens::ScanProgress>("spacelens::ScanProgress");
    qRegisterMetaType<spacelens::ScanState>("spacelens::ScanState");
}

ScanSession::~ScanSession()
{
    cancel();
    joinWorker();
}

bool ScanSession::isRunning() const
{
    std::lock_guard lock(m_mutex);
    return m_running;
}

bool ScanSession::start(const QString& rootPath, ScanOptions options)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_running) {
            return false;
        }
        m_running = true;
        m_result.reset();
    }

    // Ensure any previous worker is joined before reuse.
    joinWorker();

    const std::wstring root = toWide(rootPath);

    // jthread provides stop_token; destructor/request_stop joins cleanly.
    m_worker = std::jthread([this, root, options](std::stop_token stopToken) {
        ScanEngine engine(m_enumerator);

        auto progressCb = [this](const ScanProgress& progress) {
            // Marshal to GUI thread; copy progress by value for queued delivery.
            QMetaObject::invokeMethod(
                this,
                [this, progress]() { emit progressUpdated(progress); },
                Qt::QueuedConnection);
        };

        ScanResult result =
            engine.scan(root, options, stopToken, std::move(progressCb));

        // Deliver completion on the GUI thread so UI can takeResult safely.
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result)]() mutable {
                onWorkerFinished(std::move(result));
            },
            Qt::QueuedConnection);
    });

    return true;
}

void ScanSession::cancel()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
    }
}

std::optional<ScanResult> ScanSession::takeResult()
{
    std::lock_guard lock(m_mutex);
    auto out = std::move(m_result);
    m_result.reset();
    return out;
}

void ScanSession::joinWorker()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

void ScanSession::onWorkerFinished(ScanResult result)
{
    const ScanState state = result.state;
    {
        std::lock_guard lock(m_mutex);
        m_result = std::move(result);
        m_running = false;
    }
    emit finished(state);
}

}  // namespace spacelens
