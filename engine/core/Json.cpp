#include "core/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace oe {

namespace {
const Json& NullJson() {
    static const Json n;
    return n;
}
const std::string& EmptyString() {
    static const std::string s;
    return s;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool Run(Json& out, std::string* error) {
        SkipWs();
        if (!ParseValue(out, 0)) {
            if (error) *error = error_;
            return false;
        }
        SkipWs();
        if (pos_ != s_.size()) {
            Fail("unexpected trailing characters");
            if (error) *error = error_;
            return false;
        }
        return true;
    }

private:
    bool Fail(const char* msg) {
        if (error_.empty()) {
            size_t line = 1, col = 1;
            for (size_t i = 0; i < pos_ && i < s_.size(); ++i) {
                if (s_[i] == '\n') { ++line; col = 1; } else { ++col; }
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf), "JSON parse error at line %zu, column %zu: %s", line, col, msg);
            error_ = buf;
        }
        return false;
    }

    void SkipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                // Tolerate // line comments: handy for hand-written scene files.
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    bool Match(const char* lit) {
        size_t n = 0;
        while (lit[n]) ++n;
        if (s_.compare(pos_, n, lit) == 0) {
            pos_ += n;
            return true;
        }
        return false;
    }

    bool ParseValue(Json& out, int depth) {
        if (depth > 256) return Fail("nesting too deep");
        if (pos_ >= s_.size()) return Fail("unexpected end of input");
        char c = s_[pos_];
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') {
            std::string str;
            if (!ParseString(str)) return false;
            out = Json(std::move(str));
            return true;
        }
        if (Match("true")) { out = Json(true); return true; }
        if (Match("false")) { out = Json(false); return true; }
        if (Match("null")) { out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        return Fail("unexpected character");
    }

    bool ParseNumber(Json& out) {
        const char* begin = s_.c_str() + pos_;
        char* end = nullptr;
        double v = std::strtod(begin, &end);
        if (end == begin) return Fail("invalid number");
        pos_ += static_cast<size_t>(end - begin);
        out = Json(v);
        return true;
    }

    bool ParseHex4(uint32_t& v) {
        if (pos_ + 4 > s_.size()) return Fail("truncated \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return Fail("invalid hex digit in \\u escape");
        }
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= s_.size()) return Fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return Fail("unterminated escape");
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t lo = 0;
                        if (!ParseHex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("invalid escape character");
            }
        }
    }

    bool ParseArray(Json& out, int depth) {
        ++pos_;
        out = Json::MakeArray();
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            SkipWs();
            Json item;
            if (!ParseValue(item, depth + 1)) return false;
            out.push(std::move(item));
            SkipWs();
            if (pos_ >= s_.size()) return Fail("unterminated array");
            char c = s_[pos_++];
            if (c == ']') return true;
            if (c != ',') return Fail("expected ',' or ']' in array");
            SkipWs();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }  // trailing comma
        }
    }

    bool ParseObject(Json& out, int depth) {
        ++pos_;
        out = Json::MakeObject();
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("expected string key in object");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("expected ':' after object key");
            ++pos_;
            SkipWs();
            Json value;
            if (!ParseValue(value, depth + 1)) return false;
            out[key] = std::move(value);
            SkipWs();
            if (pos_ >= s_.size()) return Fail("unterminated object");
            char c = s_[pos_++];
            if (c == '}') return true;
            if (c != ',') return Fail("expected ',' or '}' in object");
            SkipWs();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }  // trailing comma
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string error_;
};

void AppendNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char buf[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%.9g", v);
    }
    out += buf;
}

void Indent(std::string& out, int indent, int depth) {
    out += '\n';
    out.append(static_cast<size_t>(indent * depth), ' ');
}
}  // namespace

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

const char* Json::typeName() const {
    switch (type_) {
        case Type::Null: return "null";
        case Type::Bool: return "boolean";
        case Type::Number: return "number";
        case Type::String: return "string";
        case Type::Array: return "array";
        case Type::Object: return "object";
    }
    return "null";
}

const std::string& Json::asString() const { return type_ == Type::String ? str_ : EmptyString(); }

Json& Json::operator[](const std::string& key) {
    if (type_ == Type::Null) type_ = Type::Object;
    for (auto& kv : obj_) {
        if (kv.first == key) return kv.second;
    }
    obj_.emplace_back(key, Json());
    return obj_.back().second;
}

const Json& Json::operator[](const std::string& key) const {
    const Json* j = find(key);
    return j ? *j : NullJson();
}

const Json* Json::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& kv : obj_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

bool Json::erase(const std::string& key) {
    for (auto it = obj_.begin(); it != obj_.end(); ++it) {
        if (it->first == key) {
            obj_.erase(it);
            return true;
        }
    }
    return false;
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

void Json::push(Json value) {
    if (type_ == Type::Null) type_ = Type::Array;
    arr_.push_back(std::move(value));
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: AppendNumber(out, num_); break;
        case Type::String: out += JsonEscape(str_); break;
        case Type::Array: {
            if (arr_.empty()) { out += "[]"; break; }
            // Short arrays of scalars (vectors, colors) stay on one line.
            bool inlineArr = arr_.size() <= 4;
            for (const Json& j : arr_) inlineArr = inlineArr && (j.isNumber() || j.isBool());
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += inlineArr || indent < 0 ? (indent < 0 ? "," : ", ") : ",";
                if (indent >= 0 && !inlineArr) Indent(out, indent, depth + 1);
                arr_[i].dumpTo(out, indent, depth + 1);
            }
            if (indent >= 0 && !inlineArr) Indent(out, indent, depth);
            out += ']';
            break;
        }
        case Type::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                if (indent >= 0) Indent(out, indent, depth + 1);
                out += JsonEscape(obj_[i].first);
                out += indent >= 0 ? ": " : ":";
                obj_[i].second.dumpTo(out, indent, depth + 1);
            }
            if (indent >= 0) Indent(out, indent, depth);
            out += '}';
            break;
        }
    }
}

Json Json::parse(const std::string& text, std::string* error) {
    Json out;
    Parser p(text);
    if (!p.Run(out, error)) return Json();
    if (error) error->clear();
    return out;
}

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == o.bool_;
        case Type::Number: return num_ == o.num_;
        case Type::String: return str_ == o.str_;
        case Type::Array: return arr_ == o.arr_;
        case Type::Object: {
            if (obj_.size() != o.obj_.size()) return false;
            for (const auto& kv : obj_) {
                const Json* other = o.find(kv.first);
                if (!other || !(kv.second == *other)) return false;
            }
            return true;
        }
    }
    return false;
}

}  // namespace oe
