#pragma once

#include "core/ZaloStorageInspector.hpp"

#include <QObject>
#include <QString>

#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace spacelens {

/// Owns one background Zalo storage review. Completion is marshalled to the
/// GUI thread; cancellation is cooperative and destruction joins the worker.
class ZaloStorageSession final : public QObject {
    Q_OBJECT

public:
    explicit ZaloStorageSession(QObject* parent = nullptr);
    ~ZaloStorageSession() override;

    ZaloStorageSession(const ZaloStorageSession&) = delete;
    ZaloStorageSession& operator=(const ZaloStorageSession&) = delete;

    [[nodiscard]] bool isRunning() const;

    /// Reviews one explicitly selected root, or bounded default roots when
    /// `rootPath` is empty. Returns false if a review is already running.
    bool start(const QString& rootPath = {});

    void cancel();

    /// Takes ownership of the completed report (empty if none is ready).
    std::optional<ZaloStorageReport> takeReport();

signals:
    void statusMessage(const QString& message);
    void finished(spacelens::ZaloStorageStatus status);

private:
    void joinWorker();
    void onWorkerFinished(ZaloStorageReport report);

    std::jthread m_worker;
    mutable std::mutex m_mutex;
    std::optional<ZaloStorageReport> m_report;
    bool m_running = false;
};

}  // namespace spacelens

Q_DECLARE_METATYPE(spacelens::ZaloStorageStatus)
