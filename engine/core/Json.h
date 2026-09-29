#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace oe {

// Small JSON value type. Objects keep insertion order so serialized files are
// stable and diff-friendly (important for AI agents reviewing changes).
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(int v) : type_(Type::Number), num_(v) {}
    Json(unsigned v) : type_(Type::Number), num_(v) {}
    Json(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(uint64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(float v) : type_(Type::Number), num_(v) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* v) : type_(Type::String), str_(v) {}
    Json(std::string v) : type_(Type::String), str_(std::move(v)) {}
    Json(Array v) : type_(Type::Array), arr_(std::move(v)) {}
    Json(Object v) : type_(Type::Object), obj_(std::move(v)) {}

    static Json MakeArray() { return Json(Array{}); }
    static Json MakeObject() { return Json(Object{}); }

    Type type() const { return type_; }
    const char* typeName() const;
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double asNumber(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    float asFloat(float def = 0.0f) const { return type_ == Type::Number ? static_cast<float>(num_) : def; }
    int asInt(int def = 0) const { return type_ == Type::Number ? static_cast<int>(num_) : def; }
    const std::string& asString() const;
    std::string asString(const std::string& def) const { return type_ == Type::String ? str_ : def; }

    // Object access. Non-const operator[] converts null to an object and inserts.
    Json& operator[](const std::string& key);
    Json& operator[](const char* key) { return (*this)[std::string(key)]; }
    const Json& operator[](const std::string& key) const;
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    const Json* find(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }
    bool erase(const std::string& key);

    // Array access.
    Json& operator[](size_t index) { return arr_[index]; }
    const Json& operator[](size_t index) const { return arr_[index]; }
    Json& operator[](int index) { return arr_[static_cast<size_t>(index)]; }
    const Json& operator[](int index) const { return arr_[static_cast<size_t>(index)]; }
    size_t size() const;
    void push(Json value);

    Array& items() { return arr_; }
    const Array& items() const { return arr_; }
    Object& members() { return obj_; }
    const Object& members() const { return obj_; }

    // indent < 0: compact single line. indent >= 0: pretty printed.
    std::string dump(int indent = -1) const;
    static Json parse(const std::string& text, std::string* error = nullptr);

    bool operator==(const Json& other) const;
    bool operator!=(const Json& other) const { return !(*this == other); }

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;
};

std::string JsonEscape(const std::string& s);

}  // namespace oe
