#include "TestRunner.hpp"

#include "core/ZaloContentIdentifier.hpp"
#include "platform/windows/FileIdentity.hpp"
#include "platform/windows/ReadOnlyPayload.hpp"

#include "miniz.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace spacelens;
namespace fs = std::filesystem;

namespace {

class TempFixture final {
public:
    TempFixture()
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_root = fs::temp_directory_path() / "spacelens_zalo_content_tests" /
                 (std::to_string(::GetCurrentProcessId()) + "_" +
                  std::to_string(stamp));
        std::error_code error;
        fs::create_directories(m_root, error);
        if (error) {
            throw spacelens::test::Failure(
                "cannot create content-identifier fixture root");
        }
    }

    ~TempFixture()
    {
        std::error_code error;
        fs::remove_all(m_root, error);
    }

    TempFixture(const TempFixture&) = delete;
    TempFixture& operator=(const TempFixture&) = delete;

    [[nodiscard]] const fs::path& root() const noexcept { return m_root; }

private:
    fs::path m_root;
};

void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw spacelens::test::Failure(
            "cannot create content-identifier fixture file");
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    if (!output) {
        throw spacelens::test::Failure(
            "cannot write content-identifier fixture file");
    }
}

std::vector<std::uint8_t> bytesOf(std::string_view text)
{
    return std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        reinterpret_cast<const std::uint8_t*>(text.data()) + text.size());
}

FileIdentity identityFor(const fs::path& path)
{
    const auto identity = queryFileIdentity(path.wstring());
    if (!identity.has_value()) {
        throw spacelens::test::Failure(
            "cannot query content-identifier fixture identity");
    }
    return *identity;
}

std::wstring canonicalPathFor(const fs::path& path)
{
    const std::wstring canonical = canonicalWin32Path(path.wstring());
    if (canonical.empty()) {
        throw spacelens::test::Failure(
            "cannot canonicalize content-identifier fixture path");
    }
    return canonical;
}

ZaloContentResult identifyBytes(TempFixture& fixture, std::string_view name,
                                const std::vector<std::uint8_t>& bytes,
                                PayloadCancellation cancellation = {})
{
    const fs::path path = fixture.root() / fs::path(std::string(name));
    writeBytes(path, bytes);
    const FileIdentity identity = identityFor(path);
    auto opened = ReadOnlyPayload::open(path.wstring(), identity,
                                        canonicalPathFor(path));
    if (!opened.ok()) {
        throw spacelens::test::Failure(
            "cannot open content-identifier fixture payload");
    }
    return identifyZaloContent(opened.payload->view(), std::move(cancellation));
}

void appendLe16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendLe32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

