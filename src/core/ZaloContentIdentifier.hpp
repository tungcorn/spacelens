#pragma once

#include "core/Types.hpp"
#include "platform/windows/ReadOnlyPayload.hpp"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spacelens {

enum class ZaloContentType {
    Unknown,
    Jpeg,
    Png,
    Webp,
    Gif,
    Mp4,
    Mov,
    Pdf,
    Zip,
    Docx,
    Xlsx,
    Pptx
};

using ZaloContentKind = ZaloContentType;

[[nodiscard]] const char* toString(ZaloContentType type) noexcept;

enum class ZaloIdentificationMethod {
    None,
    Signature,
    Structural,
    EmbeddedPayload
};

using ZaloContentIdentificationMethod = ZaloIdentificationMethod;

[[nodiscard]] const char* toString(ZaloIdentificationMethod method) noexcept;

enum class ZaloContentConfidence {
    Unknown,
    Medium,
    Strong,
    Verified
};

using ZaloConfidence = ZaloContentConfidence;

[[nodiscard]] const char* toString(ZaloContentConfidence confidence) noexcept;

enum class ZaloContentStatus {
    Identified,
    Unknown,
    Ambiguous,
    Cancelled,
    ReadError,
    Changed
};

using ZaloIdentificationStatus = ZaloContentStatus;

[[nodiscard]] const char* toString(ZaloContentStatus status) noexcept;

enum class ZaloSemanticMetadataStatus {
    NotRequested,
    NotApplicable,
    Available,
    Unavailable,
    NotAvailable = Unavailable,
    Malformed,
    Ambiguous,
    LimitExceeded,
    Cancelled,
    ReadError,
    Changed
};

using ZaloMetadataStatus = ZaloSemanticMetadataStatus;

[[nodiscard]] const char* toString(ZaloSemanticMetadataStatus status) noexcept;

enum class ZaloContentEvidenceCode {
    None,
    JpegSignature,
    JpegMarkers,
    JpegEntropy,
    JpegEnd,
    PngSignature,
    PngIhdr,
    PngEnd,
    WebpSignature,
    WebpHeader,
    GifSignature,
    GifHeader,
    Mp4Ftyp,
    Mp4Moov,
    Mp4Trak,
    PdfHeader,
    PdfStartXref,
    PdfXref,
    PdfTrailer,
    PdfEnd,
    ZipSignature,
    ZipCentralDirectory,
    ZipLocalHeaders,
    ZipCrc32,
    OoxmlContentTypes,
    OoxmlRelationships,
    OoxmlMainPart,
    EmbeddedPayload
};

using ZaloEvidenceCode = ZaloContentEvidenceCode;

[[nodiscard]] const char* toString(ZaloContentEvidenceCode code) noexcept;

enum class ZaloIdentitySource {
    Unknown,
    OriginalFilename,
    EmbeddedMetadata,
    DocumentStructure,
    MediaMetadata,
    BoundedContentProbe,
    VisualPreview
};

[[nodiscard]] const char* toString(ZaloIdentitySource source) noexcept;

enum class ZaloPreviewKind {
    None,
    Image,
    VideoContactSheet,
    DocumentText
};

[[nodiscard]] const char* toString(ZaloPreviewKind kind) noexcept;

struct ZaloImageDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

using ZaloJpegDimensions = ZaloImageDimensions;

struct ZaloVideoMetadata {
    std::uint64_t durationMs = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t timescale = 0;
    std::string codec;
};

struct ZaloSemanticTextItem {
    bool heading = false;
    std::string text;

    [[nodiscard]] bool operator==(const ZaloSemanticTextItem&) const = default;
};

/// Bounded, privacy-safe semantic fields. Text is limited to the first
/// meaningful visible document content; comments, headers, footers, notes,
/// macros, relationships to external resources, and message data are never
/// represented here.
struct ZaloSemanticMetadata {
    ZaloSemanticMetadataStatus status = ZaloSemanticMetadataStatus::NotRequested;
    std::optional<std::uint32_t> pdfPageCount;
    std::optional<std::string> title;
    std::optional<std::string> author;
    std::optional<std::string> creator;
    std::vector<ZaloSemanticTextItem> visibleText;
};

inline constexpr std::size_t kZaloSemanticMaxStringBytes = 256U;
inline constexpr std::size_t kZaloSemanticMaxStringScalars = 128U;
inline constexpr std::size_t kZaloSemanticMaxTextItems = 16U;
inline constexpr std::size_t kZaloSemanticMaxTextBytes = 4096U;
inline constexpr std::size_t kZaloSemanticMaxTextScalars = 1024U;

