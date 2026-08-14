#include "mcp/Protocol.hpp"

#include "core/Json.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <queue>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace spacelens::mcp {
namespace {

constexpr const char* kMetaProtocolVersion =
    "io.modelcontextprotocol/protocolVersion";
constexpr const char* kMetaClientCapabilities =
    "io.modelcontextprotocol/clientCapabilities";
constexpr const char* kMetaServerInfo = "io.modelcontextprotocol/serverInfo";

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kUnsupportedProtocolVersion = -32022;

const char* kInstructions =
    "SpaceLens is a read-only Windows storage intelligence server. "
    "Ask storage_overview, then storage_opportunities, then storage_query "
    "to inspect a large candidate. storage_query and storage_duplicates "
    "require a published index and never refresh it. "
    "AI recommendation is not filesystem permission: filesystem_mutation "
    "is false. unique_review_bytes is not guaranteed reclaim and is not "
    "authorization to delete.";

JsonValue idOrNull(const JsonValue& message)
{
    const JsonValue* id = message.get("id");
    return id != nullptr ? *id : JsonValue::null();
}

bool hasId(const JsonValue& message)
{
    return message.get("id") != nullptr;
}

const JsonValue* paramsOf(const JsonValue& message)
{
    return message.get("params");
}

const JsonValue* metaOf(const JsonValue* params)
{
    if (params == nullptr) {
        return nullptr;
    }
    return params->get("_meta");
}

void writeStdoutLine(const std::string& line)
{
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void setBinaryStdio()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

bool isBlankLine(std::string_view line)
{
    for (const unsigned char ch : line) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            return false;
        }
    }
    return true;
}

}  // namespace

void McpServer::addTool(ToolDefinition tool)
{
    m_tools.push_back(std::move(tool));
}

bool McpServer::versionSupported(std::string_view version) const
{
    return version == kModernProtocolVersion || version == kLegacyProtocolVersion;
}

JsonValue McpServer::serverInfo() const
{
    JsonValue info = JsonValue::emptyObject();
    info.set("name", JsonValue::fromString(kServerName));
    info.set("version", JsonValue::fromString(SPACELENS_MCP_VERSION));
    info.set("title", JsonValue::fromString("SpaceLens"));
    info.set("description",
             JsonValue::fromString(
                 "Read-only Windows storage intelligence for AI harnesses"));
    return info;
}

JsonValue McpServer::toolsCapability() const
{
    JsonValue tools = JsonValue::emptyObject();
    JsonValue caps = JsonValue::emptyObject();
    caps.set("tools", std::move(tools));
    return caps;
}

JsonValue McpServer::makeResult(const JsonValue& id, JsonValue result,
                                bool modern) const
{
    if (modern && result.get("resultType") == nullptr) {
        result.set("resultType", JsonValue::fromString("complete"));
    }
    JsonValue message = JsonValue::emptyObject();
    message.set("jsonrpc", JsonValue::fromString("2.0"));
    message.set("id", id);
    message.set("result", std::move(result));
    return message;
}

JsonValue McpServer::makeError(const JsonValue& id, int code, std::string message,
                               JsonValue data) const
{
    JsonValue err = JsonValue::emptyObject();
    err.set("code", JsonValue::fromInt(code));
    err.set("message", JsonValue::fromString(std::move(message)));
    if (!data.isNull()) {
        err.set("data", std::move(data));
    }
    JsonValue out = JsonValue::emptyObject();
    out.set("jsonrpc", JsonValue::fromString("2.0"));
    out.set("id", id);
    out.set("error", std::move(err));
    return out;
}

JsonValue McpServer::toolListItem(const ToolDefinition& tool) const
{
    JsonValue item = JsonValue::emptyObject();
    item.set("name", JsonValue::fromString(tool.name));
    if (!tool.title.empty()) {
        item.set("title", JsonValue::fromString(tool.title));
    }
    item.set("description", JsonValue::fromString(tool.description));
    item.set("inputSchema", tool.inputSchema);
    if (!tool.outputSchema.isNull() && tool.outputSchema.isObject()) {
        item.set("outputSchema", tool.outputSchema);
    }
    JsonValue ann = JsonValue::emptyObject();
    ann.set("readOnlyHint", JsonValue::fromBool(tool.annotations.readOnlyHint));
    ann.set("destructiveHint",
            JsonValue::fromBool(tool.annotations.destructiveHint));
    ann.set("idempotentHint",
            JsonValue::fromBool(tool.annotations.idempotentHint));
    ann.set("openWorldHint", JsonValue::fromBool(tool.annotations.openWorldHint));
    item.set("annotations", std::move(ann));
    return item;
}

