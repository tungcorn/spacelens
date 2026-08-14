#include "TestRunner.hpp"

#include "mcp/Protocol.hpp"
#include "mcp/StorageTools.hpp"

#include <string>

using namespace spacelens::mcp;

namespace {

std::string modernMeta()
{
    return "\"_meta\":{"
           "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
           "\"io.modelcontextprotocol/clientCapabilities\":{},"
           "\"io.modelcontextprotocol/clientInfo\":{\"name\":\"test\",\"version\":\"0\"}"
           "}";
}

JsonValue mustParse(const std::string& text)
{
    const auto parsed = parseJson(text);
    if (!parsed.ok) {
        throw spacelens::test::Failure("parse failed: " + parsed.error +
                                       " text=" + text);
    }
    return parsed.value;
}

}  // namespace

SPACELENS_TEST(McpProtocol_discover_lists_supported_versions)
{
    McpServer server;
    registerStorageTools(server);
    const std::string req =
        std::string("{\"jsonrpc\":\"2.0\",\"id\":\"d1\",\"method\":\"server/discover\","
                    "\"params\":{") +
        modernMeta() + "}}";
    const auto line = server.handleLine(req);
    SPACELENS_REQUIRE(line.has_value());
    const auto msg = mustParse(*line);
    SPACELENS_REQUIRE(msg.get("result") != nullptr);
    const auto* result = msg.get("result");
    SPACELENS_REQUIRE_EQ(*result->stringAt("resultType"), std::string("complete"));
    const auto* versions = result->get("supportedVersions");
    SPACELENS_REQUIRE(versions != nullptr && versions->isArray());
    SPACELENS_REQUIRE(versions->array.size() >= 2);
    SPACELENS_REQUIRE_EQ(versions->array[0].string, std::string(kModernProtocolVersion));
    const auto* caps = result->get("capabilities");
    SPACELENS_REQUIRE(caps != nullptr && caps->get("tools") != nullptr);
    SPACELENS_REQUIRE(caps->get("resources") == nullptr);
    SPACELENS_REQUIRE(caps->get("prompts") == nullptr);
}

SPACELENS_TEST(McpProtocol_legacy_initialize_then_tools_list)
{
    McpServer server;
    registerStorageTools(server);
    const auto init = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"legacy\",\"version\":\"1\"}}}");
    SPACELENS_REQUIRE(init.has_value());
    const auto initMsg = mustParse(*init);
    SPACELENS_REQUIRE_EQ(*initMsg.get("result")->stringAt("protocolVersion"),
                         std::string(kLegacyProtocolVersion));

    SPACELENS_REQUIRE(!server
                           .handleLine("{\"jsonrpc\":\"2.0\",\"method\":"
                                       "\"notifications/initialized\"}")
                           .has_value());

    const auto listed = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}");
    SPACELENS_REQUIRE(listed.has_value());
    const auto listMsg = mustParse(*listed);
    const auto* tools = listMsg.get("result")->get("tools");
    SPACELENS_REQUIRE(tools != nullptr && tools->isArray());
    SPACELENS_REQUIRE_EQ(tools->array.size(), 6ULL);
    SPACELENS_REQUIRE_EQ(tools->array[0].stringAt("name").value_or(""),
                         std::string("storage_capabilities"));
    SPACELENS_REQUIRE_EQ(tools->array[5].stringAt("name").value_or(""),
                         std::string("storage_index_status"));
    for (const auto& tool : tools->array) {
        const auto* ann = tool.get("annotations");
        SPACELENS_REQUIRE(ann != nullptr);
        SPACELENS_REQUIRE(*ann->boolAt("readOnlyHint"));
        SPACELENS_REQUIRE(!(*ann->boolAt("destructiveHint")));
    }
}

SPACELENS_TEST(McpProtocol_unknown_tool_is_invalid_params)
{
    McpServer server;
    registerStorageTools(server);
    const std::string req =
        std::string("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                    "\"params\":{\"name\":\"index_refresh\",\"arguments\":{},") +
        modernMeta() + "}}";
    const auto line = server.handleLine(req);
    SPACELENS_REQUIRE(line.has_value());
    const auto msg = mustParse(*line);
    SPACELENS_REQUIRE(msg.get("error") != nullptr);
    SPACELENS_REQUIRE_EQ(*msg.get("error")->intAt("code"), -32602);
}

SPACELENS_TEST(McpProtocol_mutation_tools_are_not_registered)
{
    McpServer server;
    registerStorageTools(server);
    for (const auto& tool : server.tools()) {
        SPACELENS_REQUIRE(tool.name.find("delete") == std::string::npos);
        SPACELENS_REQUIRE(tool.name.find("recycle") == std::string::npos);
        SPACELENS_REQUIRE(tool.name.find("restore") == std::string::npos);
        SPACELENS_REQUIRE(tool.name.find("refresh") == std::string::npos);
        SPACELENS_REQUIRE(tool.name.find("execute") == std::string::npos);
        SPACELENS_REQUIRE(tool.annotations.readOnlyHint);
        SPACELENS_REQUIRE(!tool.annotations.destructiveHint);
    }
}

