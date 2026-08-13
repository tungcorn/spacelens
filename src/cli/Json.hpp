#pragma once

#include "core/Json.hpp"

namespace spacelens::cli {

// Compatibility aliases. JSON implementation lives in core so it can be used
// by core planning/export code without depending on the CLI target.
using ::spacelens::jsonBool;
using ::spacelens::jsonEscape;
using ::spacelens::jsonInt;
using ::spacelens::jsonString;
using ::spacelens::jsonUInt;
using ::spacelens::utf8FromWide;

}  // namespace spacelens::cli