struct ZipMember final {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

std::vector<std::uint8_t> makeStoredZip(
    const std::vector<ZipMember>& members)
{
    struct Record final {
        std::string name;
        std::uint32_t crc = 0;
        std::uint32_t size = 0;
        std::uint32_t localOffset = 0;
    };

    std::vector<std::uint8_t> output;
    std::vector<Record> records;
    records.reserve(members.size());
    for (const ZipMember& member : members) {
        if (member.name.size() > 0xffffU || member.bytes.size() > 0xffffffffULL ||
            output.size() > 0xffffffffULL) {
            throw spacelens::test::Failure("ZIP test fixture is too large");
        }
        const std::uint32_t localOffset =
            static_cast<std::uint32_t>(output.size());
        const auto* nameBytes =
            reinterpret_cast<const std::uint8_t*>(member.name.data());
        appendLe32(output, 0x04034b50U);
        appendLe16(output, 20U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        const std::uint32_t crc =
            static_cast<std::uint32_t>(mz_crc32(0U, member.bytes.data(),
                                                member.bytes.size()));
        appendLe32(output, crc);
        appendLe32(output, static_cast<std::uint32_t>(member.bytes.size()));
        appendLe32(output, static_cast<std::uint32_t>(member.bytes.size()));
        appendLe16(output, static_cast<std::uint16_t>(member.name.size()));
        appendLe16(output, 0U);
        output.insert(output.end(), nameBytes, nameBytes + member.name.size());
        output.insert(output.end(), member.bytes.begin(), member.bytes.end());
        records.push_back(Record{member.name, crc,
                                 static_cast<std::uint32_t>(member.bytes.size()),
                                 localOffset});
    }

    if (output.size() > 0xffffffffULL) {
        throw spacelens::test::Failure("ZIP test fixture is too large");
    }
    const std::uint32_t centralOffset = static_cast<std::uint32_t>(output.size());
    for (const Record& record : records) {
        const auto* nameBytes =
            reinterpret_cast<const std::uint8_t*>(record.name.data());
        appendLe32(output, 0x02014b50U);
        appendLe16(output, 20U);
        appendLe16(output, 20U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe32(output, record.crc);
        appendLe32(output, record.size);
        appendLe32(output, record.size);
        appendLe16(output, static_cast<std::uint16_t>(record.name.size()));
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe32(output, 0U);
        appendLe32(output, record.localOffset);
        output.insert(output.end(), nameBytes,
                      nameBytes + record.name.size());
    }

    const std::uint32_t centralSize =
        static_cast<std::uint32_t>(output.size()) - centralOffset;
    if (records.size() > 0xffffU) {
        throw spacelens::test::Failure("ZIP test fixture has too many entries");
    }
    appendLe32(output, 0x06054b50U);
    appendLe16(output, 0U);
    appendLe16(output, 0U);
    appendLe16(output, static_cast<std::uint16_t>(records.size()));
    appendLe16(output, static_cast<std::uint16_t>(records.size()));
    appendLe32(output, centralSize);
    appendLe32(output, centralOffset);
    appendLe16(output, 0U);
    return output;
}

std::vector<std::uint8_t> makeJpeg()
{
    return {
        0xff, 0xd8,
        0xff, 0xe0, 0x00, 0x10,
        0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0xff, 0xc0, 0x00, 0x11,
        0x08, 0x00, 0x10, 0x00, 0x20, 0x03,
        0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
        0xff, 0xda, 0x00, 0x0c,
        0x03, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
        0x00, 0x3f, 0x00,
        0x11, 0x22, 0xff, 0x00, 0x33,
        0xff, 0xd9};
}

void appendBe16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void appendBe32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

std::vector<std::uint8_t> makePng(std::uint32_t width = 64, std::uint32_t height = 48)
{
    std::vector<std::uint8_t> bytes = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
    };
    appendBe32(bytes, 13U);
    bytes.push_back('I'); bytes.push_back('H'); bytes.push_back('D'); bytes.push_back('R');
    appendBe32(bytes, width);
    appendBe32(bytes, height);
    bytes.push_back(8);
    bytes.push_back(2);
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(0);
    appendBe32(bytes, 0x12345678U);
    appendBe32(bytes, 0U);
    bytes.push_back('I'); bytes.push_back('E'); bytes.push_back('N'); bytes.push_back('D');
    appendBe32(bytes, 0xae426082U);
    return bytes;
}

std::vector<std::uint8_t> makeWebp(std::uint32_t width = 64, std::uint32_t height = 48)
{
    std::vector<std::uint8_t> bytes = {
        'R', 'I', 'F', 'F',
        0x20, 0x00, 0x00, 0x00,
        'W', 'E', 'B', 'P',
        'V', 'P', '8', ' ',
        0x0a, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x9d, 0x01, 0x2a,
    };
    appendLe16(bytes, static_cast<std::uint16_t>(width & 0x3fffU));
    appendLe16(bytes, static_cast<std::uint16_t>(height & 0x3fffU));
    return bytes;
}

std::vector<std::uint8_t> makeGif(std::uint16_t width = 64, std::uint16_t height = 48)
{
    std::vector<std::uint8_t> bytes = {
        'G', 'I', 'F', '8', '9', 'a',
    };
    appendLe16(bytes, width);
    appendLe16(bytes, height);
    bytes.push_back(0x80);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    bytes.push_back(0x00); bytes.push_back(0x00); bytes.push_back(0x00);
    bytes.push_back(0xff); bytes.push_back(0xff); bytes.push_back(0xff);
    bytes.push_back(0x3b);
    return bytes;
}

std::vector<std::uint8_t> makeMp4(std::uint32_t durationSec = 15,
                                  std::uint32_t width = 1280,
                                  std::uint32_t height = 720)
{
    std::vector<std::uint8_t> bytes;
    appendBe32(bytes, 20U);
    bytes.push_back('f'); bytes.push_back('t'); bytes.push_back('y'); bytes.push_back('p');
    bytes.push_back('i'); bytes.push_back('s'); bytes.push_back('o'); bytes.push_back('m');
    appendBe32(bytes, 512U);
    bytes.push_back('m'); bytes.push_back('p'); bytes.push_back('4'); bytes.push_back('2');

    std::vector<std::uint8_t> moovBody;
    std::vector<std::uint8_t> mvhd;
    appendBe32(mvhd, 108U);
    mvhd.push_back('m'); mvhd.push_back('v'); mvhd.push_back('h'); mvhd.push_back('d');
    mvhd.push_back(0);
    mvhd.push_back(0); mvhd.push_back(0); mvhd.push_back(0);
    appendBe32(mvhd, 0U);
    appendBe32(mvhd, 0U);
    appendBe32(mvhd, 1000U);
    appendBe32(mvhd, durationSec * 1000U);
    mvhd.resize(108U, 0);
    moovBody.insert(moovBody.end(), mvhd.begin(), mvhd.end());

    std::vector<std::uint8_t> trakBody;
    std::vector<std::uint8_t> tkhd;
    appendBe32(tkhd, 92U);
    tkhd.push_back('t'); tkhd.push_back('k'); tkhd.push_back('h'); tkhd.push_back('d');
    tkhd.push_back(0);
    tkhd.push_back(0); tkhd.push_back(0); tkhd.push_back(1);
    tkhd.resize(84U, 0);
    appendBe16(tkhd, static_cast<std::uint16_t>(width));
    appendBe16(tkhd, 0U);
    appendBe16(tkhd, static_cast<std::uint16_t>(height));
    appendBe16(tkhd, 0U);
    trakBody.insert(trakBody.end(), tkhd.begin(), tkhd.end());

    appendBe32(moovBody, static_cast<std::uint32_t>(trakBody.size() + 8U));
    moovBody.push_back('t'); moovBody.push_back('r'); moovBody.push_back('a'); moovBody.push_back('k');
    moovBody.insert(moovBody.end(), trakBody.begin(), trakBody.end());

    appendBe32(bytes, static_cast<std::uint32_t>(moovBody.size() + 8U));
    bytes.push_back('m'); bytes.push_back('o'); bytes.push_back('o'); bytes.push_back('v');
    bytes.insert(bytes.end(), moovBody.begin(), moovBody.end());

    return bytes;
}

std::vector<std::uint8_t> makePdf()
{
    std::string output = "%PDF-1.7\n";
    const std::size_t objectOffset = output.size();
    output += "1 0 obj\n<< /Type /Catalog >>\nendobj\n";
    const std::size_t xrefOffset = output.size();
    output += "xref\n0 2\n";
    output += "0000000000 65535 f \n";
    char objectLine[64]{};
    std::snprintf(objectLine, sizeof(objectLine), "%010zu 00000 n \n",
                  objectOffset);
    output += objectLine;
    output += "trailer\n<< /Size 2 /Root 1 0 R >>\nstartxref\n";
    output += std::to_string(xrefOffset);
    output += "\n%%EOF\n";
    return bytesOf(output);
}

std::vector<std::uint8_t> makeSemanticPdf()
{
    const std::array<std::string_view, 5> objects{{
        "<< /Type /Catalog /Pages 2 0 R /Info 5 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R >>",
        "<< >>",
        "<< /Title (Quarterly Report) /Author (Ada Lovelace) >>",
    }};
    std::string output = "%PDF-1.7\n";
    std::array<std::size_t, 5> offsets{};
    for (std::size_t index = 0; index < objects.size(); ++index) {
        offsets[index] = output.size();
        output += std::to_string(index + 1U);
        output += " 0 obj\n";
        output += objects[index];
        output += "\nendobj\n";
    }
    const std::size_t xrefOffset = output.size();
    output += "xref\n0 6\n0000000000 65535 f \n";
    char objectLine[64]{};
    for (const std::size_t offset : offsets) {
        std::snprintf(objectLine, sizeof(objectLine), "%010zu 00000 n \n",
                      offset);
        output += objectLine;
    }
    output += "trailer\n<< /Size 6 /Root 1 0 R /Info 5 0 R >>\nstartxref\n";
    output += std::to_string(xrefOffset);
    output += "\n%%EOF\n";
    return bytesOf(output);
}

std::vector<std::uint8_t> makeSemanticDocx()
{
    constexpr std::string_view contentType =
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
    constexpr std::string_view mainPart = "word/document.xml";
    const std::string contentTypes =
        "<?xml version=\"1.0\"?>"
        "<ct:Types xmlns:ct=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<ct:Override PartName=\"/word/document.xml\" ContentType=\"" +
        std::string(contentType) + "\"/>"
        "</ct:Types>";
    const std::string relationships =
        "<rel:Relationships xmlns:rel=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<rel:Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"" +
        std::string(mainPart) + "\"/>"
        "</rel:Relationships>";
    const std::string core =
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>Project Brief</dc:title><dc:creator>Grace Hopper</dc:creator>"
        "</cp:coreProperties>";
    const std::string document =
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>"
        "<w:p><w:pPr><w:pStyle val=\"Heading1\"/></w:pPr>"
        "<w:r><w:t>Executive Summary</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>Bounded visible text.</w:t></w:r></w:p>"
        "</w:body></w:document>";
    return makeStoredZip({
        {"[Content_Types].xml", bytesOf(contentTypes)},
        {"_rels/.rels", bytesOf(relationships)},
        {"docProps/core.xml", bytesOf(core)},
        {std::string(mainPart), bytesOf(document)},
    });
}

std::vector<std::uint8_t> makeSemanticXlsx()
{
    constexpr std::string_view spreadsheetNamespace =
        "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
    constexpr std::string_view contentType =
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml";
    const std::string contentTypes =
        "<?xml version=\"1.0\"?>"
        "<ct:Types xmlns:ct=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<ct:Override PartName=\"/xl/workbook.xml\" ContentType=\"" +
        std::string(contentType) + "\"/>"
        "</ct:Types>";
    const std::string relationships =
        "<rel:Relationships xmlns:rel=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<rel:Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</rel:Relationships>";
    const std::string workbook =
        "<x:workbook xmlns:x=\"" + std::string(spreadsheetNamespace) +
        "\"><x:sheets><x:sheet name=\"Summary\" sheetId=\"1\"/></x:sheets></x:workbook>";
    const std::string sharedStrings =
        "<x:sst xmlns:x=\"" + std::string(spreadsheetNamespace) +
        "\"><x:si><x:t>Shared cell</x:t></x:si></x:sst>";
    const std::string worksheet =
        "<x:worksheet xmlns:x=\"" + std::string(spreadsheetNamespace) +
        "\"><x:sheetData>"
        "<x:row r=\"1\"><x:c t=\"s\"><x:v>0</x:v></x:c></x:row>"
        "<x:row r=\"2\"><x:c t=\"inlineStr\"><x:is><x:t>Inline cell</x:t></x:is></x:c></x:row>"
        "</x:sheetData></x:worksheet>";
    return makeStoredZip({
        {"[Content_Types].xml", bytesOf(contentTypes)},
        {"_rels/.rels", bytesOf(relationships)},
        {"xl/workbook.xml", bytesOf(workbook)},
        {"xl/sharedStrings.xml", bytesOf(sharedStrings)},
        {"xl/worksheets/sheet1.xml", bytesOf(worksheet)},
    });
}

std::vector<std::uint8_t> makeSemanticPptx()
{
    constexpr std::string_view presentationNamespace =
        "http://schemas.openxmlformats.org/presentationml/2006/main";
    constexpr std::string_view drawingNamespace =
        "http://schemas.openxmlformats.org/drawingml/2006/main";
    constexpr std::string_view contentType =
        "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml";
    const std::string contentTypes =
        "<?xml version=\"1.0\"?>"
        "<ct:Types xmlns:ct=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<ct:Override PartName=\"/ppt/presentation.xml\" ContentType=\"" +
        std::string(contentType) + "\"/>"
        "</ct:Types>";
    const std::string relationships =
        "<rel:Relationships xmlns:rel=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<rel:Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/>"
        "</rel:Relationships>";
    const std::string presentation =
        "<p:presentation xmlns:p=\"" + std::string(presentationNamespace) +
        "\"></p:presentation>";
    const std::string slide =
        "<p:sld xmlns:p=\"" + std::string(presentationNamespace) +
        "\" xmlns:a=\"" + std::string(drawingNamespace) + "\">"
        "<p:cSld><p:spTree><p:sp><p:nvSpPr><p:nvPr>"
        "<p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
        "<p:txBody><a:p><a:r><a:t>Slide title</a:t></a:r></a:p></p:txBody>"
        "</p:sp></p:spTree></p:cSld></p:sld>";
    return makeStoredZip({
        {"[Content_Types].xml", bytesOf(contentTypes)},
        {"_rels/.rels", bytesOf(relationships)},
        {"ppt/presentation.xml", bytesOf(presentation)},
        {"ppt/slides/slide1.xml", bytesOf(slide)},
    });
}

std::vector<std::uint8_t> makeOoxml(
    std::string_view contentType, std::string_view mainPart,
    std::string_view mainRoot, std::string_view mainNamespace)
{
    const std::string contentTypes =
        "<?xml version=\"1.0\"?>"
        "<ct:Types xmlns:ct=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<ct:Override PartName=\"/" + std::string(mainPart) +
        "\" ContentType=\"" + std::string(contentType) + "\"/>"
        "</ct:Types>";
    const std::string relationships =
        "<rel:Relationships xmlns:rel=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<rel:Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"" +
        std::string(mainPart) +
        "\"/>"
        "</rel:Relationships>";
    const std::string main = "<main:" + std::string(mainRoot) +
                             " xmlns:main=\"" + std::string(mainNamespace) +
                             "\"><main:body/></main:" +
                             std::string(mainRoot) + ">";
    return makeStoredZip({
        {"[Content_Types].xml", bytesOf(contentTypes)},
        {"_rels/.rels", bytesOf(relationships)},
        {std::string(mainPart), bytesOf(main)},
    });
}

std::vector<std::uint8_t> makeOoxmlWithContentTypesSuffix(
    std::string_view contentType, std::string_view mainPart,
    std::string_view mainRoot, std::string_view mainNamespace,
    std::string_view suffix)
{
    std::string contentTypes =
        "<?xml version=\"1.0\"?>"
        "<ct:Types xmlns:ct=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<ct:Override PartName=\"/" + std::string(mainPart) +
        "\" ContentType=\"" + std::string(contentType) + "\"/>"
        "</ct:Types>";
    contentTypes += suffix;
    const std::string relationships =
        "<rel:Relationships xmlns:rel=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<rel:Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"" +
        std::string(mainPart) +
        "\"/>"
        "</rel:Relationships>";
    const std::string main = "<main:" + std::string(mainRoot) +
                             " xmlns:main=\"" + std::string(mainNamespace) +
                             "\"><main:body/></main:" +
                             std::string(mainRoot) + ">";
    return makeStoredZip({
        {"[Content_Types].xml", bytesOf(contentTypes)},
        {"_rels/.rels", bytesOf(relationships)},
        {std::string(mainPart), bytesOf(main)},
    });
}

std::vector<std::uint8_t> makeJpegWith256ByteAppSegment()
{
    const auto base = makeJpeg();
    std::vector<std::uint8_t> result{0xffU, 0xd8U, 0xffU, 0xe1U, 0x01U, 0x00U};
    result.insert(result.end(), 254U, 0x41U);
    result.insert(result.end(), base.begin() + 20, base.end());
    return result;
}

std::vector<std::uint8_t> readFileBytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw spacelens::test::Failure(
            "cannot read content-identifier fixture file");
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

bool sameStableIdentity(const FileIdentity& left, const FileIdentity& right)
{
    const bool sameFileId =
        left.volumeSerial == right.volumeSerial &&
        left.fileId128Known == right.fileId128Known &&
        left.fileIndex64Known == right.fileIndex64Known &&
        ((!left.fileId128Known || left.fileId128 == right.fileId128) &&
         (!left.fileIndex64Known || left.fileId == right.fileId));
    return sameFileId && left.identityKnown == right.identityKnown &&
           left.isDirectory == right.isDirectory &&
           left.sizeBytes == right.sizeBytes &&
           left.logicalSizeKnown == right.logicalSizeKnown &&
           left.allocatedBytes == right.allocatedBytes &&
           left.allocationKnown == right.allocationKnown &&
           left.numberOfLinks == right.numberOfLinks &&
           left.linkCountKnown == right.linkCountKnown &&
           left.sparse == right.sparse && left.compressed == right.compressed &&
           left.creationTimeTicks == right.creationTimeTicks &&
           left.changeTimeTicks == right.changeTimeTicks &&
           left.lastWriteTicks == right.lastWriteTicks &&
           left.basicMetadataKnown == right.basicMetadataKnown &&
           left.attributes == right.attributes &&
           left.observationConsistent == right.observationConsistent;
}

std::size_t directoryEntryCount(const fs::path& path)
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(path)) {
        ++count;
    }
    return count;
}

