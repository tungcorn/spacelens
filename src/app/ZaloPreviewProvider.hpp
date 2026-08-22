#pragma once

#include "core/ZaloContentIdentifier.hpp"
#include "core/ZaloStorageInspector.hpp"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPixmap>
#include <QString>

#include <memory>
#include <optional>

namespace spacelens {

/// Bounded, read-only preview generator.
/// Operates strictly within SpaceLens-owned AppData/temp directories.
/// Never mutates, moves, or creates files in Zalo directories.
class ZaloPreviewProvider final : public QObject {
    Q_OBJECT

public:
    explicit ZaloPreviewProvider(QObject* parent = nullptr);
    ~ZaloPreviewProvider() override;

    ZaloPreviewProvider(const ZaloPreviewProvider&) = delete;
    ZaloPreviewProvider& operator=(const ZaloPreviewProvider&) = delete;

    /// Returns a cached or synchronously generated thumbnail/card.
    /// Returns a null QPixmap if preview is unavailable or unreadable.
    [[nodiscard]] QPixmap getPreviewPixmap(
        const QString& filePath,
        const ZaloHumanIdentity& identity,
        const QSize& targetSize = QSize(96, 96));

    /// Generates a 3-frame visual contact sheet for a video file (~10%, ~50%, ~90%).
    [[nodiscard]] QPixmap getVideoContactSheet(
        const QString& filePath,
        const ZaloVideoMetadata& videoMeta,
        const QSize& targetSize = QSize(240, 72));

    /// Generates a bounded document preview card.
    [[nodiscard]] static QPixmap getDocumentCard(
        const ZaloHumanIdentity& identity,
        const QSize& targetSize = QSize(96, 96));

    /// Clears any in-memory and on-disk preview caches owned by SpaceLens.
    void clearCache();

    /// Returns SpaceLens-owned cache directory path.
    [[nodiscard]] static QString cacheDirectory();

private:
    [[nodiscard]] QImage loadBoundedImage(const QString& filePath, const QSize& targetSize);
    [[nodiscard]] QImage extractVideoFrameAtTime(const QString& filePath, qint64 timestampMs);

    mutable QMutex m_mutex;
    QCache<QString, QPixmap> m_memCache;
};

}  // namespace spacelens