namespace {

void eraseMatchingIds(std::vector<JsonValue>& ids, const JsonValue& id)
{
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](const JsonValue& candidate) {
                                 return jsonIdsEqual(candidate, id);
                             }),
              ids.end());
}

bool containsId(const std::vector<JsonValue>& ids, const JsonValue& id)
{
    return std::any_of(ids.begin(), ids.end(), [&](const JsonValue& candidate) {
        return jsonIdsEqual(candidate, id);
    });
}

}  // namespace

void McpServer::beginRequest(const JsonValue& id)
{
    std::lock_guard lock(m_cancelMutex);
    eraseMatchingIds(m_recentFinished, id);
    m_currentId = id;
    m_currentStop = std::stop_source{};
    m_currentCancelled = containsId(m_pendingCancelled, id);
    eraseMatchingIds(m_pendingCancelled, id);
    if (m_currentCancelled) {
        m_currentStop.request_stop();
    }
}

bool McpServer::finishRequest(const JsonValue& id)
{
    std::lock_guard lock(m_cancelMutex);
    const bool cancelled = m_currentCancelled && m_currentId &&
                           jsonIdsEqual(*m_currentId, id);
    m_currentId.reset();
    m_currentCancelled = false;
    eraseMatchingIds(m_pendingCancelled, id);
    m_recentFinished.push_back(id);
    constexpr std::size_t kMaxRemembered = 32;
    if (m_recentFinished.size() > kMaxRemembered) {
        m_recentFinished.erase(m_recentFinished.begin());
    }
    return cancelled;
}

void McpServer::cancel(const JsonValue& requestId)
{
    std::lock_guard lock(m_cancelMutex);
    if (m_currentId && jsonIdsEqual(*m_currentId, requestId)) {
        m_currentCancelled = true;
        m_currentStop.request_stop();
        return;
    }
    if (containsId(m_recentFinished, requestId)) {
        return;
    }
    if (!containsId(m_pendingCancelled, requestId)) {
        m_pendingCancelled.push_back(requestId);
    }
}

bool McpServer::requireModernMeta(const JsonValue* params, const JsonValue& id,
                                  JsonValue& errorOut,
                                  std::string& requestedVersion)
{
    const JsonValue* meta = metaOf(params);
    if (meta == nullptr || !meta->isObject()) {
        errorOut = makeError(id, kInvalidParams,
                             "missing _meta.io.modelcontextprotocol/protocolVersion");
        return false;
    }
    const auto version = meta->stringAt(kMetaProtocolVersion);
    if (!version) {
        errorOut = makeError(id, kInvalidParams,
                             "missing _meta.io.modelcontextprotocol/protocolVersion");
        return false;
    }
    requestedVersion = *version;
    if (!versionSupported(*version)) {
        JsonValue data = JsonValue::emptyObject();
        JsonValue supported = JsonValue::emptyArray();
        supported.array.push_back(JsonValue::fromString(kModernProtocolVersion));
        supported.array.push_back(JsonValue::fromString(kLegacyProtocolVersion));
        data.set("supported", std::move(supported));
        data.set("requested", JsonValue::fromString(*version));
        errorOut = makeError(id, kUnsupportedProtocolVersion,
                             "Unsupported protocol version", std::move(data));
        return false;
    }
    if (meta->get(kMetaClientCapabilities) == nullptr) {
        errorOut = makeError(
            id, kInvalidParams,
            "missing _meta.io.modelcontextprotocol/clientCapabilities");
        return false;
    }
    return true;
}

JsonValue McpServer::handleDiscover(const JsonValue& id, const JsonValue* params)
{
    std::string requested;
    if (metaOf(params) != nullptr) {
        JsonValue error;
        if (!requireModernMeta(params, id, error, requested)) {
            return error;
        }
    }
    m_era = Era::Modern;
    JsonValue result = JsonValue::emptyObject();
    result.set("resultType", JsonValue::fromString("complete"));
    JsonValue versions = JsonValue::emptyArray();
    versions.array.push_back(JsonValue::fromString(kModernProtocolVersion));
    versions.array.push_back(JsonValue::fromString(kLegacyProtocolVersion));
    result.set("supportedVersions", std::move(versions));
    result.set("capabilities", toolsCapability());
    result.set("instructions", JsonValue::fromString(kInstructions));
    result.set("ttlMs", JsonValue::fromInt(3'600'000));
    result.set("cacheScope", JsonValue::fromString("public"));
    JsonValue meta = JsonValue::emptyObject();
    meta.set(kMetaServerInfo, serverInfo());
    result.set("_meta", std::move(meta));
    return makeResult(id, std::move(result), true);
}