bool hasEvidence(const ZaloContentResult& result, ZaloContentEvidenceCode code)
{
    for (const auto actual : result.evidence) {
        if (actual == code) {
            return true;
        }
    }
    return false;
}

}  // namespace

SPACELENS_TEST(ZaloContent_direct_jpeg_is_structurally_verified)
{
    TempFixture fixture;
    const ZaloContentResult result = identifyBytes(fixture, "image.bin", makeJpeg());
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Jpeg);
    SPACELENS_REQUIRE(!result.wrapper);
    SPACELENS_REQUIRE_EQ(result.payloadOffset, 0ULL);
    SPACELENS_REQUIRE_EQ(result.payloadLength, makeJpeg().size());
    SPACELENS_REQUIRE(result.jpegDimensions.has_value());
    SPACELENS_REQUIRE_EQ(result.jpegDimensions->width, 32U);
    SPACELENS_REQUIRE_EQ(result.jpegDimensions->height, 16U);
    SPACELENS_REQUIRE(hasEvidence(result, ZaloContentEvidenceCode::JpegEnd));
    SPACELENS_REQUIRE(result.description.find("image") != std::string::npos);
}

SPACELENS_TEST(ZaloContent_direct_pdf_requires_exact_structural_end)
{
    TempFixture fixture;
    const auto pdf = makePdf();
    const ZaloContentResult result = identifyBytes(fixture, "document.bin", pdf);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Pdf);
    SPACELENS_REQUIRE(result.confidence == ZaloContentConfidence::Strong);
    SPACELENS_REQUIRE_EQ(result.payloadLength, pdf.size());
    SPACELENS_REQUIRE(hasEvidence(result, ZaloContentEvidenceCode::PdfXref));

    auto withSuffix = pdf;
    withSuffix.push_back(0x7fU);
    const ZaloContentResult rejected =
        identifyBytes(fixture, "document-with-suffix.bin", withSuffix);
    SPACELENS_REQUIRE(rejected.status == ZaloContentStatus::Unknown);

    auto withFakeFinalEof = pdf;
    const auto unrelated = bytesOf("not part of the PDF\n%%EOF\n");
    withFakeFinalEof.insert(withFakeFinalEof.end(), unrelated.begin(),
                            unrelated.end());
    const ZaloContentResult fakeFinalEof = identifyBytes(
        fixture, "document-with-fake-final-eof.bin", withFakeFinalEof);
    SPACELENS_REQUIRE(fakeFinalEof.status == ZaloContentStatus::Unknown);
}

