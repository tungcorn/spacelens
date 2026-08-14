#include "mcp/JsonValue.hpp"

#include "core/Json.hpp"

#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>

namespace spacelens::mcp {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text)
        : m_text(text)
    {
    }

    JsonParseResult parse()
    {
        skipWs();
        if (m_pos >= m_text.size()) {
            return fail("empty JSON");
        }
        JsonValue value;
        if (!parseValue(value, 0)) {
            return fail(m_error.empty() ? "invalid JSON" : m_error);
        }
        skipWs();
        if (m_pos < m_text.size()) {
            return fail("trailing characters after JSON value");
        }
        JsonParseResult ok;
        ok.ok = true;
        ok.value = std::move(value);
        return ok;
    }

private:
    std::string_view m_text;
    std::size_t m_pos = 0;
    std::string m_error;

    JsonParseResult fail(std::string message)
    {
        JsonParseResult r;
        r.error = std::move(message);
        return r;
    }

    void skipWs()
    {
        while (m_pos < m_text.size()) {
            const unsigned char ch = static_cast<unsigned char>(m_text[m_pos]);
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    bool parseValue(JsonValue& out, int depth)
    {
        if (depth > kMaxDepth) {
            m_error = "JSON nesting too deep";
            return false;
        }
        skipWs();
        if (m_pos >= m_text.size()) {
            m_error = "unexpected end of JSON";
            return false;
        }
        const char ch = m_text[m_pos];
        if (ch == 'n') {
            if (!parseLiteral("null")) {
                return false;
            }
            out = JsonValue::null();
            return true;
        }
        if (ch == 't') {
            if (!parseLiteral("true")) {
                return false;
            }
            out = JsonValue::fromBool(true);
            return true;
        }
        if (ch == 'f') {
            if (!parseLiteral("false")) {
                return false;
            }
            out = JsonValue::fromBool(false);
            return true;
        }
        if (ch == '"') {
            std::string s;
            if (!parseString(s)) {
                return false;
            }
            out = JsonValue::fromString(std::move(s));
            return true;
        }
        if (ch == '{') {
            return parseObject(out, depth + 1);
        }
        if (ch == '[') {
            return parseArray(out, depth + 1);
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            return parseNumber(out);
        }
        m_error = "unexpected character in JSON";
        return false;
    }

    bool parseLiteral(std::string_view lit)
    {
        if (m_text.substr(m_pos, lit.size()) != lit) {
            m_error = "invalid literal";
            return false;
        }
        m_pos += lit.size();
        return true;
    }

    bool parseString(std::string& out)
    {
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
            m_error = "expected string";
            return false;
        }
        ++m_pos;
        out.clear();
        while (m_pos < m_text.size()) {
            const unsigned char ch = static_cast<unsigned char>(m_text[m_pos]);
            ++m_pos;
            if (ch == '"') {
                return true;
            }
            if (ch == '\\') {
                if (m_pos >= m_text.size()) {
                    m_error = "unterminated string escape";
                    return false;
                }
                const char esc = m_text[m_pos++];
                switch (esc) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t code = 0;
                    if (!parseHex4(code)) {
                        return false;
                    }
                    if (code >= 0xd800U && code <= 0xdbffU) {
                        if (m_pos + 1 < m_text.size() && m_text[m_pos] == '\\' &&
                            m_text[m_pos + 1] == 'u') {
                            m_pos += 2;
                            std::uint32_t low = 0;
                            if (!parseHex4(low)) {
                                return false;
                            }
                            if (low < 0xdc00U || low > 0xdfffU) {
                                m_error = "invalid UTF-16 surrogate pair";
                                return false;
                            }
                            code = 0x10000U + ((code - 0xd800U) << 10U) +
                                   (low - 0xdc00U);
                        }
                    } else if (code >= 0xdc00U && code <= 0xdfffU) {
                        m_error = "unexpected UTF-16 low surrogate";
                        return false;
                    }
                    appendUtf8(out, code);
                    break;
                }
                default:
                    m_error = "invalid string escape";
                    return false;
                }
                continue;
            }
            if (ch < 0x20U) {
                m_error = "unescaped control character in string";
                return false;
            }
            out.push_back(static_cast<char>(ch));
        }
        m_error = "unterminated string";
        return false;
    }

    bool parseHex4(std::uint32_t& code)
    {
        if (m_pos + 4 > m_text.size()) {
            m_error = "truncated \\u escape";
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = m_text[m_pos++];
            code <<= 4U;
            if (ch >= '0' && ch <= '9') {
                code |= static_cast<std::uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                code |= static_cast<std::uint32_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                code |= static_cast<std::uint32_t>(ch - 'A' + 10);
            } else {
                m_error = "invalid hex digit in \\u escape";
                return false;
            }
        }
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t codePoint)
    {
        if (codePoint <= 0x7fU) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ffU) {
            out.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
            out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
        } else if (codePoint <= 0xffffU) {
            out.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
            out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
        } else {
            out.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
            out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
            out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
        }
    }

    bool parseNumber(JsonValue& out)
    {
        const std::size_t start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        if (m_pos >= m_text.size() || !std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
            m_error = "invalid number";
            return false;
        }
        if (m_text[m_pos] == '0') {
            ++m_pos;
        } else {
            while (m_pos < m_text.size() &&
                   std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
                ++m_pos;
            }
        }
        bool isFloat = false;
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            isFloat = true;
            ++m_pos;
            if (m_pos >= m_text.size() ||
                !std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
                m_error = "invalid number fraction";
                return false;
            }
            while (m_pos < m_text.size() &&
                   std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
                ++m_pos;
            }
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            isFloat = true;
            ++m_pos;
            if (m_pos < m_text.size() &&
                (m_text[m_pos] == '+' || m_text[m_pos] == '-')) {
                ++m_pos;
            }
            if (m_pos >= m_text.size() ||
                !std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
                m_error = "invalid number exponent";
                return false;
            }
            while (m_pos < m_text.size() &&
                   std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
                ++m_pos;
            }
        }
        const std::string raw(m_text.substr(start, m_pos - start));
        if (!isFloat) {
            try {
                std::size_t idx = 0;
                const long long parsed = std::stoll(raw, &idx, 10);
                if (idx == raw.size()) {
                    out = JsonValue::fromInt(static_cast<std::int64_t>(parsed));
                    return true;
                }
            } catch (...) {
                // Fall through to floating parse for very large integers.
            }
        }
        try {
            std::size_t idx = 0;
            const double parsed = std::stod(raw, &idx);
            if (idx != raw.size() || !std::isfinite(parsed)) {
                m_error = "invalid number";
                return false;
            }
            out = JsonValue::fromNumber(parsed);
            return true;
        } catch (...) {
            m_error = "invalid number";
            return false;
        }
    }

    bool parseObject(JsonValue& out, int depth)
    {
        if (m_text[m_pos] != '{') {
            return false;
        }
        ++m_pos;
        out = JsonValue::emptyObject();
        skipWs();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        while (m_pos < m_text.size()) {
            skipWs();
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWs();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                m_error = "expected ':' in object";
                return false;
            }
            ++m_pos;
            JsonValue value;
            if (!parseValue(value, depth)) {
                return false;
            }
            out.set(std::move(key), std::move(value));
            skipWs();
            if (m_pos >= m_text.size()) {
                m_error = "unterminated object";
                return false;
            }
            if (m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            m_error = "expected ',' or '}' in object";
            return false;
        }
        m_error = "unterminated object";
        return false;
    }

    bool parseArray(JsonValue& out, int depth)
    {
        if (m_text[m_pos] != '[') {
            return false;
        }
        ++m_pos;
        out = JsonValue::emptyArray();
        skipWs();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (m_pos < m_text.size()) {
            JsonValue value;
            if (!parseValue(value, depth)) {
                return false;
            }
            out.array.push_back(std::move(value));
            skipWs();
            if (m_pos >= m_text.size()) {
                m_error = "unterminated array";
                return false;
            }
            if (m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            m_error = "expected ',' or ']' in array";
            return false;
        }
        m_error = "unterminated array";
        return false;
    }
};

