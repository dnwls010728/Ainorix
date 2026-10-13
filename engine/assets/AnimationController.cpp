#include "assets/AnimationController.h"
#include <cmath>
#include "api/Commands.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"

namespace oe {
namespace {
void Fields(const Json& value, std::initializer_list<const char*> allowed) {
    if (!value.isObject()) throw std::runtime_error("expected a controller object");
    for (const auto& field : value.members()) {
        bool found = false;
        for (const char* key : allowed) found = found || field.first == key;
        if (!found) throw std::runtime_error("unknown controller field: " + field.first);
    }
}
void Name(const std::string& name) {
    if (name.empty() || name.size() > 128) throw std::runtime_error("controller names need 1..128 bytes");
}
void Number(const Json& value, double low, double high) {
    if (!value.isNumber() || !std::isfinite(value.asNumber()) || value.asNumber() < low || value.asNumber() > high)
        throw std::runtime_error("controller number outside allowed range");
}
void Typed(const Json& value, const std::string& type) {
    if (type == "number") Number(value, -1e9, 1e9);
    else if (!value.isBool()) throw std::runtime_error("boolean controller value required");
}
void Path(Engine& engine, const std::string& path) {
    engine.ResolvePath(path);
    if (AssetManager::KindOf(path) != "controller") throw ApiError("invalid_path", "controller path must end with .controller.json");
}
void Check(const Json& data) {
    std::string error;
    if (!ValidateAnimationController(data, &error)) throw ApiError("invalid_controller", error);
}
}  // namespace

bool ValidateAnimationController(const Json& data, std::string* error) {
    try {
        Fields(data, {"entry", "parameters", "states", "transitions"});
        if (!data["parameters"].isObject() || data["parameters"].size() > 64) throw std::runtime_error("parameters needs an object with at most 64 entries");
        if (!data["states"].isObject() || data["states"].size() == 0 || data["states"].size() > 64) throw std::runtime_error("states needs 1..64 entries");
        if (!data["entry"].isString() || !data["states"].has(data["entry"].asString())) throw std::runtime_error("entry must name a state");
        for (const auto& item : data["parameters"].members()) {
            Name(item.first); Fields(item.second, {"type", "default"});
            const std::string type = item.second["type"].asString();
            if (type != "number" && type != "boolean" && type != "trigger") throw std::runtime_error("parameter type must be number, boolean or trigger");
            Typed(item.second["default"], type);
            if (type == "trigger" && item.second["default"].asBool()) throw std::runtime_error("triggers default to false");
        }
        for (const auto& item : data["states"].members()) {
            Name(item.first);
            if (item.first == "*") throw std::runtime_error("state name * is reserved for wildcard transitions");
            Fields(item.second, {"clip", "speed", "loop", "position"});
            if (!item.second["clip"].isString()) throw std::runtime_error("state clip must be a string");
            Name(item.second["clip"].asString());
            if (item.second.has("speed")) Number(item.second["speed"], -10000, 10000);
            if (item.second.has("loop") && !item.second["loop"].isBool()) throw std::runtime_error("state loop must be boolean");
            if (item.second.has("position")) {
                const Json& p = item.second["position"];
                if (!p.isArray() || p.size() != 2) throw std::runtime_error("state position needs [x,y]");
                Number(p[0], -100000, 100000); Number(p[1], -100000, 100000);
            }
        }
        if (!data["transitions"].isArray() || data["transitions"].size() > 256) throw std::runtime_error("transitions needs an array with at most 256 entries");
        for (const Json& t : data["transitions"].items()) {
            Fields(t, {"from", "to", "conditions", "blend", "minTime"});
            if (!t["from"].isString() || (t["from"].asString() != "*" && !data["states"].has(t["from"].asString())) ||
                !t["to"].isString() || !data["states"].has(t["to"].asString())) throw std::runtime_error("transition endpoints must name states (from may be *)");
            if (t.has("blend")) Number(t["blend"], 0, 5);
            if (t.has("minTime")) Number(t["minTime"], 0, 3600);
            if (!t["conditions"].isArray() || t["conditions"].size() == 0 || t["conditions"].size() > 16) throw std::runtime_error("transition needs 1..16 AND conditions");
            for (const Json& condition : t["conditions"].items()) {
                Fields(condition, {"parameter", "op", "value"});
                if (!condition["parameter"].isString() || !data["parameters"].has(condition["parameter"].asString())) throw std::runtime_error("condition parameter not found");
                const std::string type = data["parameters"][condition["parameter"].asString()]["type"].asString();
                const std::string op = condition["op"].asString();
                if (type == "trigger") {
                    if (op != "trigger" || condition.has("value")) throw std::runtime_error("trigger condition uses op:trigger without value");
                } else {
                    if (op != "==" && op != "!=" && !(type == "number" && (op == ">" || op == ">=" || op == "<" || op == "<=")))
                        throw std::runtime_error("invalid condition operator for parameter type");
                    Typed(condition["value"], type);
                }
            }
        }
        return true;
    } catch (const std::exception& ex) { if (error) *error = ex.what(); return false; }
}

Json ReadAnimationController(Engine& engine, const std::string& path) {
    Path(engine, path); std::string text, error;
    if (!ReadTextFile(engine.ResolvePath(path), text)) throw ApiError("not_found", "cannot read " + path);
    if (text.size() > 1024 * 1024) throw ApiError("invalid_controller", "controller exceeds 1 MiB");
    Json values = Json::parse(text, &error);
    if (!error.empty()) throw ApiError("invalid_controller", error);
    Check(values); return values;
}

void RegisterAnimationControllerCommands(CommandRegistry& registry) {
    for (bool check : {false, true}) registry.Add({check ? "animation.controller_check" : "animation.controller_get",
        "Read and validate a .controller.json; optional id verifies every state clip on a sprite/model before playback.",
        Params().Req("path", "string", "Project-relative .controller.json.").Opt("id", "entity", "Optional sprite/model id or unique name."), false,
        [check](Engine& engine, const Json& args) {
            Json values = ReadAnimationController(engine, args["path"].asString());
            if (args.has("id")) for (const auto& state : values["states"].members()) {
                Json query = Json::MakeObject(); query["id"] = args["id"]; query["clip"] = state.second["clip"];
                const Json result = engine.Call("animation.events", query);
                if (!result["ok"].asBool()) throw ApiError("invalid_controller", "state " + state.first + ": " + result["error"]["message"].asString());
            }
            Json out = Json::MakeObject(); out["path"] = args["path"]; out["valid"] = true;
            if (!check) out["values"] = values;
            return out;
        }});
    for (bool create : {true, false}) registry.Add({create ? "animation.controller_create" : "animation.controller_set",
        "Write a complete validated controller asset; immediate file edit, no scene undo/revision.",
        Params().Req("path", "string", "Project-relative .controller.json.").Req("values", "object", "Complete entry, parameters, states, transitions definition.").Opt("overwrite", "boolean", "Allow create to replace an existing file."), false,
        [create](Engine& engine, const Json& args) {
            const std::string path = args["path"].asString(); Path(engine, path);
            const std::string full = engine.ResolvePath(path);
            if (create && FileExists(full) && !args["overwrite"].asBool()) throw ApiError("already_exists", "controller already exists");
            if (!create && !FileExists(full)) throw ApiError("not_found", "controller does not exist");
            Check(args["values"]);
            const std::string text = args["values"].dump(2) + "\n";
            if (text.size() > 1024 * 1024) throw ApiError("invalid_controller", "controller exceeds 1 MiB");
            CreateDirectories(ParentPath(full));
            if (!WriteTextFile(full, text)) throw ApiError("write_failed", "cannot write controller");
            Json out = Json::MakeObject(); out["path"] = path; out["values"] = args["values"]; return out;
        }});
}
}  // namespace oe