SPACELENS_TEST(ZaloContent_semantic_pdf_extracts_bounded_identity)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "semantic-document.bin";
    const auto source = makeSemanticPdf();
    writeBytes(path, source);
    const FileIdentity identity = identityFor(path);
    auto opened = ReadOnlyPayload::open(path.wstring(), identity,
                                        canonicalPathFor(path));
    SPACELENS_REQUIRE(opened.ok());

    const ZaloContentResult identified =
        identifyZaloContent(opened.payload->view());
    SPACELENS_REQUIRE(identified.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(identified.type == ZaloContentType::Pdf);
    const ZaloSemanticMetadata metadata = extractZaloSemanticMetadata(
        opened.payload->view(), identified);
    SPACELENS_REQUIRE(metadata.status == ZaloSemanticMetadataStatus::Available);
    SPACELENS_REQUIRE(metadata.pdfPageCount.has_value());
    SPACELENS_REQUIRE_EQ(*metadata.pdfPageCount, 1U);
    SPACELENS_REQUIRE(metadata.title.has_value());
    SPACELENS_REQUIRE_EQ(*metadata.title, "Quarterly Report");
    SPACELENS_REQUIRE(metadata.author.has_value());
    SPACELENS_REQUIRE_EQ(*metadata.author, "Ada Lovelace");
    SPACELENS_REQUIRE_EQ(
        std::string_view(toString(ZaloSemanticMetadataStatus::Available)),
        "available");
}

