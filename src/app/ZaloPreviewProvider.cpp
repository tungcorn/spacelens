#include "app/ZaloPreviewProvider.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QImageReader>
#include <QPainter>
#include <QStandardPaths>

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace spacelens {

namespace {

class MfInitializer {
public:
    MfInitializer()
    {
#ifdef _WIN32
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_hr = ::MFStartup(MF_VERSION);
#endif
    }
    ~MfInitializer()
    {
#ifdef _WIN32
        if (SUCCEEDED(m_hr)) {
            ::MFShutdown();
        }
        ::CoUninitialize();
#endif
    }
    [[nodiscard]] bool ok() const noexcept
    {
#ifdef _WIN32
        return SUCCEEDED(m_hr);
#else
        return false;
#endif
    }

private:
#ifdef _WIN32
    HRESULT m_hr = E_FAIL;
#endif
};

#ifdef _WIN32
static QImage hBitmapToQImage(HBITMAP hBitmap)
{
    if (!hBitmap) return {};
    BITMAP bmp{};
    if (!::GetObjectW(hBitmap, sizeof(bmp), &bmp)) return {};
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bmp.bmWidth;
    bi.bmiHeader.biHeight = -bmp.bmHeight; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    QImage image(bmp.bmWidth, bmp.bmHeight, QImage::Format_ARGB32_Premultiplied);
    HDC hdc = ::GetDC(nullptr);
    if (!hdc) return {};
    const int lines = ::GetDIBits(hdc, hBitmap, 0, bmp.bmHeight, image.bits(), &bi, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, hdc);
    if (lines <= 0) return {};
    return image;
}

static QString ensureExtensionPath(const QString& filePath, const QString& defaultExt = QStringLiteral("mp4"))
{
    if (filePath.isEmpty()) {
        return {};
    }
    QFileInfo fi(filePath);
    if (!fi.suffix().isEmpty()) {
        return filePath;
    }

    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/SpaceLens/preview_links");
    QDir().mkpath(tempDir);
    const QString linkPath = tempDir + QStringLiteral("/") + fi.fileName() + QStringLiteral(".") + defaultExt;

    if (!QFileInfo::exists(linkPath)) {
        if (!::CreateHardLinkW(linkPath.toStdWString().c_str(), filePath.toStdWString().c_str(), nullptr)) {
            ::CopyFileW(filePath.toStdWString().c_str(), linkPath.toStdWString().c_str(), FALSE);
        }
    }
    return QFileInfo::exists(linkPath) ? linkPath : filePath;
}

static QImage extractShellThumbnail(const std::wstring& widePath, int targetW, int targetH)
{
    if (widePath.empty()) return {};
    const QString qPath = QString::fromStdWString(widePath);
    const QString effectivePath = ensureExtensionPath(qPath, QStringLiteral("mp4"));
    const std::wstring effectiveWide = effectivePath.toStdWString();

    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(::SHCreateItemFromParsingName(effectiveWide.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        return {};
    }
    SIZE size{targetW, targetH};
    HBITMAP hBmp = nullptr;
    // SIIGBF_BIGGERSIZEOK (0x1) | SIIGBF_THUMBNAILONLY (0x8)
    HRESULT hr = factory->GetImage(size, 0x00000001 | 0x00000008, &hBmp);
    if (FAILED(hr) || !hBmp) {
        hr = factory->GetImage(size, 0x00000001, &hBmp);
    }
    if (SUCCEEDED(hr) && hBmp) {
        QImage result = hBitmapToQImage(hBmp);
        ::DeleteObject(hBmp);
        return result;
    }
    return {};
}
#endif

MfInitializer& globalMf()
{
    static MfInitializer init;
    return init;
}

}  // namespace

ZaloPreviewProvider::ZaloPreviewProvider(QObject* parent)
    : QObject(parent), m_memCache(200)  // Cache up to 200 items
{
    globalMf();
}

ZaloPreviewProvider::~ZaloPreviewProvider() = default;

QString ZaloPreviewProvider::cacheDirectory()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = base + QStringLiteral("/SpaceLens/previews");
    QDir().mkpath(dir);
    return dir;
}

void ZaloPreviewProvider::clearCache()
{
    QMutexLocker locker(&m_mutex);
    m_memCache.clear();
    const QString dir = cacheDirectory();
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
}

QPixmap ZaloPreviewProvider::getPreviewPixmap(
    const QString& filePath,
    const ZaloHumanIdentity& identity,
    const QSize& targetSize)
{
    if (filePath.isEmpty() && identity.previewKind != ZaloPreviewKind::DocumentText) {
        return {};
    }

    const QString cacheKey = filePath + QStringLiteral("@") +
                             QString::number(targetSize.width()) + QStringLiteral("x") +
                             QString::number(targetSize.height());

    {
        QMutexLocker locker(&m_mutex);
        if (auto* cached = m_memCache.object(cacheKey)) {
            return *cached;
        }
    }

    QPixmap result;
    switch (identity.previewKind) {
    case ZaloPreviewKind::Image: {
        const QImage img = loadBoundedImage(filePath, targetSize);
        if (!img.isNull()) {
            result = QPixmap::fromImage(img);
        }
        break;
    }
    case ZaloPreviewKind::VideoContactSheet: {
        ZaloVideoMetadata vm{};
        if (identity.videoDurationMs.has_value()) {
            vm.durationMs = *identity.videoDurationMs;
        }
        if (identity.videoWidth.has_value()) {
            vm.width = *identity.videoWidth;
        }
        if (identity.videoHeight.has_value()) {
            vm.height = *identity.videoHeight;
        }
        if (identity.videoCodec.has_value()) {
            vm.codec = *identity.videoCodec;
        }
        result = getVideoContactSheet(filePath, vm, targetSize);
        break;
    }
    case ZaloPreviewKind::DocumentText: {
        result = getDocumentCard(identity, targetSize);
        break;
    }
    case ZaloPreviewKind::None:
        break;
    }

    if (!result.isNull()) {
        QMutexLocker locker(&m_mutex);
        m_memCache.insert(cacheKey, new QPixmap(result));
    }
    return result;
}

QImage ZaloPreviewProvider::loadBoundedImage(const QString& filePath, const QSize& targetSize)
{
    if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
        return {};
    }

    const QString effectivePath = ensureExtensionPath(filePath, QStringLiteral("jpg"));
    QImageReader reader(effectivePath);
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        return {};
    }

    const QSize origSize = reader.size();
    if (origSize.isValid() && (origSize.width() > targetSize.width() || origSize.height() > targetSize.height())) {
        reader.setScaledSize(origSize.scaled(targetSize, Qt::KeepAspectRatio));
    }

    QImage img = reader.read();
    if (!img.isNull() && (img.width() > targetSize.width() || img.height() > targetSize.height())) {
        img = img.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return img;
}

