#include "core/ZaloContentIdentifier.hpp"

#include "core/SizeFormatter.hpp"
#include "miniz.h"
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spacelens {
namespace {

template <typename T>
[[nodiscard]] constexpr bool checkedAdd(T left, T right, T& result) noexcept
{
    if (left > std::numeric_limits<T>::max() - right) {
        return false;
    }
    result = left + right;
    return true;
}

template <typename T>
[[nodiscard]] constexpr bool checkedMul(T left, T right, T& result) noexcept
{
    if (left != 0 && right > std::numeric_limits<T>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] inline bool checkedAddSize(std::size_t left, std::size_t right, std::size_t& result) noexcept
{
    return checkedAdd(left, right, result);
}

[[nodiscard]] inline bool checkedMulSize(std::size_t left, std::size_t right, std::size_t& result) noexcept
{
    return checkedMul(left, right, result);
}

constexpr std::size_t kCursorChunkBytes = 64U * 1024U;
constexpr ByteSize kMaxReadBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaxReadCalls = 32U * 1024U;
constexpr ByteSize kMaxJpegBytes = 128U * 1024U * 1024U;
constexpr ByteSize kMaxPngBytes = 128U * 1024U * 1024U;
constexpr ByteSize kMaxWebpBytes = 128U * 1024U * 1024U;
constexpr ByteSize kMaxGifBytes = 128U * 1024U * 1024U;
constexpr ByteSize kMaxMp4Bytes = 256U * 1024U * 1024U;
constexpr ByteSize kMaxPdfBytes = 128U * 1024U * 1024U;
constexpr std::size_t kPdfTailBytes = 1024U * 1024U;
constexpr std::size_t kPdfXrefBytes = 8U * 1024U * 1024U;
constexpr std::size_t kPdfMaxXrefEntries = 4096U;
constexpr std::size_t kPdfMaxDeclaredSize = 1U * 1024U * 1024U;
constexpr std::size_t kPdfObjectProbeBytes = 64U;
constexpr std::size_t kZipMaxEntries = 4096U;
constexpr std::size_t kZipCentralDirectoryBytes = 16U * 1024U * 1024U;
constexpr std::size_t kZipAllocationBytes = 64U * 1024U * 1024U;
constexpr ByteSize kZipMaxEntryBytes = 32U * 1024U * 1024U;
constexpr ByteSize kZipMaxValidatedBytes = 128U * 1024U * 1024U;
constexpr ByteSize kZipMaxCompressionRatio = 1000U;
constexpr std::size_t kOoxmlXmlBytes = 2U * 1024U * 1024U;
constexpr std::size_t kOoxmlTotalXmlBytes = 6U * 1024U * 1024U;
constexpr std::size_t kOoxmlMaxXmlNodes = 65536U;
constexpr std::size_t kOoxmlMaxXmlDepth = 64U;
constexpr std::size_t kZipNameBytes = 512U;
constexpr std::size_t kZipCommentBytes = 512U;
constexpr ByteSize kSemanticMaxPdfBytes = 32U * 1024U * 1024U;
constexpr std::size_t kSemanticMaxPdfObjects = 4096U;
constexpr std::size_t kSemanticMaxPageCount = 1U * 1000U * 1000U;
constexpr std::size_t kSemanticMaxOoxmlParts = 64U;
constexpr std::size_t kSemanticMaxOoxmlTextBytes = 8U * 1024U;
constexpr ByteSize kSemanticMaxOoxmlBytes = 64U * 1024U * 1024U;
constexpr std::size_t kSemanticMaxSharedStrings = 8192U;
constexpr std::size_t kSemanticMaxParagraphBytes = 65536U;
constexpr std::size_t kSemanticMaxCoreFields = 8U;
constexpr std::string_view kCorePropertiesNamespace =
    "http://schemas.openxmlformats.org/package/2006/metadata/core-properties";
constexpr std::string_view kDcNamespace = "http://purl.org/dc/elements/1.1/";

constexpr std::uint32_t kZipLocalHeaderSignature = 0x04034b50U;
constexpr std::uint32_t kZipCentralHeaderSignature = 0x02014b50U;
constexpr std::uint32_t kZipEndSignature = 0x06054b50U;
constexpr std::uint32_t kZipCentralDigitalSignature = 0x05054b50U;

constexpr std::string_view kOfficeDocumentRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
constexpr std::string_view kStrictOfficeDocumentRelationship =
    "http://purl.oclc.org/ooxml/officeDocument/relationships/officeDocument";
constexpr std::string_view kWordMainContentType =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
constexpr std::string_view kSpreadsheetMainContentType =
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml";
constexpr std::string_view kPresentationMainContentType =
    "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml";
constexpr std::string_view kContentTypesNamespace =
    "http://schemas.openxmlformats.org/package/2006/content-types";
constexpr std::string_view kRelationshipsNamespace =
    "http://schemas.openxmlformats.org/package/2006/relationships";
constexpr std::string_view kStrictRelationshipsNamespace =
    "http://purl.oclc.org/ooxml/package/relationships";
constexpr std::string_view kWordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kSpreadsheetNamespace =
    "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr std::string_view kPresentationNamespace =
    "http://schemas.openxmlformats.org/presentationml/2006/main";
constexpr std::string_view kWordStrictNamespace =
    "http://purl.oclc.org/ooxml/wordprocessingml/main";
constexpr std::string_view kSpreadsheetStrictNamespace =
    "http://purl.oclc.org/ooxml/spreadsheetml/main";
constexpr std::string_view kPresentationStrictNamespace =
    "http://purl.oclc.org/ooxml/presentationml/main";

enum class TerminalState {
    None,
    Cancelled,
    ReadError,
    Changed
};

class ScanContext final {
public:
    ScanContext(const PayloadView& payload, PayloadCancellation cancellation)
        : view(payload), cancellation(std::move(cancellation))
    {
    }

    [[nodiscard]] bool checkCancellation() noexcept
    {
        if (terminal != TerminalState::None) {
            return false;
        }
        if (cancellation) {
            try {
                if (cancellation()) {
                    terminal = TerminalState::Cancelled;
                    return false;
                }
            } catch (...) {
                terminal = TerminalState::Cancelled;
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool readAt(ByteSize offset, void* buffer,
                              std::size_t length) noexcept
    {
        try {
            if (terminal != TerminalState::None || !checkCancellation()) {
                return false;
            }
            if (length == 0U) {
                return true;
            }
            if (offset > view.size() ||
                static_cast<ByteSize>(length) > view.size() - offset) {
                return false;
            }
            if (readCalls >= kMaxReadCalls ||
                static_cast<ByteSize>(length) > kMaxReadBytes - readBytes) {
                budgetExceeded = true;
                return false;
            }

            ++readCalls;
            readBytes += static_cast<ByteSize>(length);
            const PayloadReadResult result =
                view.readAt(offset, buffer, length, cancellation);
            if (result.status == PayloadReadStatus::Ok &&
                result.bytesRead == length) {
                if (xorMask != 0U && buffer != nullptr) {
                    auto* ptr = static_cast<std::uint8_t*>(buffer);
                    for (std::size_t i = 0; i < length; ++i) {
                        ptr[i] ^= xorMask;
                    }
                }
                return true;
            }

            switch (result.status) {
            case PayloadReadStatus::Cancelled:
                terminal = TerminalState::Cancelled;
                break;
            case PayloadReadStatus::IdentityChanged:
            case PayloadReadStatus::PathChanged:
                terminal = TerminalState::Changed;
                break;
            case PayloadReadStatus::Bounds:
            case PayloadReadStatus::EndOfFile:
                break;
            case PayloadReadStatus::Ok:
            case PayloadReadStatus::ReadError:
                terminal = TerminalState::ReadError;
                break;
            }
            return false;
        } catch (...) {
            terminal = TerminalState::ReadError;
            return false;
        }
    }

    [[nodiscard]] bool readRange(ByteSize offset,
                                 std::span<std::uint8_t> output) noexcept
    {
        return readAt(offset, output.data(), output.size());
    }

    PayloadView view;
    PayloadCancellation cancellation;
    TerminalState terminal = TerminalState::None;
    bool budgetExceeded = false;
    std::size_t readCalls = 0;
    ByteSize readBytes = 0;
    std::uint8_t xorMask = 0;
};

[[nodiscard]] bool checkedAdd(ByteSize left, ByteSize right,
                              ByteSize& result) noexcept
{
    if (right > std::numeric_limits<ByteSize>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> readVector(
    ScanContext& context, ByteSize offset, ByteSize length)
{
    if (!context.checkCancellation() ||
        length > static_cast<ByteSize>(std::numeric_limits<std::size_t>::max()) ||
        offset > context.view.size() || length > context.view.size() - offset) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> result(static_cast<std::size_t>(length));
    if (!result.empty() && !context.readRange(offset, result)) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::uint16_t readLe16(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                      (static_cast<std::uint16_t>(bytes[1]) << 8U));
}

[[nodiscard]] std::uint32_t readLe32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] std::uint64_t readLe64(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint64_t>(readLe32(bytes)) |
           (static_cast<std::uint64_t>(readLe32(bytes + 4U)) << 32U);
}

[[nodiscard]] std::uint16_t readBe16(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t readBe32(const std::uint8_t* bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] std::uint64_t readBe64(const std::uint8_t* bytes) noexcept
{
    return (static_cast<std::uint64_t>(readBe32(bytes)) << 32U) |
           static_cast<std::uint64_t>(readBe32(bytes + 4U));
}

class RelativeCursor final {
public:
    RelativeCursor(ScanContext& context, ByteSize base, ByteSize length)
        : m_context(context), m_base(base), m_length(length)
    {
    }

    [[nodiscard]] bool readByte(std::uint8_t& value)
    {
        if (m_position >= m_length) {
            return false;
        }
        const bool inBuffer =
            m_position >= m_bufferStart &&
            m_position - m_bufferStart < static_cast<ByteSize>(m_bufferLength);
        if (!inBuffer) {
            if (!m_context.checkCancellation()) {
                return false;
            }
            const ByteSize remaining = m_length - m_position;
            const std::size_t count = static_cast<std::size_t>(
                std::min<ByteSize>(remaining, kCursorChunkBytes));
            ByteSize absolute = 0;
            if (!checkedAdd(m_base, m_position, absolute) ||
                !m_context.readAt(absolute, m_buffer.data(), count)) {
                return false;
            }
            m_bufferStart = m_position;
            m_bufferLength = count;
        }
        value = m_buffer[static_cast<std::size_t>(m_position - m_bufferStart)];
        ++m_position;
        return true;
    }

    [[nodiscard]] bool readBytes(std::span<std::uint8_t> bytes)
    {
        if (m_position > m_length ||
            static_cast<ByteSize>(bytes.size()) > m_length - m_position) {
            return false;
        }
        for (std::uint8_t& byte : bytes) {
            if (!readByte(byte)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool skip(ByteSize count)
    {
        if (m_position > m_length || count > m_length - m_position) {
            return false;
        }
        m_position += count;
        return m_context.checkCancellation();
    }

    [[nodiscard]] ByteSize position() const noexcept { return m_position; }
    [[nodiscard]] ByteSize remaining() const noexcept
    {
        return m_position <= m_length ? m_length - m_position : 0;
    }

private:
    ScanContext& m_context;
    ByteSize m_base = 0;
    ByteSize m_length = 0;
    ByteSize m_position = 0;
    ByteSize m_bufferStart = 0;
    std::size_t m_bufferLength = 0;
    std::array<std::uint8_t, kCursorChunkBytes> m_buffer{};
};

struct Candidate final {
    ZaloContentType type = ZaloContentType::Unknown;
    ZaloIdentificationMethod method = ZaloIdentificationMethod::Structural;
    ZaloContentConfidence confidence = ZaloContentConfidence::Strong;
    ByteSize offset = 0;
    ByteSize length = 0;
    bool masked = false;
    std::uint8_t maskByte = 0;
    std::optional<ZaloImageDimensions> dimensions;
    std::optional<ZaloVideoMetadata> videoMetadata;
    std::vector<ZaloContentEvidenceCode> evidence;
};

void addEvidence(std::vector<ZaloContentEvidenceCode>& evidence,
                 ZaloContentEvidenceCode code)
{
    if (std::find(evidence.begin(), evidence.end(), code) == evidence.end()) {
        evidence.push_back(code);
    }
}

[[nodiscard]] Candidate makeCandidate(
    ZaloContentType type, ZaloContentConfidence confidence, ByteSize offset,
    ByteSize length, std::vector<ZaloContentEvidenceCode> evidence,
    std::optional<ZaloImageDimensions> dimensions = std::nullopt,
    std::optional<ZaloVideoMetadata> videoMetadata = std::nullopt)
{
    Candidate candidate;
    candidate.type = type;
    candidate.confidence = confidence;
    candidate.offset = offset;
    candidate.length = length;
    candidate.evidence = std::move(evidence);
    candidate.dimensions = dimensions;
    candidate.videoMetadata = std::move(videoMetadata);
    if (offset != 0U) {
        candidate.method = ZaloIdentificationMethod::EmbeddedPayload;
        addEvidence(candidate.evidence, ZaloContentEvidenceCode::EmbeddedPayload);
    }
    return candidate;
}

[[nodiscard]] bool isJpegSof(std::uint8_t marker) noexcept
{
    switch (marker) {
    case 0xc0:
    case 0xc1:
    case 0xc2:
    case 0xc3:
    case 0xc5:
    case 0xc6:
    case 0xc7:
    case 0xc9:
    case 0xca:
    case 0xcb:
    case 0xcd:
    case 0xce:
    case 0xcf:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool nextJpegMarker(RelativeCursor& cursor,
                                  std::uint8_t& marker)
{
    std::uint8_t byte = 0;
    if (!cursor.readByte(byte) || byte != 0xffU) {
        return false;
    }
    do {
        if (!cursor.readByte(byte)) {
            return false;
        }
    } while (byte == 0xffU);
    if (byte == 0x00U) {
        return false;
    }
    marker = byte;
    return true;
}

[[nodiscard]] bool nextJpegEntropyMarker(RelativeCursor& cursor,
                                         std::uint8_t& marker,
                                         bool& sawEntropy)
{
    std::uint8_t byte = 0;
    for (;;) {
        if (!cursor.readByte(byte)) {
            return false;
        }
        if (byte != 0xffU) {
            sawEntropy = true;
            continue;
        }
        do {
            if (!cursor.readByte(byte)) {
                return false;
            }
        } while (byte == 0xffU);
        if (byte == 0x00U) {
            sawEntropy = true;
            continue;
        }
        if (byte >= 0xd0U && byte <= 0xd7U) {
            continue;
        }
        marker = byte;
        return true;
    }
}

[[nodiscard]] std::optional<Candidate> parseJpeg(ScanContext& context,
                                                   ByteSize base)
{
    if (base > context.view.size() || context.view.size() - base < 2U) {
        return std::nullopt;
    }
    const ByteSize available = context.view.size() - base;
    const ByteSize parseLength = std::min(available, kMaxJpegBytes);
    RelativeCursor cursor(context, base, parseLength);
    std::array<std::uint8_t, 2> signature{};
    if (!cursor.readBytes(signature) || signature[0] != 0xffU ||
        signature[1] != 0xd8U) {
        return std::nullopt;
    }

    bool sawSof = false;
    bool sawSos = false;
    bool sawEntropy = false;
    std::uint8_t frameComponents = 0;
    std::optional<ZaloImageDimensions> dimensions;
    std::uint8_t marker = 0;
    if (!nextJpegMarker(cursor, marker)) {
        return std::nullopt;
    }

    for (;;) {
        if (marker == 0xd9U) {
            if (!sawSof || !sawSos || !sawEntropy) {
                return std::nullopt;
            }
            const ByteSize candidateLength = cursor.position();
            if (base == 0U && candidateLength != context.view.size()) {
                return std::nullopt;
            }
            return makeCandidate(
                ZaloContentType::Jpeg, ZaloContentConfidence::Strong, base,
                candidateLength,
                {ZaloContentEvidenceCode::JpegSignature,
                 ZaloContentEvidenceCode::JpegMarkers,
                 ZaloContentEvidenceCode::JpegEntropy,
                 ZaloContentEvidenceCode::JpegEnd},
                dimensions);
        }
        if (marker == 0xd8U || (marker >= 0xd0U && marker <= 0xd7U) ||
            marker == 0x00U) {
            return std::nullopt;
        }
        if (marker == 0x01U) {
            if (!nextJpegMarker(cursor, marker)) {
                return std::nullopt;
            }
            continue;
        }

        std::array<std::uint8_t, 2> lengthBytes{};
        if (!cursor.readBytes(lengthBytes)) {
            return std::nullopt;
        }
        const ByteSize bigEndianLength =
            static_cast<ByteSize>((static_cast<std::uint16_t>(lengthBytes[0]) <<
                                   8U) |
                                  lengthBytes[1]);
        if (bigEndianLength < 2U || bigEndianLength > cursor.remaining()) {
            return std::nullopt;
        }
        const ByteSize payloadLength = bigEndianLength - 2U;

        if (isJpegSof(marker)) {
            if (sawSof || payloadLength < 6U) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> segment(
                static_cast<std::size_t>(payloadLength));
            if (!cursor.readBytes(segment) || segment[0] == 0U) {
                return std::nullopt;
            }
            const std::uint16_t height = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(segment[1]) << 8U) | segment[2]);
            const std::uint16_t width = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(segment[3]) << 8U) | segment[4]);
            const std::uint8_t components = segment[5];
            const std::size_t expectedPayload =
                6U + static_cast<std::size_t>(components) * 3U;
            if (width == 0U || height == 0U || components == 0U ||
                components > 4U || payloadLength != expectedPayload) {
                return std::nullopt;
            }
            dimensions = ZaloImageDimensions{width, height};
            frameComponents = components;
            sawSof = true;
        } else if (marker == 0xdaU) {
            if (!sawSof || payloadLength < 4U) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> segment(
                static_cast<std::size_t>(payloadLength));
            if (!cursor.readBytes(segment)) {
                return std::nullopt;
            }
            const std::uint8_t scanComponents = segment[0];
            const std::size_t expectedPayload =
                4U + static_cast<std::size_t>(scanComponents) * 2U;
            if (scanComponents == 0U || scanComponents > frameComponents ||
                payloadLength != expectedPayload) {
                return std::nullopt;
            }
            sawSos = true;
            if (!nextJpegEntropyMarker(cursor, marker, sawEntropy)) {
                return std::nullopt;
            }
            continue;
        } else if (!cursor.skip(payloadLength)) {
            return std::nullopt;
        }

        if (!nextJpegMarker(cursor, marker)) {
            return std::nullopt;
        }
    }
}

[[nodiscard]] std::optional<Candidate> parsePng(ScanContext& context,
                                                  ByteSize base)
{
    if (base > context.view.size() || context.view.size() - base < 8U + 12U + 13U) {
        return std::nullopt;
    }
    const ByteSize available = context.view.size() - base;
    const ByteSize parseLength = std::min(available, kMaxPngBytes);
    RelativeCursor cursor(context, base, parseLength);

    std::array<std::uint8_t, 8> signature{};
    if (!cursor.readBytes(signature) ||
        signature[0] != 0x89U || signature[1] != 'P' || signature[2] != 'N' ||
        signature[3] != 'G' || signature[4] != 0x0dU || signature[5] != 0x0aU ||
        signature[6] != 0x1aU || signature[7] != 0x0aU) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 8> ihdrHeader{};
    if (!cursor.readBytes(ihdrHeader)) {
        return std::nullopt;
    }
    const std::uint32_t ihdrLength = readBe32(ihdrHeader.data());
    if (ihdrLength != 13U ||
        std::memcmp(ihdrHeader.data() + 4U, "IHDR", 4U) != 0) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 13 + 4> ihdrDataAndCrc{};
    if (!cursor.readBytes(ihdrDataAndCrc)) {
        return std::nullopt;
    }
    const std::uint32_t width = readBe32(ihdrDataAndCrc.data());
    const std::uint32_t height = readBe32(ihdrDataAndCrc.data() + 4U);
    if (width == 0U || height == 0U) {
        return std::nullopt;
    }

    std::vector<ZaloContentEvidenceCode> evidence{
        ZaloContentEvidenceCode::PngSignature,
        ZaloContentEvidenceCode::PngIhdr};

    bool sawIend = false;
    ByteSize totalLength = 0;
    while (cursor.remaining() >= 12U) {
        std::array<std::uint8_t, 8> chunkHdr{};
        if (!cursor.readBytes(chunkHdr)) {
            break;
        }
        const std::uint32_t chunkLen = readBe32(chunkHdr.data());
        const bool isIend = std::memcmp(chunkHdr.data() + 4U, "IEND", 4U) == 0;
        if (static_cast<ByteSize>(chunkLen) + 4U > cursor.remaining()) {
            break;
        }
        if (!cursor.skip(static_cast<ByteSize>(chunkLen) + 4U)) {
            break;
        }
        if (isIend) {
            sawIend = true;
            totalLength = cursor.position();
            evidence.push_back(ZaloContentEvidenceCode::PngEnd);
            break;
        }
    }
    if (!sawIend) {
        totalLength = cursor.position();
    }

    return makeCandidate(
        ZaloContentType::Png,
        sawIend ? ZaloContentConfidence::Strong : ZaloContentConfidence::Medium,
        base, totalLength > 0 ? totalLength : cursor.position(),
        std::move(evidence),
        ZaloImageDimensions{width, height});
}

[[nodiscard]] std::optional<Candidate> parseWebp(ScanContext& context,
                                                   ByteSize base)
{
    if (base > context.view.size() || context.view.size() - base < 16U) {
        return std::nullopt;
    }
    const ByteSize available = context.view.size() - base;
    const ByteSize parseLength = std::min(available, kMaxWebpBytes);
    RelativeCursor cursor(context, base, parseLength);

    std::array<std::uint8_t, 12> riffHeader{};
    if (!cursor.readBytes(riffHeader) ||
        std::memcmp(riffHeader.data(), "RIFF", 4) != 0 ||
        std::memcmp(riffHeader.data() + 8, "WEBP", 4) != 0) {
        return std::nullopt;
    }
    const std::uint32_t riffSize = readLe32(riffHeader.data() + 4);
    if (riffSize < 4U) {
        return std::nullopt;
    }
    const ByteSize totalLength = std::min<ByteSize>(
        available, static_cast<ByteSize>(riffSize) + 8U);

    std::array<std::uint8_t, 8> chunkHdr{};
    if (!cursor.readBytes(chunkHdr)) {
        return std::nullopt;
    }
    const std::uint32_t chunkSize = readLe32(chunkHdr.data() + 4);
    std::optional<ZaloImageDimensions> dimensions;
    std::vector<ZaloContentEvidenceCode> evidence{
        ZaloContentEvidenceCode::WebpSignature};

    if (std::memcmp(chunkHdr.data(), "VP8 ", 4) == 0) {
        if (chunkSize >= 10U) {
            std::array<std::uint8_t, 10> vp8Header{};
            if (cursor.readBytes(vp8Header)) {
                if (vp8Header[3] == 0x9dU && vp8Header[4] == 0x01U &&
                    vp8Header[5] == 0x2aU) {
                    const std::uint32_t width =
                        (static_cast<std::uint32_t>(vp8Header[6]) |
                         (static_cast<std::uint32_t>(vp8Header[7]) << 8U)) &
                        0x3fffU;
                    const std::uint32_t height =
                        (static_cast<std::uint32_t>(vp8Header[8]) |
                         (static_cast<std::uint32_t>(vp8Header[9]) << 8U)) &
                        0x3fffU;
                    if (width > 0 && height > 0) {
                        dimensions = ZaloImageDimensions{width, height};
                        evidence.push_back(ZaloContentEvidenceCode::WebpHeader);
                    }
                }
            }
        }
    } else if (std::memcmp(chunkHdr.data(), "VP8L", 4) == 0) {
        if (chunkSize >= 5U) {
            std::array<std::uint8_t, 5> vp8lHeader{};
            if (cursor.readBytes(vp8lHeader) && vp8lHeader[0] == 0x2fU) {
                const std::uint32_t val = readLe32(vp8lHeader.data() + 1);
                const std::uint32_t width = 1U + (val & 0x3fffU);
                const std::uint32_t height = 1U + ((val >> 14U) & 0x3fffU);
                if (width > 0 && height > 0) {
                    dimensions = ZaloImageDimensions{width, height};
                    evidence.push_back(ZaloContentEvidenceCode::WebpHeader);
                }
            }
        }
    } else if (std::memcmp(chunkHdr.data(), "VP8X", 4) == 0) {
        if (chunkSize >= 10U) {
            std::array<std::uint8_t, 10> vp8xHeader{};
            if (cursor.readBytes(vp8xHeader)) {
                const std::uint32_t width =
                    1U + (static_cast<std::uint32_t>(vp8xHeader[4]) |
                          (static_cast<std::uint32_t>(vp8xHeader[5]) << 8U) |
                          (static_cast<std::uint32_t>(vp8xHeader[6]) << 16U));
                const std::uint32_t height =
                    1U + (static_cast<std::uint32_t>(vp8xHeader[7]) |
                          (static_cast<std::uint32_t>(vp8xHeader[8]) << 8U) |
                          (static_cast<std::uint32_t>(vp8xHeader[9]) << 16U));
                if (width > 0 && height > 0) {
                    dimensions = ZaloImageDimensions{width, height};
                    evidence.push_back(ZaloContentEvidenceCode::WebpHeader);
                }
            }
        }
    }

    return makeCandidate(
        ZaloContentType::Webp,
        dimensions.has_value() ? ZaloContentConfidence::Strong
                               : ZaloContentConfidence::Medium,
        base, totalLength, std::move(evidence), dimensions);
}

[[nodiscard]] std::optional<Candidate> parseGif(ScanContext& context,
                                                  ByteSize base)
{
    if (base > context.view.size() || context.view.size() - base < 10U) {
        return std::nullopt;
    }
    const ByteSize available = context.view.size() - base;
    const ByteSize parseLength = std::min(available, kMaxGifBytes);
    RelativeCursor cursor(context, base, parseLength);

    std::array<std::uint8_t, 6> sig{};
    if (!cursor.readBytes(sig)) {
        return std::nullopt;
    }
    if ((std::memcmp(sig.data(), "GIF87a", 6) != 0) &&
        (std::memcmp(sig.data(), "GIF89a", 6) != 0)) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 4> screenDesc{};
    if (!cursor.readBytes(screenDesc)) {
        return std::nullopt;
    }
    const std::uint16_t width = readLe16(screenDesc.data());
    const std::uint16_t height = readLe16(screenDesc.data() + 2);
    if (width == 0 || height == 0) {
        return std::nullopt;
    }
    std::vector<ZaloContentEvidenceCode> evidence{
        ZaloContentEvidenceCode::GifSignature,
        ZaloContentEvidenceCode::GifHeader};
    return makeCandidate(
        ZaloContentType::Gif, ZaloContentConfidence::Strong,
        base, available, std::move(evidence),
        ZaloImageDimensions{width, height});
}

[[nodiscard]] std::optional<Candidate> parseMp4(ScanContext& context,
                                                  ByteSize base)
{
    if (base > context.view.size() || context.view.size() - base < 8U) {
        return std::nullopt;
    }
    const ByteSize available = context.view.size() - base;
    const ByteSize parseLength = std::min<ByteSize>(available, kMaxMp4Bytes);
    RelativeCursor cursor(context, base, parseLength);

    std::vector<ZaloContentEvidenceCode> evidence;
    bool sawFtyp = false;
    bool sawMoov = false;
    bool isMov = false;
    ZaloVideoMetadata videoMeta;
    std::optional<ZaloImageDimensions> dimensions;

    std::size_t boxCount = 0;
    while (cursor.remaining() >= 8U && boxCount < 64U) {
        ++boxCount;
        const ByteSize boxStart = cursor.position();
        std::array<std::uint8_t, 8> boxHdr{};
        if (!cursor.readBytes(boxHdr)) {
            break;
        }
        std::uint64_t boxSize = readBe32(boxHdr.data());
        const char* typeTag = reinterpret_cast<const char*>(boxHdr.data() + 4);
        ByteSize headerSize = 8U;
        if (boxSize == 1U) {
            std::array<std::uint8_t, 8> extSize{};
            if (!cursor.readBytes(extSize)) {
                break;
            }
            boxSize = readBe64(extSize.data());
            headerSize = 16U;
        } else if (boxSize == 0U) {
            boxSize = parseLength - boxStart;
        }
        if (boxSize < headerSize || boxSize > parseLength - boxStart) {
            break;
        }

        const ByteSize dataSize = static_cast<ByteSize>(boxSize) - headerSize;

        if (std::memcmp(typeTag, "ftyp", 4) == 0) {
            sawFtyp = true;
            evidence.push_back(ZaloContentEvidenceCode::Mp4Ftyp);
            if (dataSize >= 4U) {
                std::array<std::uint8_t, 4> brand{};
                if (cursor.readBytes(brand) &&
                    std::memcmp(brand.data(), "qt  ", 4) == 0) {
                    isMov = true;
                }
                (void)cursor.skip(dataSize - 4U);
            } else {
                (void)cursor.skip(dataSize);
            }
        } else if (std::memcmp(typeTag, "moov", 4) == 0) {
            sawMoov = true;
            evidence.push_back(ZaloContentEvidenceCode::Mp4Moov);
            const ByteSize moovEnd = cursor.position() + dataSize;
            while (cursor.position() + 8U <= moovEnd) {
                std::array<std::uint8_t, 8> subHdr{};
                if (!cursor.readBytes(subHdr)) {
                    break;
                }
                std::uint64_t subSize = readBe32(subHdr.data());
                const char* subType =
                    reinterpret_cast<const char*>(subHdr.data() + 4);
                if (subSize < 8U ||
                    subSize > moovEnd - (cursor.position() - 8U)) {
                    break;
                }
                const ByteSize subDataSize = static_cast<ByteSize>(subSize) - 8U;
                if (std::memcmp(subType, "mvhd", 4) == 0) {
                    if (subDataSize >= 20U) {
                        std::vector<std::uint8_t> mvhdBytes(static_cast<std::size_t>(
                            std::min<ByteSize>(subDataSize, 32U)));
                        if (cursor.readBytes(mvhdBytes)) {
                            const std::uint8_t version = mvhdBytes[0];
                            if (version == 0 && mvhdBytes.size() >= 20U) {
                                const std::uint32_t timescale =
                                    readBe32(mvhdBytes.data() + 12U);
                                const std::uint32_t duration =
                                    readBe32(mvhdBytes.data() + 16U);
                                videoMeta.timescale = timescale;
                                if (timescale > 0) {
                                    videoMeta.durationMs =
                                        (static_cast<std::uint64_t>(duration) *
                                         1000ULL) /
                                        timescale;
                                }
                            } else if (version == 1 && mvhdBytes.size() >= 32U) {
                                const std::uint32_t timescale =
                                    readBe32(mvhdBytes.data() + 20U);
                                const std::uint64_t duration =
                                    readBe64(mvhdBytes.data() + 24U);
                                videoMeta.timescale = timescale;
                                if (timescale > 0) {
                                    videoMeta.durationMs =
                                        (duration * 1000ULL) / timescale;
                                }
                            }
                            (void)cursor.skip(subDataSize - mvhdBytes.size());
                        } else {
                            (void)cursor.skip(subDataSize);
                        }
                    } else {
                        (void)cursor.skip(subDataSize);
                    }
                } else if (std::memcmp(subType, "trak", 4) == 0) {
                    evidence.push_back(ZaloContentEvidenceCode::Mp4Trak);
                    const ByteSize trakEnd = cursor.position() + subDataSize;
                    while (cursor.position() + 8U <= trakEnd) {
                        std::array<std::uint8_t, 8> trakSubHdr{};
                        if (!cursor.readBytes(trakSubHdr)) {
                            break;
                        }
                        std::uint64_t trakSubSize = readBe32(trakSubHdr.data());
                        const char* trakSubType =
                            reinterpret_cast<const char*>(trakSubHdr.data() + 4);
                        if (trakSubSize < 8U ||
                            trakSubSize > trakEnd - (cursor.position() - 8U)) {
                            break;
                        }
                        const ByteSize trakSubDataSize =
                            static_cast<ByteSize>(trakSubSize) - 8U;
                        if (std::memcmp(trakSubType, "tkhd", 4) == 0 &&
                            trakSubDataSize >= 84U) {
                            std::vector<std::uint8_t> tkhdBytes(
                                static_cast<std::size_t>(
                                    std::min<ByteSize>(trakSubDataSize, 96U)));
                            if (cursor.readBytes(tkhdBytes)) {
                                const std::uint8_t version = tkhdBytes[0];
                                std::uint32_t w = 0;
                                std::uint32_t h = 0;
                                if (version == 0 && tkhdBytes.size() >= 84U) {
                                    w = readBe16(tkhdBytes.data() + 76U);
                                    h = readBe16(tkhdBytes.data() + 80U);
                                } else if (version == 1 &&
                                           tkhdBytes.size() >= 96U) {
                                    w = readBe16(tkhdBytes.data() + 88U);
                                    h = readBe16(tkhdBytes.data() + 92U);
                                }
                                if (w > 0 && h > 0 && videoMeta.width == 0) {
                                    videoMeta.width = w;
                                    videoMeta.height = h;
                                    dimensions = ZaloImageDimensions{w, h};
                                }
                                (void)cursor.skip(trakSubDataSize - tkhdBytes.size());
                            } else {
                                (void)cursor.skip(trakSubDataSize);
                            }
                        } else {
                            (void)cursor.skip(trakSubDataSize);
                        }
                    }
                } else {
                    (void)cursor.skip(subDataSize);
                }
            }
        } else {
            (void)cursor.skip(dataSize);
        }
    }

    if (!sawFtyp && !sawMoov) {
        return std::nullopt;
    }

    Candidate candidate;
    candidate.type = isMov ? ZaloContentType::Mov : ZaloContentType::Mp4;
    candidate.confidence = (sawFtyp && sawMoov)
                               ? ZaloContentConfidence::Verified
                               : ZaloContentConfidence::Strong;
    candidate.offset = base;
    candidate.length = available;
    candidate.evidence = std::move(evidence);
    candidate.dimensions = dimensions;
    candidate.videoMetadata = videoMeta;
    if (base != 0U) {
        candidate.method = ZaloIdentificationMethod::EmbeddedPayload;
        addEvidence(candidate.evidence, ZaloContentEvidenceCode::EmbeddedPayload);
    }
    return candidate;
}

[[nodiscard]] bool pdfWhitespace(std::uint8_t byte) noexcept
{
    return byte == 0x00U || byte == 0x09U || byte == 0x0aU ||
           byte == 0x0cU || byte == 0x0dU || byte == 0x20U;
}

void skipPdfWhitespace(std::span<const std::uint8_t> bytes,
                       std::size_t& position)
{
    while (position < bytes.size() && pdfWhitespace(bytes[position])) {
        ++position;
    }
}

[[nodiscard]] bool parsePdfUnsigned(std::span<const std::uint8_t> bytes,
                                    std::size_t& position,
                                    std::uint64_t& value)
{
    skipPdfWhitespace(bytes, position);
    if (position >= bytes.size() || bytes[position] < '0' ||
        bytes[position] > '9') {
        return false;
    }
    value = 0;
    while (position < bytes.size() && bytes[position] >= '0' &&
           bytes[position] <= '9') {
        const std::uint64_t digit = bytes[position] - '0';
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        ++position;
    }
    return true;
}

[[nodiscard]] bool startsWith(std::span<const std::uint8_t> bytes,
                              std::size_t position, std::string_view text)
{
    if (position > bytes.size() || text.size() > bytes.size() - position) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (bytes[position + i] != static_cast<std::uint8_t>(text[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::size_t> findLast(
    std::span<const std::uint8_t> bytes, std::string_view text)
{
    if (text.empty() || text.size() > bytes.size()) {
        return std::nullopt;
    }
    for (std::size_t position = bytes.size() - text.size() + 1U;
         position-- > 0U;) {
        if (startsWith(bytes, position, text)) {
            return position;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool parsePdfFixedDigits(std::span<const std::uint8_t> line,
                                       std::size_t start, std::size_t count,
                                       std::uint64_t& value)
{
    if (start > line.size() || count > line.size() - start) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t byte = line[start + i];
        if (byte < '0' || byte > '9') {
            return false;
        }
        value = value * 10U + static_cast<std::uint64_t>(byte - '0');
    }
    return true;
}

struct PdfXrefEntry final {
    std::uint64_t objectNumber = 0;
    std::uint64_t generation = 0;
    ByteSize offset = 0;
    bool inUse = false;
};

struct PdfObjectReference final {
    std::uint64_t objectNumber = 0;
    std::uint64_t generation = 0;

    [[nodiscard]] bool operator==(const PdfObjectReference&) const noexcept =
        default;
};

struct PdfStructure final {
    std::vector<PdfXrefEntry> entries;
    PdfObjectReference root;
    std::optional<PdfObjectReference> info;
    bool infoReferenceMalformed = false;
};

[[nodiscard]] bool pdfObjectHeader(ScanContext& context, ByteSize pdfBase,
                                   ByteSize pdfLength,
                                   const PdfXrefEntry& expected)
{
    if (expected.offset >= pdfLength) {
        return false;
    }
    const ByteSize available = pdfLength - expected.offset;
    const ByteSize probeLength =
        std::min<ByteSize>(available, kPdfObjectProbeBytes);
    ByteSize absolute = 0;
    if (!checkedAdd(pdfBase, expected.offset, absolute)) {
        return false;
    }
    auto bytes = readVector(context, absolute, probeLength);
    if (!bytes.has_value()) {
        return false;
    }
    const std::span<const std::uint8_t> span(*bytes);
    std::size_t position = 0;
    std::uint64_t objectNumber = 0;
    std::uint64_t generation = 0;
    if (!parsePdfUnsigned(span, position, objectNumber) ||
        !parsePdfUnsigned(span, position, generation)) {
        return false;
    }
    skipPdfWhitespace(span, position);
    if (position + 3U > span.size() || span[position] != 'o' ||
        span[position + 1U] != 'b' || span[position + 2U] != 'j' ||
        objectNumber != expected.objectNumber ||
        generation != expected.generation) {
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::size_t> findPdfDictionaryEnd(
    std::span<const std::uint8_t> bytes, std::size_t start)
{
    if (!startsWith(bytes, start, "<<")) {
        return std::nullopt;
    }
    std::size_t position = start + 2U;
    std::size_t depth = 1U;
    while (position < bytes.size()) {
        if (bytes[position] == '(') {
            ++position;
            bool escaped = false;
            while (position < bytes.size()) {
                const std::uint8_t byte = bytes[position++];
                if (escaped) {
                    escaped = false;
                } else if (byte == '\\') {
                    escaped = true;
                } else if (byte == ')') {
                    break;
                }
            }
            continue;
        }
        if (startsWith(bytes, position, "<<")) {
            ++depth;
            position += 2U;
            continue;
        }
        if (startsWith(bytes, position, ">>")) {
            --depth;
            if (depth == 0U) {
                return position;
            }
            position += 2U;
            continue;
        }
        ++position;
    }
    return std::nullopt;
}

[[nodiscard]] bool pdfNameBoundary(std::uint8_t byte) noexcept
{
    return pdfWhitespace(byte) || byte == '/' || byte == '<' || byte == '>' ||
           byte == '[' || byte == ']' || byte == '(' || byte == ')' ||
           byte == '{' || byte == '}';
}

[[nodiscard]] std::optional<std::size_t> findPdfName(
    std::span<const std::uint8_t> bytes, std::string_view name)
{
    std::size_t position = 0;
    while (position + name.size() <= bytes.size()) {
        const auto found = findLast(bytes.subspan(position), name);
        if (!found.has_value()) {
            return std::nullopt;
        }
        const std::size_t absolute = position + *found;
        const bool preceding =
            absolute == 0U || pdfNameBoundary(bytes[absolute - 1U]);
        const std::size_t end = absolute + name.size();
        const bool following = end == bytes.size() || pdfNameBoundary(bytes[end]);
        if (preceding && following) {
            return absolute;
        }
        position = absolute + 1U;
    }
    return std::nullopt;
}

[[nodiscard]] bool parsePdfClassicXref(
    ScanContext& context, ByteSize pdfBase, ByteSize pdfLength,
    ByteSize xrefOffset, std::span<const std::uint8_t> bytes,
    PdfStructure* structure = nullptr)
{
    std::size_t position = 0;
    skipPdfWhitespace(bytes, position);
    if (!startsWith(bytes, position, "xref")) {
        return false;
    }
    position += 4U;

    std::vector<PdfXrefEntry> entries;
    entries.reserve(kPdfMaxXrefEntries);
    std::size_t totalEntries = 0U;
    bool foundTrailer = false;
    for (;;) {
        skipPdfWhitespace(bytes, position);
        if (startsWith(bytes, position, "trailer")) {
            position += 7U;
            foundTrailer = true;
            break;
        }

        std::uint64_t firstObject = 0;
        std::uint64_t entryCount = 0;
        if (!parsePdfUnsigned(bytes, position, firstObject) ||
            !parsePdfUnsigned(bytes, position, entryCount) ||
            entryCount > kPdfMaxXrefEntries ||
            totalEntries > kPdfMaxXrefEntries -
                               static_cast<std::size_t>(entryCount)) {
            return false;
        }
        totalEntries += static_cast<std::size_t>(entryCount);

        for (std::uint64_t i = 0; i < entryCount; ++i) {
            skipPdfWhitespace(bytes, position);
            const std::size_t lineStart = position;
            while (position < bytes.size() && bytes[position] != '\r' &&
                   bytes[position] != '\n') {
                ++position;
            }
            if (position == lineStart || position >= bytes.size()) {
                return false;
            }
            const auto line = bytes.subspan(lineStart, position - lineStart);
            if (line.size() < 18U || line[10] != ' ' || line[16] != ' ' ||
                (line[17] != 'n' && line[17] != 'f')) {
                return false;
            }
            std::uint64_t objectOffset = 0;
            std::uint64_t generation = 0;
            if (!parsePdfFixedDigits(line, 0U, 10U, objectOffset) ||
                !parsePdfFixedDigits(line, 11U, 5U, generation)) {
                return false;
            }
            for (std::size_t j = 18U; j < line.size(); ++j) {
                if (!pdfWhitespace(line[j])) {
                    return false;
                }
            }
            if (firstObject > std::numeric_limits<std::uint64_t>::max() - i) {
                return false;
            }
            entries.push_back(PdfXrefEntry{
                firstObject + i, generation,
                static_cast<ByteSize>(objectOffset), line[17] == 'n'});
            while (position < bytes.size() &&
                   (bytes[position] == '\r' || bytes[position] == '\n')) {
                ++position;
            }
        }
    }

    if (!foundTrailer) {
        return false;
    }
    skipPdfWhitespace(bytes, position);
    const auto dictionaryEnd = findPdfDictionaryEnd(bytes, position);
    if (!dictionaryEnd.has_value()) {
        return false;
    }
    const auto dictionary = bytes.subspan(position, *dictionaryEnd + 2U - position);

    const auto sizeName = findPdfName(dictionary, "/Size");
    if (!sizeName.has_value()) {
        return false;
    }
    std::size_t sizePosition = *sizeName + 5U;
    std::uint64_t declaredSize = 0;
    if (!parsePdfUnsigned(dictionary, sizePosition, declaredSize) ||
        declaredSize < totalEntries || declaredSize == 0U ||
        declaredSize > kPdfMaxDeclaredSize) {
        return false;
    }

    const auto rootName = findPdfName(dictionary, "/Root");
    if (!rootName.has_value()) {
        return false;
    }
    std::size_t rootPosition = *rootName + 5U;
    std::uint64_t rootObject = 0;
    std::uint64_t rootGeneration = 0;
    if (!parsePdfUnsigned(dictionary, rootPosition, rootObject) ||
        !parsePdfUnsigned(dictionary, rootPosition, rootGeneration)) {
        return false;
    }
    skipPdfWhitespace(dictionary, rootPosition);
    if (rootPosition >= dictionary.size() || dictionary[rootPosition] != 'R') {
        return false;
    }

    bool rootFound = false;
    for (const PdfXrefEntry& entry : entries) {
        if (!entry.inUse) {
            continue;
        }
        if (!pdfObjectHeader(context, pdfBase, pdfLength, entry)) {
            return false;
        }
        if (entry.objectNumber == rootObject &&
            entry.generation == rootGeneration) {
            rootFound = true;
        }
    }
    if (rootFound && structure != nullptr) {
        structure->entries = entries;
        structure->root = PdfObjectReference{rootObject, rootGeneration};
        const auto infoNames = findPdfName(dictionary, "/Info");
        if (infoNames.has_value()) {
            std::size_t infoPosition = *infoNames + 5U;
            std::uint64_t infoObject = 0;
            std::uint64_t infoGeneration = 0;
            if (parsePdfUnsigned(dictionary, infoPosition, infoObject) &&
                parsePdfUnsigned(dictionary, infoPosition, infoGeneration)) {
                skipPdfWhitespace(dictionary, infoPosition);
                if (infoPosition < dictionary.size() &&
                    dictionary[infoPosition] == 'R') {
                    structure->info = PdfObjectReference{infoObject,
                                                         infoGeneration};
                } else {
                    structure->infoReferenceMalformed = true;
                }
            } else {
                structure->infoReferenceMalformed = true;
            }
        }
    }
    (void)xrefOffset;
    return rootFound && context.terminal == TerminalState::None;
}

[[nodiscard]] std::optional<Candidate> parsePdf(ScanContext& context,
                                                 ByteSize base)
{
    if (base > context.view.size() || context.view.size() - base < 5U) {
        return std::nullopt;
    }
    const ByteSize length = context.view.size() - base;
    if (length > kMaxPdfBytes) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 5> header{};
    if (!context.readRange(base, header) || header[0] != '%' ||
        header[1] != 'P' || header[2] != 'D' || header[3] != 'F' ||
        header[4] != '-') {
        return std::nullopt;
    }

    const ByteSize tailStart =
        length > kPdfTailBytes ? length - kPdfTailBytes : 0U;
    ByteSize absoluteTailStart = 0;
    if (!checkedAdd(base, tailStart, absoluteTailStart)) {
        return std::nullopt;
    }
    auto tail = readVector(context, absoluteTailStart, length - tailStart);
    if (!tail.has_value()) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> tailSpan(*tail);
    std::size_t searchEnd = tailSpan.size();
    while (searchEnd >= 5U) {
        const auto eofPosition =
            findLast(tailSpan.subspan(0, searchEnd), "%%EOF");
        if (!eofPosition.has_value()) {
            break;
        }
        std::size_t afterEof = *eofPosition + 5U;
        while (afterEof < tailSpan.size() && pdfWhitespace(tailSpan[afterEof])) {
            ++afterEof;
        }
        ByteSize candidateLength = 0;
        if (!checkedAdd(tailStart, static_cast<ByteSize>(afterEof),
                        candidateLength) || candidateLength == 0U) {
            return std::nullopt;
        }

        const auto beforeEof = tailSpan.subspan(0, *eofPosition);
        const auto startxrefPosition = findLast(beforeEof, "startxref");
        if (startxrefPosition.has_value()) {
            std::size_t startPosition = *startxrefPosition + 9U;
            std::uint64_t xrefOffsetValue = 0;
            const bool parsedXrefOffset =
                parsePdfUnsigned(beforeEof, startPosition, xrefOffsetValue);
            skipPdfWhitespace(beforeEof, startPosition);
            if (parsedXrefOffset && startPosition == beforeEof.size() &&
                xrefOffsetValue < candidateLength) {
                const ByteSize xrefOffset =
                    static_cast<ByteSize>(xrefOffsetValue);
                const ByteSize pdfLength = candidateLength;
                const ByteSize xrefAvailable = pdfLength - xrefOffset;
                const ByteSize xrefLength =
                    std::min<ByteSize>(xrefAvailable, kPdfXrefBytes);
                ByteSize absoluteXref = 0;
                if (checkedAdd(base, xrefOffset, absoluteXref)) {
                    auto xref = readVector(context, absoluteXref, xrefLength);
                    if (xref.has_value() &&
                        parsePdfClassicXref(context, base, pdfLength,
                                            xrefOffset, *xref)) {
                        if (base == 0U && candidateLength != context.view.size()) {
                            return std::nullopt;
                        }
                        return makeCandidate(
                            ZaloContentType::Pdf,
                            ZaloContentConfidence::Strong, base,
                            candidateLength,
                            {ZaloContentEvidenceCode::PdfHeader,
                             ZaloContentEvidenceCode::PdfStartXref,
                             ZaloContentEvidenceCode::PdfXref,
                             ZaloContentEvidenceCode::PdfTrailer,
                             ZaloContentEvidenceCode::PdfEnd});
                    }
                }
            }
        }

        // A later false %%EOF must not make an earlier structurally valid
        // embedded PDF disappear, but a direct payload still requires exact
        // whole-payload coverage above.
        searchEnd = *eofPosition;
    }
    return std::nullopt;
}

struct RawZipEntry final {
    std::string name;
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint32_t crc32 = 0;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t index = 0;
    ByteSize localHeaderOffset = 0;
};

struct ZipAllocationTracker final {
    ScanContext* scan = nullptr;
    std::size_t liveBytes = 0;
    bool limitExceeded = false;
};

struct ZipAllocationHeader final {
    std::size_t payloadBytes = 0;
};

struct ZipIoContext final {
    ScanContext* scan = nullptr;
    ByteSize base = 0;
    ByteSize archiveLength = 0;
    ZipAllocationTracker* allocations = nullptr;
};

[[nodiscard]] std::size_t zipRead(void* opaque, mz_uint64 offset, void* buffer,
                                  std::size_t length)
{
    auto* io = static_cast<ZipIoContext*>(opaque);
    if (io == nullptr || io->scan == nullptr || length == 0U) {
        return 0U;
    }
    if (buffer == nullptr || offset > io->archiveLength ||
        static_cast<ByteSize>(length) > io->archiveLength - offset) {
        return 0U;
    }
    ByteSize absolute = 0;
    if (!checkedAdd(io->base, static_cast<ByteSize>(offset), absolute)) {
        return 0U;
    }
    if (!io->scan->readAt(absolute, buffer, length)) {
        // Do not retry. A failed callback is terminal for real read/change/
        // cancellation failures, and parser bounds remain malformed input.
        return 0U;
    }
    return length;
}

[[nodiscard]] void* zipAlloc(void* opaque, std::size_t items,
                             std::size_t size)
{
    auto* io = static_cast<ZipIoContext*>(opaque);
    if (io == nullptr || io->allocations == nullptr || io->scan == nullptr ||
        !io->scan->checkCancellation()) {
        return nullptr;
    }
    std::size_t payloadBytes = 0;
    if (!checkedMulSize(items, size, payloadBytes)) {
        io->allocations->limitExceeded = true;
        return nullptr;
    }
    const std::size_t actualPayload = std::max<std::size_t>(payloadBytes, 1U);
    std::size_t totalBytes = 0;
    if (!checkedAddSize(sizeof(ZipAllocationHeader), actualPayload, totalBytes) ||
        actualPayload > kZipAllocationBytes -
                            std::min(io->allocations->liveBytes,
                                     kZipAllocationBytes)) {
        io->allocations->limitExceeded = true;
        return nullptr;
    }
    auto* header = static_cast<ZipAllocationHeader*>(std::malloc(totalBytes));
    if (header == nullptr) {
        io->allocations->limitExceeded = true;
        return nullptr;
    }
    header->payloadBytes = actualPayload;
    io->allocations->liveBytes += actualPayload;
    return header + 1;
}

void zipFree(void* opaque, void* address)
{
    if (address == nullptr) {
        return;
    }
    auto* io = static_cast<ZipIoContext*>(opaque);
    auto* header = static_cast<ZipAllocationHeader*>(address) - 1;
    if (io != nullptr && io->allocations != nullptr &&
        header->payloadBytes <= io->allocations->liveBytes) {
        io->allocations->liveBytes -= header->payloadBytes;
    }
    std::free(header);
}

[[nodiscard]] void* zipRealloc(void* opaque, void* address,
                               std::size_t items, std::size_t size)
{
    if (address == nullptr) {
        return zipAlloc(opaque, items, size);
    }
    auto* io = static_cast<ZipIoContext*>(opaque);
    if (io == nullptr || io->allocations == nullptr || io->scan == nullptr ||
        !io->scan->checkCancellation()) {
        return nullptr;
    }
    std::size_t payloadBytes = 0;
    if (!checkedMulSize(items, size, payloadBytes)) {
        io->allocations->limitExceeded = true;
        return nullptr;
    }
    const std::size_t actualPayload = std::max<std::size_t>(payloadBytes, 1U);
    auto* oldHeader = static_cast<ZipAllocationHeader*>(address) - 1;
    const std::size_t oldPayload = oldHeader->payloadBytes;
    const std::size_t liveWithoutOld =
        io->allocations->liveBytes >= oldPayload
            ? io->allocations->liveBytes - oldPayload
            : kZipAllocationBytes;
    if (actualPayload > kZipAllocationBytes -
                            std::min(liveWithoutOld, kZipAllocationBytes)) {
        io->allocations->limitExceeded = true;
        return nullptr;
    }
    std::size_t totalBytes = 0;
    if (!checkedAddSize(sizeof(ZipAllocationHeader), actualPayload, totalBytes)) {
        io->allocations->limitExceeded = true;
        return nullptr;
    }
    auto* newHeader = static_cast<ZipAllocationHeader*>(
        std::realloc(oldHeader, totalBytes));
    if (newHeader == nullptr) {
        io->allocations->limitExceeded = true;
        return nullptr;
    }
    newHeader->payloadBytes = actualPayload;
    io->allocations->liveBytes = liveWithoutOld + actualPayload;
    return newHeader + 1;
}

struct ZipReader final {
    mz_zip_archive archive{};
    ZipAllocationTracker allocations{};
    ZipIoContext io{};
    bool initialized = false;

    ZipReader() = default;

    ~ZipReader()
    {
        if (initialized) {
            (void)mz_zip_reader_end(&archive);
        }
    }

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;
};

struct ZipEnd final {
    ByteSize end = 0;
    ByteSize eocd = 0;
};

[[nodiscard]] std::optional<ZipEnd> findZipEnd(ScanContext& context,
                                               ByteSize base)
{
    if (base > context.view.size()) {
        return std::nullopt;
    }
    const ByteSize length = context.view.size() - base;
    if (length < 22U || length > kZipMaxValidatedBytes) {
        return std::nullopt;
    }
    const ByteSize tailStart =
        length > (65535U + 22U) ? length - (65535U + 22U) : 0U;
    ByteSize absoluteTailStart = 0;
    if (!checkedAdd(base, tailStart, absoluteTailStart)) {
        return std::nullopt;
    }
    auto tail = readVector(context, absoluteTailStart, length - tailStart);
    if (!tail.has_value()) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> bytes(*tail);
    if (bytes.size() < 22U) {
        return std::nullopt;
    }
    for (std::size_t position = bytes.size() - 22U;; --position) {
        if (readLe32(bytes.data() + position) == kZipEndSignature) {
            const std::uint16_t commentLength =
                readLe16(bytes.data() + position + 20U);
            const std::size_t endInTail =
                position + 22U + static_cast<std::size_t>(commentLength);
            if (endInTail <= bytes.size()) {
                ByteSize end = 0;
                ByteSize eocd = 0;
                if (checkedAdd(tailStart, static_cast<ByteSize>(endInTail),
                               end) &&
                    checkedAdd(tailStart, static_cast<ByteSize>(position),
                               eocd)) {
                    return ZipEnd{end, eocd};
                }
                return std::nullopt;
            }
        }
        if (position == 0U) {
            break;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool zipCompressionRatioOk(ByteSize compressed,
                                         ByteSize uncompressed) noexcept
{
    if (uncompressed == 0U) {
        return true;
    }
    if (compressed == 0U) {
        return false;
    }
    if (compressed >
        (std::numeric_limits<ByteSize>::max() - 1024U) /
            kZipMaxCompressionRatio) {
        return true;
    }
    return uncompressed <= compressed * kZipMaxCompressionRatio + 1024U;
}

[[nodiscard]] bool parseZipCentralDirectory(
    ScanContext& context, ByteSize base, ByteSize archiveLength,
    ByteSize eocdOffset, mz_zip_archive& archive,
    std::vector<RawZipEntry>& entries)
{
    const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
    if (fileCount > kZipMaxEntries) {
        return false;
    }
    const std::size_t centralSize = mz_zip_get_central_dir_size(&archive);
    if (centralSize > kZipCentralDirectoryBytes) {
        return false;
    }
    const ByteSize centralOffset = archive.m_central_directory_file_ofs;
    if (mz_zip_get_archive_file_start_offset(&archive) != 0U ||
        mz_zip_get_archive_size(&archive) != archiveLength ||
        centralOffset > archiveLength ||
        static_cast<ByteSize>(centralSize) > archiveLength - centralOffset ||
        centralOffset + static_cast<ByteSize>(centralSize) > eocdOffset) {
        return false;
    }

    ByteSize absoluteCentral = 0;
    if (!checkedAdd(base, centralOffset, absoluteCentral)) {
        return false;
    }
    auto central = readVector(context, absoluteCentral, centralSize);
    if (!central.has_value()) {
        return false;
    }

    entries.clear();
    entries.reserve(fileCount);
    std::size_t position = 0U;
    ByteSize totalUncompressed = 0;
    ByteSize totalCompressed = 0;
    for (mz_uint index = 0; index < fileCount; ++index) {
        if (position > central->size() || central->size() - position < 46U ||
            readLe32(central->data() + position) !=
                kZipCentralHeaderSignature) {
            return false;
        }
        const std::uint16_t nameLength =
            readLe16(central->data() + position + 28U);
        const std::uint16_t extraLength =
            readLe16(central->data() + position + 30U);
        const std::uint16_t commentLength =
            readLe16(central->data() + position + 32U);
        std::size_t recordLength = 0;
        if (nameLength > kZipNameBytes || commentLength > kZipCommentBytes ||
            !checkedAddSize(46U, nameLength, recordLength) ||
            !checkedAddSize(recordLength, extraLength, recordLength) ||
            !checkedAddSize(recordLength, commentLength, recordLength) ||
            recordLength > central->size() - position) {
            return false;
        }

        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat) ||
            !mz_zip_reader_is_file_supported(&archive, index) ||
            mz_zip_reader_is_file_encrypted(&archive, index)) {
            return false;
        }
        if (stat.m_uncomp_size > kZipMaxEntryBytes ||
            stat.m_comp_size > kZipMaxEntryBytes ||
            totalUncompressed > kZipMaxValidatedBytes - stat.m_uncomp_size ||
            totalCompressed > kZipMaxValidatedBytes - stat.m_comp_size ||
            !zipCompressionRatioOk(stat.m_comp_size, stat.m_uncomp_size) ||
            (stat.m_uncomp_size == 0U && stat.m_crc32 != 0U)) {
            return false;
        }
        totalUncompressed += stat.m_uncomp_size;
        totalCompressed += stat.m_comp_size;

        RawZipEntry entry;
        entry.name.assign(
            reinterpret_cast<const char*>(central->data() + position + 46U),
            nameLength);
        if (entry.name.find('\0') != std::string::npos) {
            return false;
        }
        entry.compressedSize = stat.m_comp_size;
        entry.uncompressedSize = stat.m_uncomp_size;
        entry.crc32 = stat.m_crc32;
        entry.flags = stat.m_bit_flag;
        entry.method = stat.m_method;
        entry.index = index;
        entry.localHeaderOffset = stat.m_local_header_ofs;
        entries.push_back(std::move(entry));
        position += recordLength;
    }

    if (position < central->size()) {
        if (central->size() - position < 6U ||
            readLe32(central->data() + position) !=
                kZipCentralDigitalSignature) {
            return false;
        }
        const std::uint16_t signatureLength =
            readLe16(central->data() + position + 4U);
        if (6U + static_cast<std::size_t>(signatureLength) !=
            central->size() - position) {
            return false;
        }
    }
    return context.terminal == TerminalState::None;
}

[[nodiscard]] bool validateZipLocalHeaders(
    ScanContext& context, ByteSize base, ByteSize archiveLength,
    ByteSize centralOffset, const std::vector<RawZipEntry>& entries)
{
    struct Range final {
        ByteSize begin = 0;
        ByteSize end = 0;
    };
    std::vector<Range> ranges;
    ranges.reserve(entries.size());

    for (const RawZipEntry& entry : entries) {
        if (entry.localHeaderOffset >= centralOffset ||
            entry.localHeaderOffset > archiveLength ||
            archiveLength - entry.localHeaderOffset < 30U) {
            return false;
        }
        ByteSize absoluteHeader = 0;
        if (!checkedAdd(base, entry.localHeaderOffset, absoluteHeader)) {
            return false;
        }
        auto header = readVector(context, absoluteHeader, 30U);
        if (!header.has_value() ||
            readLe32(header->data()) != kZipLocalHeaderSignature) {
            return false;
        }
        const std::uint16_t localFlags = readLe16(header->data() + 6U);
        const std::uint16_t localMethod = readLe16(header->data() + 8U);
        const std::uint32_t localCrc = readLe32(header->data() + 14U);
        const std::uint32_t localCompressed = readLe32(header->data() + 18U);
        const std::uint32_t localUncompressed = readLe32(header->data() + 22U);
        const std::uint16_t nameLength = readLe16(header->data() + 26U);
        const std::uint16_t extraLength = readLe16(header->data() + 28U);
        if (localFlags != entry.flags || localMethod != entry.method ||
            nameLength != entry.name.size()) {
            return false;
        }

        ByteSize dataOffset = 0;
        const ByteSize headerBytes =
            30U + static_cast<ByteSize>(nameLength) + extraLength;
        if (!checkedAdd(entry.localHeaderOffset, headerBytes, dataOffset) ||
            dataOffset > archiveLength ||
            entry.compressedSize > archiveLength - dataOffset ||
            dataOffset + entry.compressedSize > centralOffset) {
            return false;
        }

        ByteSize absoluteName = 0;
        if (!checkedAdd(base, entry.localHeaderOffset + 30U, absoluteName)) {
            return false;
        }
        auto name = readVector(context, absoluteName, nameLength);
        if (!name.has_value() ||
            std::string_view(reinterpret_cast<const char*>(name->data()),
                             name->size()) != entry.name) {
            return false;
        }

        if ((localFlags & 8U) == 0U) {
            if (localCrc != entry.crc32 ||
                (localCompressed != 0xffffffffU &&
                 localCompressed != entry.compressedSize) ||
                (localUncompressed != 0xffffffffU &&
                 localUncompressed != entry.uncompressedSize)) {
                return false;
            }
        }
        ByteSize recordEnd = 0;
        if (!checkedAdd(dataOffset, entry.compressedSize, recordEnd)) {
            return false;
        }
        ranges.push_back(Range{entry.localHeaderOffset, recordEnd});
    }

    std::sort(ranges.begin(), ranges.end(),
              [](const Range& left, const Range& right) {
                  return left.begin < right.begin;
              });
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].begin < ranges[i - 1U].end) {
            return false;
        }
    }
    return context.terminal == TerminalState::None;
}

[[nodiscard]] bool initializeValidatedZip(
    ScanContext& context, ByteSize base, const ZipEnd& zipEnd,
    ZipReader& reader, std::vector<RawZipEntry>& entries)
{
    if (zipEnd.end < 22U ||
        (base == 0U && zipEnd.end != context.view.size()) ||
        !context.view.trySlice(base, zipEnd.end).has_value()) {
        return false;
    }
    const ByteSize archiveLength = zipEnd.end;
    reader.allocations = ZipAllocationTracker{&context};
    reader.io = ZipIoContext{&context, base, archiveLength,
                              &reader.allocations};
    mz_zip_zero_struct(&reader.archive);
    reader.archive.m_pRead = zipRead;
    reader.archive.m_pIO_opaque = &reader.io;
    reader.archive.m_pAlloc = zipAlloc;
    reader.archive.m_pFree = zipFree;
    reader.archive.m_pRealloc = zipRealloc;
    reader.archive.m_pAlloc_opaque = &reader.io;
    if (!mz_zip_reader_init(
            &reader.archive, archiveLength,
            MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
        return false;
    }
    reader.initialized = true;
    if (context.terminal != TerminalState::None ||
        reader.allocations.limitExceeded) {
        return false;
    }
    if (!parseZipCentralDirectory(context, base, archiveLength, zipEnd.eocd,
                                  reader.archive, entries) ||
        !validateZipLocalHeaders(context, base, archiveLength,
                                 reader.archive.m_central_directory_file_ofs,
                                 entries) ||
        !mz_zip_validate_archive(&reader.archive, 0U)) {
        return false;
    }
    return context.terminal == TerminalState::None &&
           !reader.allocations.limitExceeded;
}

struct EntryLookup final {
    std::size_t index = 0;
    std::size_t count = 0;
};

[[nodiscard]] EntryLookup lookupEntry(const std::vector<RawZipEntry>& entries,
                                      std::string_view name)
{
    EntryLookup result;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == name) {
            result.index = i;
            ++result.count;
        }
    }
    return result;
}

[[nodiscard]] bool normalizeOoxmlTarget(std::string_view target,
                                        std::string& normalized)
{
    if (target.empty() || target.find('?') != std::string_view::npos ||
        target.find('#') != std::string_view::npos ||
        target.find('\\') != std::string_view::npos) {
        return false;
    }
    if (target.front() == '/') {
        target.remove_prefix(1U);
    }
    if (target.empty() || target.front() == '/' || target.back() == '/') {
        return false;
    }
    std::size_t position = 0;
    while (position < target.size()) {
        const std::size_t slash = target.find('/', position);
        const std::size_t end = slash == std::string_view::npos
                                    ? target.size()
                                    : slash;
        const std::string_view part = target.substr(position, end - position);
        if (part.empty() || part == "." || part == ".." ||
            part.find(':') != std::string_view::npos) {
            return false;
        }
        for (const unsigned char byte : part) {
            if (byte < 0x20U || byte == 0x7fU) {
                return false;
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        position = slash + 1U;
    }
    normalized.assign(target);
    return true;
}

struct QualifiedName final {
    std::string_view prefix;
    std::string_view local;
    bool valid = false;
};

[[nodiscard]] QualifiedName splitQualifiedName(const char* raw) noexcept
{
    const std::string_view name = raw == nullptr ? std::string_view{} : raw;
    const std::size_t colon = name.find(':');
    if (colon == std::string_view::npos) {
        return QualifiedName{{}, name, !name.empty()};
    }
    if (colon == 0U || colon + 1U >= name.size() ||
        name.find(':', colon + 1U) != std::string_view::npos) {
        return {};
    }
    return QualifiedName{name.substr(0, colon), name.substr(colon + 1U), true};
}

[[nodiscard]] std::optional<std::string_view> namespaceForElement(
    const pugi::xml_node& node)
{
    const QualifiedName name = splitQualifiedName(node.name());
    if (!name.valid) {
        return std::nullopt;
    }
    if (name.prefix == "xml") {
        return std::string_view{"http://www.w3.org/XML/1998/namespace"};
    }
    for (pugi::xml_node current = node; current;
         current = current.parent()) {
        for (pugi::xml_attribute attribute = current.first_attribute();
             attribute; attribute = attribute.next_attribute()) {
            const std::string_view attributeName = attribute.name();
            if (name.prefix.empty()) {
                if (attributeName == "xmlns") {
                    return std::string_view{attribute.value()};
                }
            } else {
                const std::string prefixName =
                    std::string("xmlns:") + std::string(name.prefix);
                if (attributeName == prefixName) {
                    return std::string_view{attribute.value()};
                }
            }
        }
    }
    if (name.prefix.empty()) {
        return std::string_view{};
    }
    return std::nullopt;
}

[[nodiscard]] bool elementMatches(const pugi::xml_node& node,
                                  std::string_view local,
                                  std::string_view namespaceUri)
{
    const QualifiedName name = splitQualifiedName(node.name());
    const auto actualNamespace = namespaceForElement(node);
    return node.type() == pugi::node_element && name.valid &&
           name.local == local && actualNamespace.has_value() &&
           *actualNamespace == namespaceUri;
}

struct AttributeLookup final {
    std::string_view value;
    bool found = false;
    bool invalid = false;
};

[[nodiscard]] AttributeLookup unqualifiedAttribute(
    const pugi::xml_node& node, std::string_view wanted)
{
    AttributeLookup result;
    for (pugi::xml_attribute attribute = node.first_attribute(); attribute;
         attribute = attribute.next_attribute()) {
        const QualifiedName name = splitQualifiedName(attribute.name());
        if (!name.valid || name.local != wanted) {
            continue;
        }
        if (!name.prefix.empty() || std::string_view(attribute.name()) != wanted ||
            result.found) {
            result.invalid = true;
            continue;
        }
        result.found = true;
        result.value = attribute.value();
    }
    return result;
}

[[nodiscard]] bool isKnownOfficeRelationship(std::string_view type) noexcept
{
    return type == kOfficeDocumentRelationship ||
           type == kStrictOfficeDocumentRelationship;
}

[[nodiscard]] ZaloContentType contentTypeFor(std::string_view value) noexcept
{
    if (value == kWordMainContentType) {
        return ZaloContentType::Docx;
    }
    if (value == kSpreadsheetMainContentType) {
        return ZaloContentType::Xlsx;
    }
    if (value == kPresentationMainContentType) {
        return ZaloContentType::Pptx;
    }
    return ZaloContentType::Unknown;
}

[[nodiscard]] std::string_view expectedMainPart(ZaloContentType type) noexcept
{
    switch (type) {
    case ZaloContentType::Docx:
        return "word/document.xml";
    case ZaloContentType::Xlsx:
        return "xl/workbook.xml";
    case ZaloContentType::Pptx:
        return "ppt/presentation.xml";
    default:
        return {};
    }
}

[[nodiscard]] bool rootMatchesMain(ZaloContentType type,
                                   const pugi::xml_node& root)
{
    switch (type) {
    case ZaloContentType::Docx:
        return elementMatches(root, "document", kWordNamespace) ||
               elementMatches(root, "document", kWordStrictNamespace);
    case ZaloContentType::Xlsx:
        return elementMatches(root, "workbook", kSpreadsheetNamespace) ||
               elementMatches(root, "workbook", kSpreadsheetStrictNamespace);
    case ZaloContentType::Pptx:
        return elementMatches(root, "presentation", kPresentationNamespace) ||
               elementMatches(root, "presentation", kPresentationStrictNamespace);
    default:
        return false;
    }
}

[[nodiscard]] bool validateXmlTree(const pugi::xml_document& document)
{
    struct PendingNode final {
        pugi::xml_node node;
        std::size_t depth = 0;
    };
    std::vector<PendingNode> pending;
    for (pugi::xml_node node = document.first_child(); node;
         node = node.next_sibling()) {
        pending.push_back(PendingNode{node, 0U});
    }

    std::size_t count = 0U;
    while (!pending.empty()) {
        const PendingNode current = pending.back();
        pending.pop_back();
        if (current.depth > kOoxmlMaxXmlDepth || count >= kOoxmlMaxXmlNodes) {
            return false;
        }
        ++count;
        for (pugi::xml_node child = current.node.first_child(); child;
             child = child.next_sibling()) {
            pending.push_back(PendingNode{child, current.depth + 1U});
        }
    }
    return true;
}

[[nodiscard]] bool xmlTopLevelWhitespace(std::string_view value) noexcept
{
    for (const unsigned char byte : value) {
        if (byte != 0x09U && byte != 0x0aU && byte != 0x0dU && byte != 0x20U) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parseXml(std::span<const std::uint8_t> bytes,
                            pugi::xml_document& document)
{
    if (bytes.empty()) {
        return false;
    }
    // Parse as a fragment so pugixml retains unparented character data and
    // every top-level element. The structural checks below then enforce the
    // single-root XML document grammar required by OOXML parts.
    const pugi::xml_parse_result result = document.load_buffer(
        bytes.data(), bytes.size(), pugi::parse_full | pugi::parse_fragment);
    if (!static_cast<bool>(result) || !document.document_element() ||
        !validateXmlTree(document)) {
        return false;
    }

    // OOXML parts must be one XML document, not a concatenation of roots. XML
    // declarations, comments, processing instructions, and whitespace outside
    // the root are harmless; doctypes and non-whitespace top-level text are not
    // accepted because they broaden the parser surface unnecessarily.
    std::size_t elementCount = 0U;
    for (pugi::xml_node node = document.first_child(); node;
         node = node.next_sibling()) {
        switch (node.type()) {
        case pugi::node_element:
            ++elementCount;
            if (elementCount > 1U) {
                return false;
            }
            break;
        case pugi::node_pcdata:
        case pugi::node_cdata:
            if (!xmlTopLevelWhitespace(node.value())) {
                return false;
            }
            break;
        case pugi::node_doctype:
            return false;
        case pugi::node_comment:
        case pugi::node_pi:
        case pugi::node_declaration:
            break;
        default:
            return false;
        }
    }
    return elementCount == 1U;
}

[[nodiscard]] bool extractXml(ScanContext& context, mz_zip_archive& archive,
                              const std::vector<RawZipEntry>& entries,
                              std::string_view name,
                              std::vector<std::uint8_t>& bytes)
{
    const EntryLookup lookup = lookupEntry(entries, name);
    if (lookup.count != 1U) {
        return false;
    }
    const RawZipEntry& entry = entries[lookup.index];
    if (entry.uncompressedSize == 0U ||
        entry.uncompressedSize > kOoxmlXmlBytes ||
        entry.uncompressedSize >
            static_cast<ByteSize>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    bytes.resize(static_cast<std::size_t>(entry.uncompressedSize));
    if (!mz_zip_reader_extract_to_mem_no_alloc(
            &archive, entry.index, bytes.data(), bytes.size(), 0U, nullptr,
            0U)) {
        return false;
    }
    return context.terminal == TerminalState::None && context.checkCancellation();
}

enum class OoxmlState {
    NoEvidence,
    Valid,
    Invalid
};

struct OoxmlResult final {
    OoxmlState state = OoxmlState::NoEvidence;
    ZaloContentType type = ZaloContentType::Unknown;
};

[[nodiscard]] OoxmlResult inspectOoxml(
    ScanContext& context, mz_zip_archive& archive,
    const std::vector<RawZipEntry>& entries)
{
    const EntryLookup contentTypesEntry =
        lookupEntry(entries, "[Content_Types].xml");
    const EntryLookup relationshipsEntry = lookupEntry(entries, "_rels/.rels");
    if (contentTypesEntry.count == 0U && relationshipsEntry.count == 0U) {
        return {};
    }
    if (contentTypesEntry.count != 1U || relationshipsEntry.count != 1U) {
        return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
    }

    std::vector<std::uint8_t> contentTypeBytes;
    std::vector<std::uint8_t> relationshipBytes;
    if (!extractXml(context, archive, entries, "[Content_Types].xml",
                    contentTypeBytes) ||
        !extractXml(context, archive, entries, "_rels/.rels", relationshipBytes) ||
        contentTypeBytes.size() + relationshipBytes.size() > kOoxmlTotalXmlBytes) {
        return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
    }

    pugi::xml_document contentTypes;
    pugi::xml_document relationships;
    if (!parseXml(contentTypeBytes, contentTypes) ||
        !parseXml(relationshipBytes, relationships)) {
        return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
    }
    const pugi::xml_node typesRoot = contentTypes.document_element();
    const pugi::xml_node relationshipsRoot = relationships.document_element();
    if (!elementMatches(typesRoot, "Types", kContentTypesNamespace) ||
        !(elementMatches(relationshipsRoot, "Relationships",
                         kRelationshipsNamespace) ||
          elementMatches(relationshipsRoot, "Relationships",
                         kStrictRelationshipsNamespace))) {
        return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
    }

    struct MainPart final {
        ZaloContentType type = ZaloContentType::Unknown;
        std::string part;
    };
    std::vector<MainPart> typedParts;
    for (pugi::xml_node node = typesRoot.first_child(); node;
         node = node.next_sibling()) {
        const QualifiedName nodeName = splitQualifiedName(node.name());
        if (!nodeName.valid || nodeName.local != "Override") {
            continue;
        }
        if (!elementMatches(node, "Override", kContentTypesNamespace)) {
            return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
        }
        const AttributeLookup partName =
            unqualifiedAttribute(node, "PartName");
        const AttributeLookup typeName =
            unqualifiedAttribute(node, "ContentType");
        if (partName.invalid || typeName.invalid || !partName.found ||
            !typeName.found) {
            return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
        }
        const ZaloContentType type = contentTypeFor(typeName.value);
        if (type == ZaloContentType::Unknown) {
            continue;
        }
        std::string normalizedPart;
        if (!normalizeOoxmlTarget(partName.value, normalizedPart) ||
            normalizedPart != expectedMainPart(type)) {
            return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
        }
        typedParts.push_back(MainPart{type, std::move(normalizedPart)});
    }
    if (typedParts.size() != 1U) {
        return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
    }

    std::string relationshipTarget;
    std::size_t officeRelationshipCount = 0U;
    for (pugi::xml_node node = relationshipsRoot.first_child(); node;
         node = node.next_sibling()) {
        const QualifiedName nodeName = splitQualifiedName(node.name());
        if (!nodeName.valid || nodeName.local != "Relationship") {
            continue;
        }
        if (!(elementMatches(node, "Relationship", kRelationshipsNamespace) ||
              elementMatches(node, "Relationship", kStrictRelationshipsNamespace))) {
            return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
        }
        const AttributeLookup typeName = unqualifiedAttribute(node, "Type");
        const AttributeLookup targetName =
            unqualifiedAttribute(node, "Target");
        const AttributeLookup targetMode =
            unqualifiedAttribute(node, "TargetMode");
        if (typeName.invalid || targetName.invalid || targetMode.invalid) {
            return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
        }
        if (!typeName.found || !isKnownOfficeRelationship(typeName.value)) {
            continue;
        }
        ++officeRelationshipCount;
        if (!targetName.found ||
            (targetMode.found && targetMode.value == "External") ||
            (targetMode.found && targetMode.value != "Internal")) {
            return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
        }
        if (officeRelationshipCount > 1U ||
            !normalizeOoxmlTarget(targetName.value, relationshipTarget)) {
            return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
        }
    }
    if (officeRelationshipCount != 1U ||
        relationshipTarget != typedParts.front().part) {
        return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
    }

    const EntryLookup mainEntry =
        lookupEntry(entries, typedParts.front().part);
    if (mainEntry.count != 1U) {
        return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
    }
    std::vector<std::uint8_t> mainBytes;
    if (!extractXml(context, archive, entries, typedParts.front().part,
                    mainBytes) ||
        contentTypeBytes.size() + relationshipBytes.size() + mainBytes.size() >
            kOoxmlTotalXmlBytes) {
        return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
    }
    pugi::xml_document mainDocument;
    if (!parseXml(mainBytes, mainDocument) ||
        !rootMatchesMain(typedParts.front().type,
                         mainDocument.document_element()) ||
        context.terminal != TerminalState::None) {
        return OoxmlResult{OoxmlState::Invalid, ZaloContentType::Unknown};
    }
    return OoxmlResult{OoxmlState::Valid, typedParts.front().type};
}

[[nodiscard]] std::optional<Candidate> parseZip(ScanContext& context,
                                                 ByteSize base)
{
    const auto zipEnd = findZipEnd(context, base);
    if (!zipEnd.has_value()) {
        // Fallback for streaming / incomplete ZIP archives without central directory:
        // Validate starting local file header signature and filename structure.
        if (base > context.view.size() || context.view.size() - base < 30U) {
            return std::nullopt;
        }
        std::array<std::uint8_t, 30> localHeader{};
        if (!context.readRange(base, localHeader) ||
            readLe32(localHeader.data()) != kZipLocalHeaderSignature) {
            return std::nullopt;
        }
        const std::uint16_t nameLength = readLe16(localHeader.data() + 26U);
        if (nameLength == 0U || nameLength > kZipNameBytes ||
            nameLength > context.view.size() - base - 30U) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> nameBytes(nameLength);
        ByteSize absoluteName = 0;
        if (!checkedAdd(base, 30U, absoluteName) ||
            !context.readRange(absoluteName, nameBytes)) {
            return std::nullopt;
        }
        bool nameValid = true;
        for (const std::uint8_t byte : nameBytes) {
            if (byte < 0x20U || byte == 0x7fU || byte == 0x00U) {
                nameValid = false;
                break;
            }
        }
        if (!nameValid) {
            return std::nullopt;
        }
        return makeCandidate(
            ZaloContentType::Zip,
            ZaloContentConfidence::Strong,
            base,
            context.view.size() - base,
            {ZaloContentEvidenceCode::ZipSignature,
             ZaloContentEvidenceCode::ZipLocalHeaders});
    }
    ZipReader reader;
    std::vector<RawZipEntry> entries;
    if (!initializeValidatedZip(context, base, *zipEnd, reader, entries)) {
        return std::nullopt;
    }
    const ByteSize archiveLength = zipEnd->end;

    const OoxmlResult ooxml = inspectOoxml(context, reader.archive, entries);
    if (context.terminal != TerminalState::None ||
        ooxml.state == OoxmlState::Invalid) {
        return std::nullopt;
    }
    const ZaloContentType type =
        ooxml.state == OoxmlState::Valid ? ooxml.type : ZaloContentType::Zip;
    std::vector<ZaloContentEvidenceCode> evidence{
        ZaloContentEvidenceCode::ZipSignature,
        ZaloContentEvidenceCode::ZipCentralDirectory,
        ZaloContentEvidenceCode::ZipLocalHeaders,
        ZaloContentEvidenceCode::ZipCrc32};
    if (ooxml.state == OoxmlState::Valid) {
        evidence.push_back(ZaloContentEvidenceCode::OoxmlContentTypes);
        evidence.push_back(ZaloContentEvidenceCode::OoxmlRelationships);
        evidence.push_back(ZaloContentEvidenceCode::OoxmlMainPart);
    }
    return makeCandidate(
        type, ooxml.state == OoxmlState::Valid
                  ? ZaloContentConfidence::Verified
                  : ZaloContentConfidence::Strong,
        base, archiveLength, std::move(evidence));
}

void appendUtf8CodePoint(std::string& output, std::uint32_t codePoint)
{
    if (codePoint <= 0x7fU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    }
}

[[nodiscard]] bool decodeUtf8CodePoint(std::string_view value,
                                       std::size_t& position,
                                       std::uint32_t& codePoint) noexcept
{
    if (position >= value.size()) {
        return false;
    }
    const auto byteAt = [&value](std::size_t index) {
        return static_cast<std::uint32_t>(
            static_cast<unsigned char>(value[index]));
    };
    const std::uint32_t lead = byteAt(position++);
    std::size_t continuationCount = 0U;
    if (lead <= 0x7fU) {
        codePoint = lead;
        return true;
    }
    if (lead >= 0xc2U && lead <= 0xdfU) {
        codePoint = lead & 0x1fU;
        continuationCount = 1U;
    } else if (lead >= 0xe0U && lead <= 0xefU) {
        codePoint = lead & 0x0fU;
        continuationCount = 2U;
    } else if (lead >= 0xf0U && lead <= 0xf4U) {
        codePoint = lead & 0x07U;
        continuationCount = 3U;
    } else {
        return false;
    }
    if (position + continuationCount > value.size()) {
        return false;
    }
    for (std::size_t i = 0; i < continuationCount; ++i) {
        const std::uint32_t continuation = byteAt(position++);
        if ((continuation & 0xc0U) != 0x80U) {
            return false;
        }
        codePoint = (codePoint << 6U) | (continuation & 0x3fU);
    }
    if ((continuationCount == 2U && codePoint < 0x800U) ||
        (continuationCount == 3U && codePoint < 0x10000U) ||
        codePoint > 0x10ffffU ||
        (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool semanticWhitespace(std::uint32_t codePoint) noexcept
{
    return codePoint == 0x09U || codePoint == 0x0aU || codePoint == 0x0bU ||
           codePoint == 0x0cU || codePoint == 0x0dU || codePoint == 0x20U ||
           (codePoint >= 0x2000U && codePoint <= 0x200aU) ||
           codePoint == 0x00a0U;
}

[[nodiscard]] bool semanticFormatControl(std::uint32_t codePoint) noexcept
{
    return codePoint == 0x00adU || codePoint == 0x061cU ||
           (codePoint >= 0x0600U && codePoint <= 0x0605U) ||
           (codePoint >= 0x200bU && codePoint <= 0x200fU) ||
           (codePoint >= 0x202aU && codePoint <= 0x202eU) ||
           (codePoint >= 0x2060U && codePoint <= 0x2064U) ||
           (codePoint >= 0x2066U && codePoint <= 0x2069U) ||
           codePoint == 0xfeffU;
}

[[nodiscard]] bool looksLikePrivateSemanticString(std::string_view value)
{
    if (value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos ||
        value.find('<') != std::string_view::npos ||
        value.find('>') != std::string_view::npos ||
        value.find("://") != std::string_view::npos) {
        return true;
    }
    if (value.size() >= 3U && std::isalpha(static_cast<unsigned char>(value[0])) &&
        value[1] == ':') {
        return true;
    }
    std::size_t hexDigits = 0U;
    bool onlyHashCharacters = !value.empty();
    for (const unsigned char byte : value) {
        if (std::isxdigit(byte) != 0) {
            ++hexDigits;
        } else if (byte != '-' && byte != '_' && byte != ' ') {
            onlyHashCharacters = false;
        }
    }
    return onlyHashCharacters && hexDigits >= 32U;
}

[[nodiscard]] std::optional<std::string> sanitizeSemanticString(
    std::string_view raw, std::size_t maximumBytes, std::size_t maximumScalars)
{
    std::string normalized;
    normalized.reserve(std::min(raw.size(), maximumBytes));
    bool pendingSpace = false;
    std::size_t scalarCount = 0U;
    for (std::size_t position = 0; position < raw.size();) {
        std::uint32_t codePoint = 0;
        if (!decodeUtf8CodePoint(raw, position, codePoint)) {
            return std::nullopt;
        }
        if (scalarCount >= maximumScalars) {
            break;
        }
        ++scalarCount;
        if (semanticWhitespace(codePoint)) {
            pendingSpace = !normalized.empty();
            continue;
        }
        if (codePoint < 0x20U || codePoint == 0x7fU ||
            semanticFormatControl(codePoint) ||
            (codePoint >= 0xfdd0U && codePoint <= 0xfdefU) ||
            (codePoint & 0xffffU) == 0xffffU) {
            return std::nullopt;
        }
        std::string encoded;
        appendUtf8CodePoint(encoded, codePoint);
        if (pendingSpace && !normalized.empty() &&
            normalized.size() + 1U <= maximumBytes) {
            normalized.push_back(' ');
        }
        pendingSpace = false;
        if (normalized.size() + encoded.size() > maximumBytes) {
            break;
        }
        normalized += encoded;
    }
    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    if (normalized.empty() || looksLikePrivateSemanticString(normalized)) {
        return std::nullopt;
    }
    return normalized;
}

[[nodiscard]] ZaloSemanticMetadata semanticResult(
    ZaloSemanticMetadataStatus status)
{
    ZaloSemanticMetadata result;
    result.status = status;
    return result;
}

[[nodiscard]] bool skipPdfLiteral(std::span<const std::uint8_t> bytes,
                                  std::size_t& position) noexcept
{
    if (position >= bytes.size() || bytes[position] != '(') {
        return false;
    }
    ++position;
    std::size_t depth = 1U;
    bool escaped = false;
    while (position < bytes.size()) {
        const std::uint8_t byte = bytes[position++];
        if (escaped) {
            escaped = false;
            if (byte == '\r' && position < bytes.size() && bytes[position] == '\n') {
                ++position;
            }
            continue;
        }
        if (byte == '\\') {
            escaped = true;
        } else if (byte == '(') {
            ++depth;
        } else if (byte == ')') {
            if (--depth == 0U) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool skipPdfHexString(std::span<const std::uint8_t> bytes,
                                    std::size_t& position) noexcept
{
    if (position >= bytes.size() || bytes[position] != '<' ||
        startsWith(bytes, position, "<<")) {
        return false;
    }
    ++position;
    while (position < bytes.size() && bytes[position] != '>') {
        if (!pdfWhitespace(bytes[position]) &&
            std::isxdigit(bytes[position]) == 0) {
            return false;
        }
        ++position;
    }
    if (position >= bytes.size()) {
        return false;
    }
    ++position;
    return true;
}

[[nodiscard]] std::optional<std::span<const std::uint8_t>> pdfDictionaryForObject(
    std::span<const std::uint8_t> bytes, const PdfStructure& structure,
    const PdfObjectReference& reference)
{
    const PdfXrefEntry* selected = nullptr;
    for (const PdfXrefEntry& entry : structure.entries) {
        if (entry.inUse && entry.objectNumber == reference.objectNumber &&
            entry.generation == reference.generation) {
            if (selected != nullptr) {
                return std::nullopt;
            }
            selected = &entry;
        }
    }
    if (selected == nullptr || selected->offset >= bytes.size()) {
        return std::nullopt;
    }
    ByteSize objectEnd = static_cast<ByteSize>(bytes.size());
    for (const PdfXrefEntry& entry : structure.entries) {
        if (entry.inUse && entry.offset > selected->offset &&
            entry.offset < objectEnd) {
            objectEnd = entry.offset;
        }
    }
    if (objectEnd <= selected->offset || objectEnd > bytes.size()) {
        return std::nullopt;
    }
    const auto object = bytes.subspan(static_cast<std::size_t>(selected->offset),
                                      static_cast<std::size_t>(
                                          objectEnd - selected->offset));
    std::size_t position = 0U;
    std::uint64_t objectNumber = 0;
    std::uint64_t generation = 0;
    if (!parsePdfUnsigned(object, position, objectNumber) ||
        !parsePdfUnsigned(object, position, generation)) {
        return std::nullopt;
    }
    skipPdfWhitespace(object, position);
    if (!startsWith(object, position, "obj")) {
        return std::nullopt;
    }
    position += 3U;
    while (position + 1U < object.size()) {
        if (object[position] == '(') {
            if (!skipPdfLiteral(object, position)) {
                return std::nullopt;
            }
            continue;
        }
        if (object[position] == '<' && !startsWith(object, position, "<<")) {
            if (!skipPdfHexString(object, position)) {
                return std::nullopt;
            }
            continue;
        }
        if (object[position] == '%') {
            while (position < object.size() && object[position] != '\r' &&
                   object[position] != '\n') {
                ++position;
            }
            continue;
        }
        if (startsWith(object, position, "<<")) {
            const auto end = findPdfDictionaryEnd(object, position);
            if (!end.has_value()) {
                return std::nullopt;
            }
            return object.subspan(position, *end + 2U - position);
        }
        ++position;
    }
    return std::nullopt;
}

struct PdfNameLookup final {
    bool found = false;
    bool ambiguous = false;
    std::size_t position = 0U;
};

[[nodiscard]] PdfNameLookup pdfNameLookup(
    std::span<const std::uint8_t> dictionary, std::string_view name)
{
    PdfNameLookup result;
    if (name.empty()) {
        return result;
    }
    for (std::size_t position = 0U; position + name.size() <= dictionary.size();
         ++position) {
        if (!startsWith(dictionary, position, name) ||
            (position != 0U && !pdfNameBoundary(dictionary[position - 1U])) ||
            (position + name.size() < dictionary.size() &&
             !pdfNameBoundary(dictionary[position + name.size()]))) {
            continue;
        }
        if (!result.found) {
            result.position = position;
            result.found = true;
        } else {
            result.ambiguous = true;
        }
    }
    return result;
}

[[nodiscard]] bool pdfParseNameAt(std::span<const std::uint8_t> bytes,
                                  std::size_t& position,
                                  std::string_view& value) noexcept
{
    skipPdfWhitespace(bytes, position);
    if (position >= bytes.size() || bytes[position] != '/') {
        return false;
    }
    const std::size_t start = ++position;
    while (position < bytes.size() && !pdfNameBoundary(bytes[position])) {
        ++position;
    }
    if (position == start) {
        return false;
    }
    value = std::string_view(reinterpret_cast<const char*>(bytes.data() + start),
                             position - start);
    return true;
}

[[nodiscard]] bool pdfParseReferenceAt(std::span<const std::uint8_t> bytes,
                                       std::size_t& position,
                                       PdfObjectReference& value) noexcept
{
    std::uint64_t object = 0;
    std::uint64_t generation = 0;
    if (!parsePdfUnsigned(bytes, position, object) ||
        !parsePdfUnsigned(bytes, position, generation)) {
        return false;
    }
    skipPdfWhitespace(bytes, position);
    if (position >= bytes.size() || bytes[position] != 'R') {
        return false;
    }
    ++position;
    value = PdfObjectReference{object, generation};
    return true;
}

[[nodiscard]] bool pdfParseReferenceValue(
    std::span<const std::uint8_t> dictionary, std::string_view name,
    PdfObjectReference& value, bool& found)
{
    const PdfNameLookup lookup = pdfNameLookup(dictionary, name);
    if (lookup.ambiguous) {
        return false;
    }
    found = lookup.found;
    if (!found) {
        return true;
    }
    std::size_t position = lookup.position + name.size();
    return pdfParseReferenceAt(dictionary, position, value);
}

[[nodiscard]] bool pdfParseUnsignedValue(
    std::span<const std::uint8_t> dictionary, std::string_view name,
    std::uint64_t& value, bool& found)
{
    const PdfNameLookup lookup = pdfNameLookup(dictionary, name);
    if (lookup.ambiguous) {
        return false;
    }
    found = lookup.found;
    if (!found) {
        return true;
    }
    std::size_t position = lookup.position + name.size();
    return parsePdfUnsigned(dictionary, position, value);
}

[[nodiscard]] bool pdfParseNameValue(
    std::span<const std::uint8_t> dictionary, std::string_view name,
    std::string_view& value, bool& found)
{
    const PdfNameLookup lookup = pdfNameLookup(dictionary, name);
    if (lookup.ambiguous) {
        return false;
    }
    found = lookup.found;
    if (!found) {
        return true;
    }
    std::size_t position = lookup.position + name.size();
    return pdfParseNameAt(dictionary, position, value);
}

[[nodiscard]] bool pdfParseReferenceArray(
    std::span<const std::uint8_t> dictionary, std::string_view name,
    std::vector<PdfObjectReference>& references, bool& found)
{
    const PdfNameLookup lookup = pdfNameLookup(dictionary, name);
    if (lookup.ambiguous) {
        return false;
    }
    found = lookup.found;
    if (!found) {
        return true;
    }
    std::size_t position = lookup.position + name.size();
    skipPdfWhitespace(dictionary, position);
    if (position >= dictionary.size() || dictionary[position] != '[') {
        return false;
    }
    ++position;
    references.clear();
    while (position < dictionary.size()) {
        skipPdfWhitespace(dictionary, position);
        if (position < dictionary.size() && dictionary[position] == ']') {
            ++position;
            return true;
        }
        if (references.size() >= kSemanticMaxPdfObjects) {
            return false;
        }
        PdfObjectReference reference;
        if (!pdfParseReferenceAt(dictionary, position, reference)) {
            return false;
        }
        references.push_back(reference);
    }
    return false;
}

[[nodiscard]] bool pdfParseLiteralBytes(
    std::span<const std::uint8_t> bytes, std::size_t& position,
    std::vector<std::uint8_t>& output)
{
    if (position >= bytes.size() || bytes[position] != '(') {
        return false;
    }
    ++position;
    std::size_t depth = 1U;
    bool escaped = false;
    output.clear();
    while (position < bytes.size()) {
        const std::uint8_t byte = bytes[position++];
        if (escaped) {
            escaped = false;
            switch (byte) {
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case '\r':
                if (position < bytes.size() && bytes[position] == '\n') {
                    ++position;
                }
                break;
            case '\n':
                break;
            default:
                if (byte >= '0' && byte <= '7') {
                    std::uint8_t value = static_cast<std::uint8_t>(byte - '0');
                    for (int count = 0; count < 2 && position < bytes.size() &&
                                       bytes[position] >= '0' &&
                                       bytes[position] <= '7';
                         ++count) {
                        value = static_cast<std::uint8_t>(
                            (value << 3U) + bytes[position++] - '0');
                    }
                    output.push_back(value);
                } else {
                    output.push_back(byte);
                }
                break;
            }
            continue;
        }
        if (byte == '\\') {
            escaped = true;
        } else if (byte == '(') {
            ++depth;
            output.push_back(byte);
        } else if (byte == ')') {
            if (--depth == 0U) {
                return true;
            }
            output.push_back(byte);
        } else {
            output.push_back(byte);
        }
    }
    return false;
}

[[nodiscard]] bool pdfParseHexBytes(
    std::span<const std::uint8_t> bytes, std::size_t& position,
    std::vector<std::uint8_t>& output)
{
    if (position >= bytes.size() || bytes[position] != '<' ||
        startsWith(bytes, position, "<<")) {
        return false;
    }
    ++position;
    output.clear();
    int highNibble = -1;
    while (position < bytes.size() && bytes[position] != '>') {
        const std::uint8_t byte = bytes[position++];
        if (pdfWhitespace(byte)) {
            continue;
        }
        if (std::isdigit(byte) == 0 &&
            (byte < 'a' || byte > 'f') && (byte < 'A' || byte > 'F')) {
            return false;
        }
        const int nibble = byte <= '9'
                               ? byte - '0'
                               : (byte <= 'F' ? byte - 'A' + 10 : byte - 'a' + 10);
        if (highNibble < 0) {
            highNibble = nibble;
        } else {
            output.push_back(static_cast<std::uint8_t>((highNibble << 4) | nibble));
            highNibble = -1;
        }
    }
    if (position >= bytes.size()) {
        return false;
    }
    ++position;
    if (highNibble >= 0) {
        output.push_back(static_cast<std::uint8_t>(highNibble << 4));
    }
    return true;
}

[[nodiscard]] std::optional<std::string> pdfBytesToUtf8(
    const std::vector<std::uint8_t>& bytes)
{
    std::string result;
    if (bytes.size() >= 2U && bytes[0] == 0xfeU && bytes[1] == 0xffU) {
        if ((bytes.size() - 2U) % 2U != 0U) {
            return std::nullopt;
        }
        for (std::size_t position = 2U; position < bytes.size(); position += 2U) {
            const std::uint32_t codePoint =
                (static_cast<std::uint32_t>(bytes[position]) << 8U) |
                bytes[position + 1U];
            if (codePoint >= 0xd800U && codePoint <= 0xdfffU) {
                return std::nullopt;
            }
            appendUtf8CodePoint(result, codePoint);
        }
    } else {
        result.reserve(bytes.size());
        for (const std::uint8_t byte : bytes) {
            if (byte < 0x80U) {
                result.push_back(static_cast<char>(byte));
            } else {
                // PDFDocEncoding is not guessed. A replacement is safer than
                // exposing an ambiguously decoded metadata value.
                appendUtf8CodePoint(result, 0xfffdU);
            }
        }
    }
    return result;
}

[[nodiscard]] bool pdfParseStringValue(
    std::span<const std::uint8_t> dictionary, std::string_view name,
    std::string& value, bool& found)
{
    const PdfNameLookup lookup = pdfNameLookup(dictionary, name);
    if (lookup.ambiguous) {
        return false;
    }
    found = lookup.found;
    if (!found) {
        return true;
    }
    std::size_t position = lookup.position + name.size();
    skipPdfWhitespace(dictionary, position);
    std::vector<std::uint8_t> bytes;
    if (position < dictionary.size() && dictionary[position] == '(') {
        if (!pdfParseLiteralBytes(dictionary, position, bytes)) {
            return false;
        }
    } else if (position < dictionary.size() && dictionary[position] == '<') {
        if (!pdfParseHexBytes(dictionary, position, bytes)) {
            return false;
        }
    } else {
        return false;
    }
    const auto utf8 = pdfBytesToUtf8(bytes);
    if (!utf8.has_value()) {
        return false;
    }
    value = *utf8;
    return true;
}

[[nodiscard]] bool pdfReferenceInList(
    const std::vector<PdfObjectReference>& values,
    const PdfObjectReference& wanted) noexcept
{
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

[[nodiscard]] bool countPdfPages(
    std::span<const std::uint8_t> bytes, const PdfStructure& structure,
    const PdfObjectReference& reference, std::vector<PdfObjectReference>& seen,
    std::size_t depth, std::size_t& count)
{
    if (depth > 64U || seen.size() >= kSemanticMaxPdfObjects ||
        pdfReferenceInList(seen, reference)) {
        return false;
    }
    const auto dictionary =
        pdfDictionaryForObject(bytes, structure, reference);
    if (!dictionary.has_value()) {
        return false;
    }
    seen.push_back(reference);
    std::string_view type;
    bool typeFound = false;
    if (!pdfParseNameValue(*dictionary, "/Type", type, typeFound) ||
        !typeFound) {
        return false;
    }
    if (type == "Page") {
        if (count >= kSemanticMaxPageCount) {
            return false;
        }
        ++count;
        return true;
    }
    if (type != "Pages") {
        return false;
    }
    std::uint64_t declaredCount = 0;
    bool countFound = false;
    if (!pdfParseUnsignedValue(*dictionary, "/Count", declaredCount,
                               countFound) ||
        !countFound || declaredCount > kSemanticMaxPageCount) {
        return false;
    }
    std::vector<PdfObjectReference> children;
    bool childrenFound = false;
    if (!pdfParseReferenceArray(*dictionary, "/Kids", children,
                                childrenFound) ||
        !childrenFound) {
        return false;
    }
    const std::size_t before = count;
    for (const PdfObjectReference& child : children) {
        if (!countPdfPages(bytes, structure, child, seen, depth + 1U, count)) {
            return false;
        }
    }
    return count - before == declaredCount;
}

[[nodiscard]] ZaloSemanticMetadata extractPdfSemanticMetadata(
    ScanContext& context)
{
    if (context.view.size() > kSemanticMaxPdfBytes) {
        return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
    }
    auto bytes = readVector(context, 0U, context.view.size());
    if (!bytes.has_value()) {
        if (context.terminal == TerminalState::Cancelled) {
            return semanticResult(ZaloSemanticMetadataStatus::Cancelled);
        }
        if (context.terminal == TerminalState::Changed) {
            return semanticResult(ZaloSemanticMetadataStatus::Changed);
        }
        if (context.budgetExceeded) {
            return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
        }
        return semanticResult(ZaloSemanticMetadataStatus::ReadError);
    }
    const std::span<const std::uint8_t> pdf(*bytes);
    if (!startsWith(pdf, 0U, "%PDF-")) {
        return semanticResult(ZaloSemanticMetadataStatus::Malformed);
    }
    const auto startxref = findLast(pdf, "startxref");
    if (!startxref.has_value()) {
        return semanticResult(ZaloSemanticMetadataStatus::Malformed);
    }
    std::size_t xrefPosition = *startxref + 9U;
    std::uint64_t xrefOffsetValue = 0;
    if (!parsePdfUnsigned(pdf, xrefPosition, xrefOffsetValue) ||
        xrefOffsetValue >= pdf.size()) {
        return semanticResult(ZaloSemanticMetadataStatus::Malformed);
    }
    const ByteSize xrefOffset = static_cast<ByteSize>(xrefOffsetValue);
    const ByteSize xrefLength =
        std::min<ByteSize>(static_cast<ByteSize>(pdf.size()) - xrefOffset,
                           kPdfXrefBytes);
    auto xref = readVector(context, xrefOffset, xrefLength);
    if (!xref.has_value()) {
        if (context.terminal == TerminalState::Cancelled) {
            return semanticResult(ZaloSemanticMetadataStatus::Cancelled);
        }
        if (context.terminal == TerminalState::Changed) {
            return semanticResult(ZaloSemanticMetadataStatus::Changed);
        }
        if (context.budgetExceeded) {
            return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
        }
        return semanticResult(ZaloSemanticMetadataStatus::ReadError);
    }
    PdfStructure structure;
    if (!parsePdfClassicXref(context, 0U, pdf.size(), xrefOffset, *xref,
                             &structure) ||
        context.terminal != TerminalState::None) {
        if (context.terminal == TerminalState::Cancelled) {
            return semanticResult(ZaloSemanticMetadataStatus::Cancelled);
        }
        if (context.terminal == TerminalState::Changed) {
            return semanticResult(ZaloSemanticMetadataStatus::Changed);
        }
        if (context.budgetExceeded) {
            return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
        }
        return semanticResult(ZaloSemanticMetadataStatus::Malformed);
    }
    if (structure.entries.size() > kSemanticMaxPdfObjects ||
        structure.infoReferenceMalformed) {
        return semanticResult(ZaloSemanticMetadataStatus::Malformed);
    }

    ZaloSemanticMetadata result;
    result.status = ZaloSemanticMetadataStatus::Available;
    bool hasAny = false;
    const auto rootDictionary =
        pdfDictionaryForObject(pdf, structure, structure.root);
    if (!rootDictionary.has_value()) {
        return semanticResult(ZaloSemanticMetadataStatus::Malformed);
    }
    PdfObjectReference pagesReference;
    bool pagesFound = false;
    if (!pdfParseReferenceValue(*rootDictionary, "/Pages", pagesReference,
                                pagesFound)) {
        return semanticResult(ZaloSemanticMetadataStatus::Malformed);
    }
    if (pagesFound) {
        std::vector<PdfObjectReference> seen;
        std::size_t pageCount = 0U;
        if (!countPdfPages(pdf, structure, pagesReference, seen, 0U,
                           pageCount)) {
            return semanticResult(ZaloSemanticMetadataStatus::Malformed);
        }
        result.pdfPageCount = static_cast<std::uint32_t>(pageCount);
        hasAny = true;
    }

    if (structure.info.has_value()) {
        const auto infoDictionary =
            pdfDictionaryForObject(pdf, structure, *structure.info);
        if (!infoDictionary.has_value()) {
            return semanticResult(ZaloSemanticMetadataStatus::Malformed);
        }
        for (const auto field : {std::pair{std::string_view{"/Title"},
                                            &result.title},
                                   std::pair{std::string_view{"/Author"},
                                             &result.author}}) {
            std::string raw;
            bool found = false;
            if (!pdfParseStringValue(*infoDictionary, field.first, raw, found)) {
                return semanticResult(ZaloSemanticMetadataStatus::Malformed);
            }
            if (!found || raw.empty()) {
                continue;
            }
            const auto sanitized =
                sanitizeSemanticString(raw, kZaloSemanticMaxStringBytes,
                                     kZaloSemanticMaxStringScalars);
            if (!sanitized.has_value()) {
                return semanticResult(ZaloSemanticMetadataStatus::Malformed);
            }
            *field.second = *sanitized;
            hasAny = true;
        }
    }
    if (!hasAny) {
        result.status = ZaloSemanticMetadataStatus::Unavailable;
    }
    return result;
}

[[nodiscard]] ZaloSemanticMetadata semanticContextResult(
    const ScanContext& context, ZaloSemanticMetadataStatus fallback)
{
    if (context.terminal == TerminalState::Cancelled) {
        return semanticResult(ZaloSemanticMetadataStatus::Cancelled);
    }
    if (context.terminal == TerminalState::Changed) {
        return semanticResult(ZaloSemanticMetadataStatus::Changed);
    }
    if (context.terminal == TerminalState::ReadError) {
        return semanticResult(ZaloSemanticMetadataStatus::ReadError);
    }
    if (context.budgetExceeded) {
        return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
    }
    return semanticResult(fallback);
}

class SemanticTextCollector final {
public:
    explicit SemanticTextCollector(ZaloSemanticMetadata& result) noexcept
        : m_result(result)
    {
    }

    [[nodiscard]] bool append(std::string_view fragment, bool heading)
    {
        if (m_full) {
            return false;
        }
        m_heading = m_heading || heading;
        if (m_discardCurrent) {
            return true;
        }
        if (fragment.size() > kSemanticMaxParagraphBytes -
                                std::min(m_current.size(),
                                         kSemanticMaxParagraphBytes)) {
            m_discardCurrent = true;
            m_current.clear();
            return true;
        }
        m_current.append(fragment);
        return true;
    }

    [[nodiscard]] bool flush()
    {
        if (m_discardCurrent || m_current.empty()) {
            m_current.clear();
            m_discardCurrent = false;
            m_heading = false;
            return !m_full;
        }
        const auto sanitized = sanitizeSemanticString(
            m_current, kZaloSemanticMaxTextBytes,
            kZaloSemanticMaxTextScalars);
        m_current.clear();
        m_discardCurrent = false;
        if (!sanitized.has_value()) {
            m_heading = false;
            return !m_full;
        }
        if (m_result.visibleText.size() >= kZaloSemanticMaxTextItems ||
            sanitized->size() > kZaloSemanticMaxTextBytes - m_totalBytes) {
            m_full = true;
            return false;
        }
        m_totalBytes += sanitized->size();
        m_result.visibleText.push_back(
            ZaloSemanticTextItem{m_heading, std::move(*sanitized)});
        m_heading = false;
        return true;
    }

    [[nodiscard]] bool full() const noexcept { return m_full; }

private:
    ZaloSemanticMetadata& m_result;
    std::string m_current;
    std::size_t m_totalBytes = 0U;
    bool m_heading = false;
    bool m_discardCurrent = false;
    bool m_full = false;
};

void pushChildrenInDocumentOrder(
    const pugi::xml_node& node, std::vector<pugi::xml_node>& pending)
{
    std::vector<pugi::xml_node> children;
    for (pugi::xml_node child = node.first_child(); child;
         child = child.next_sibling()) {
        children.push_back(child);
    }
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
        pending.push_back(*child);
    }
}

[[nodiscard]] bool collectMatchingText(
    ScanContext& context, const pugi::xml_node& root, std::string_view local,
    std::string_view namespaceUri, std::string& output, bool& truncated)
{
    std::vector<pugi::xml_node> pending;
    pending.push_back(root);
    while (!pending.empty()) {
        if (!context.checkCancellation()) {
            return false;
        }
        const pugi::xml_node node = pending.back();
        pending.pop_back();
        if (elementMatches(node, local, namespaceUri)) {
            const std::string_view text = node.child_value();
            if (text.size() > kSemanticMaxParagraphBytes -
                                std::min(output.size(),
                                         kSemanticMaxParagraphBytes)) {
                truncated = true;
            } else {
                output.append(text);
            }
        }
        pushChildrenInDocumentOrder(node, pending);
    }
    return true;
}

[[nodiscard]] bool parseSemanticUnsigned(std::string_view value,
                                          std::uint64_t& result) noexcept
{
    if (value.empty()) {
        return false;
    }
    result = 0U;
    for (const unsigned char byte : value) {
        if (byte < '0' || byte > '9') {
            return false;
        }
        const std::uint64_t digit = byte - '0';
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    return true;
}

[[nodiscard]] bool extractSemanticXml(
    ScanContext& context, mz_zip_archive& archive,
    const std::vector<RawZipEntry>& entries, std::string_view name,
    std::size_t& partCount, ByteSize& totalBytes,
    std::vector<std::uint8_t>& bytes, bool& missing, bool& limitExceeded)
{
    missing = false;
    limitExceeded = false;
    const EntryLookup lookup = lookupEntry(entries, name);
    if (lookup.count == 0U) {
        missing = true;
        return true;
    }
    if (lookup.count != 1U) {
        return false;
    }
    const RawZipEntry& entry = entries[lookup.index];
    if (partCount >= kSemanticMaxOoxmlParts ||
        entry.uncompressedSize > kOoxmlXmlBytes ||
        entry.uncompressedSize >
            static_cast<ByteSize>(std::numeric_limits<std::size_t>::max()) ||
        totalBytes > static_cast<ByteSize>(kOoxmlTotalXmlBytes) ||
        entry.uncompressedSize >
            static_cast<ByteSize>(kOoxmlTotalXmlBytes) - totalBytes) {
        limitExceeded = true;
        return false;
    }
    ++partCount;
    totalBytes += entry.uncompressedSize;
    if (!extractXml(context, archive, entries, name, bytes)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool coreField(
    const pugi::xml_document& document, std::string_view local,
    std::string_view namespaceUri, std::string& value, bool& found)
{
    found = false;
    value.clear();
    std::vector<pugi::xml_node> pending;
    pending.push_back(document.document_element());
    while (!pending.empty()) {
        const pugi::xml_node node = pending.back();
        pending.pop_back();
        if (elementMatches(node, local, namespaceUri)) {
            if (found) {
                return false;
            }
            found = true;
            value = node.child_value();
        }
        pushChildrenInDocumentOrder(node, pending);
    }
    return true;
}

[[nodiscard]] bool extractOoxmlCoreMetadata(
    ScanContext& context, mz_zip_archive& archive,
    const std::vector<RawZipEntry>& entries, std::size_t& partCount,
    ByteSize& totalBytes, ZaloSemanticMetadata& result, bool& hasAny)
{
    std::vector<std::uint8_t> bytes;
    bool missing = false;
    bool limitExceeded = false;
    if (!extractSemanticXml(context, archive, entries, "docProps/core.xml",
                             partCount, totalBytes, bytes, missing,
                             limitExceeded)) {
        if (limitExceeded) {
            result.status = ZaloSemanticMetadataStatus::LimitExceeded;
        }
        return false;
    }
    if (missing) {
        return true;
    }
    pugi::xml_document document;
    if (!parseXml(bytes, document)) {
        return false;
    }
    const pugi::xml_node root = document.document_element();
    if (!(elementMatches(root, "coreProperties", kCorePropertiesNamespace) ||
          elementMatches(root, "coreProperties",
                         "http://purl.oclc.org/ooxml/package/metadata/core-properties"))) {
        return false;
    }

    struct CoreField final {
        std::string_view local;
        std::string_view namespaceUri;
        std::optional<std::string>* destination;
    };
    const std::array<CoreField, 2> fields{{
        {"title", kDcNamespace, &result.title},
        {"creator", kDcNamespace, &result.creator},
    }};
    std::size_t fieldCount = 0U;
    for (const CoreField& field : fields) {
        if (++fieldCount > kSemanticMaxCoreFields) {
            return false;
        }
        std::string raw;
        bool found = false;
        if (!coreField(document, field.local, field.namespaceUri, raw, found)) {
            return false;
        }
        if (!found) {
            continue;
        }
        const auto sanitized = sanitizeSemanticString(
            raw, kZaloSemanticMaxStringBytes,
            kZaloSemanticMaxStringScalars);
        if (sanitized.has_value()) {
            *field.destination = std::move(*sanitized);
            hasAny = true;
        }
    }
    return context.checkCancellation();
}

[[nodiscard]] bool docxParagraphHeading(const pugi::xml_node& paragraph)
{
    std::vector<pugi::xml_node> pending;
    pending.push_back(paragraph);
    while (!pending.empty()) {
        const pugi::xml_node node = pending.back();
        pending.pop_back();
        if (elementMatches(node, "pStyle", kWordNamespace) ||
            elementMatches(node, "pStyle", kWordStrictNamespace)) {
            const AttributeLookup value = unqualifiedAttribute(node, "val");
            if (!value.invalid && value.found &&
                (value.value.starts_with("Heading") || value.value == "Title" ||
                 value.value == "Subtitle")) {
                return true;
            }
        }
        pushChildrenInDocumentOrder(node, pending);
    }
    return false;
}

[[nodiscard]] bool extractDocxText(
    ScanContext& context, const pugi::xml_document& document,
    ZaloSemanticMetadata& result)
{
    const pugi::xml_node root = document.document_element();
    if (!(rootMatchesMain(ZaloContentType::Docx, root))) {
        return false;
    }
    SemanticTextCollector collector(result);
    std::vector<pugi::xml_node> pending;
    pending.push_back(root);
    while (!pending.empty()) {
        if (!context.checkCancellation()) {
            return false;
        }
        const pugi::xml_node node = pending.back();
        pending.pop_back();
        if (elementMatches(node, "p", kWordNamespace) ||
            elementMatches(node, "p", kWordStrictNamespace)) {
            std::string raw;
            bool truncated = false;
            if (!collectMatchingText(context, node, "t", kWordNamespace, raw,
                                     truncated) ||
                !collectMatchingText(context, node, "t", kWordStrictNamespace,
                                     raw, truncated)) {
                return false;
            }
            if (!collector.append(raw, docxParagraphHeading(node)) ||
                !collector.flush() || collector.full()) {
                break;
            }
            continue;
        }
        pushChildrenInDocumentOrder(node, pending);
    }
    return context.checkCancellation();
}

[[nodiscard]] bool extractSharedStrings(
    ScanContext& context, mz_zip_archive& archive,
    const std::vector<RawZipEntry>& entries, std::size_t& partCount,
    ByteSize& totalBytes, std::vector<std::string>& sharedStrings,
    bool& present, bool& limitExceeded)
{
    present = false;
    limitExceeded = false;
    std::vector<std::uint8_t> bytes;
    bool missing = false;
    if (!extractSemanticXml(context, archive, entries, "xl/sharedStrings.xml",
                             partCount, totalBytes, bytes, missing,
                             limitExceeded)) {
        return false;
    }
    if (missing) {
        return true;
    }
    present = true;
    pugi::xml_document document;
    if (!parseXml(bytes, document)) {
        return false;
    }
    const pugi::xml_node root = document.document_element();
    if (!(elementMatches(root, "sst", kSpreadsheetNamespace) ||
          elementMatches(root, "sst", kSpreadsheetStrictNamespace))) {
        return false;
    }
    for (pugi::xml_node node = root.first_child(); node;
         node = node.next_sibling()) {
        if (!(elementMatches(node, "si", kSpreadsheetNamespace) ||
              elementMatches(node, "si", kSpreadsheetStrictNamespace))) {
            continue;
        }
        if (sharedStrings.size() >= kSemanticMaxSharedStrings) {
            limitExceeded = true;
            return false;
        }
        std::string value;
        bool truncated = false;
        if (!collectMatchingText(context, node, "t", kSpreadsheetNamespace,
                                 value, truncated) ||
            !collectMatchingText(context, node, "t",
                                 kSpreadsheetStrictNamespace, value,
                                 truncated)) {
            return false;
        }
        if (truncated) {
            value.clear();
        }
        sharedStrings.push_back(std::move(value));
    }
    return context.checkCancellation();
}

[[nodiscard]] std::optional<pugi::xml_node> uniqueChildElement(
    const pugi::xml_node& parent, std::string_view local,
    std::string_view namespaceUri)
{
    std::optional<pugi::xml_node> result;
    for (pugi::xml_node node = parent.first_child(); node;
         node = node.next_sibling()) {
        if (!elementMatches(node, local, namespaceUri)) {
            continue;
        }
        if (result.has_value()) {
            return std::nullopt;
        }
        result = node;
    }
    return result;
}

[[nodiscard]] bool extractWorksheetText(
    ScanContext& context, const pugi::xml_document& document,
    const std::vector<std::string>& sharedStrings, ZaloSemanticMetadata& result,
    bool& hasAny)
{
    const pugi::xml_node root = document.document_element();
    if (!(elementMatches(root, "worksheet", kSpreadsheetNamespace) ||
          elementMatches(root, "worksheet", kSpreadsheetStrictNamespace))) {
        return false;
    }
    SemanticTextCollector collector(result);
    std::vector<pugi::xml_node> pending;
    pending.push_back(root);
    std::uint64_t nextRow = 1U;
    while (!pending.empty()) {
        if (!context.checkCancellation()) {
            return false;
        }
        const pugi::xml_node node = pending.back();
        pending.pop_back();
        if (elementMatches(node, "row", kSpreadsheetNamespace) ||
            elementMatches(node, "row", kSpreadsheetStrictNamespace)) {
            std::uint64_t rowNumber = nextRow++;
            const AttributeLookup rowValue = unqualifiedAttribute(node, "r");
            if (rowValue.invalid ||
                (rowValue.found &&
                 !parseSemanticUnsigned(rowValue.value, rowNumber))) {
                return false;
            }
            for (pugi::xml_node cell = node.first_child(); cell;
                 cell = cell.next_sibling()) {
                if (!(elementMatches(cell, "c", kSpreadsheetNamespace) ||
                      elementMatches(cell, "c", kSpreadsheetStrictNamespace))) {
                    continue;
                }
                const AttributeLookup type = unqualifiedAttribute(cell, "t");
                if (type.invalid) {
                    return false;
                }
                if (!type.found ||
                    (type.value != "s" && type.value != "inlineStr" &&
                     type.value != "str")) {
                    continue;
                }
                std::string value;
                if (type.value == "s") {
                    const auto valueNode = uniqueChildElement(
                        cell, "v", kSpreadsheetNamespace);
                    const auto strictValueNode = uniqueChildElement(
                        cell, "v", kSpreadsheetStrictNamespace);
                    if (!valueNode.has_value() && !strictValueNode.has_value()) {
                        return false;
                    }
                    const pugi::xml_node selected =
                        valueNode.has_value() ? *valueNode : *strictValueNode;
                    std::uint64_t index = 0U;
                    if (!parseSemanticUnsigned(selected.child_value(), index) ||
                        index >= sharedStrings.size()) {
                        return false;
                    }
                    value = sharedStrings[static_cast<std::size_t>(index)];
                } else {
                    const auto inlineNode = uniqueChildElement(
                        cell, "is", kSpreadsheetNamespace);
                    const auto strictInlineNode = uniqueChildElement(
                        cell, "is", kSpreadsheetStrictNamespace);
                    const pugi::xml_node selected =
                        inlineNode.has_value() ? *inlineNode
                                               : (strictInlineNode.has_value()
                                                      ? *strictInlineNode
                                                      : pugi::xml_node{});
                    if (!selected) {
                        return false;
                    }
                    bool truncated = false;
                    if (!collectMatchingText(
                            context, selected, "t", kSpreadsheetNamespace, value,
                            truncated) ||
                        !collectMatchingText(context, selected, "t",
                                             kSpreadsheetStrictNamespace, value,
                                             truncated)) {
                        return false;
                    }
                    if (truncated) {
                        value.clear();
                    }
                }
                if (!value.empty()) {
                    const std::size_t before = result.visibleText.size();
                    if (!collector.append(value, rowNumber == 1U) ||
                        !collector.flush() || collector.full()) {
                        return true;
                    }
                    hasAny = hasAny || result.visibleText.size() > before;
                }
            }
            continue;
        }
        pushChildrenInDocumentOrder(node, pending);
    }
    return context.checkCancellation();
}

constexpr std::string_view kDrawingNamespace =
    "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr std::string_view kStrictDrawingNamespace =
    "http://purl.oclc.org/ooxml/drawingml/main";

[[nodiscard]] bool pptShapeHeading(const pugi::xml_node& shape)
{
    std::vector<pugi::xml_node> pending;
    pending.push_back(shape);
    while (!pending.empty()) {
        const pugi::xml_node node = pending.back();
        pending.pop_back();
        if (elementMatches(node, "ph", kPresentationNamespace) ||
            elementMatches(node, "ph", kPresentationStrictNamespace)) {
            const AttributeLookup type = unqualifiedAttribute(node, "type");
            if (!type.invalid && type.found &&
                (type.value == "title" || type.value == "ctrTitle")) {
                return true;
            }
        }
        pushChildrenInDocumentOrder(node, pending);
    }
    return false;
}

[[nodiscard]] bool extractPptSlideText(
    ScanContext& context, const pugi::xml_document& document,
    ZaloSemanticMetadata& result, bool& hasAny)
{
    const pugi::xml_node root = document.document_element();
    if (!(elementMatches(root, "sld", kPresentationNamespace) ||
          elementMatches(root, "sld", kPresentationStrictNamespace))) {
        return false;
    }
    SemanticTextCollector collector(result);
    std::vector<pugi::xml_node> pending;
    pending.push_back(root);
    while (!pending.empty()) {
        if (!context.checkCancellation()) {
            return false;
        }
        const pugi::xml_node node = pending.back();
        pending.pop_back();
        const bool shape = elementMatches(node, "sp", kPresentationNamespace) ||
                           elementMatches(node, "sp",
                                          kPresentationStrictNamespace);
        const bool graphic =
            elementMatches(node, "graphicFrame", kPresentationNamespace) ||
            elementMatches(node, "graphicFrame", kPresentationStrictNamespace);
        if (shape || graphic) {
            std::string value;
            bool truncated = false;
            if (!collectMatchingText(context, node, "t", kDrawingNamespace,
                                     value, truncated) ||
                !collectMatchingText(context, node, "t", kStrictDrawingNamespace,
                                     value, truncated)) {
                return false;
            }
            if (!value.empty()) {
                hasAny = true;
                if (truncated) {
                    value.clear();
                }
                if (!value.empty() &&
                    (!collector.append(value, shape && pptShapeHeading(node)) ||
                     !collector.flush() || collector.full())) {
                    return true;
                }
            }
            continue;
        }
        pushChildrenInDocumentOrder(node, pending);
    }
    return context.checkCancellation();
}

[[nodiscard]] bool semanticPartName(std::string_view name,
                                     std::string_view prefix) noexcept
{
    return name.starts_with(prefix) && name.ends_with(".xml") &&
           name.size() > prefix.size() + 4U;
}

[[nodiscard]] ZaloSemanticMetadata extractOoxmlSemanticMetadata(
    ScanContext& context, ZaloContentType type)
{
    if (context.view.size() > kSemanticMaxOoxmlBytes) {
        return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
    }
    const auto zipEnd = findZipEnd(context, 0U);
    if (!zipEnd.has_value()) {
        return semanticContextResult(context, ZaloSemanticMetadataStatus::Malformed);
    }
    ZipReader reader;
    std::vector<RawZipEntry> entries;
    if (!initializeValidatedZip(context, 0U, *zipEnd, reader, entries)) {
        return semanticContextResult(context, ZaloSemanticMetadataStatus::Malformed);
    }
    if (entries.size() > kSemanticMaxOoxmlParts) {
        return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
    }
    const OoxmlResult validation = inspectOoxml(context, reader.archive, entries);
    if (context.terminal != TerminalState::None) {
        return semanticContextResult(context, ZaloSemanticMetadataStatus::Malformed);
    }
    if (validation.state != OoxmlState::Valid || validation.type != type) {
        return semanticResult(ZaloSemanticMetadataStatus::Malformed);
    }

    ZaloSemanticMetadata result;
    result.status = ZaloSemanticMetadataStatus::Available;
    bool hasAny = false;
    std::size_t partCount = 0U;
    ByteSize totalBytes = 0U;
    if (!extractOoxmlCoreMetadata(context, reader.archive, entries, partCount,
                                  totalBytes, result, hasAny)) {
        if (result.status == ZaloSemanticMetadataStatus::LimitExceeded) {
            return result;
        }
        return semanticContextResult(context, ZaloSemanticMetadataStatus::Malformed);
    }

    const std::string_view mainPart = expectedMainPart(type);
    std::vector<std::uint8_t> mainBytes;
    bool missing = false;
    bool limitExceeded = false;
    if (!extractSemanticXml(context, reader.archive, entries, mainPart,
                             partCount, totalBytes, mainBytes, missing,
                             limitExceeded) ||
        missing) {
        if (limitExceeded) {
            return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
        }
        return semanticContextResult(context, ZaloSemanticMetadataStatus::Malformed);
    }
    pugi::xml_document mainDocument;
    if (!parseXml(mainBytes, mainDocument)) {
        return semanticResult(ZaloSemanticMetadataStatus::Malformed);
    }
    if (type == ZaloContentType::Docx) {
        if (!extractDocxText(context, mainDocument, result)) {
            return semanticContextResult(context,
                                         ZaloSemanticMetadataStatus::Malformed);
        }
        hasAny = hasAny || !result.visibleText.empty();
    } else if (type == ZaloContentType::Xlsx) {
        std::vector<std::string> sharedStrings;
        bool sharedPresent = false;
        if (!extractSharedStrings(context, reader.archive, entries, partCount,
                                  totalBytes, sharedStrings, sharedPresent,
                                  limitExceeded)) {
            if (limitExceeded) {
                return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
            }
            return semanticContextResult(context,
                                         ZaloSemanticMetadataStatus::Malformed);
        }
        (void)sharedPresent;
        std::vector<const RawZipEntry*> worksheets;
        for (const RawZipEntry& entry : entries) {
            if (semanticPartName(entry.name, "xl/worksheets/")) {
                worksheets.push_back(&entry);
            }
        }
        if (worksheets.size() > kSemanticMaxOoxmlParts - partCount) {
            return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
        }
        std::sort(worksheets.begin(), worksheets.end(),
                  [](const RawZipEntry* left, const RawZipEntry* right) {
                      return left->name < right->name;
                  });
        for (const RawZipEntry* worksheet : worksheets) {
            std::vector<std::uint8_t> worksheetBytes;
            bool worksheetMissing = false;
            if (!extractSemanticXml(context, reader.archive, entries,
                                     worksheet->name, partCount, totalBytes,
                                     worksheetBytes, worksheetMissing,
                                     limitExceeded) ||
                worksheetMissing) {
                if (limitExceeded) {
                    return semanticResult(
                        ZaloSemanticMetadataStatus::LimitExceeded);
                }
                return semanticContextResult(
                    context, ZaloSemanticMetadataStatus::Malformed);
            }
            pugi::xml_document worksheetDocument;
            if (!parseXml(worksheetBytes, worksheetDocument) ||
                !extractWorksheetText(context, worksheetDocument, sharedStrings,
                                      result, hasAny)) {
                return semanticContextResult(
                    context, ZaloSemanticMetadataStatus::Malformed);
            }
            if (result.visibleText.size() >= kZaloSemanticMaxTextItems) {
                break;
            }
        }
    } else if (type == ZaloContentType::Pptx) {
        std::vector<const RawZipEntry*> slides;
        for (const RawZipEntry& entry : entries) {
            if (semanticPartName(entry.name, "ppt/slides/slide")) {
                slides.push_back(&entry);
            }
        }
        if (slides.size() > kSemanticMaxOoxmlParts - partCount) {
            return semanticResult(ZaloSemanticMetadataStatus::LimitExceeded);
        }
        std::sort(slides.begin(), slides.end(),
                  [](const RawZipEntry* left, const RawZipEntry* right) {
                      return left->name < right->name;
                  });
        for (const RawZipEntry* slide : slides) {
            std::vector<std::uint8_t> slideBytes;
            bool slideMissing = false;
            if (!extractSemanticXml(context, reader.archive, entries,
                                     slide->name, partCount, totalBytes,
                                     slideBytes, slideMissing, limitExceeded) ||
                slideMissing) {
                if (limitExceeded) {
                    return semanticResult(
                        ZaloSemanticMetadataStatus::LimitExceeded);
                }
                return semanticContextResult(
                    context, ZaloSemanticMetadataStatus::Malformed);
            }
            pugi::xml_document slideDocument;
            if (!parseXml(slideBytes, slideDocument) ||
                !extractPptSlideText(context, slideDocument, result, hasAny)) {
                return semanticContextResult(
                    context, ZaloSemanticMetadataStatus::Malformed);
            }
            if (result.visibleText.size() >= kZaloSemanticMaxTextItems) {
                break;
            }
        }
    }
    if (!context.checkCancellation()) {
        return semanticContextResult(context,
                                     ZaloSemanticMetadataStatus::Malformed);
    }
    if (!hasAny) {
        result.status = ZaloSemanticMetadataStatus::Unavailable;
    }
    return result;
}

[[nodiscard]] bool isJpegSignature(std::span<const std::uint8_t> bytes,
                                   std::size_t position) noexcept
{
    return position + 2U <= bytes.size() && bytes[position] == 0xffU &&
           bytes[position + 1U] == 0xd8U;
}

[[nodiscard]] bool isPdfSignature(std::span<const std::uint8_t> bytes,
                                  std::size_t position) noexcept
{
    return position + 5U <= bytes.size() && bytes[position] == '%' &&
           bytes[position + 1U] == 'P' && bytes[position + 2U] == 'D' &&
           bytes[position + 3U] == 'F' && bytes[position + 4U] == '-';
}

[[nodiscard]] bool isPngSignature(std::span<const std::uint8_t> bytes,
                                  std::size_t position) noexcept
{
    return position + 8U <= bytes.size() &&
           bytes[position] == 0x89U && bytes[position + 1U] == 'P' &&
           bytes[position + 2U] == 'N' && bytes[position + 3U] == 'G' &&
           bytes[position + 4U] == 0x0dU && bytes[position + 5U] == 0x0aU &&
           bytes[position + 6U] == 0x1aU && bytes[position + 7U] == 0x0aU;
}

[[nodiscard]] bool isWebpSignature(std::span<const std::uint8_t> bytes,
                                   std::size_t position) noexcept
{
    return position + 12U <= bytes.size() &&
           bytes[position] == 'R' && bytes[position + 1U] == 'I' &&
           bytes[position + 2U] == 'F' && bytes[position + 3U] == 'F' &&
           bytes[position + 8U] == 'W' && bytes[position + 9U] == 'E' &&
           bytes[position + 10U] == 'B' && bytes[position + 11U] == 'P';
}

[[nodiscard]] bool isGifSignature(std::span<const std::uint8_t> bytes,
                                  std::size_t position) noexcept
{
    return position + 6U <= bytes.size() &&
           bytes[position] == 'G' && bytes[position + 1U] == 'I' &&
           bytes[position + 2U] == 'F' && bytes[position + 3U] == '8' &&
           (bytes[position + 4U] == '7' || bytes[position + 4U] == '9') &&
           bytes[position + 5U] == 'a';
}

[[nodiscard]] bool isMp4Signature(std::span<const std::uint8_t> bytes,
                                  std::size_t position) noexcept
{
    if (position + 8U > bytes.size()) {
        return false;
    }
    return (bytes[position + 4U] == 'f' && bytes[position + 5U] == 't' &&
            bytes[position + 6U] == 'y' && bytes[position + 7U] == 'p') ||
           (position == 0U && bytes[position + 4U] == 'm' &&
            bytes[position + 5U] == 'o' && bytes[position + 6U] == 'o' &&
            bytes[position + 7U] == 'v');
}

[[nodiscard]] bool isZipSignature(std::span<const std::uint8_t> bytes,
                                  std::size_t position) noexcept
{
    if (position + 4U > bytes.size() || bytes[position] != 'P' ||
        bytes[position + 1U] != 'K') {
        return false;
    }
    return (bytes[position + 2U] == 0x03U && bytes[position + 3U] == 0x04U) ||
           (bytes[position + 2U] == 0x05U && bytes[position + 3U] == 0x06U);
}

[[nodiscard]] std::vector<Candidate> parseAtSignatures(
    ScanContext& context, ByteSize offset,
    std::span<const std::uint8_t> knownBytes, std::size_t knownPosition)
{
    std::vector<Candidate> candidates;
    if (isJpegSignature(knownBytes, knownPosition)) {
        if (auto candidate = parseJpeg(context, offset); candidate.has_value()) {
            candidates.push_back(std::move(*candidate));
        }
    } else if (isPngSignature(knownBytes, knownPosition)) {
        if (auto candidate = parsePng(context, offset); candidate.has_value()) {
            candidates.push_back(std::move(*candidate));
        }
    } else if (isWebpSignature(knownBytes, knownPosition)) {
        if (auto candidate = parseWebp(context, offset); candidate.has_value()) {
            candidates.push_back(std::move(*candidate));
        }
    } else if (isGifSignature(knownBytes, knownPosition)) {
        if (auto candidate = parseGif(context, offset); candidate.has_value()) {
            candidates.push_back(std::move(*candidate));
        }
    } else if (isMp4Signature(knownBytes, knownPosition)) {
        if (auto candidate = parseMp4(context, offset); candidate.has_value()) {
            candidates.push_back(std::move(*candidate));
        }
    } else if (isPdfSignature(knownBytes, knownPosition)) {
        if (auto candidate = parsePdf(context, offset); candidate.has_value()) {
            candidates.push_back(std::move(*candidate));
        }
    } else if (isZipSignature(knownBytes, knownPosition)) {
        if (auto candidate = parseZip(context, offset); candidate.has_value()) {
            candidates.push_back(std::move(*candidate));
        }
    }
    return candidates;
}

[[nodiscard]] bool sameCandidate(const Candidate& left,
                                 const Candidate& right) noexcept
{
    return left.offset == right.offset && left.length == right.length &&
           left.type == right.type;
}

[[nodiscard]] std::vector<Candidate> uniqueCandidates(
    std::vector<Candidate> candidates)
{
    std::vector<Candidate> result;
    result.reserve(candidates.size());
    for (Candidate& candidate : candidates) {
        const auto duplicate = std::find_if(
            result.begin(), result.end(), [&](const Candidate& existing) {
                return sameCandidate(existing, candidate);
            });
        if (duplicate == result.end()) {
            result.push_back(std::move(candidate));
        } else {
            for (const auto code : candidate.evidence) {
                addEvidence(duplicate->evidence, code);
            }
            if (candidate.confidence > duplicate->confidence) {
                duplicate->confidence = candidate.confidence;
            }
        }
    }
    return result;
}

[[nodiscard]] ZaloContentResult unknownResult(
    ZaloContentStatus status = ZaloContentStatus::Unknown)
{
    ZaloContentResult result;
    result.status = status;
    switch (status) {
    case ZaloContentStatus::Unknown:
        result.description = "Unresolved app-managed binary";
        break;
    case ZaloContentStatus::Ambiguous:
        result.description = "Ambiguous app-managed binary candidates";
        break;
    case ZaloContentStatus::Cancelled:
        result.description = "Content identification cancelled";
        break;
    case ZaloContentStatus::ReadError:
        result.description = "Payload read failed during content identification";
        break;
    case ZaloContentStatus::Changed:
        result.description = "Payload changed during content identification";
        break;
    case ZaloContentStatus::Identified:
        break;
    }
    return result;
}

[[nodiscard]] ZaloContentStatus statusForTerminal(TerminalState terminal) noexcept
{
    switch (terminal) {
    case TerminalState::Cancelled:
        return ZaloContentStatus::Cancelled;
    case TerminalState::ReadError:
        return ZaloContentStatus::ReadError;
    case TerminalState::Changed:
        return ZaloContentStatus::Changed;
    case TerminalState::None:
        return ZaloContentStatus::Unknown;
    }
    return ZaloContentStatus::Unknown;
}

[[nodiscard]] ZaloContentResult resultForContext(const ScanContext& context)
{
    return unknownResult(statusForTerminal(context.terminal));
}

[[nodiscard]] ZaloContentResult resultFromCandidate(const Candidate& candidate)
{
    ZaloContentResult result;
    result.status = ZaloContentStatus::Identified;
    result.type = candidate.type;
    result.method = candidate.method;
    result.confidence = candidate.confidence;
    result.wrapper = candidate.offset != 0U;
    result.masked = candidate.masked;
    result.maskByte = candidate.maskByte;
    result.payloadOffset = candidate.offset;
    result.payloadLength = candidate.length;
    result.imageDimensions = candidate.dimensions;
    result.jpegDimensions = candidate.dimensions;
    result.videoMetadata = candidate.videoMetadata;
    result.evidence = candidate.evidence;
    switch (candidate.type) {
    case ZaloContentType::Jpeg:
        result.description = "JPEG image";
        break;
    case ZaloContentType::Png:
        result.description = "PNG image";
        break;
    case ZaloContentType::Webp:
        result.description = "WebP image";
        break;
    case ZaloContentType::Gif:
        result.description = "GIF image";
        break;
    case ZaloContentType::Mp4:
        result.description = "MP4 video";
        break;
    case ZaloContentType::Mov:
        result.description = "QuickTime video";
        break;
    case ZaloContentType::Pdf:
        result.description = "PDF document";
        break;
    case ZaloContentType::Zip:
        result.description = "ZIP archive";
        break;
    case ZaloContentType::Docx:
        result.description = "DOCX document";
        break;
    case ZaloContentType::Xlsx:
        result.description = "XLSX workbook";
        break;
    case ZaloContentType::Pptx:
        result.description = "PPTX presentation";
        break;
    case ZaloContentType::Unknown:
        result.description = "Unresolved app-managed binary";
        break;
    }
    if (candidate.masked) {
        result.description += " (transit payload)";
    }
    return result;
}

[[nodiscard]] ZaloContentResult identifyImpl(const PayloadView& payload,
                                             PayloadCancellation cancellation)
{
    ScanContext context(payload, std::move(cancellation));
    if (!context.checkCancellation()) {
        return resultForContext(context);
    }
    if (!payload.valid()) {
        return unknownResult(ZaloContentStatus::ReadError);
    }

    const ByteSize payloadSize = payload.size();
    std::vector<Candidate> direct;
    if (payloadSize >= 2U) {
        const ByteSize prefixLength = std::min<ByteSize>(payloadSize, 16U);
        auto prefix = readVector(context, 0U, prefixLength);
        if (!prefix.has_value()) {
            return context.terminal == TerminalState::None
                       ? unknownResult()
                       : resultForContext(context);
        }
        auto parsed = parseAtSignatures(context, 0U, *prefix, 0U);
        direct.insert(direct.end(), std::make_move_iterator(parsed.begin()),
                     std::make_move_iterator(parsed.end()));
        if (!direct.empty()) {
            direct = uniqueCandidates(std::move(direct));
            if (direct.size() == 1U) {
                return resultFromCandidate(direct.front());
            }
            return unknownResult(ZaloContentStatus::Ambiguous);
        }
    }
    if (context.terminal != TerminalState::None) {
        return resultForContext(context);
    }

    const ByteSize scanLength =
        std::min<ByteSize>(payloadSize, kZaloContentMaxScanBytes);
    auto scan = readVector(context, 0U, scanLength);
    if (!scan.has_value()) {
        return context.terminal == TerminalState::None
                   ? unknownResult()
                   : resultForContext(context);
    }
    const std::span<const std::uint8_t> scanBytes(*scan);
    std::vector<std::size_t> offsets;
    offsets.reserve(kZaloContentMaxCandidates);
    bool overCandidateLimit = false;
    for (std::size_t position = 0; position < scanBytes.size(); ++position) {
        if (!isJpegSignature(scanBytes, position) &&
            !isPngSignature(scanBytes, position) &&
            !isWebpSignature(scanBytes, position) &&
            !isGifSignature(scanBytes, position) &&
            !isMp4Signature(scanBytes, position) &&
            !isPdfSignature(scanBytes, position) &&
            !isZipSignature(scanBytes, position)) {
            continue;
        }
        if (position == 0U) {
            continue;
        }
        if (offsets.size() >= kZaloContentMaxCandidates) {
            overCandidateLimit = true;
            break;
        }
        offsets.push_back(position);
    }

    std::vector<Candidate> candidates;
    candidates.reserve(kZaloContentMaxCandidates);
    for (const std::size_t position : offsets) {
        auto parsed = parseAtSignatures(context, static_cast<ByteSize>(position),
                                        scanBytes, position);
        candidates.insert(candidates.end(),
                          std::make_move_iterator(parsed.begin()),
                          std::make_move_iterator(parsed.end()));
        if (context.terminal != TerminalState::None) {
            break;
        }
    }
    if (context.terminal != TerminalState::None) {
        return resultForContext(context);
    }
    candidates = uniqueCandidates(std::move(candidates));
    if (overCandidateLimit && !candidates.empty()) {
        return unknownResult(ZaloContentStatus::Ambiguous);
    }
    if (candidates.empty()) {
        // If no direct or embedded candidate matched, probe candidate reversible transforms
        // (e.g. Zalo transit cache XOR masks)
        constexpr std::array<std::uint8_t, 1> kZaloCandidateMasks = {0x93};
        for (const std::uint8_t mask : kZaloCandidateMasks) {
            if (!context.checkCancellation()) {
                return resultForContext(context);
            }
            if (scanBytes.size() >= 2U) {
                std::vector<std::uint8_t> unmaskedPrefix(scanBytes.begin(), scanBytes.end());
                for (auto& b : unmaskedPrefix) {
                    b ^= mask;
                }
                if (isJpegSignature(unmaskedPrefix, 0U) ||
                    isPngSignature(unmaskedPrefix, 0U) ||
                    isWebpSignature(unmaskedPrefix, 0U) ||
                    isGifSignature(unmaskedPrefix, 0U) ||
                    isMp4Signature(unmaskedPrefix, 0U) ||
                    isPdfSignature(unmaskedPrefix, 0U) ||
                    isZipSignature(unmaskedPrefix, 0U)) {
                    ScanContext maskedContext(payload, cancellation);
                    maskedContext.xorMask = mask;
                    auto parsed = parseAtSignatures(maskedContext, 0U, unmaskedPrefix, 0U);
                    if (!parsed.empty()) {
                        parsed = uniqueCandidates(std::move(parsed));
                        if (parsed.size() == 1U) {
                            Candidate c = std::move(parsed.front());
                            c.masked = true;
                            c.maskByte = mask;
                            addEvidence(c.evidence, ZaloContentEvidenceCode::MaskedPayload);
                            return resultFromCandidate(c);
                        }
                    }
                }
            }
        }
        return unknownResult();
    }
    if (candidates.size() != 1U) {
        return unknownResult(ZaloContentStatus::Ambiguous);
    }
    return resultFromCandidate(candidates.front());
}

}  // namespace

const char* toString(ZaloContentType type) noexcept
{
    switch (type) {
    case ZaloContentType::Unknown:
        return "unknown";
    case ZaloContentType::Jpeg:
        return "jpeg";
    case ZaloContentType::Png:
        return "png";
    case ZaloContentType::Webp:
        return "webp";
    case ZaloContentType::Gif:
        return "gif";
    case ZaloContentType::Mp4:
        return "mp4";
    case ZaloContentType::Mov:
        return "mov";
    case ZaloContentType::Pdf:
        return "pdf";
    case ZaloContentType::Zip:
        return "zip";
    case ZaloContentType::Docx:
        return "docx";
    case ZaloContentType::Xlsx:
        return "xlsx";
    case ZaloContentType::Pptx:
        return "pptx";
    }
    return "unknown";
}

const char* toString(ZaloIdentificationMethod method) noexcept
{
    switch (method) {
    case ZaloIdentificationMethod::None:
        return "none";
    case ZaloIdentificationMethod::Signature:
        return "signature";
    case ZaloIdentificationMethod::Structural:
        return "structural";
    case ZaloIdentificationMethod::EmbeddedPayload:
        return "embedded-payload";
    }
    return "none";
}

const char* toString(ZaloContentConfidence confidence) noexcept
{
    switch (confidence) {
    case ZaloContentConfidence::Unknown:
        return "unknown";
    case ZaloContentConfidence::Medium:
        return "medium";
    case ZaloContentConfidence::Strong:
        return "strong";
    case ZaloContentConfidence::Verified:
        return "verified";
    }
    return "unknown";
}

const char* toString(ZaloContentStatus status) noexcept
{
    switch (status) {
    case ZaloContentStatus::Identified:
        return "identified";
    case ZaloContentStatus::Unknown:
        return "unknown";
    case ZaloContentStatus::Ambiguous:
        return "ambiguous";
    case ZaloContentStatus::Cancelled:
        return "cancelled";
    case ZaloContentStatus::ReadError:
        return "read-error";
    case ZaloContentStatus::Changed:
        return "changed";
    }
    return "unknown";
}

const char* toString(ZaloSemanticMetadataStatus status) noexcept
{
    switch (status) {
    case ZaloSemanticMetadataStatus::NotRequested:
        return "not-requested";
    case ZaloSemanticMetadataStatus::NotApplicable:
        return "not-applicable";
    case ZaloSemanticMetadataStatus::Available:
        return "available";
    case ZaloSemanticMetadataStatus::Unavailable:
        return "unavailable";
    case ZaloSemanticMetadataStatus::Malformed:
        return "malformed";
    case ZaloSemanticMetadataStatus::Ambiguous:
        return "ambiguous";
    case ZaloSemanticMetadataStatus::LimitExceeded:
        return "limit-exceeded";
    case ZaloSemanticMetadataStatus::Cancelled:
        return "cancelled";
    case ZaloSemanticMetadataStatus::ReadError:
        return "read-error";
    case ZaloSemanticMetadataStatus::Changed:
        return "changed";
    }
    return "not-requested";
}

const char* toString(ZaloIdentitySource source) noexcept
{
    switch (source) {
    case ZaloIdentitySource::Unknown:
        return "unknown";
    case ZaloIdentitySource::OriginalFilename:
        return "original_filename";
    case ZaloIdentitySource::EmbeddedMetadata:
        return "embedded_metadata";
    case ZaloIdentitySource::DocumentStructure:
        return "document_structure";
    case ZaloIdentitySource::MediaMetadata:
        return "media_metadata";
    case ZaloIdentitySource::BoundedContentProbe:
        return "bounded_content_probe";
    case ZaloIdentitySource::VisualPreview:
        return "visual_preview";
    }
    return "unknown";
}

const char* toString(ZaloPreviewKind kind) noexcept
{
    switch (kind) {
    case ZaloPreviewKind::None:
        return "none";
    case ZaloPreviewKind::Image:
        return "image";
    case ZaloPreviewKind::VideoContactSheet:
        return "video_contact_sheet";
    case ZaloPreviewKind::DocumentText:
        return "document_text";
    }
    return "none";
}

const char* toString(ZaloContentEvidenceCode code) noexcept
{
    switch (code) {
    case ZaloContentEvidenceCode::None:
        return "none";
    case ZaloContentEvidenceCode::JpegSignature:
        return "jpeg-signature";
    case ZaloContentEvidenceCode::JpegMarkers:
        return "jpeg-markers";
    case ZaloContentEvidenceCode::JpegEntropy:
        return "jpeg-entropy";
    case ZaloContentEvidenceCode::JpegEnd:
        return "jpeg-end";
    case ZaloContentEvidenceCode::PngSignature:
        return "png-signature";
    case ZaloContentEvidenceCode::PngIhdr:
        return "png-ihdr";
    case ZaloContentEvidenceCode::PngEnd:
        return "png-end";
    case ZaloContentEvidenceCode::WebpSignature:
        return "webp-signature";
    case ZaloContentEvidenceCode::WebpHeader:
        return "webp-header";
    case ZaloContentEvidenceCode::GifSignature:
        return "gif-signature";
    case ZaloContentEvidenceCode::GifHeader:
        return "gif-header";
    case ZaloContentEvidenceCode::Mp4Ftyp:
        return "mp4-ftyp";
    case ZaloContentEvidenceCode::Mp4Moov:
        return "mp4-moov";
    case ZaloContentEvidenceCode::Mp4Trak:
        return "mp4-trak";
    case ZaloContentEvidenceCode::PdfHeader:
        return "pdf-header";
    case ZaloContentEvidenceCode::PdfStartXref:
        return "pdf-startxref";
    case ZaloContentEvidenceCode::PdfXref:
        return "pdf-xref";
    case ZaloContentEvidenceCode::PdfTrailer:
        return "pdf-trailer";
    case ZaloContentEvidenceCode::PdfEnd:
        return "pdf-end";
    case ZaloContentEvidenceCode::ZipSignature:
        return "zip-signature";
    case ZaloContentEvidenceCode::ZipCentralDirectory:
        return "zip-central-directory";
    case ZaloContentEvidenceCode::ZipLocalHeaders:
        return "zip-local-headers";
    case ZaloContentEvidenceCode::ZipCrc32:
        return "zip-crc32";
    case ZaloContentEvidenceCode::OoxmlContentTypes:
        return "ooxml-content-types";
    case ZaloContentEvidenceCode::OoxmlRelationships:
        return "ooxml-relationships";
    case ZaloContentEvidenceCode::OoxmlMainPart:
        return "ooxml-main-part";
    case ZaloContentEvidenceCode::EmbeddedPayload:
        return "embedded-payload";
    case ZaloContentEvidenceCode::MaskedPayload:
        return "masked-payload";
    }
    return "none";
}

ZaloHumanIdentity buildHumanIdentity(
    const ZaloContentResult& content,
    std::string_view trustedOriginalFilename,
    ByteSize logicalBytes,
    std::string_view categoryAlias)
{
    ZaloHumanIdentity id;
    id.identitySource = ZaloIdentitySource::Unknown;

    const bool hasTrustedName = !trustedOriginalFilename.empty();
    if (hasTrustedName) {
        id.displayName = std::string(trustedOriginalFilename);
        id.identitySource = ZaloIdentitySource::OriginalFilename;
    }

    std::string sizeStr;
    if (logicalBytes > 0) {
        sizeStr = SizeFormatter::format(logicalBytes);
    }

    if (!content.identified()) {
        if (!hasTrustedName) {
            if (!categoryAlias.empty() && categoryAlias != "unknown") {
                id.displayName = "Zalo item (" + std::string(categoryAlias) + ")";
            } else {
                id.displayName = "Unknown Zalo data";
            }
        }
        id.contentSummary = "Content could not be identified safely";
        id.previewKind = ZaloPreviewKind::None;
        id.previewAvailable = false;
        return id;
    }

    switch (content.type) {
    case ZaloContentType::Jpeg:
    case ZaloContentType::Png:
    case ZaloContentType::Webp:
    case ZaloContentType::Gif: {
        const char* typeName = spacelens::toString(content.type);
        std::string upperType;
        for (const char* p = typeName; *p; ++p) {
            upperType.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(*p))));
        }
        id.previewKind = ZaloPreviewKind::Image;
        id.previewAvailable = true;
        if (id.identitySource == ZaloIdentitySource::Unknown) {
            id.identitySource = ZaloIdentitySource::MediaMetadata;
        }
        if (content.imageDimensions.has_value() &&
            content.imageDimensions->width > 0 &&
            content.imageDimensions->height > 0) {
            id.imageWidth = content.imageDimensions->width;
            id.imageHeight = content.imageDimensions->height;
            const std::string dimStr =
                std::to_string(content.imageDimensions->width) + "×" +
                std::to_string(content.imageDimensions->height);
            id.contentSummary =
                upperType + (sizeStr.empty() ? "" : " · " + sizeStr) + " · " +
                dimStr;
            if (!hasTrustedName) {
                id.displayName = upperType + " Image (" + dimStr + ")";
            }
        } else {
            id.contentSummary =
                upperType + (sizeStr.empty() ? "" : " · " + sizeStr);
            if (!hasTrustedName) {
                id.displayName = upperType + " Image";
            }
        }
        if (content.wrapper) {
            id.contentSummary = "Wrapped " + id.contentSummary;
        }
        break;
    }
    case ZaloContentType::Mp4:
    case ZaloContentType::Mov: {
        const char* typeName = spacelens::toString(content.type);
        std::string upperType;
        for (const char* p = typeName; *p; ++p) {
            upperType.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(*p))));
        }
        id.previewKind = ZaloPreviewKind::VideoContactSheet;
        id.previewAvailable = true;
        if (id.identitySource == ZaloIdentitySource::Unknown) {
            id.identitySource = ZaloIdentitySource::MediaMetadata;
        }
        std::string durStr;
        std::string resStr;
        if (content.videoMetadata.has_value()) {
            const auto& vm = *content.videoMetadata;
            id.videoDurationMs = vm.durationMs;
            id.videoWidth = vm.width;
            id.videoHeight = vm.height;
            id.videoCodec = vm.codec;
            if (vm.durationMs > 0) {
                const uint64_t totalSec = vm.durationMs / 1000ULL;
                const uint64_t minutes = totalSec / 60ULL;
                const uint64_t seconds = totalSec % 60ULL;
                char buf[32]{};
                if (minutes >= 60ULL) {
                    const uint64_t hours = minutes / 60ULL;
                    const uint64_t remMin = minutes % 60ULL;
                    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
                                  static_cast<unsigned long long>(hours),
                                  static_cast<unsigned long long>(remMin),
                                  static_cast<unsigned long long>(seconds));
                } else {
                    std::snprintf(buf, sizeof(buf), "%02llu:%02llu",
                                  static_cast<unsigned long long>(minutes),
                                  static_cast<unsigned long long>(seconds));
                }
                durStr = buf;
            }
            if (vm.width > 0 && vm.height > 0) {
                resStr = std::to_string(vm.width) + "×" + std::to_string(vm.height);
            }
        }
        id.contentSummary = upperType;
        if (!sizeStr.empty()) id.contentSummary += " · " + sizeStr;
        if (!durStr.empty()) id.contentSummary += " · " + durStr;
        if (!resStr.empty()) id.contentSummary += " · " + resStr;
        if (content.videoMetadata.has_value() &&
            !content.videoMetadata->codec.empty()) {
            id.contentSummary += " · " + content.videoMetadata->codec;
        }
        if (!hasTrustedName) {
            if (!durStr.empty() && !resStr.empty()) {
                id.displayName =
                    upperType + " Video (" + durStr + " · " + resStr + ")";
            } else if (!resStr.empty()) {
                id.displayName = upperType + " Video (" + resStr + ")";
            } else {
                id.displayName = upperType + " Video";
            }
        }
        if (content.wrapper) {
            id.contentSummary = "Wrapped " + id.contentSummary;
        }
        break;
    }
    case ZaloContentType::Pdf: {
        id.previewKind = ZaloPreviewKind::DocumentText;
        if (id.identitySource == ZaloIdentitySource::Unknown) {
            id.identitySource = ZaloIdentitySource::DocumentStructure;
        }
        id.contentSummary = "PDF";
        if (!sizeStr.empty()) id.contentSummary += " · " + sizeStr;
        if (content.semanticMetadata.has_value()) {
            const auto& sem = *content.semanticMetadata;
            if (sem.pdfPageCount.has_value() && *sem.pdfPageCount > 0) {
                id.documentPageCount = sem.pdfPageCount;
                id.contentSummary +=
                    " · " + std::to_string(*sem.pdfPageCount) +
                    (*sem.pdfPageCount == 1 ? " page" : " pages");
            }
            if (sem.title.has_value() && !sem.title->empty()) {
                id.displayTitle = sem.title;
                id.identitySource = ZaloIdentitySource::EmbeddedMetadata;
                if (!hasTrustedName) {
                    id.displayName = *sem.title;
                }
            }
            if (sem.author.has_value() && !sem.author->empty()) {
                id.contentSummary += " · " + *sem.author;
            }
            if (!sem.visibleText.empty()) {
                id.textPreview = sem.visibleText.front().text;
            }
        }
        if (!hasTrustedName && id.displayName.empty()) {
            if (id.documentPageCount.has_value()) {
                id.displayName = "PDF Document (" +
                                 std::to_string(*id.documentPageCount) + " pages)";
            } else {
                id.displayName = "PDF Document";
            }
        }
        id.previewAvailable = id.textPreview.has_value() ||
                              id.displayTitle.has_value() ||
                              id.documentPageCount.has_value();
        if (content.wrapper) {
            id.contentSummary = "Wrapped " + id.contentSummary;
        }
        break;
    }
    case ZaloContentType::Docx:
    case ZaloContentType::Xlsx:
    case ZaloContentType::Pptx: {
        const char* typeName = spacelens::toString(content.type);
        std::string upperType;
        for (const char* p = typeName; *p; ++p) {
            upperType.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(*p))));
        }
        id.previewKind = ZaloPreviewKind::DocumentText;
        if (id.identitySource == ZaloIdentitySource::Unknown) {
            id.identitySource = ZaloIdentitySource::DocumentStructure;
        }
        id.contentSummary = upperType;
        if (!sizeStr.empty()) id.contentSummary += " · " + sizeStr;
        if (content.semanticMetadata.has_value()) {
            const auto& sem = *content.semanticMetadata;
            if (sem.title.has_value() && !sem.title->empty()) {
                id.displayTitle = sem.title;
                id.identitySource = ZaloIdentitySource::EmbeddedMetadata;
                if (!hasTrustedName) {
                    id.displayName = *sem.title;
                }
            }
            if (sem.creator.has_value() && !sem.creator->empty()) {
                id.contentSummary += " · " + *sem.creator;
            }
            if (!sem.visibleText.empty()) {
                id.textPreview = sem.visibleText.front().text;
            }
        }
        if (!hasTrustedName && id.displayName.empty()) {
            id.displayName = upperType + " Document";
        }
        id.previewAvailable =
            id.textPreview.has_value() || id.displayTitle.has_value();
        if (content.wrapper) {
            id.contentSummary = "Wrapped " + id.contentSummary;
        }
        break;
    }
    case ZaloContentType::Zip: {
        id.previewKind = ZaloPreviewKind::DocumentText;
        if (content.semanticMetadata.has_value() &&
            content.semanticMetadata->title.has_value() &&
            !content.semanticMetadata->title->empty()) {
            const std::string& title = *content.semanticMetadata->title;
            id.displayTitle = title;
            if (!hasTrustedName) {
                id.displayName = title + " (ZIP)";
            }
            id.contentSummary = "ZIP Archive · " + title;
            if (!sizeStr.empty()) id.contentSummary += " · " + sizeStr;
            id.previewAvailable = true;
        } else {
            id.contentSummary = "ZIP Archive" + (sizeStr.empty() ? "" : " · " + sizeStr);
            if (!hasTrustedName) {
                id.displayName = "ZIP Archive";
            }
        }
        id.identitySource = ZaloIdentitySource::DocumentStructure;
        if (content.wrapper) {
            id.contentSummary = "Wrapped " + id.contentSummary;
        }
        break;
    }
    case ZaloContentType::Unknown:
        if (!hasTrustedName) {
            id.displayName = "Unknown Zalo data";
        }
        id.contentSummary = "Content could not be identified safely";
        id.previewKind = ZaloPreviewKind::None;
        id.previewAvailable = false;
        break;
    }

    return id;
}

[[nodiscard]] ZaloSemanticMetadata extractZipSemanticMetadata(ScanContext& context)
{
    ZaloSemanticMetadata result;
    result.status = ZaloSemanticMetadataStatus::Available;
    
    // First try full zip directory if available
    const auto zipEnd = findZipEnd(context, 0U);
    if (zipEnd.has_value()) {
        ZipReader reader;
        std::vector<RawZipEntry> entries;
        if (initializeValidatedZip(context, 0U, *zipEnd, reader, entries) && !entries.empty()) {
            for (const auto& entry : entries) {
                std::string name = entry.name;
                while (!name.empty() && (name.back() == '/' || name.back() == '\\')) {
                    name.pop_back();
                }
                if (!name.empty() && name != "." && name != "..") {
                    result.title = name;
                    return result;
                }
            }
        }
    }
    
    // Fallback: Read starting local header
    if (context.view.size() >= 30U) {
        std::array<std::uint8_t, 30> localHeader{};
        if (context.readRange(0U, localHeader) &&
            readLe32(localHeader.data()) == kZipLocalHeaderSignature) {
            const std::uint16_t nameLength = readLe16(localHeader.data() + 26U);
            if (nameLength > 0U && nameLength <= kZipNameBytes &&
                nameLength <= context.view.size() - 30U) {
                std::vector<std::uint8_t> nameBytes(nameLength);
                if (context.readRange(30U, nameBytes)) {
                    std::string rawName(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
                    while (!rawName.empty() && (rawName.back() == '/' || rawName.back() == '\\')) {
                        rawName.pop_back();
                    }
                    if (!rawName.empty()) {
                        result.title = rawName;
                        return result;
                    }
                }
            }
        }
    }
    return result;
}

ZaloContentResult identifyZaloContent(const PayloadView& payload,
                                      PayloadCancellation cancellation)
{
    try {
        ZaloContentResult result = identifyImpl(payload, std::move(cancellation));
        if (result.identified()) {
            result.humanIdentity = buildHumanIdentity(result);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return unknownResult(ZaloContentStatus::ReadError);
    } catch (...) {
        return unknownResult(ZaloContentStatus::ReadError);
    }
}

ZaloContentResult identifyZaloContent(const PayloadView& payload,
                                      std::stop_token stop)
{
    return identifyZaloContent(
        payload, [stop]() noexcept { return stop.stop_requested(); });
}

ZaloSemanticMetadata extractZaloSemanticMetadata(
    const PayloadView& payload, const ZaloContentResult& identification,
    PayloadCancellation cancellation)
{
    try {
        if (!identification.identified()) {
            return semanticResult(ZaloSemanticMetadataStatus::NotApplicable);
        }
        switch (identification.type) {
        case ZaloContentType::Pdf:
        case ZaloContentType::Docx:
        case ZaloContentType::Xlsx:
        case ZaloContentType::Pptx:
        case ZaloContentType::Zip:
            break;
        case ZaloContentType::Unknown:
        case ZaloContentType::Jpeg:
        case ZaloContentType::Png:
        case ZaloContentType::Webp:
        case ZaloContentType::Gif:
        case ZaloContentType::Mp4:
        case ZaloContentType::Mov:
            return semanticResult(ZaloSemanticMetadataStatus::NotApplicable);
        }
        if (!payload.valid() || identification.payloadLength == 0U) {
            return semanticResult(ZaloSemanticMetadataStatus::Malformed);
        }
        const auto payloadSlice = payload.trySlice(
            identification.payloadOffset, identification.payloadLength);
        if (!payloadSlice.has_value()) {
            return semanticResult(ZaloSemanticMetadataStatus::Malformed);
        }

        ScanContext context(*payloadSlice, std::move(cancellation));
        if (identification.masked && identification.maskByte != 0U) {
            context.xorMask = identification.maskByte;
        }
        if (!context.checkCancellation()) {
            return semanticContextResult(
                context, ZaloSemanticMetadataStatus::Cancelled);
        }
        ZaloSemanticMetadata result;
        switch (identification.type) {
        case ZaloContentType::Pdf:
            result = extractPdfSemanticMetadata(context);
            break;
        case ZaloContentType::Docx:
        case ZaloContentType::Xlsx:
        case ZaloContentType::Pptx:
            result = extractOoxmlSemanticMetadata(context, identification.type);
            break;
        case ZaloContentType::Zip:
            result = extractZipSemanticMetadata(context);
            break;
        case ZaloContentType::Unknown:
        case ZaloContentType::Jpeg:
        case ZaloContentType::Png:
        case ZaloContentType::Webp:
        case ZaloContentType::Gif:
        case ZaloContentType::Mp4:
        case ZaloContentType::Mov:
            return semanticResult(ZaloSemanticMetadataStatus::NotApplicable);
        }
        if (context.terminal != TerminalState::None || context.budgetExceeded) {
            return semanticContextResult(context, result.status);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return semanticResult(ZaloSemanticMetadataStatus::ReadError);
    } catch (...) {
        return semanticResult(ZaloSemanticMetadataStatus::ReadError);
    }
}

ZaloSemanticMetadata extractZaloSemanticMetadata(
    const PayloadView& payload, const ZaloContentResult& identification,
    std::stop_token stop)
{
    return extractZaloSemanticMetadata(
        payload, identification,
        [stop]() noexcept { return stop.stop_requested(); });
}

}  // namespace spacelens