SPACELENS_TEST(ZaloContent_semantic_docx_extracts_core_and_visible_text)
{
    TempFixture fixture;
    const auto source = makeSemanticDocx();
    const fs::path directPath = fixture.root() / "semantic-document.bin";
    writeBytes(directPath, source);
    const FileIdentity directIdentity = identityFor(directPath);
    auto direct = ReadOnlyPayload::open(
        directPath.wstring(), directIdentity, canonicalPathFor(directPath));
    SPACELENS_REQUIRE(direct.ok());
    const ZaloContentResult directIdentification =
        identifyZaloContent(direct.payload->view());
    SPACELENS_REQUIRE(directIdentification.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(directIdentification.type == ZaloContentType::Docx);
    const ZaloSemanticMetadata directMetadata = extractZaloSemanticMetadata(
        direct.payload->view(), directIdentification);
    SPACELENS_REQUIRE(directMetadata.status ==
                      ZaloSemanticMetadataStatus::Available);
    SPACELENS_REQUIRE(directMetadata.title.has_value());
    SPACELENS_REQUIRE_EQ(*directMetadata.title, "Project Brief");
    SPACELENS_REQUIRE(directMetadata.creator.has_value());
    SPACELENS_REQUIRE_EQ(*directMetadata.creator, "Grace Hopper");
    SPACELENS_REQUIRE_EQ(directMetadata.visibleText.size(), 2U);
    SPACELENS_REQUIRE(directMetadata.visibleText[0].heading);
    SPACELENS_REQUIRE_EQ(directMetadata.visibleText[0].text,
                         "Executive Summary");
    SPACELENS_REQUIRE_EQ(directMetadata.visibleText[1].text,
                         "Bounded visible text.");

    const std::vector<std::uint8_t> prefix{0x01U, 0x02U, 0x03U};
    const std::vector<std::uint8_t> suffix{0xf0U, 0xf1U};
    std::vector<std::uint8_t> wrapped = prefix;
    wrapped.insert(wrapped.end(), source.begin(), source.end());
    wrapped.insert(wrapped.end(), suffix.begin(), suffix.end());
    const fs::path wrappedPath = fixture.root() / "wrapped-semantic.bin";
    writeBytes(wrappedPath, wrapped);
    const FileIdentity wrappedIdentity = identityFor(wrappedPath);
    auto wrappedPayload = ReadOnlyPayload::open(
        wrappedPath.wstring(), wrappedIdentity, canonicalPathFor(wrappedPath));
    SPACELENS_REQUIRE(wrappedPayload.ok());
    const ZaloContentResult wrappedIdentification =
        identifyZaloContent(wrappedPayload.payload->view());
    SPACELENS_REQUIRE(wrappedIdentification.status ==
                      ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(wrappedIdentification.wrapper);
    const ZaloSemanticMetadata wrappedMetadata = extractZaloSemanticMetadata(
        wrappedPayload.payload->view(), wrappedIdentification);
    SPACELENS_REQUIRE(wrappedMetadata.status ==
                      ZaloSemanticMetadataStatus::Available);
    SPACELENS_REQUIRE(wrappedMetadata.title.has_value());
    SPACELENS_REQUIRE_EQ(*wrappedMetadata.title, "Project Brief");
}

SPACELENS_TEST(ZaloContent_semantic_xlsx_and_pptx_extract_visible_text)
{
    TempFixture fixture;

    const fs::path xlsxPath = fixture.root() / "semantic-sheet.bin";
    writeBytes(xlsxPath, makeSemanticXlsx());
    const FileIdentity xlsxIdentity = identityFor(xlsxPath);
    auto xlsxPayload = ReadOnlyPayload::open(
        xlsxPath.wstring(), xlsxIdentity, canonicalPathFor(xlsxPath));
    SPACELENS_REQUIRE(xlsxPayload.ok());
    const ZaloContentResult xlsxIdentification =
        identifyZaloContent(xlsxPayload.payload->view());
    SPACELENS_REQUIRE(xlsxIdentification.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(xlsxIdentification.type == ZaloContentType::Xlsx);
    const ZaloSemanticMetadata xlsxMetadata = extractZaloSemanticMetadata(
        xlsxPayload.payload->view(), xlsxIdentification);
    SPACELENS_REQUIRE(xlsxMetadata.status ==
                      ZaloSemanticMetadataStatus::Available);
    SPACELENS_REQUIRE_EQ(xlsxMetadata.visibleText.size(), 2U);
    SPACELENS_REQUIRE(xlsxMetadata.visibleText[0].heading);
    SPACELENS_REQUIRE_EQ(xlsxMetadata.visibleText[0].text, "Shared cell");
    SPACELENS_REQUIRE(!xlsxMetadata.visibleText[1].heading);
    SPACELENS_REQUIRE_EQ(xlsxMetadata.visibleText[1].text, "Inline cell");

    const fs::path pptxPath = fixture.root() / "semantic-slides.bin";
    writeBytes(pptxPath, makeSemanticPptx());
    const FileIdentity pptxIdentity = identityFor(pptxPath);
    auto pptxPayload = ReadOnlyPayload::open(
        pptxPath.wstring(), pptxIdentity, canonicalPathFor(pptxPath));
    SPACELENS_REQUIRE(pptxPayload.ok());
    const ZaloContentResult pptxIdentification =
        identifyZaloContent(pptxPayload.payload->view());
    SPACELENS_REQUIRE(pptxIdentification.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(pptxIdentification.type == ZaloContentType::Pptx);
    const ZaloSemanticMetadata pptxMetadata = extractZaloSemanticMetadata(
        pptxPayload.payload->view(), pptxIdentification);
    SPACELENS_REQUIRE(pptxMetadata.status ==
                      ZaloSemanticMetadataStatus::Available);
    SPACELENS_REQUIRE_EQ(pptxMetadata.visibleText.size(), 1U);
    SPACELENS_REQUIRE(pptxMetadata.visibleText.front().heading);
    SPACELENS_REQUIRE_EQ(pptxMetadata.visibleText.front().text, "Slide title");
}

SPACELENS_TEST(ZaloContent_semantic_metadata_is_typed_for_unsupported_and_cancelled)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "image.bin";
    const auto source = makeJpeg();
    writeBytes(path, source);
    const FileIdentity identity = identityFor(path);
    auto opened = ReadOnlyPayload::open(path.wstring(), identity,
                                        canonicalPathFor(path));
    SPACELENS_REQUIRE(opened.ok());
    const ZaloContentResult identified =
        identifyZaloContent(opened.payload->view());
    const ZaloSemanticMetadata unsupported = extractZaloSemanticMetadata(
        opened.payload->view(), identified);
    SPACELENS_REQUIRE(unsupported.status ==
                      ZaloSemanticMetadataStatus::NotApplicable);

    const ZaloSemanticMetadata cancelled = extractZaloSemanticMetadata(
        opened.payload->view(), identified,
        []() noexcept { return true; });
    SPACELENS_REQUIRE(cancelled.status ==
                      ZaloSemanticMetadataStatus::NotApplicable);

    ZaloContentResult invalidIdentification;
    invalidIdentification.status = ZaloContentStatus::Identified;
    invalidIdentification.type = ZaloContentType::Pdf;
    invalidIdentification.payloadLength = 1U;
    const ZaloSemanticMetadata malformed = extractZaloSemanticMetadata(
        opened.payload->view(), invalidIdentification);
    SPACELENS_REQUIRE(malformed.status ==
                      ZaloSemanticMetadataStatus::Malformed);
}

SPACELENS_TEST(ZaloContent_semantic_supported_payload_honors_cancellation)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "cancelled-document.bin";
    writeBytes(path, makeSemanticPdf());
    const FileIdentity identity = identityFor(path);
    auto opened = ReadOnlyPayload::open(path.wstring(), identity,
                                        canonicalPathFor(path));
    SPACELENS_REQUIRE(opened.ok());

    const ZaloContentResult identified =
        identifyZaloContent(opened.payload->view());
    SPACELENS_REQUIRE(identified.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(identified.type == ZaloContentType::Pdf);

    const ZaloSemanticMetadata cancelled = extractZaloSemanticMetadata(
        opened.payload->view(), identified,
        []() noexcept { return true; });
    SPACELENS_REQUIRE(cancelled.status == ZaloSemanticMetadataStatus::Cancelled);
}

SPACELENS_TEST(ZaloContent_direct_zip_is_checked_without_filesystem_extraction)
{
    TempFixture fixture;
    const auto zip = makeStoredZip({{"payload.bin", bytesOf("payload")}});
    const ZaloContentResult result = identifyBytes(fixture, "archive.bin", zip);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Zip);
    SPACELENS_REQUIRE(!result.wrapper);
    SPACELENS_REQUIRE_EQ(result.payloadLength, zip.size());
    SPACELENS_REQUIRE(hasEvidence(result, ZaloContentEvidenceCode::ZipCrc32));

    auto withSuffix = zip;
    withSuffix.push_back(0x01U);
    const ZaloContentResult rejected =
        identifyBytes(fixture, "archive-with-suffix.bin", withSuffix);
    SPACELENS_REQUIRE(rejected.status == ZaloContentStatus::Unknown);
}

SPACELENS_TEST(ZaloContent_ooxml_namespaces_identify_docx_xlsx_and_pptx)
{
    TempFixture fixture;
    struct Fixture final {
        std::string name;
        std::string contentType;
        std::string mainPart;
        std::string root;
        std::string nameSpace;
        ZaloContentType type;
    };
    const std::array<Fixture, 3> fixtures{{
        {"word.bin",
         "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml",
         "word/document.xml", "document",
         "http://schemas.openxmlformats.org/wordprocessingml/2006/main",
         ZaloContentType::Docx},
        {"sheet.bin",
         "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml",
         "xl/workbook.xml", "workbook",
         "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
         ZaloContentType::Xlsx},
        {"slides.bin",
         "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml",
         "ppt/presentation.xml", "presentation",
         "http://schemas.openxmlformats.org/presentationml/2006/main",
         ZaloContentType::Pptx},
    }};

    for (const Fixture& item : fixtures) {
        const ZaloContentResult result = identifyBytes(
            fixture, item.name,
            makeOoxml(item.contentType, item.mainPart, item.root,
                      item.nameSpace));
        SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
        SPACELENS_REQUIRE(result.type == item.type);
        SPACELENS_REQUIRE(result.confidence == ZaloContentConfidence::Verified);
        SPACELENS_REQUIRE(hasEvidence(
            result, ZaloContentEvidenceCode::OoxmlMainPart));
    }
}

SPACELENS_TEST(ZaloContent_invalid_ooxml_is_not_downgraded_to_generic_zip)
{
    TempFixture fixture;
    const auto malformed = makeStoredZip({
        {"[Content_Types].xml", bytesOf("<ct:Types>")},
        {"_rels/.rels", bytesOf("not xml")},
        {"word/document.xml", bytesOf("<w:document/>")},
    });
    const ZaloContentResult result =
        identifyBytes(fixture, "malformed-office.bin", malformed);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Unknown);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Unknown);
}

SPACELENS_TEST(ZaloContent_embedded_payload_reports_exact_offset_and_length)
{
    TempFixture fixture;
    const auto jpeg = makeJpeg();
    std::vector<std::uint8_t> wrapped{0x7a, 0x61, 0x6c, 0x6f, 0x00, 0x01};
    const ByteSize offset = wrapped.size();
    wrapped.insert(wrapped.end(), jpeg.begin(), jpeg.end());
    wrapped.insert(wrapped.end(), {0x44, 0x55, 0x66});
    const ZaloContentResult result =
        identifyBytes(fixture, "wrapped.bin", wrapped);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Jpeg);
    SPACELENS_REQUIRE(result.wrapper);
    SPACELENS_REQUIRE_EQ(result.payloadOffset, offset);
    SPACELENS_REQUIRE_EQ(result.payloadLength, jpeg.size());
    SPACELENS_REQUIRE(result.method == ZaloIdentificationMethod::EmbeddedPayload);
    SPACELENS_REQUIRE(hasEvidence(result,
                                  ZaloContentEvidenceCode::EmbeddedPayload));
}

SPACELENS_TEST(ZaloContent_multiple_embedded_candidates_are_ambiguous)
{
    TempFixture fixture;
    const auto jpeg = makeJpeg();
    std::vector<std::uint8_t> wrapped{0xaa, 0xbb};
    wrapped.insert(wrapped.end(), jpeg.begin(), jpeg.end());
    wrapped.push_back(0xcc);
    wrapped.insert(wrapped.end(), jpeg.begin(), jpeg.end());
    const ZaloContentResult result =
        identifyBytes(fixture, "ambiguous.bin", wrapped);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Ambiguous);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Unknown);
    SPACELENS_REQUIRE(result.evidence.empty());
}

