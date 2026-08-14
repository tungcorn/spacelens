#include "TestRunner.hpp"

#include "core/Json.hpp"
#include "mcp/JsonValue.hpp"

using namespace spacelens;
using namespace spacelens::mcp;

SPACELENS_TEST(McpJson_roundtrip_object_array_types)
{
    const char* src =
        "{\"a\":1,\"b\":true,\"c\":null,\"d\":\"x\\ny\",\"e\":[1,2],\"f\":{\"g\":-3}}";
    const auto parsed = parseJson(src);
    SPACELENS_REQUIRE(parsed.ok);
    SPACELENS_REQUIRE(parsed.value.isObject());
    SPACELENS_REQUIRE_EQ(*parsed.value.intAt("a"), 1);
    SPACELENS_REQUIRE(*parsed.value.boolAt("b"));
    SPACELENS_REQUIRE(parsed.value.get("c") != nullptr);
    SPACELENS_REQUIRE(parsed.value.get("c")->isNull());
    SPACELENS_REQUIRE_EQ(*parsed.value.stringAt("d"), std::string("x\ny"));
    const auto again = parseJson(parsed.value.stringify());
    SPACELENS_REQUIRE(again.ok);
    SPACELENS_REQUIRE_EQ(*again.value.intAt("a"), 1);
}

SPACELENS_TEST(McpJson_rejects_trailing_and_unescaped_newline)
{
    SPACELENS_REQUIRE(!parseJson("{\"a\":1} extra").ok);
    SPACELENS_REQUIRE(!parseJson("{\"a\":\"line\nbreak\"}").ok);
    SPACELENS_REQUIRE(!parseJson("").ok);
    SPACELENS_REQUIRE(!parseJson("[").ok);
}

SPACELENS_TEST(McpJson_stringify_has_no_raw_newlines)
{
    JsonValue v = JsonValue::emptyObject();
    v.set("text", JsonValue::fromString("line1\nline2\r\n"));
    const std::string out = v.stringify();
    SPACELENS_REQUIRE(out.find('\n') == std::string::npos);
    SPACELENS_REQUIRE(out.find('\r') == std::string::npos);
    SPACELENS_REQUIRE(out.find("\\n") != std::string::npos);
}

SPACELENS_TEST(McpJson_wide_utf8_roundtrip)
{
    const std::wstring wide = L"D:\\unicodé-文件";
    const std::string utf8 = utf8FromWide(wide);
    SPACELENS_REQUIRE(wideFromUtf8(utf8) == wide);
}

SPACELENS_TEST(McpJson_ids_equal)
{
    SPACELENS_REQUIRE(jsonIdsEqual(JsonValue::fromInt(7), JsonValue::fromInt(7)));
    SPACELENS_REQUIRE(jsonIdsEqual(JsonValue::fromString("a"),
                                   JsonValue::fromString("a")));
    SPACELENS_REQUIRE(!jsonIdsEqual(JsonValue::fromInt(7),
                                    JsonValue::fromString("7")));
}
