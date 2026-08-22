#pragma once

#include "core/JsonValue.hpp"

namespace spacelens::mcp {

/// Compatibility aliases keep the MCP wire implementation source-compatible;
/// the parser implementation is owned by the Qt-free core library.
using JsonKind = ::spacelens::json::JsonKind;
using JsonValue = ::spacelens::json::JsonValue;
using JsonParseResult = ::spacelens::json::JsonParseResult;
using ::spacelens::json::jsonIdsEqual;
using ::spacelens::json::parseJson;

}  // namespace spacelens::mcp
