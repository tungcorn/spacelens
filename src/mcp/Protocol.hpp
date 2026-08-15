#pragma once

#include "mcp/JsonValue.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens::mcp {

inline constexpr const char* kModernProtocolVersion = "2026-07-28";
inline constexpr const char* kLegacyProtocolVersion = "2025-11-25";
inline constexpr std::size_t kMaxIncomingMessageBytes = 1024U * 1024U;
inline constexpr const char* kServerName = "spacelens-mcp";

#ifndef SPACELENS_VERSION_STRING
#define SPACELENS_MCP_VERSION "0.1.5"
#else
#define SPACELENS_MCP_VERSION SPACELENS_VERSION_STRING
#endif

struct ToolAnnotations {
    bool readOnlyHint = true;
    bool destructiveHint = false;
    bool idempotentHint = true;
    bool openWorldHint = true;
};

struct ToolResult {
    bool isError = false;
    std::string textJson;
    JsonValue structured = JsonValue::emptyObject();
    bool cancelled = false;
};

struct ToolDefinition {
    std::string name;
    std::string title;
    std::string description;
    JsonValue inputSchema = JsonValue::emptyObject();
    JsonValue outputSchema = JsonValue::emptyObject();
    ToolAnnotations annotations;
    std::function<ToolResult(const JsonValue& arguments, std::stop_token stop)>
        handler;
};

class McpServer {
public:
    void addTool(ToolDefinition tool);

    /// Process one NDJSON line. Returns a protocol line without the trailing
    /// newline, or nullopt for notifications / cancelled in-flight work.
    std::optional<std::string> handleLine(std::string_view line);

    /// Cancel the in-flight request if `requestId` matches.
    void cancel(const JsonValue& requestId);

    /// stdio loop: stdout is protocol-only. Returns 0 on stdin EOF.
    int runStdio();

    [[nodiscard]] const std::vector<ToolDefinition>& tools() const noexcept
    {
        return m_tools;
    }

private:
    enum class Era {
        Undecided,
        Modern,
        Legacy
    };

    std::vector<ToolDefinition> m_tools;
    Era m_era = Era::Undecided;
    std::string m_legacyVersion = kLegacyProtocolVersion;

    std::mutex m_cancelMutex;
    std::optional<JsonValue> m_currentId;
    std::stop_source m_currentStop;
    bool m_currentCancelled = false;
    std::vector<JsonValue> m_pendingCancelled;
    std::vector<JsonValue> m_recentFinished;

    std::mutex m_analysisMutex;

    JsonValue handleRequest(const JsonValue& message);
    JsonValue handleDiscover(const JsonValue& id, const JsonValue* params);
    JsonValue handleInitialize(const JsonValue& id, const JsonValue* params);
    JsonValue handleToolsList(const JsonValue& id, bool modern);
    JsonValue handleToolsCall(const JsonValue& id, const JsonValue* params,
                              bool modern);

    JsonValue makeResult(const JsonValue& id, JsonValue result, bool modern) const;
    JsonValue makeError(const JsonValue& id, int code, std::string message,
                        JsonValue data = JsonValue::null()) const;
    JsonValue serverInfo() const;
    JsonValue toolsCapability() const;
    JsonValue toolListItem(const ToolDefinition& tool) const;
    bool requireModernMeta(const JsonValue* params, const JsonValue& id,
                           JsonValue& errorOut, std::string& requestedVersion);
    bool versionSupported(std::string_view version) const;
    void beginRequest(const JsonValue& id);
    bool finishRequest(const JsonValue& id);
};

}  // namespace spacelens::mcp