SPACELENS_TEST(McpProtocol_unsupported_version_32022)
{
    McpServer server;
    registerStorageTools(server);
    const auto line = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":\"v\",\"method\":\"tools/list\",\"params\":{"
        "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"1999-01-01\","
        "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    SPACELENS_REQUIRE(line.has_value());
    const auto msg = mustParse(*line);
    SPACELENS_REQUIRE_EQ(*msg.get("error")->intAt("code"), -32022);
    SPACELENS_REQUIRE(msg.get("error")->get("data")->get("supported") != nullptr);
}

SPACELENS_TEST(McpProtocol_invalid_json_does_not_throw)
{
    McpServer server;
    registerStorageTools(server);
    const auto line = server.handleLine("{not json");
    SPACELENS_REQUIRE(line.has_value());
    const auto msg = mustParse(*line);
    SPACELENS_REQUIRE_EQ(*msg.get("error")->intAt("code"), -32700);
}

SPACELENS_TEST(McpProtocol_capabilities_tool_reports_read_only)
{
    McpServer server;
    registerStorageTools(server);
    const auto line = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"storage_capabilities\",\"arguments\":{}}}");
    SPACELENS_REQUIRE(line.has_value());
    const auto msg = mustParse(*line);
    const auto* result = msg.get("result");
    SPACELENS_REQUIRE(result != nullptr);
    SPACELENS_REQUIRE(!(*result->boolAt("isError")));
    const auto* structured = result->get("structuredContent");
    SPACELENS_REQUIRE(structured != nullptr);
    SPACELENS_REQUIRE(*structured->boolAt("read_only"));
    SPACELENS_REQUIRE(!(*structured->boolAt("filesystem_mutation")));
    SPACELENS_REQUIRE(*structured->get("features")->boolAt("filesystem_mutation") ==
                      false);
}

SPACELENS_TEST(McpProtocol_query_live_scan_is_domain_error)
{
    McpServer server;
    registerStorageTools(server);
    const auto line = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"storage_query\",\"arguments\":{"
        "\"path\":\"C:\\\\nope\",\"object_type\":\"file\",\"source\":\"live_scan\"}}}");
    SPACELENS_REQUIRE(line.has_value());
    const auto msg = mustParse(*line);
    SPACELENS_REQUIRE(*msg.get("result")->boolAt("isError"));
}

SPACELENS_TEST(McpProtocol_cancel_before_begin_suppresses_result)
{
    McpServer server;
    registerStorageTools(server);
    server.cancel(JsonValue::fromInt(1));
    const auto line = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"storage_capabilities\",\"arguments\":{}}}");
    SPACELENS_REQUIRE(!line.has_value());
}

SPACELENS_TEST(McpProtocol_late_cancel_does_not_poison_next_request)
{
    McpServer server;
    registerStorageTools(server);
    const auto first = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"storage_capabilities\",\"arguments\":{}}}");
    SPACELENS_REQUIRE(first.has_value());
    server.cancel(JsonValue::fromInt(1));
    const auto second = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"storage_capabilities\",\"arguments\":{}}}");
    SPACELENS_REQUIRE(second.has_value());
    const auto msg = mustParse(*second);
    SPACELENS_REQUIRE(msg.get("result") != nullptr);
    SPACELENS_REQUIRE(!(*msg.get("result")->boolAt("isError")));
}

SPACELENS_TEST(McpProtocol_ping_returns_empty_result)
{
    McpServer server;
    registerStorageTools(server);
    const auto line = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":\"p\",\"method\":\"ping\",\"params\":{}}");
    SPACELENS_REQUIRE(line.has_value());
    const auto msg = mustParse(*line);
    SPACELENS_REQUIRE(msg.get("result") != nullptr);
    SPACELENS_REQUIRE(msg.get("error") == nullptr);
}

SPACELENS_TEST(McpProtocol_blank_line_is_ignored)
{
    McpServer server;
    registerStorageTools(server);
    SPACELENS_REQUIRE(!server.handleLine("").has_value());
    SPACELENS_REQUIRE(!server.handleLine("   ").has_value());
    SPACELENS_REQUIRE(!server.handleLine("\t").has_value());
}

SPACELENS_TEST(McpProtocol_oversized_line_is_parse_error)
{
    McpServer server;
    registerStorageTools(server);
    const std::string huge(kMaxIncomingMessageBytes + 8, 'x');
    const auto line = server.handleLine(huge);
    SPACELENS_REQUIRE(line.has_value());
    const auto msg = mustParse(*line);
    SPACELENS_REQUIRE_EQ(*msg.get("error")->intAt("code"), -32700);
    SPACELENS_REQUIRE(line->find('\n') == std::string::npos);
}