QImage ZaloPreviewProvider::extractVideoFrameAtTime(const QString& filePath, qint64 timestampMs)
{
#ifdef _WIN32
    if (!globalMf().ok() || filePath.isEmpty()) {
        return {};
    }

    const QString effectivePath = ensureExtensionPath(filePath, QStringLiteral("mp4"));
    const std::wstring widePath = effectivePath.toStdWString();
    ComPtr<IMFAttributes> attributes;
    if (FAILED(::MFCreateAttributes(&attributes, 1))) {
        return {};
    }
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(::MFCreateSourceReaderFromURL(widePath.c_str(), attributes.Get(), &reader))) {
        return {};
    }

    ComPtr<IMFMediaType> mediaType;
    if (FAILED(::MFCreateMediaType(&mediaType))) {
        return {};
    }
    mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

    if (FAILED(reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, mediaType.Get()))) {
        return {};
    }

    PROPVARIANT varPos;
    ::PropVariantInit(&varPos);
    varPos.vt = VT_I8;
    varPos.hVal.QuadPart = timestampMs * 10000LL;  // 100ns units
    reader->SetCurrentPosition(GUID_NULL, varPos);
    ::PropVariantClear(&varPos);

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG sampleTime = 0;
    ComPtr<IMFSample> sample;

    for (int attempts = 0; attempts < 10; ++attempts) {
        sample.Reset();
        if (FAILED(reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                &streamIndex, &flags, &sampleTime, &sample))) {
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }
        if (sample) {
            break;
        }
    }

    if (!sample) {
        return {};
    }

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
        return {};
    }

    BYTE* data = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    if (FAILED(buffer->Lock(&data, &maxLen, &curLen))) {
        return {};
    }

    ComPtr<IMFMediaType> currentType;
    reader->GetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &currentType);

    UINT32 width = 0;
    UINT32 height = 0;
    if (currentType) {
        ::MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    }

    QImage frame;
    if (width > 0 && height > 0 && curLen >= width * height * 4) {
        // Media Foundation RGB32 is top-down or bottom-up depending on stride.
        QImage rawImage(data, static_cast<int>(width), static_cast<int>(height),
                        static_cast<int>(width * 4), QImage::Format_RGB32);
        frame = rawImage.copy();
    }

    buffer->Unlock();
    return frame;