SPACELENS_TEST(ZaloContent_cancellation_and_invalid_views_are_typed)
{
    TempFixture fixture;
    const auto jpeg = makeJpeg();
    const ZaloContentResult cancelled = identifyBytes(
        fixture, "cancelled.bin", jpeg,
        []() noexcept { return true; });
    SPACELENS_REQUIRE(cancelled.status == ZaloContentStatus::Cancelled);
    SPACELENS_REQUIRE(cancelled.type == ZaloContentType::Unknown);
    SPACELENS_REQUIRE(cancelled.description.find("cancelled") !=
                      std::string::npos);

    const ZaloContentResult invalid = identifyZaloContent(PayloadView{});
    SPACELENS_REQUIRE(invalid.status == ZaloContentStatus::ReadError);
    SPACELENS_REQUIRE(invalid.type == ZaloContentType::Unknown);
}

SPACELENS_TEST(ZaloContent_evidence_is_privacy_safe)
{
    TempFixture fixture;
    const ZaloContentResult result = identifyBytes(
        fixture, "private-looking-name-token-123.bin", makeJpeg());
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.description.find("private-looking") ==
                      std::string::npos);
    SPACELENS_REQUIRE(result.description.find("token-") == std::string::npos);
    for (const auto code : result.evidence) {
        const std::string_view text = toString(code);
        SPACELENS_REQUIRE(text.find("/") == std::string_view::npos);
        SPACELENS_REQUIRE(text.find("\\") == std::string_view::npos);
    }
    SPACELENS_REQUIRE_EQ(std::string_view(toString(ZaloContentType::Docx)),
                         "docx");
    SPACELENS_REQUIRE_EQ(
        std::string_view(toString(ZaloIdentificationMethod::EmbeddedPayload)),
        "embedded-payload");
    SPACELENS_REQUIRE_EQ(
        std::string_view(toString(ZaloContentConfidence::Verified)),
        "verified");
    SPACELENS_REQUIRE_EQ(
        std::string_view(toString(ZaloContentStatus::Changed)), "changed");
}

SPACELENS_TEST(ZaloContent_jpeg_segment_lengths_are_big_endian)
{
    TempFixture fixture;
    const auto jpeg = makeJpegWith256ByteAppSegment();
    const ZaloContentResult result =
        identifyBytes(fixture, "long-app-segment.bin", jpeg);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Jpeg);
    SPACELENS_REQUIRE_EQ(result.payloadLength, jpeg.size());
    SPACELENS_REQUIRE(result.jpegDimensions.has_value());
    SPACELENS_REQUIRE_EQ(result.jpegDimensions->width, 32U);
    SPACELENS_REQUIRE_EQ(result.jpegDimensions->height, 16U);
}

SPACELENS_TEST(ZaloContent_malformed_jpeg_grammar_is_not_accepted)
{
    TempFixture fixture;

    const std::vector<std::uint8_t> sosBeforeSof{
        0xffU, 0xd8U,
        0xffU, 0xdaU, 0x00U, 0x0cU,
        0x03U, 0x01U, 0x00U, 0x02U, 0x00U, 0x03U, 0x00U,
        0x00U, 0x3fU, 0x00U,
        0x11U, 0xffU, 0xd9U};
    const ZaloContentResult rejectedSos =
        identifyBytes(fixture, "jpeg-sos-before-sof.bin", sosBeforeSof);
    SPACELENS_REQUIRE(rejectedSos.status == ZaloContentStatus::Unknown);
    SPACELENS_REQUIRE(rejectedSos.type == ZaloContentType::Unknown);

    auto malformedSof = makeJpeg();
    // The SOF payload must be exactly 6 + 3 * component_count bytes.
    malformedSof[23] = 0x10U;
    const ZaloContentResult rejectedSof =
        identifyBytes(fixture, "jpeg-malformed-sof.bin", malformedSof);
    SPACELENS_REQUIRE(rejectedSof.status == ZaloContentStatus::Unknown);
    SPACELENS_REQUIRE(rejectedSof.type == ZaloContentType::Unknown);

    auto malformedSos = makeJpeg();
    // The SOS payload must be exactly 4 + 2 * scan_component_count bytes.
    malformedSos[42] = 0x0bU;
    const ZaloContentResult rejectedSosLength =
        identifyBytes(fixture, "jpeg-malformed-sos.bin", malformedSos);
    SPACELENS_REQUIRE(rejectedSosLength.status == ZaloContentStatus::Unknown);
    SPACELENS_REQUIRE(rejectedSosLength.type == ZaloContentType::Unknown);

    auto zeroEntropy = makeJpeg();
    // After the SOS header, an immediate EOI is not entropy-coded content.
    zeroEntropy[53] = 0xffU;
    zeroEntropy[54] = 0xd9U;
    const ZaloContentResult rejectedEntropy =
        identifyBytes(fixture, "jpeg-zero-entropy.bin", zeroEntropy);
    SPACELENS_REQUIRE(rejectedEntropy.status == ZaloContentStatus::Unknown);
    SPACELENS_REQUIRE(rejectedEntropy.type == ZaloContentType::Unknown);
}

SPACELENS_TEST(ZaloContent_ooxml_rejects_extra_roots_and_top_level_text)
{
    TempFixture fixture;
    constexpr std::string_view contentType =
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
    constexpr std::string_view mainPart = "word/document.xml";
    constexpr std::string_view mainRoot = "document";
    constexpr std::string_view mainNamespace =
        "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

    const auto extraRoot = makeOoxmlWithContentTypesSuffix(
        contentType, mainPart, mainRoot, mainNamespace, "<ct:Extra/>");
    const ZaloContentResult rejectedExtraRoot =
        identifyBytes(fixture, "ooxml-extra-root.bin", extraRoot);
    SPACELENS_REQUIRE(rejectedExtraRoot.status == ZaloContentStatus::Unknown);
    SPACELENS_REQUIRE(rejectedExtraRoot.type == ZaloContentType::Unknown);

    const auto topLevelText = makeOoxmlWithContentTypesSuffix(
        contentType, mainPart, mainRoot, mainNamespace, "not-whitespace");
    const ZaloContentResult rejectedTopLevelText =
        identifyBytes(fixture, "ooxml-top-level-text.bin", topLevelText);
    SPACELENS_REQUIRE(rejectedTopLevelText.status == ZaloContentStatus::Unknown);
    SPACELENS_REQUIRE(rejectedTopLevelText.type == ZaloContentType::Unknown);
}

SPACELENS_TEST(ZaloContent_throwing_cancellation_fails_closed)
{
    TempFixture fixture;
    const ZaloContentResult result = identifyBytes(
        fixture, "throwing-cancellation.bin", makeJpeg(),
        []() -> bool { throw 7; });
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Cancelled);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Unknown);

    const fs::path path = fixture.root() / "payload.bin";
    const auto bytes = makeJpeg();
    writeBytes(path, bytes);
    const FileIdentity identity = identityFor(path);
    auto opened = ReadOnlyPayload::open(path.wstring(), identity,
                                        canonicalPathFor(path));
    SPACELENS_REQUIRE(opened.ok());

    std::array<std::uint8_t, 1> destination{};
    const auto read = opened.payload->view().readAt(
        0U, destination.data(), destination.size(), []() -> bool { throw 9; });
    SPACELENS_REQUIRE(read.status == PayloadReadStatus::Cancelled);
    SPACELENS_REQUIRE(opened.payload->revalidate(
                           []() -> bool { throw 11; }) ==
                      PayloadRevalidationStatus::Cancelled);
}

