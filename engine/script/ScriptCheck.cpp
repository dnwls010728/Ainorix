#include "script/ScriptCheck.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

#include "lauxlib.h"
#include "lua.h"
// Lua internals (Lua is compiled into the engine): walking the compiled
// bytecode is how globals are found without a Lua parser of our own.
#include "ldebug.h"
#include "lfunc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"

namespace oe {

namespace {

// ----- Bytecode walk ---------------------------------------------------------

struct GlobalUse {
    std::string name;
    int line;
    bool write;
};

bool IsEnvUpvalue(const Proto* p, int index) {
    if (index < 0 || index >= p->sizeupvalues) return false;
    const TString* name = p->upvalues[index].name;
    return name && std::string(getstr(name)) == "_ENV";
}

void CollectGlobals(const Proto* p, std::vector<GlobalUse>& out) {
    for (int pc = 0; pc < p->sizecode; ++pc) {
        Instruction i = p->code[pc];
        OpCode op = GET_OPCODE(i);
        int up = -1, key = -1;
        bool write = false;
        if (op == OP_GETTABUP) {
            up = GETARG_B(i);
            key = GETARG_C(i);
        } else if (op == OP_SETTABUP) {
            up = GETARG_A(i);
            key = GETARG_B(i);
            write = true;
        } else {
            continue;
        }
        if (!IsEnvUpvalue(p, up) || key < 0 || key >= p->sizek || !ttisstring(&p->k[key])) continue;
        out.push_back({getstr(tsvalue(&p->k[key])), luaG_getfuncline(p, pc), write});
    }
    for (int k = 0; k < p->sizep; ++k) CollectGlobals(p->p[k], out);
}

// "name:12: message" -> 12 and "message".
void ParseLuaError(const std::string& raw, const std::string& chunk, LuaDiagnostic& d) {
    d.message = raw;
    std::string prefix = chunk + ":";
    size_t at = raw.find(prefix);
    if (at == std::string::npos) return;
    const char* s = raw.c_str() + at + prefix.size();
    char* end = nullptr;
    long line = std::strtol(s, &end, 10);
    if (end == s || *end != ':') return;
    d.line = static_cast<int>(line);
    d.message = std::string(end + 1);
    while (!d.message.empty() && d.message[0] == ' ') d.message.erase(0, 1);
}

// ----- Tokens (params inference) --------------------------------------------

struct Token {
    enum Kind { Name, Number, String, Symbol } kind;
    std::string text;  // strings: the value; others: as written
    int line;
    std::string comment;  // `--` comment that ends this token's line
};

std::vector<Token> Tokenize(const std::string& s) {
    std::vector<Token> t;
    size_t i = 0;
    int line = 1;
    auto longBracket = [&](size_t at, size_t& level) -> bool {  // at '['
        size_t j = at + 1;
        level = 0;
        while (j < s.size() && s[j] == '=') ++level, ++j;
        return j < s.size() && s[j] == '[';
    };
    auto skipLong = [&](size_t at, size_t level, std::string* content) {  // returns index after close
        size_t open = at + level + 2;
        std::string close = "]" + std::string(level, '=') + "]";
        size_t end = s.find(close, open);
        if (end == std::string::npos) end = s.size();
        for (size_t k = at; k < end && k < s.size(); ++k) line += s[k] == '\n';
        if (content) *content = s.substr(open, end - open);
        return std::min(s.size(), end + close.size());
    };
    while (i < s.size()) {
        char c = s[i];
        if (c == '\n') {
            ++line;
            ++i;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            size_t level;
            if (i + 2 < s.size() && s[i + 2] == '[' && longBracket(i + 2, level)) {
                i = skipLong(i + 2, level, nullptr);
            } else {
                size_t end = s.find('\n', i);
                if (end == std::string::npos) end = s.size();
                std::string text = s.substr(i + 2, end - i - 2);
                while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(0, 1);
                while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
                if (!t.empty() && t.back().line == line) t.back().comment = text;
                i = end;
            }
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            t.push_back({Token::Name, s.substr(i, j - i), line, ""});
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.' ||
                                    ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E' || s[j - 1] == 'p' || s[j - 1] == 'P'))))
                ++j;
            t.push_back({Token::Number, s.substr(i, j - i), line, ""});
            i = j;
        } else if (c == '"' || c == '\'') {
            std::string value;
            size_t j = i + 1;
            for (; j < s.size() && s[j] != c && s[j] != '\n'; ++j) {
                if (s[j] == '\\' && j + 1 < s.size()) {
                    char e = s[++j];
                    value += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else {
                    value += s[j];
                }
            }
            t.push_back({Token::String, value, line, ""});
            i = std::min(s.size(), j + 1);
        } else if (c == '[' && [&] { size_t l; return longBracket(i, l); }()) {
            size_t level;
            longBracket(i, level);
            std::string value;
            int startLine = line;
            i = skipLong(i, level, &value);
            t.push_back({Token::String, value, startLine, ""});
        } else {
            // Multi-character operators that matter here.
            std::string op(1, c);
            if (i + 1 < s.size()) {
                std::string two = s.substr(i, 2);
                if (two == "==" || two == "~=" || two == "<=" || two == ">=" || two == ".." || two == "::") op = two;
            }
            t.push_back({Token::Symbol, op, line, ""});
            i += op.size();
        }
    }
    return t;
}

bool Is(const std::vector<Token>& t, size_t i, const char* text) { return i < t.size() && t[i].text == text && t[i].kind != Token::String; }

// Literal default at t[i] ("5", "-2", "\"none\"", "true", "{0, 45, 0}");
// sets `next` to the token after it. Null when it is not a literal.
Json ParseLiteral(const std::vector<Token>& t, size_t i, size_t& next) {
    next = i + 1;
    if (i >= t.size()) return Json();
    const Token& k = t[i];
    if (k.kind == Token::Number) return Json(std::strtod(k.text.c_str(), nullptr));
    if (k.kind == Token::Symbol && k.text == "-" && i + 1 < t.size() && t[i + 1].kind == Token::Number) {
        next = i + 2;
        return Json(-std::strtod(t[i + 1].text.c_str(), nullptr));
    }
    if (k.kind == Token::String) return Json(k.text);
    if (k.kind == Token::Name && (k.text == "true" || k.text == "false")) return Json(k.text == "true");
    if (k.kind == Token::Symbol && k.text == "{") {
        // Only flat lists of numbers ({x, y, z}) and empty tables.
        Json arr = Json::MakeArray();
        size_t j = i + 1;
        if (Is(t, j, "}")) {
            next = j + 1;
            return Json::MakeObject();
        }
        while (j < t.size()) {
            size_t after;
            Json v = ParseLiteral(t, j, after);
            if (!v.isNumber()) return Json();
            arr.push(v);
            j = after;
            if (Is(t, j, ",")) {
                ++j;
                continue;
            }
            if (Is(t, j, "}")) {
                next = j + 1;
                return arr;
            }
            return Json();
        }
    }
    return Json();
}

std::string TypeOf(const Json& v) {
    if (v.isNumber()) return "number";
    if (v.isBool()) return "boolean";
    if (v.isString()) return "string";
    if (v.isArray()) return v.size() == 3 ? "vec3" : "array";
    if (v.isObject()) return "object";
    return "any";
}

}  // namespace

std::vector<LuaDiagnostic> CheckLuaSource(const std::string& source, const std::string& chunkName, const std::set<std::string>& knownGlobals) {
    std::vector<LuaDiagnostic> out;
    lua_State* L = luaL_newstate();
    if (!L) return out;
    std::string chunk = chunkName.empty() ? "script" : chunkName;
    if (luaL_loadbufferx(L, source.data(), source.size(), ("=" + chunk).c_str(), "t") != LUA_OK) {
        LuaDiagnostic d;
        d.severity = "error";
        const char* msg = lua_tostring(L, -1);
        ParseLuaError(msg ? msg : "syntax error", chunk, d);
        out.push_back(d);
        lua_close(L);
        return out;
    }
    const LClosure* cl = clLvalue(s2v(L->top.p - 1));
    std::vector<GlobalUse> uses;
    CollectGlobals(cl->p, uses);
    lua_close(L);

    std::set<std::string> assigned;
    for (const GlobalUse& u : uses) {
        if (u.write) assigned.insert(u.name);
    }
    std::set<std::pair<std::string, int>> reported;  // one warning per name and line
    for (const GlobalUse& u : uses) {
        if (!reported.insert({u.name, u.line}).second) continue;
        LuaDiagnostic d;
        d.line = u.line;
        d.severity = "warning";
        if (u.write && !knownGlobals.count(u.name)) {
            d.message = "assigns global '" + u.name + "' - shared by every script; add 'local'?";
        } else if (!u.write && !knownGlobals.count(u.name) && !assigned.count(u.name)) {
            d.message = "unknown global '" + u.name + "' - typo, or a missing 'local'?";
        } else {
            continue;
        }
        out.push_back(d);
    }
    return out;
}

std::vector<ScriptParam> InferScriptParams(const std::string& source) {
    std::vector<Token> t = Tokenize(source);
    // Names that stand for the params table at a token index: bare `params`
    // everywhere, and `local p = self.params` until the next `function`.
    struct Alias {
        std::string name;
        size_t from, to;
    };
    std::vector<Alias> aliases = {{"params", 0, t.size()}};
    auto aliasAt = [&](const std::string& name, size_t at) {
        for (const Alias& a : aliases) {
            if (a.name == name && at >= a.from && at < a.to) return true;
        }
        return false;
    };
    std::vector<ScriptParam> out;
    std::map<std::string, size_t> index;
    // Where each param lands (`self.move = p.move or ...` -> "self.move"), for enum-like options.
    std::map<std::string, std::string> targets;

    // Aliases: `local p = self.params` (optionally `or {}`).
    for (size_t i = 0; i + 4 < t.size(); ++i) {
        if (Is(t, i, "local") && t[i + 1].kind == Token::Name && Is(t, i + 2, "=") && Is(t, i + 3, "self") && Is(t, i + 4, ".") && Is(t, i + 5, "params") &&
            !Is(t, i + 6, ".")) {
            size_t end = i + 6;
            while (end < t.size() && !Is(t, end, "function")) ++end;
            aliases.push_back({t[i + 1].text, i, end});
        }
    }
    for (size_t i = 0; i < t.size(); ++i) {
        // self.params.NAME | ALIAS.NAME
        size_t nameAt = 0;
        if (Is(t, i, "self") && Is(t, i + 1, ".") && Is(t, i + 2, "params") && Is(t, i + 3, ".") && i + 4 < t.size() && t[i + 4].kind == Token::Name) {
            nameAt = i + 4;
        } else if (t[i].kind == Token::Name && aliasAt(t[i].text, i) && Is(t, i + 1, ".") && i + 2 < t.size() && t[i + 2].kind == Token::Name &&
                   !(i > 0 && (Is(t, i - 1, ".") || Is(t, i - 1, ":")))) {
            nameAt = i + 2;
        } else {
            continue;
        }
        // Not a method call or deeper access (p.x.y, p:foo()).
        if (Is(t, nameAt + 1, "(") || Is(t, nameAt + 1, ":")) continue;
        const std::string& name = t[nameAt].text;
        Json def;
        if (Is(t, nameAt + 1, "or")) {
            size_t after;
            def = ParseLiteral(t, nameAt + 2, after);
        }
        auto it = index.find(name);
        if (it == index.end()) {
            ScriptParam p;
            p.name = name;
            p.line = t[nameAt].line;
            index[name] = out.size();
            out.push_back(p);
            it = index.find(name);
        }
        ScriptParam& p = out[it->second];
        if (p.defaultValue.isNull() && !def.isNull()) {
            p.defaultValue = def;
            p.line = t[nameAt].line;
        }
        // Trailing comment on the line that reads it describes it.
        if (p.description.empty()) {
            for (size_t k = nameAt; k < t.size() && t[k].line == t[nameAt].line; ++k) {
                if (!t[k].comment.empty()) p.description = t[k].comment;
            }
        }
        // `target = <access>`: remember the assigned variable.
        size_t lhsEnd = i;  // token before `=`
        if (lhsEnd >= 2 && Is(t, lhsEnd - 1, "=")) {
            size_t s = lhsEnd - 2;
            std::string lhs = t[s].text;
            while (s >= 2 && Is(t, s - 1, ".") && t[s - 2].kind == Token::Name) {
                lhs = t[s - 2].text + "." + lhs;
                s -= 2;
            }
            if (t[lhsEnd - 2].kind == Token::Name) targets[lhs] = name;
        }
    }
    // Options: string literals compared with the param (or where it was stored).
    for (size_t i = 0; i < t.size(); ++i) {
        if (!(Is(t, i, "==") || Is(t, i, "~="))) continue;
        auto side = [&](size_t end, bool left) -> std::string {  // dotted name ending/starting at `end`
            if (left) {
                if (t[end].kind != Token::Name) return "";
                std::string s = t[end].text;
                size_t k = end;
                while (k >= 2 && Is(t, k - 1, ".") && t[k - 2].kind == Token::Name) {
                    s = t[k - 2].text + "." + s;
                    k -= 2;
                }
                return s;
            }
            if (end >= t.size() || t[end].kind != Token::Name) return "";
            std::string s = t[end].text;
            for (size_t k = end; k + 2 < t.size() && Is(t, k + 1, ".") && t[k + 2].kind == Token::Name; k += 2) s += "." + t[k + 2].text;
            return s;
        };
        std::string var;
        std::string value;
        if (i > 0 && i + 1 < t.size() && t[i + 1].kind == Token::String) {
            var = side(i - 1, true);
            value = t[i + 1].text;
        } else if (i > 0 && t[i - 1].kind == Token::String) {
            var = side(i + 1, false);
            value = t[i - 1].text;
        }
        if (var.empty()) continue;
        std::string param;
        auto tg = targets.find(var);
        if (tg != targets.end()) param = tg->second;
        for (const Alias& a : aliases) {
            if (var.rfind(a.name + ".", 0) == 0 && aliasAt(a.name, i)) param = var.substr(a.name.size() + 1);
        }
        if (var.rfind("self.params.", 0) == 0) param = var.substr(12);
        auto it = index.find(param);
        if (it == index.end()) continue;
        std::vector<std::string>& opts = out[it->second].options;
        if (std::find(opts.begin(), opts.end(), value) == opts.end()) opts.push_back(value);
    }
    for (ScriptParam& p : out) {
        p.type = TypeOf(p.defaultValue);
        if (!p.options.empty() && p.defaultValue.isString()) {
            std::string d = p.defaultValue.asString();
            if (std::find(p.options.begin(), p.options.end(), d) == p.options.end()) p.options.insert(p.options.begin(), d);
        } else if (p.type != "string") {
            p.options.clear();
        }
        // A single comparison (`~= ""`) is not a list of choices.
        size_t named = static_cast<size_t>(std::count_if(p.options.begin(), p.options.end(), [](const std::string& o) { return !o.empty(); }));
        if (named < 2) p.options.clear();
    }
    return out;
}

}  // namespace oe