#else
    (void)filePath;
    (void)timestampMs;
    return {};
#endif
}

QPixmap ZaloPreviewProvider::getVideoContactSheet(
    const QString& filePath,
    const ZaloVideoMetadata& videoMeta,
    const QSize& targetSize)
{
    const int w = std::max(targetSize.width(), 96);
    const int h = std::max(targetSize.height(), 54);

    QPixmap sheet(w, h);
    sheet.fill(QColor(18, 20, 24));

    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int subWidth = (w - 8) / 3;
    const int subHeight = h - 4;

    const uint64_t dur = videoMeta.durationMs > 0 ? videoMeta.durationMs : 10000ULL;
    const std::array<qint64, 3> timestamps = {
        static_cast<qint64>(dur * 0.10),
        static_cast<qint64>(dur * 0.50),
        static_cast<qint64>(dur * 0.90)};

    bool anyFrameExtracted = false;
    for (int i = 0; i < 3; ++i) {
        const QRect rect(2 + i * (subWidth + 2), 2, subWidth, subHeight);
        QImage frame = extractVideoFrameAtTime(filePath, timestamps[static_cast<size_t>(i)]);
        if (frame.isNull() && i == 0) {
            frame = extractVideoFrameAtTime(filePath, 0);
        }
        if (!frame.isNull()) {
            anyFrameExtracted = true;
            QImage scaled = frame.scaled(rect.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            // Center crop into rect
            const int cropX = (scaled.width() - rect.width()) / 2;
            const int cropY = (scaled.height() - rect.height()) / 2;
            painter.drawImage(rect.topLeft(), scaled, QRect(cropX, cropY, rect.width(), rect.height()));
        } else {
            painter.fillRect(rect, QColor(28, 32, 40));
            painter.setPen(QColor(100, 110, 130));
            painter.drawRect(rect.adjusted(0, 0, -1, -1));
        }

        // Draw subtle timestamp badge
        const uint64_t sec = static_cast<uint64_t>(timestamps[static_cast<size_t>(i)]) / 1000ULL;
        const QString timeText = QString::asprintf("%02llu:%02llu", sec / 60ULL, sec % 60ULL);
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 7, QFont::DemiBold));
        const QRect textRect(rect.left() + 2, rect.bottom() - 14, rect.width() - 4, 12);
        painter.fillRect(textRect, QColor(0, 0, 0, 160));
        painter.setPen(Qt::white);
        painter.drawText(textRect, Qt::AlignCenter, timeText);
    }

    if (!anyFrameExtracted) {
#ifdef _WIN32
        QImage shellThumb = extractShellThumbnail(filePath.toStdWString(), w, h);
        if (!shellThumb.isNull()) {
            anyFrameExtracted = true;
            QImage scaled = shellThumb.scaled(QSize(w, h), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            const int cropX = (scaled.width() - w) / 2;
            const int cropY = (scaled.height() - h) / 2;
            painter.drawImage(QPoint(0, 0), scaled, QRect(cropX, cropY, w, h));

            // Dark bottom overlay badge
            painter.fillRect(QRect(0, h - 18, w, 18), QColor(0, 0, 0, 180));
            painter.setPen(Qt::white);
            painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::Bold));
            QString overlayText = QStringLiteral("▶ VIDEO");
            if (dur > 0 && dur != 10000ULL) {
                const uint64_t totalSec = dur / 1000ULL;
                overlayText = QString::asprintf("▶ %02llu:%02llu", totalSec / 60ULL, totalSec % 60ULL);
            }
            painter.drawText(QRect(4, h - 18, w - 8, 18), Qt::AlignLeft | Qt::AlignVCenter, overlayText);
        }