SPACELENS_TEST(ZaloContent_malformed_jpeg_callback_frequency_is_bounded)
{
    TempFixture fixture;
    constexpr std::size_t payloadBytes = 4U * 1024U * 1024U;
    std::vector<std::uint8_t> malformed(payloadBytes, 0xffU);
    malformed[0] = 0xffU;
    malformed[1] = 0xd8U;

    std::size_t callbackCount = 0U;
    const ZaloContentResult result = identifyBytes(
        fixture, "large-malformed-jpeg.bin", malformed,
        [&callbackCount]() noexcept {
            ++callbackCount;
            return false;
        });
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Unknown);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Unknown);
    SPACELENS_REQUIRE(callbackCount > 0U);
    SPACELENS_REQUIRE(callbackCount < 1000U);
}

SPACELENS_TEST(ZaloContent_prefixed_pdf_and_docx_report_exact_payload_slices)
{
    TempFixture fixture;
    const std::vector<std::uint8_t> prefix{0x5aU, 0x41U, 0x4cU, 0x4fU,
                                           0x00U, 0x7fU, 0x01U};
    const std::vector<std::uint8_t> suffix{0xdeU, 0xadU, 0xbeU, 0xefU};

    const auto pdf = makePdf();
    std::vector<std::uint8_t> wrappedPdf = prefix;
    wrappedPdf.insert(wrappedPdf.end(), pdf.begin(), pdf.end());
    wrappedPdf.insert(wrappedPdf.end(), suffix.begin(), suffix.end());
    const ZaloContentResult pdfResult =
        identifyBytes(fixture, "prefixed-pdf.bin", wrappedPdf);
    SPACELENS_REQUIRE(pdfResult.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(pdfResult.type == ZaloContentType::Pdf);
    SPACELENS_REQUIRE(pdfResult.wrapper);
    SPACELENS_REQUIRE_EQ(pdfResult.payloadOffset, prefix.size());
    SPACELENS_REQUIRE_EQ(pdfResult.payloadLength, pdf.size());

    const auto docx = makeOoxml(
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml",
        "word/document.xml", "document",
        "http://schemas.openxmlformats.org/wordprocessingml/2006/main");
    std::vector<std::uint8_t> wrappedDocx = prefix;
    wrappedDocx.insert(wrappedDocx.end(), docx.begin(), docx.end());
    wrappedDocx.insert(wrappedDocx.end(), suffix.begin(), suffix.end());
    const ZaloContentResult docxResult =
        identifyBytes(fixture, "prefixed-docx.bin", wrappedDocx);
    SPACELENS_REQUIRE(docxResult.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(docxResult.type == ZaloContentType::Docx);
    SPACELENS_REQUIRE(docxResult.confidence == ZaloContentConfidence::Verified);
    SPACELENS_REQUIRE(docxResult.wrapper);
    SPACELENS_REQUIRE_EQ(docxResult.payloadOffset, prefix.size());
    SPACELENS_REQUIRE_EQ(docxResult.payloadLength, docx.size());
}

SPACELENS_TEST(ZaloContent_false_magic_and_truncation_remain_unknown)
{
    TempFixture fixture;
    const std::array<std::vector<std::uint8_t>, 4> inputs{{
        bytesOf("noise %PDF-1.7 not-a-document PK\\003\\004 tail"),
        {0xffU, 0xd8U, 0xffU, 0xe0U, 0x00U},
        bytesOf("%PDF-1.7\n1 0 obj\n<<>>\nendobj\n%%EOF"),
        {0x50U, 0x4bU, 0x03U, 0x04U, 0x14U, 0x00U},
    }};
    const std::array<std::string_view, 4> names{{
        "false-magic.bin", "truncated-jpeg.bin", "truncated-pdf.bin",
        "truncated-zip.bin"}};

    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const ZaloContentResult result =
            identifyBytes(fixture, names[index], inputs[index]);
        SPACELENS_REQUIRE(result.status == ZaloContentStatus::Unknown);
        SPACELENS_REQUIRE(result.type == ZaloContentType::Unknown);
        SPACELENS_REQUIRE_EQ(result.description,
                             "Unresolved app-managed binary");
        SPACELENS_REQUIRE(result.evidence.empty());
    }
}

SPACELENS_TEST(ZaloContent_stop_token_and_mid_scan_change_are_typed)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "changed-during-identification.bin";
    const auto jpeg = makeJpeg();
    writeBytes(path, jpeg);
    const FileIdentity identity = identityFor(path);
    auto opened = ReadOnlyPayload::open(path.wstring(), identity,
                                        canonicalPathFor(path));
    SPACELENS_REQUIRE(opened.ok());

    std::stop_source stopped;
    stopped.request_stop();
    const ZaloContentResult cancelled =
        identifyZaloContent(opened.payload->view(), stopped.get_token());
    SPACELENS_REQUIRE(cancelled.status == ZaloContentStatus::Cancelled);

    bool changed = false;
    const ZaloContentResult changedResult = identifyZaloContent(
        opened.payload->view(), [&]() {
            if (!changed) {
                auto replacement = jpeg;
                replacement[6] ^= 0x01U;
                writeBytes(path, replacement);
                changed = true;
            }
            return false;
        });
    SPACELENS_REQUIRE(changed);
    SPACELENS_REQUIRE(changedResult.status == ZaloContentStatus::Changed);
    SPACELENS_REQUIRE(changedResult.type == ZaloContentType::Unknown);

    const fs::path truncatedPath = fixture.root() / "truncated-during-read.bin";
    writeBytes(truncatedPath, jpeg);
    const FileIdentity truncateIdentity = identityFor(truncatedPath);
    auto truncateOpened = ReadOnlyPayload::open(
        truncatedPath.wstring(), truncateIdentity,
        canonicalPathFor(truncatedPath));
    SPACELENS_REQUIRE(truncateOpened.ok());
    bool truncated = false;
    const ZaloContentResult truncatedResult = identifyZaloContent(
        truncateOpened.payload->view(), [&]() {
            if (!truncated) {
                writeBytes(truncatedPath, {0xffU});
                truncated = true;
            }
            return false;
        });
    SPACELENS_REQUIRE(truncated);
    SPACELENS_REQUIRE(truncatedResult.status == ZaloContentStatus::Changed);
    SPACELENS_REQUIRE(truncatedResult.type == ZaloContentType::Unknown);
}

SPACELENS_TEST(ZaloContent_identification_does_not_mutate_or_create_artifacts)
{
    TempFixture fixture;
    const fs::path path = fixture.root() / "source.bin";
    const auto source = makeOoxml(
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml",
        "word/document.xml", "document",
        "http://schemas.openxmlformats.org/wordprocessingml/2006/main");
    writeBytes(path, source);
    const FileIdentity before = identityFor(path);
    const auto bytesBefore = readFileBytes(path);
    const std::size_t entriesBefore = directoryEntryCount(fixture.root());

    auto opened = ReadOnlyPayload::open(path.wstring(), before,
                                        canonicalPathFor(path));
    SPACELENS_REQUIRE(opened.ok());
    const ZaloContentResult result =
        identifyZaloContent(opened.payload->view());
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Docx);

    const FileIdentity after = identityFor(path);
    SPACELENS_REQUIRE(sameStableIdentity(before, after));
    SPACELENS_REQUIRE(readFileBytes(path) == bytesBefore);
    SPACELENS_REQUIRE_EQ(directoryEntryCount(fixture.root()), entriesBefore);
}

SPACELENS_TEST(ZaloContent_png_identification_and_dimensions)
{
    TempFixture fixture;
    const auto png = makePng(320, 240);
    const ZaloContentResult result = identifyBytes(fixture, "sample.png", png);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Png);
    SPACELENS_REQUIRE(result.confidence == ZaloContentConfidence::Strong);
    SPACELENS_REQUIRE(!result.wrapper);
    SPACELENS_REQUIRE(result.imageDimensions.has_value());
    SPACELENS_REQUIRE_EQ(result.imageDimensions->width, 320U);
    SPACELENS_REQUIRE_EQ(result.imageDimensions->height, 240U);
}