JsonValue McpServer::handleInitialize(const JsonValue& id,
                                      const JsonValue* params)
{
    std::string requested = kLegacyProtocolVersion;
    if (params != nullptr) {
        if (const auto v = params->stringAt("protocolVersion")) {
            requested = *v;
        }
    }
    if (versionSupported(requested)) {
        m_legacyVersion = requested;
    } else {
        m_legacyVersion = kLegacyProtocolVersion;
    }
    m_era = Era::Legacy;
    JsonValue result = JsonValue::emptyObject();
    result.set("protocolVersion", JsonValue::fromString(m_legacyVersion));
    result.set("capabilities", toolsCapability());
    result.set("serverInfo", serverInfo());
    result.set("instructions", JsonValue::fromString(kInstructions));
    return makeResult(id, std::move(result), false);
}

JsonValue McpServer::handleToolsList(const JsonValue& id, bool modern)
{
    JsonValue tools = JsonValue::emptyArray();
    for (const auto& tool : m_tools) {
        tools.array.push_back(toolListItem(tool));
    }
    JsonValue result = JsonValue::emptyObject();
    result.set("tools", std::move(tools));
    if (modern) {
        result.set("ttlMs", JsonValue::fromInt(3'600'000));
        result.set("cacheScope", JsonValue::fromString("public"));
    }
    return makeResult(id, std::move(result), modern);
}

JsonValue McpServer::handleToolsCall(const JsonValue& id, const JsonValue* params,
                                     bool modern)
{
    if (params == nullptr || !params->isObject()) {
        return makeError(id, kInvalidParams, "tools/call requires params");
    }
    const auto name = params->stringAt("name");
    if (!name || name->empty()) {
        return makeError(id, kInvalidParams, "tools/call requires params.name");
    }
    const ToolDefinition* tool = nullptr;
    for (const auto& candidate : m_tools) {
        if (candidate.name == *name) {
            tool = &candidate;
            break;
        }
    }
    if (tool == nullptr) {
        return makeError(id, kInvalidParams, "Unknown tool: " + *name);
    }
    JsonValue arguments = JsonValue::emptyObject();
    if (const JsonValue* args = params->get("arguments")) {
        if (!args->isObject()) {
            return makeError(id, kInvalidParams, "params.arguments must be an object");
        }
        arguments = *args;
    }

    beginRequest(id);
    std::stop_token stop;
    {
        std::lock_guard lock(m_cancelMutex);
        stop = m_currentStop.get_token();
    }

    ToolResult toolResult;
    try {
        std::lock_guard analysis(m_analysisMutex);
        toolResult = tool->handler(arguments, stop);
    } catch (const std::exception& ex) {
        const bool cancelled = finishRequest(id);
        if (cancelled) {
            return JsonValue::null();
        }
        return makeError(id, kInternalError, ex.what());
    } catch (...) {
        const bool cancelled = finishRequest(id);
        if (cancelled) {
            return JsonValue::null();
        }
        return makeError(id, kInternalError, "internal error");
    }

    const bool cancelled = finishRequest(id);
    if (cancelled || toolResult.cancelled) {
        return JsonValue::null();
    }

    JsonValue contentItem = JsonValue::emptyObject();
    contentItem.set("type", JsonValue::fromString("text"));
    contentItem.set("text", JsonValue::fromString(toolResult.textJson));
    JsonValue content = JsonValue::emptyArray();
    content.array.push_back(std::move(contentItem));

    JsonValue result = JsonValue::emptyObject();
    result.set("content", std::move(content));
    result.set("structuredContent", toolResult.structured);
    result.set("isError", JsonValue::fromBool(toolResult.isError));
    return makeResult(id, std::move(result), modern);
}

JsonValue McpServer::handleRequest(const JsonValue& message)
{
    const JsonValue id = idOrNull(message);
    const auto method = message.stringAt("method");
    if (!method) {
        return makeError(id, kInvalidRequest, "missing method");
    }
    const JsonValue* params = paramsOf(message);

    if (*method == "initialize") {
        return handleInitialize(id, params);
    }
    if (*method == "server/discover") {
        return handleDiscover(id, params);
    }

    bool modern = m_era == Era::Modern;
    if (const JsonValue* meta = metaOf(params)) {
        std::string requested;
        JsonValue error;
        if (!requireModernMeta(params, id, error, requested)) {
            return error;
        }
        modern = requested == kModernProtocolVersion || m_era != Era::Legacy;
        if (m_era == Era::Undecided) {
            m_era = modern ? Era::Modern : Era::Legacy;
        }
    } else if (m_era == Era::Modern) {
        return makeError(id, kInvalidParams,
                         "missing _meta.io.modelcontextprotocol/protocolVersion");
    } else if (m_era == Era::Undecided) {
        modern = false;
    }

    if (*method == "ping") {
        return makeResult(id, JsonValue::emptyObject(), modern);
    }
    if (*method == "tools/list") {
        return handleToolsList(id, modern);
    }
    if (*method == "tools/call") {
        return handleToolsCall(id, params, modern);
    }
    return makeError(id, kMethodNotFound, "Method not found: " + *method);
}