#endif
    }

    if (!anyFrameExtracted) {
        // Synthesize a clean, attractive video slate card
        painter.fillRect(QRect(0, 0, w, h), QColor(24, 28, 36));
        painter.setPen(QColor(60, 130, 246));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 9, QFont::Bold));
        QString label = QStringLiteral("🎬 VIDEO");
        if (videoMeta.width > 0 && videoMeta.height > 0) {
            label += QString::asprintf(" · %ux%u", videoMeta.width, videoMeta.height);
        }
        if (!videoMeta.codec.empty()) {
            label += QStringLiteral(" · ") + QString::fromStdString(videoMeta.codec);
        }
        painter.drawText(QRect(0, 0, w, h), Qt::AlignCenter, label);
    }

    // Outer border
    painter.setPen(QColor(50, 56, 68));
    painter.drawRect(0, 0, w - 1, h - 1);

    return sheet;
}

QPixmap ZaloPreviewProvider::getDocumentCard(
    const ZaloHumanIdentity& identity,
    const QSize& targetSize)
{
    const int w = std::max(targetSize.width(), 80);
    const int h = std::max(targetSize.height(), 80);

    QPixmap pixmap(w, h);
    pixmap.fill(QColor(24, 28, 36));

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Accent color based on document type
    QColor accent(70, 130, 240);
    QString typeLabel = QStringLiteral("DOC");
    if (identity.displayName.find(".pdf") != std::string::npos ||
        identity.contentSummary.find("PDF") != std::string::npos) {
        accent = QColor(230, 57, 70);
        typeLabel = QStringLiteral("PDF");
    } else if (identity.contentSummary.find("DOCX") != std::string::npos) {
        accent = QColor(30, 144, 255);
        typeLabel = QStringLiteral("DOCX");
    } else if (identity.contentSummary.find("XLSX") != std::string::npos) {
        accent = QColor(46, 139, 87);
        typeLabel = QStringLiteral("XLSX");
    } else if (identity.contentSummary.find("PPTX") != std::string::npos) {
        accent = QColor(255, 127, 80);
        typeLabel = QStringLiteral("PPTX");
    }

    // Top banner
    painter.fillRect(QRect(0, 0, w, 22), accent);
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::Bold));
    painter.drawText(QRect(4, 0, w - 8, 22), Qt::AlignVCenter | Qt::AlignLeft, typeLabel);

    if (identity.documentPageCount.has_value()) {
        const QString pages = QString::number(*identity.documentPageCount) + QStringLiteral("p");
        painter.drawText(QRect(4, 0, w - 8, 22), Qt::AlignVCenter | Qt::AlignRight, pages);
    }

    // Body title / snippet
    painter.setPen(QColor(220, 225, 235));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 7, QFont::Normal));
    QString textToShow;
    if (identity.displayTitle.has_value() && !identity.displayTitle->empty()) {
        textToShow = QString::fromStdString(*identity.displayTitle);
    } else if (identity.textPreview.has_value() && !identity.textPreview->empty()) {
        textToShow = QString::fromStdString(*identity.textPreview);
    } else {
        textToShow = QString::fromStdString(identity.displayName);
    }

    const QRect bodyRect(4, 26, w - 8, h - 30);
    painter.drawText(bodyRect, Qt::TextWordWrap | Qt::AlignTop, textToShow);

    // Border
    painter.setPen(QColor(50, 56, 68));
    painter.drawRect(0, 0, w - 1, h - 1);

    return pixmap;
}

}  // namespace spacelens