void writeValue(std::ostream& os, const JsonValue& value)
{
    switch (value.kind) {
    case JsonKind::Null:
        os << "null";
        return;
    case JsonKind::Bool:
        os << jsonBool(value.boolean);
        return;
    case JsonKind::Number:
        if (value.numberIsInteger) {
            os << jsonInt(value.integer);
        } else {
            std::ostringstream num;
            num.precision(17);
            num << value.number;
            os << num.str();
        }
        return;
    case JsonKind::String:
        os << jsonString(value.string);
        return;
    case JsonKind::Array:
        os << '[';
        for (std::size_t i = 0; i < value.array.size(); ++i) {
            if (i > 0) {
                os << ',';
            }
            writeValue(os, value.array[i]);
        }
        os << ']';
        return;
    case JsonKind::Object:
        os << '{';
        for (std::size_t i = 0; i < value.object.size(); ++i) {
            if (i > 0) {
                os << ',';
            }
            os << jsonString(value.object[i].first) << ':';
            writeValue(os, value.object[i].second);
        }
        os << '}';
        return;
    }
}

}  // namespace

JsonValue JsonValue::fromBool(bool value)
{
    JsonValue v;
    v.kind = JsonKind::Bool;
    v.boolean = value;
    return v;
}

JsonValue JsonValue::fromInt(std::int64_t value)
{
    JsonValue v;
    v.kind = JsonKind::Number;
    v.numberIsInteger = true;
    v.integer = value;
    v.number = static_cast<double>(value);
    return v;
}

