#include "mcp/StorageTools.hpp"

#include "core/Classification.hpp"
#include "core/Json.hpp"
#include "core/StorageAnalysis.hpp"
#include "core/index/IndexQuery.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace spacelens::mcp {
namespace {

JsonValue objectType(const char* type)
{
    JsonValue v = JsonValue::emptyObject();
    v.set("type", JsonValue::fromString(type));
    return v;
}

JsonValue stringEnum(std::initializer_list<const char*> values)
{
    JsonValue t = objectType("string");
    JsonValue enums = JsonValue::emptyArray();
    for (const char* value : values) {
        enums.array.push_back(JsonValue::fromString(value));
    }
    t.set("enum", std::move(enums));
    return t;
}

JsonValue integerSchema(std::int64_t minimum, std::int64_t maximum)
{
    JsonValue t = objectType("integer");
    t.set("minimum", JsonValue::fromInt(minimum));
    t.set("maximum", JsonValue::fromInt(maximum));
    return t;
}

JsonValue analysisOutputSchema()
{
    JsonValue schema = objectType("object");
    JsonValue props = JsonValue::emptyObject();
    props.set("ok", objectType("boolean"));
    props.set("read_only", objectType("boolean"));
    props.set("filesystem_mutation", objectType("boolean"));
    schema.set("properties", std::move(props));
    schema.set("additionalProperties", JsonValue::fromBool(true));
    return schema;
}

JsonValue requiredPathSchema(JsonValue properties)
{
    JsonValue schema = objectType("object");
    schema.set("properties", std::move(properties));
    JsonValue required = JsonValue::emptyArray();
    required.array.push_back(JsonValue::fromString("path"));
    schema.set("required", std::move(required));
    schema.set("additionalProperties", JsonValue::fromBool(false));
    return schema;
}

ToolResult fromDomainJson(std::string jsonText, bool isError)
{
    ToolResult result;
    result.isError = isError;
    while (!jsonText.empty() &&
           (jsonText.back() == '\n' || jsonText.back() == '\r')) {
        jsonText.pop_back();
    }
    result.textJson = jsonText;
    const auto parsed = parseJson(jsonText);
    if (parsed.ok) {
        result.structured = parsed.value;
    } else {
        result.structured = JsonValue::fromString(jsonText);
    }
    return result;
}

ToolResult invalidArgs(const std::string& message)
{
    JsonValue body = JsonValue::emptyObject();
    body.set("ok", JsonValue::fromBool(false));
    body.set("error", JsonValue::fromString(message));
    body.set("read_only", JsonValue::fromBool(true));
    body.set("filesystem_mutation", JsonValue::fromBool(false));
    ToolResult result;
    result.isError = true;
    result.structured = body;
    result.textJson = body.stringify();
    return result;
}

std::optional<std::wstring> requiredPath(const JsonValue& args, ToolResult& error)
{
    const auto path = args.stringAt("path");
    if (!path || path->empty()) {
        error = invalidArgs("path is required");
        return std::nullopt;
    }
    return spacelens::wideFromUtf8(*path);
}

bool parseLimit(const JsonValue& args, const char* key, std::size_t fallback,
                std::size_t maxValue, std::size_t& out, ToolResult& error)
{
    if (args.get(key) == nullptr) {
        out = fallback;
        return true;
    }
    const auto value = args.intAt(key);
    if (!value || *value < 1 ||
        static_cast<std::uint64_t>(*value) > maxValue) {
        error = invalidArgs(std::string(key) + " must be an integer from 1 to " +
                            std::to_string(maxValue));
        return false;
    }
    out = static_cast<std::size_t>(*value);
    return true;
}

bool parseOptionalUint(const JsonValue& args, const char* key,
                       std::optional<std::uint64_t>& out, ToolResult& error)
{
    if (args.get(key) == nullptr) {
        return true;
    }
    const auto value = args.intAt(key);
    if (!value || *value < 0) {
        error = invalidArgs(std::string(key) + " must be a non-negative integer");
        return false;
    }
    out = static_cast<std::uint64_t>(*value);
    return true;
}

ToolAnnotations readOnlyAnnotations()
{
    return {};
}

ToolResult callCapabilities(const JsonValue&, std::stop_token)
{
    return fromDomainJson(spacelens::mcpCapabilitiesJson(), false);
}

ToolResult callOverview(const JsonValue& args, std::stop_token stop)
{
    ToolResult error;
    const auto path = requiredPath(args, error);
    if (!path) {
        return error;
    }
    bool fromIndex = false;
    if (const auto source = args.stringAt("source")) {
        std::string sourceError;
        if (!spacelens::parseEvidenceSource(*source, fromIndex, sourceError)) {
            return invalidArgs(sourceError);
        }
    }
    std::size_t limit = spacelens::kDefaultOverviewLimit;
    if (!parseLimit(args, "limit", spacelens::kDefaultOverviewLimit,
                    spacelens::kMaxOverviewLimit, limit, error)) {
        return error;
    }
    spacelens::OverviewRequest request;
    request.root = *path;
    request.fromIndex = fromIndex;
    request.limit = limit;
    if (stop.stop_requested()) {
        ToolResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
    const auto analysis = spacelens::analyzeOverview(request, stop);
    if (stop.stop_requested() || analysis.error == spacelens::AnalysisError::Cancelled) {
        ToolResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
    return fromDomainJson(analysis.report.toJson(), !analysis.report.ok);
}

ToolResult callOpportunities(const JsonValue& args, std::stop_token stop)
{
    ToolResult error;
    const auto path = requiredPath(args, error);
    if (!path) {
        return error;
    }
    bool fromIndex = false;
    if (const auto source = args.stringAt("source")) {
        std::string sourceError;
        if (!spacelens::parseEvidenceSource(*source, fromIndex, sourceError)) {
            return invalidArgs(sourceError);
        }
    }
    std::size_t limit = spacelens::kDefaultOpportunityLimit;
    if (!parseLimit(args, "limit", spacelens::kDefaultOpportunityLimit,
                    spacelens::kMaxOpportunityLimit, limit, error)) {
        return error;
    }
    std::optional<std::uint64_t> minSize;
    std::optional<std::uint64_t> olderThan;
    if (!parseOptionalUint(args, "min_size_bytes", minSize, error) ||
        !parseOptionalUint(args, "older_than_days", olderThan, error)) {
        return error;
    }
    spacelens::OpportunityRequest request;
    request.root = *path;
    request.fromIndex = fromIndex;
    request.query.limit = limit;
    request.query.minSize =
        minSize.value_or(spacelens::kDefaultOpportunityMinSize);
    request.query.olderThanDays =
        olderThan.value_or(spacelens::kDefaultOldLargeDays);
    if (const auto classification = args.stringAt("classification")) {
        if (!spacelens::isKnownStorageCategoryName(*classification)) {
            request.query.matchNone = true;
        } else {
            request.query.categoryOnly =
                spacelens::parseStorageCategory(*classification);
        }
    }
    if (const auto under = args.stringAt("under")) {
        request.query.pathPrefix = spacelens::wideFromUtf8(*under);
    }
    if (stop.stop_requested()) {
        ToolResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
    const auto analysis = spacelens::analyzeOpportunities(request, stop);
    if (stop.stop_requested() ||
        analysis.error == spacelens::AnalysisError::Cancelled) {
        ToolResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
    return fromDomainJson(analysis.report.toJson(), !analysis.report.ok);
}

ToolResult callQuery(const JsonValue& args, std::stop_token stop)
{
    ToolResult error;
    const auto path = requiredPath(args, error);
    if (!path) {
        return error;
    }
    if (const auto source = args.stringAt("source")) {
        if (*source == "live_scan") {
            return invalidArgs(
                "storage_query is index-only; use storage_overview with "
                "source=live_scan for a live snapshot");
        }
        if (*source != "persistent_index") {
            return invalidArgs("source must be persistent_index for storage_query");
        }
    }
    const auto objectType = args.stringAt("object_type");
    if (!objectType) {
        return invalidArgs("object_type is required (file or directory)");
    }
    spacelens::IndexQuerySpec spec;
    if (*objectType == "file") {
        spec.includeFiles = true;
        spec.includeDirectories = false;
    } else if (*objectType == "directory") {
        spec.includeFiles = false;
        spec.includeDirectories = true;
    } else {
        return invalidArgs("object_type must be file or directory");
    }
    std::size_t limit = 20;
    if (!parseLimit(args, "limit", 20, spacelens::kMaxQueryLimit, limit, error)) {
        return error;
    }
    spec.limit = limit;
    std::optional<std::uint64_t> minSize;
    std::optional<std::uint64_t> olderThan;
    if (!parseOptionalUint(args, "min_size_bytes", minSize, error) ||
        !parseOptionalUint(args, "older_than_days", olderThan, error)) {
        return error;
    }
    spec.minSize = minSize;
    spec.olderThanDays = olderThan;
    if (const auto under = args.stringAt("under")) {
        spec.pathPrefix = spacelens::wideFromUtf8(*under);
    }
    if (const auto classification = args.stringAt("classification")) {
        const auto cat = spacelens::parseStorageCategory(*classification);
        spec.classification = spacelens::toString(cat);
    }
    if (const auto reclaim = args.stringAt("reclaimability")) {
        spec.reclaimability = *reclaim;
    }
    if (const auto strength = args.stringAt("candidate_strength")) {
        if (*strength == "strong" || *strength == "Strong") {
            spec.candidateStrength = "Strong";
        } else if (*strength == "moderate" || *strength == "Moderate") {
            spec.candidateStrength = "Moderate";
        } else if (*strength == "reviewonly" || *strength == "ReviewOnly" ||
                   *strength == "review") {
            spec.candidateStrength = "ReviewOnly";
        } else if (*strength == "none" || *strength == "None") {
            spec.candidateStrength = "None";
        } else {
            spec.candidateStrength = *strength;
        }
    }
    if (const auto sort = args.stringAt("sort")) {
        if (*sort == "size") {
            spec.sortBy = spacelens::IndexSortKey::Size;
        } else if (*sort == "name") {
            spec.sortBy = spacelens::IndexSortKey::Name;
        } else if (*sort == "last_write") {
            spec.sortBy = spacelens::IndexSortKey::LastWrite;
        } else if (*sort == "classification") {
            spec.sortBy = spacelens::IndexSortKey::Classification;
        } else if (*sort == "candidate_strength") {
            spec.sortBy = spacelens::IndexSortKey::CandidateStrength;
        } else {
            return invalidArgs(
                "sort must be size, name, last_write, classification, or "
                "candidate_strength");
        }
    }
    spec.sortDescending = true;
    if (stop.stop_requested()) {
        ToolResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
    const auto result = spacelens::queryIndex(*path, spec);
    return fromDomainJson(spacelens::indexQueryToJson(result), !result.ok);
}

ToolResult callDuplicates(const JsonValue& args, std::stop_token stop)
{
    ToolResult error;
    const auto path = requiredPath(args, error);
    if (!path) {
        return error;
    }
    std::optional<std::uint64_t> minSize;
    if (!parseOptionalUint(args, "min_size_bytes", minSize, error)) {
        return error;
    }
    spacelens::DuplicateRequest request;
    request.root = *path;
    request.minSize = minSize.value_or(spacelens::kDefaultDuplicateMinSize);
    if (stop.stop_requested()) {
        ToolResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
    const auto result = spacelens::analyzeDuplicates(request, stop);
    if (stop.stop_requested() || result.cancelled) {
        ToolResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
    spacelens::DuplicateScanOptions options;
    options.minimumSize = request.minSize;
    return fromDomainJson(result.toJson(options),
                          !result.error.empty() || !result.completed);
}

ToolResult callIndexStatus(const JsonValue& args, std::stop_token)
{
    ToolResult error;
    const auto path = requiredPath(args, error);
    if (!path) {
        return error;
    }
    const auto doc = spacelens::analyzeIndexStatus(*path);
    return fromDomainJson(spacelens::indexStatusToJson(doc.status, doc.probe),
                          !doc.status.ok);
}

}  // namespace

void registerStorageTools(McpServer& server)
{
    {
        ToolDefinition tool;
        tool.name = "storage_capabilities";
        tool.title = "Storage capabilities";
        tool.description =
            "Report the read-only SpaceLens MCP contract: supported tools, "
            "protocol versions, and filesystem_mutation=false. Call this first.";
        JsonValue props = JsonValue::emptyObject();
        JsonValue schema = objectType("object");
        schema.set("properties", std::move(props));
        schema.set("additionalProperties", JsonValue::fromBool(false));
        tool.inputSchema = std::move(schema);
        tool.outputSchema = analysisOutputSchema();
        tool.annotations = readOnlyAnnotations();
        tool.handler = callCapabilities;
        server.addTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "storage_overview";
        tool.title = "Storage overview";
        tool.description =
            "What is consuming a path: largest directories and files from one "
            "live scan or one published index. Does not recommend deletion.";
        JsonValue props = JsonValue::emptyObject();
        JsonValue path = objectType("string");
        path.set("description", JsonValue::fromString("Absolute directory path"));
        props.set("path", std::move(path));
        props.set("source", stringEnum({"live_scan", "persistent_index"}));
        props.set("limit", integerSchema(1, static_cast<std::int64_t>(
                                                spacelens::kMaxOverviewLimit)));
        tool.inputSchema = requiredPathSchema(std::move(props));
        tool.outputSchema = analysisOutputSchema();
        tool.annotations = readOnlyAnnotations();
        tool.handler = callOverview;
        server.addTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "storage_opportunities";
        tool.title = "Storage opportunities";
        tool.description =
            "Highest-value review opportunities (regenerable developer storage "
            "and old/large items). unique_review_bytes is not reclaim and not "
            "authorization to delete.";
        JsonValue props = JsonValue::emptyObject();
        props.set("path", objectType("string"));
        props.set("source", stringEnum({"live_scan", "persistent_index"}));
        props.set("limit", integerSchema(1, static_cast<std::int64_t>(
                                                spacelens::kMaxOpportunityLimit)));
        props.set("min_size_bytes", integerSchema(0, 9'000'000'000'000'000));
        props.set("older_than_days", integerSchema(0, 36500));
        props.set("classification", objectType("string"));
        props.set("under", objectType("string"));
        tool.inputSchema = requiredPathSchema(std::move(props));
        tool.outputSchema = analysisOutputSchema();
        tool.annotations = readOnlyAnnotations();
        tool.handler = callOpportunities;
        server.addTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "storage_query";
        tool.title = "Indexed storage query";
        tool.description =
            "Bounded index-only drill-down under a path. Never live-scans and "
            "never refreshes the index. Missing index is a domain error.";
        JsonValue props = JsonValue::emptyObject();
        props.set("path", objectType("string"));
        props.set("under", objectType("string"));
        props.set("object_type", stringEnum({"file", "directory"}));
        props.set("classification", objectType("string"));
        props.set("reclaimability", objectType("string"));
        props.set("candidate_strength", objectType("string"));
        props.set("min_size_bytes", integerSchema(0, 9'000'000'000'000'000));
        props.set("older_than_days", integerSchema(0, 36500));
        props.set("sort", stringEnum({"size", "name", "last_write",
                                      "classification", "candidate_strength"}));
        props.set("limit", integerSchema(1, static_cast<std::int64_t>(
                                                spacelens::kMaxQueryLimit)));
        props.set("source", stringEnum({"persistent_index"}));
        JsonValue schema = objectType("object");
        schema.set("properties", std::move(props));
        JsonValue required = JsonValue::emptyArray();
        required.array.push_back(JsonValue::fromString("path"));
        required.array.push_back(JsonValue::fromString("object_type"));
        schema.set("required", std::move(required));
        schema.set("additionalProperties", JsonValue::fromBool(false));
        tool.inputSchema = std::move(schema);
        tool.outputSchema = analysisOutputSchema();
        tool.annotations = readOnlyAnnotations();
        tool.handler = callQuery;
        server.addTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "storage_duplicates";
        tool.title = "Verified duplicates";
        tool.description =
            "Hash-verified, hardlink-aware duplicate groups from a published "
            "index. Hard-link aliases are not redundant copies.";
        JsonValue props = JsonValue::emptyObject();
        props.set("path", objectType("string"));
        props.set("min_size_bytes", integerSchema(0, 9'000'000'000'000'000));
        tool.inputSchema = requiredPathSchema(std::move(props));
        tool.outputSchema = analysisOutputSchema();
        tool.annotations = readOnlyAnnotations();
        tool.handler = callDuplicates;
        server.addTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "storage_index_status";
        tool.title = "Index status";
        tool.description =
            "Whether a published index exists and how old it is. Never "
            "refreshes or rebuilds the index.";
        JsonValue props = JsonValue::emptyObject();
        props.set("path", objectType("string"));
        tool.inputSchema = requiredPathSchema(std::move(props));
        tool.outputSchema = analysisOutputSchema();
        tool.annotations = readOnlyAnnotations();
        tool.handler = callIndexStatus;
        server.addTool(std::move(tool));
    }
}

}  // namespace spacelens::mcp