/// Strongly deterministic, bounded human identity describing "What is this item?"
struct ZaloHumanIdentity {
    std::string displayName;
    std::optional<std::string> displayTitle;
    std::string contentSummary;
    std::optional<std::string> textPreview;
    ZaloPreviewKind previewKind = ZaloPreviewKind::None;
    bool previewAvailable = false;
    std::string previewReference;
    std::optional<std::uint32_t> imageWidth;
    std::optional<std::uint32_t> imageHeight;
    std::optional<std::uint64_t> videoDurationMs;
    std::optional<std::uint32_t> videoWidth;
    std::optional<std::uint32_t> videoHeight;
    std::optional<std::string> videoCodec;
    std::optional<std::uint32_t> documentPageCount;
    ZaloIdentitySource identitySource = ZaloIdentitySource::Unknown;
};

/// Privacy-safe result for one bounded payload-identification attempt. The
/// result never contains a path, filename, hash, account identifier, or XML
/// text.
struct ZaloContentResult {
    ZaloContentStatus status = ZaloContentStatus::Unknown;
    ZaloContentType type = ZaloContentType::Unknown;
    ZaloIdentificationMethod method = ZaloIdentificationMethod::None;
    ZaloContentConfidence confidence = ZaloContentConfidence::Unknown;
    bool wrapper = false;
    ByteSize payloadOffset = 0;
    ByteSize payloadLength = 0;
    std::optional<ZaloImageDimensions> imageDimensions;
    std::optional<ZaloJpegDimensions> jpegDimensions;
    std::optional<ZaloVideoMetadata> videoMetadata;
    std::optional<ZaloSemanticMetadata> semanticMetadata;
    std::optional<ZaloHumanIdentity> humanIdentity;
    std::vector<ZaloContentEvidenceCode> evidence;
    std::string description = "Unresolved app-managed binary";

    [[nodiscard]] bool identified() const noexcept
    {
        return status == ZaloContentStatus::Identified;
    }
};

using ZaloContentIdentification = ZaloContentResult;

inline constexpr std::size_t kZaloContentMaxScanBytes = 64U * 1024U;
inline constexpr std::size_t kZaloContentMaxCandidates = 32U;

[[nodiscard]] ZaloContentResult identifyZaloContent(
    const PayloadView& payload, PayloadCancellation cancellation = {});
[[nodiscard]] ZaloContentResult identifyZaloContent(
    const PayloadView& payload, std::stop_token stop);

/// Extract bounded semantic fields only from an already identified payload.
/// The caller must perform a stable post-identification revalidation before
/// publishing the result and another revalidation after this function returns.
[[nodiscard]] ZaloSemanticMetadata extractZaloSemanticMetadata(
    const PayloadView& payload, const ZaloContentResult& identification,
    PayloadCancellation cancellation = {});
[[nodiscard]] ZaloSemanticMetadata extractZaloSemanticMetadata(
    const PayloadView& payload, const ZaloContentResult& identification,
    std::stop_token stop);

/// Deterministically synthesize human identity from parsed content, trusted
/// metadata, and physical accounting context.
[[nodiscard]] ZaloHumanIdentity buildHumanIdentity(
    const ZaloContentResult& content,
    std::string_view trustedOriginalFilename = {},
    ByteSize logicalBytes = 0,
    std::string_view categoryAlias = {});

class ZaloContentIdentifier final {
public:
    [[nodiscard]] static ZaloContentResult identify(
        const PayloadView& payload, PayloadCancellation cancellation = {})
    {
        return identifyZaloContent(payload, std::move(cancellation));
    }

    [[nodiscard]] static ZaloContentResult identify(
        const PayloadView& payload, std::stop_token stop)
    {
        return identifyZaloContent(payload, stop);
    }

    [[nodiscard]] static ZaloSemanticMetadata extractMetadata(
        const PayloadView& payload, const ZaloContentResult& identification,
        PayloadCancellation cancellation = {})
    {
        return extractZaloSemanticMetadata(payload, identification,
                                            std::move(cancellation));
    }

    [[nodiscard]] static ZaloSemanticMetadata extractMetadata(
        const PayloadView& payload, const ZaloContentResult& identification,
        std::stop_token stop)
    {
        return extractZaloSemanticMetadata(payload, identification, stop);
    }

    [[nodiscard]] static ZaloHumanIdentity buildIdentity(
        const ZaloContentResult& content,
        std::string_view trustedOriginalFilename = {},
        ByteSize logicalBytes = 0,
        std::string_view categoryAlias = {})
    {
        return buildHumanIdentity(content, trustedOriginalFilename,
                                  logicalBytes, categoryAlias);
    }
};

}  // namespace spacelens