SPACELENS_TEST(ZaloContent_webp_identification_and_dimensions)
{
    TempFixture fixture;
    const auto webp = makeWebp(400, 300);
    const ZaloContentResult result = identifyBytes(fixture, "sample.webp", webp);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Webp);
    SPACELENS_REQUIRE(result.confidence == ZaloContentConfidence::Strong);
    SPACELENS_REQUIRE(!result.wrapper);
    SPACELENS_REQUIRE(result.imageDimensions.has_value());
    SPACELENS_REQUIRE_EQ(result.imageDimensions->width, 400U);
    SPACELENS_REQUIRE_EQ(result.imageDimensions->height, 300U);
}

SPACELENS_TEST(ZaloContent_gif_identification_and_dimensions)
{
    TempFixture fixture;
    const auto gif = makeGif(128, 96);
    const ZaloContentResult result = identifyBytes(fixture, "sample.gif", gif);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Gif);
    SPACELENS_REQUIRE(result.confidence == ZaloContentConfidence::Strong);
    SPACELENS_REQUIRE(!result.wrapper);
    SPACELENS_REQUIRE(result.imageDimensions.has_value());
    SPACELENS_REQUIRE_EQ(result.imageDimensions->width, 128U);
    SPACELENS_REQUIRE_EQ(result.imageDimensions->height, 96U);
}

SPACELENS_TEST(ZaloContent_mp4_identification_and_metadata)
{
    TempFixture fixture;
    const auto mp4 = makeMp4(30, 1920, 1080);
    const ZaloContentResult result = identifyBytes(fixture, "sample.mp4", mp4);
    SPACELENS_REQUIRE(result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(result.type == ZaloContentType::Mp4);
    SPACELENS_REQUIRE(result.confidence == ZaloContentConfidence::Verified);
    SPACELENS_REQUIRE(!result.wrapper);
    SPACELENS_REQUIRE(result.videoMetadata.has_value());
    SPACELENS_REQUIRE_EQ(result.videoMetadata->durationMs, 30000ULL);
    SPACELENS_REQUIRE_EQ(result.videoMetadata->width, 1920U);
    SPACELENS_REQUIRE_EQ(result.videoMetadata->height, 1080U);
}

SPACELENS_TEST(ZaloContent_wrapped_media_payloads)
{
    TempFixture fixture;
    const std::vector<std::uint8_t> prefix{0x5aU, 0x41U, 0x4cU, 0x4fU, 0x01U};
    const std::vector<std::uint8_t> suffix{0xeeU, 0xffU};

    // Wrapped PNG
    const auto png = makePng(100, 100);
    std::vector<std::uint8_t> wrappedPng = prefix;
    wrappedPng.insert(wrappedPng.end(), png.begin(), png.end());
    wrappedPng.insert(wrappedPng.end(), suffix.begin(), suffix.end());
    const ZaloContentResult pngResult =
        identifyBytes(fixture, "wrapped-png.bin", wrappedPng);
    SPACELENS_REQUIRE(pngResult.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(pngResult.type == ZaloContentType::Png);
    SPACELENS_REQUIRE(pngResult.wrapper);
    SPACELENS_REQUIRE_EQ(pngResult.payloadOffset, prefix.size());
    SPACELENS_REQUIRE(pngResult.imageDimensions.has_value());
    SPACELENS_REQUIRE_EQ(pngResult.imageDimensions->width, 100U);

    // Wrapped MP4
    const auto mp4 = makeMp4(10, 640, 480);
    std::vector<std::uint8_t> wrappedMp4 = prefix;
    wrappedMp4.insert(wrappedMp4.end(), mp4.begin(), mp4.end());
    wrappedMp4.insert(wrappedMp4.end(), suffix.begin(), suffix.end());
    const ZaloContentResult mp4Result =
        identifyBytes(fixture, "wrapped-mp4.bin", wrappedMp4);
    SPACELENS_REQUIRE(mp4Result.status == ZaloContentStatus::Identified);
    SPACELENS_REQUIRE(mp4Result.type == ZaloContentType::Mp4);
    SPACELENS_REQUIRE(mp4Result.wrapper);
    SPACELENS_REQUIRE_EQ(mp4Result.payloadOffset, prefix.size());
    SPACELENS_REQUIRE(mp4Result.videoMetadata.has_value());
    SPACELENS_REQUIRE_EQ(mp4Result.videoMetadata->width, 640U);
}

SPACELENS_TEST(ZaloContent_human_identity_deterministic_synthesis)
{
    // 1. JPEG with dimensions
    ZaloContentResult jpegRes;
    jpegRes.status = ZaloContentStatus::Identified;
    jpegRes.type = ZaloContentType::Jpeg;
    jpegRes.imageDimensions = ZaloImageDimensions{1920, 1080};
    const auto jpegId = buildHumanIdentity(jpegRes, "photo.jpg", 1024 * 1024, "picture");
    SPACELENS_REQUIRE_EQ(jpegId.displayName, "photo.jpg");
    SPACELENS_REQUIRE(jpegId.contentSummary.find("JPEG") != std::string::npos);
    SPACELENS_REQUIRE(jpegId.contentSummary.find("1920×1080") != std::string::npos);
    SPACELENS_REQUIRE(jpegId.previewKind == ZaloPreviewKind::Image);
    SPACELENS_REQUIRE(jpegId.previewAvailable);

    // 2. MP4 without original filename
    ZaloContentResult mp4Res;
    mp4Res.status = ZaloContentStatus::Identified;
    mp4Res.type = ZaloContentType::Mp4;
    ZaloVideoMetadata vm;
    vm.durationMs = 65000ULL; // 01:05
    vm.width = 1280;
    vm.height = 720;
    mp4Res.videoMetadata = vm;
    const auto mp4Id = buildHumanIdentity(mp4Res, "", 10 * 1024 * 1024, "video");
    SPACELENS_REQUIRE(mp4Id.displayName.find("MP4 Video") != std::string::npos);
    SPACELENS_REQUIRE(mp4Id.displayName.find("01:05") != std::string::npos);
    SPACELENS_REQUIRE(mp4Id.contentSummary.find("1280×720") != std::string::npos);
    SPACELENS_REQUIRE(mp4Id.previewKind == ZaloPreviewKind::VideoContactSheet);
    SPACELENS_REQUIRE(mp4Id.previewAvailable);

    // 3. PDF with semantic title and page count
    ZaloContentResult pdfRes;
    pdfRes.status = ZaloContentStatus::Identified;
    pdfRes.type = ZaloContentType::Pdf;
    ZaloSemanticMetadata sem;
    sem.title = "Annual Financial Report";
    sem.pdfPageCount = 12;
    pdfRes.semanticMetadata = sem;
    const auto pdfId = buildHumanIdentity(pdfRes, "", 500 * 1024, "file");
    SPACELENS_REQUIRE_EQ(pdfId.displayName, "Annual Financial Report");
    SPACELENS_REQUIRE(pdfId.contentSummary.find("12 pages") != std::string::npos);
    SPACELENS_REQUIRE(pdfId.previewKind == ZaloPreviewKind::DocumentText);
    SPACELENS_REQUIRE(pdfId.previewAvailable);

    // 4. Unknown item
    ZaloContentResult unkRes;
    unkRes.status = ZaloContentStatus::Unknown;
    const auto unkId = buildHumanIdentity(unkRes, "", 100, "cache");
    SPACELENS_REQUIRE_EQ(unkId.displayName, "Zalo item (cache)");
    SPACELENS_REQUIRE_EQ(unkId.contentSummary, "Content could not be identified safely");
    SPACELENS_REQUIRE(unkId.previewKind == ZaloPreviewKind::None);
    SPACELENS_REQUIRE(!unkId.previewAvailable);
}
