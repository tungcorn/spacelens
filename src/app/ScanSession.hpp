#pragma once

#include "core/ScanTypes.hpp"
#include "platform/windows/WindowsFileEnumerator.hpp"

#include <QObject>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace spacelens {

/// Owns one background scan. Emits progress on the GUI thread via queued signals.
/// Destruction requests stop and joins the worker (no dangling callbacks).
class ScanSession final : public QObject {
    Q_OBJECT

public:
    explicit ScanSession(QObject* parent = nullptr);
    ~ScanSession() override;

    ScanSession(const ScanSession&) = delete;
    ScanSession& operator=(const ScanSession&) = delete;

    [[nodiscard]] bool isRunning() const;

    /// Starts a scan of `rootPath`. Returns false if a scan is already running.
    bool start(const QString& rootPath, ScanOptions options = {});

    /// Cooperative cancel; worker exits at the next directory boundary.
    void cancel();

    /// Takes ownership of the completed result (empty if none / still running).
    std::optional<ScanResult> takeResult();

signals:
    void progressUpdated(const spacelens::ScanProgress& progress);
    void finished(spacelens::ScanState state);

private:
    void joinWorker();
    void onWorkerFinished(ScanResult result);

    WindowsFileEnumerator m_enumerator;
    std::jthread m_worker;
    mutable std::mutex m_mutex;
    std::optional<ScanResult> m_result;
    bool m_running = false;
};

}  // namespace spacelens

Q_DECLARE_METATYPE(spacelens::ScanProgress)
Q_DECLARE_METATYPE(spacelens::ScanState)
