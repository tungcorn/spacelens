#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spacelens::json {

enum class JsonKind {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

struct JsonValue {
    JsonKind kind = JsonKind::Null;
    bool boolean = false;
    bool numberIsInteger = false;
    double number = 0.0;
    std::int64_t integer = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    [[nodiscard]] static JsonValue null() { return {}; }
    [[nodiscard]] static JsonValue fromBool(bool value);
    [[nodiscard]] static JsonValue fromInt(std::int64_t value);
    [[nodiscard]] static JsonValue fromUint(std::uint64_t value);
    [[nodiscard]] static JsonValue fromNumber(double value);
    [[nodiscard]] static JsonValue fromString(std::string value);
    [[nodiscard]] static JsonValue emptyArray();
    [[nodiscard]] static JsonValue emptyObject();

    [[nodiscard]] bool isNull() const noexcept { return kind == JsonKind::Null; }
    [[nodiscard]] bool isBool() const noexcept { return kind == JsonKind::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return kind == JsonKind::Number; }
    [[nodiscard]] bool isString() const noexcept { return kind == JsonKind::String; }
    [[nodiscard]] bool isArray() const noexcept { return kind == JsonKind::Array; }
    [[nodiscard]] bool isObject() const noexcept { return kind == JsonKind::Object; }

    [[nodiscard]] const JsonValue* get(std::string_view key) const;
    [[nodiscard]] std::optional<std::string> stringAt(std::string_view key) const;
    [[nodiscard]] std::optional<bool> boolAt(std::string_view key) const;
    [[nodiscard]] std::optional<std::int64_t> intAt(std::string_view key) const;
    [[nodiscard]] std::optional<std::uint64_t> uintAt(std::string_view key) const;

    void set(std::string key, JsonValue value);

    [[nodiscard]] std::string stringify() const;
};

struct JsonParseResult {
    bool ok = false;
    JsonValue value;
    std::string error;
};

[[nodiscard]] JsonParseResult parseJson(std::string_view text);
[[nodiscard]] bool jsonIdsEqual(const JsonValue& a, const JsonValue& b);

}  // namespace spacelens::json