JsonValue JsonValue::fromUint(std::uint64_t value)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        JsonValue v;
        v.kind = JsonKind::Number;
        v.numberIsInteger = false;
        v.number = static_cast<double>(value);
        return v;
    }
    return fromInt(static_cast<std::int64_t>(value));
}

JsonValue JsonValue::fromNumber(double value)
{
    JsonValue v;
    v.kind = JsonKind::Number;
    v.numberIsInteger = false;
    v.number = value;
    return v;
}

JsonValue JsonValue::fromString(std::string value)
{
    JsonValue v;
    v.kind = JsonKind::String;
    v.string = std::move(value);
    return v;
}

JsonValue JsonValue::emptyArray()
{
    JsonValue v;
    v.kind = JsonKind::Array;
    return v;
}

JsonValue JsonValue::emptyObject()
{
    JsonValue v;
    v.kind = JsonKind::Object;
    return v;
}

const JsonValue* JsonValue::get(std::string_view key) const
{
    if (kind != JsonKind::Object) {
        return nullptr;
    }
    for (const auto& field : object) {
        if (field.first == key) {
            return &field.second;
        }
    }
    return nullptr;
}

std::optional<std::string> JsonValue::stringAt(std::string_view key) const
{
    const JsonValue* v = get(key);
    if (v == nullptr || !v->isString()) {
        return std::nullopt;
    }
    return v->string;
}

std::optional<bool> JsonValue::boolAt(std::string_view key) const
{
    const JsonValue* v = get(key);
    if (v == nullptr || !v->isBool()) {
        return std::nullopt;
    }
    return v->boolean;
}

std::optional<std::int64_t> JsonValue::intAt(std::string_view key) const
{
    const JsonValue* v = get(key);
    if (v == nullptr || !v->isNumber()) {
        return std::nullopt;
    }
    if (v->numberIsInteger) {
        return v->integer;
    }
    if (std::isfinite(v->number) && std::floor(v->number) == v->number &&
        v->number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        v->number <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(v->number);
    }
    return std::nullopt;
}

std::optional<std::uint64_t> JsonValue::uintAt(std::string_view key) const
{
    const auto signedValue = intAt(key);
    if (!signedValue || *signedValue < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*signedValue);
}

void JsonValue::set(std::string key, JsonValue value)
{
    kind = JsonKind::Object;
    for (auto& field : object) {
        if (field.first == key) {
            field.second = std::move(value);
            return;
        }
    }
    object.emplace_back(std::move(key), std::move(value));
}

std::string JsonValue::stringify() const
{
    std::ostringstream os;
    writeValue(os, *this);
    return os.str();
}

JsonParseResult parseJson(std::string_view text)
{
    Parser parser(text);
    return parser.parse();
}

bool jsonIdsEqual(const JsonValue& a, const JsonValue& b)
{
    if (a.kind != b.kind) {
        return false;
    }
    if (a.isString()) {
        return a.string == b.string;
    }
    if (a.isNumber()) {
        if (a.numberIsInteger && b.numberIsInteger) {
            return a.integer == b.integer;
        }
        return a.number == b.number;
    }
    if (a.isNull() && b.isNull()) {
        return true;
    }
    return false;
}

}  // namespace spacelens::mcp