std::optional<std::string> McpServer::handleLine(std::string_view line)
{
    if (isBlankLine(line)) {
        return std::nullopt;
    }
    if (line.size() > kMaxIncomingMessageBytes) {
        return makeError(JsonValue::null(), kParseError,
                         "message exceeds 1 MiB limit")
            .stringify();
    }
    const auto parsed = parseJson(line);
    if (!parsed.ok) {
        return makeError(JsonValue::null(), kParseError, parsed.error).stringify();
    }
    if (!parsed.value.isObject()) {
        return makeError(JsonValue::null(), kInvalidRequest,
                         "JSON-RPC message must be an object")
            .stringify();
    }
    const auto jsonrpc = parsed.value.stringAt("jsonrpc");
    if (!jsonrpc || *jsonrpc != "2.0") {
        return makeError(idOrNull(parsed.value), kInvalidRequest,
                         "jsonrpc must be \"2.0\"")
            .stringify();
    }
    const auto method = parsed.value.stringAt("method");
    if (!method) {
        return makeError(idOrNull(parsed.value), kInvalidRequest, "missing method")
            .stringify();
    }

    if (!hasId(parsed.value)) {
        if (*method == "notifications/cancelled") {
            if (const JsonValue* params = paramsOf(parsed.value)) {
                if (const JsonValue* requestId = params->get("requestId")) {
                    cancel(*requestId);
                }
            }
        }
        return std::nullopt;
    }

    JsonValue response = handleRequest(parsed.value);
    if (response.isNull()) {
        return std::nullopt;
    }
    return response.stringify();
}

int McpServer::runStdio()
{
    setBinaryStdio();

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::queue<std::string> lines;
    bool eof = false;

    std::thread reader([&] {
        std::string line;
        int ch = 0;
        while ((ch = std::fgetc(stdin)) != EOF) {
            if (ch == '\n') {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const auto parsed = parseJson(line);
                if (parsed.ok && parsed.value.isObject() &&
                    !hasId(parsed.value)) {
                    const auto method = parsed.value.stringAt("method");
                    if (method && *method == "notifications/cancelled") {
                        if (const JsonValue* params = paramsOf(parsed.value)) {
                            if (const JsonValue* requestId =
                                    params->get("requestId")) {
                                cancel(*requestId);
                            }
                        }
                        line.clear();
                        continue;
                    }
                }
                {
                    std::lock_guard lock(queueMutex);
                    lines.push(std::move(line));
                }
                queueCv.notify_one();
                line.clear();
                continue;
            }
            if (line.size() >= kMaxIncomingMessageBytes) {
                // Discard the rest of this line; enqueue an oversize marker.
                while ((ch = std::fgetc(stdin)) != EOF && ch != '\n') {
                }
                {
                    std::lock_guard lock(queueMutex);
                    lines.push(std::string(kMaxIncomingMessageBytes + 1, 'x'));
                }
                queueCv.notify_one();
                line.clear();
                continue;
            }
            line.push_back(static_cast<char>(ch));
        }
        if (!line.empty()) {
            std::lock_guard lock(queueMutex);
            lines.push(std::move(line));
        }
        {
            std::lock_guard lock(queueMutex);
            eof = true;
        }
        queueCv.notify_one();
    });

    while (true) {
        std::string line;
        {
            std::unique_lock lock(queueMutex);
            queueCv.wait(lock, [&] { return !lines.empty() || eof; });
            if (lines.empty() && eof) {
                break;
            }
            line = std::move(lines.front());
            lines.pop();
        }
        const auto parsed = parseJson(line);
        if (parsed.ok && parsed.value.isObject() && !hasId(parsed.value)) {
            const auto method = parsed.value.stringAt("method");
            if (method && *method == "notifications/cancelled") {
                if (const JsonValue* params = paramsOf(parsed.value)) {
                    if (const JsonValue* requestId = params->get("requestId")) {
                        cancel(*requestId);
                    }
                }
                continue;
            }
        }
        const auto response = handleLine(line);
        if (response) {
            writeStdoutLine(*response);
        }
    }

    reader.join();
    return 0;
}

}  // namespace spacelens::mcp
