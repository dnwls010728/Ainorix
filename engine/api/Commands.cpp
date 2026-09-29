#include "api/Commands.h"

#include <algorithm>
#include <cctype>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Log.h"
#include "render/Mesh.h"
#include "scene/Components.h"

namespace oe {

// ---------------------------------------------------------------------------
// Registry / schema helpers
// ---------------------------------------------------------------------------

void CommandRegistry::Add(Command command) { commands_.push_back(std::move(command)); }

const Command* CommandRegistry::Find(const std::string& name) const {
    for (const Command& c : commands_) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

Params::Params() {
    schema_ = Json::MakeObject();
    schema_["type"] = "object";
    schema_["properties"] = Json::MakeObject();
}

Params& Params::Prop(const char* name, const char* type, const char* doc, bool required) {
    Json p = Json::MakeObject();
    std::string t = type;
    if (t == "vec3") {
        p["type"] = "array";
        p["items"]["type"] = "number";
        p["minItems"] = 3;
        p["maxItems"] = 3;
    } else {
        p["type"] = type;
    }
    p["description"] = doc;
    schema_["properties"][name] = p;
    if (required) schema_["required"].push(name);
    return *this;
}

Params& Params::Req(const char* name, const char* type, const char* doc) { return Prop(name, type, doc, true); }
Params& Params::Opt(const char* name, const char* type, const char* doc) { return Prop(name, type, doc, false); }
Params& Params::OptWith(const char* name, Json schema) {
    schema_["properties"][name] = std::move(schema);
    return *this;
}
Params& Params::ReqWith(const char* name, Json schema) {
    schema_["properties"][name] = std::move(schema);
    schema_["required"].push(name);
    return *this;
}

namespace {

bool TypeMatches(const Json& v, const std::string& type) {
    if (type == "integer") return v.isNumber() && v.asNumber() == static_cast<double>(static_cast<int64_t>(v.asNumber()));
    if (type == "number") return v.isNumber();
    if (type == "string") return v.isString();
    if (type == "boolean") return v.isBool();
    if (type == "object") return v.isObject();
    if (type == "array") return v.isArray();
    return true;
}

std::string Usage(const Command& c) {
    std::string out = c.name + " {";
    bool first = true;
    for (const auto& kv : c.params["properties"].members()) {
        bool req = false;
        for (const Json& r : c.params["required"].items()) req = req || r.asString() == kv.first;
        out += (first ? "" : ", ") + kv.first + (req ? "" : "?") + ": " + kv.second["type"].asString("any");
        first = false;
    }
    return out + "}";
}

}  // namespace

void ValidateArgs(const Command& c, const Json& args) {
    if (!args.isObject()) throw ApiError("invalid_args", "arguments must be a JSON object", "Usage: " + Usage(c));
    const Json& props = c.params["properties"];
    for (const Json& r : c.params["required"].items()) {
        if (!args.has(r.asString())) {
            throw ApiError("missing_argument", "missing required argument '" + r.asString() + "'", "Usage: " + Usage(c));
        }
    }
    for (const auto& kv : args.members()) {
        const Json* p = props.find(kv.first);
        if (!p) throw ApiError("unknown_argument", "unknown argument '" + kv.first + "'", "Usage: " + Usage(c));
        std::string type = (*p)["type"].asString("");
        if (!type.empty() && !TypeMatches(kv.second, type)) {
            throw ApiError("invalid_argument", "argument '" + kv.first + "' must be " + type + ", got " + kv.second.typeName(), "Usage: " + Usage(c));
        }
    }
}

namespace {

// ---------------------------------------------------------------------------
// Command helpers
// ---------------------------------------------------------------------------

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

EntityId RequireEntity(Engine& e, const Json& args, const char* key = "id") {
    Scene& scene = e.GetScene();
    const Json& v = args[key];
    if (v.isNumber()) {
        EntityId id = static_cast<EntityId>(v.asNumber());
        if (!scene.Exists(id)) throw ApiError("entity_not_found", "entity " + std::to_string(id) + " does not exist", "Call scene.summary to list entity ids.");
        return id;
    }
    if (v.isString()) {
        EntityId id = scene.FindByName(v.asString());
        if (id == kNullEntity) throw ApiError("entity_not_found", "no entity named '" + v.asString() + "'", "Call scene.summary to list entities.");
        return id;
    }
    throw ApiError("invalid_argument", std::string("argument '") + key + "' must be an entity id (integer) or entity name (string)");
}

Json EntityRefSchema(const char* doc) {
    Json s = Json::MakeObject();
    Json types = Json::MakeArray();
    types.push("integer");
    types.push("string");
    s["type"] = types;
    s["description"] = doc;
    return s;
}

const ComponentType& RequireType(const Json& name) {
    const ComponentType* t = TypeRegistry::Find(name.asString());
    if (!t) {
        std::string names;
        for (const ComponentType& ct : TypeRegistry::All()) names += (names.empty() ? "" : ", ") + ct.name;
        throw ApiError("unknown_component", "unknown component type '" + name.asString() + "'", "Known types: " + names + ". Call component.types for field schemas.");
    }
    return *t;
}

Vec3 ReadVec3(const Json& v, Vec3 def) {
    if (v.isArray() && v.size() == 3) return Vec3(v[0].asFloat(), v[1].asFloat(), v[2].asFloat());
    return def;
}

Json EntitySummary(const Scene& scene, EntityId id) {
    const EntityRecord* rec = scene.Record(id);
    Json e = Json::MakeObject();
    e["id"] = id;
    e["name"] = rec->name;
    if (rec->parent != kNullEntity) e["parent"] = rec->parent;
    Json comps = Json::MakeArray();
    for (const ComponentType* t : scene.ComponentsOf(id)) comps.push(t->name);
    e["components"] = comps;
    return e;
}

Json SceneSummary(Engine& e) {
    Scene& s = e.GetScene();
    Json out = Json::MakeObject();
    out["name"] = s.name;
    out["path"] = e.ScenePath();
    Json list = Json::MakeArray();
    for (const auto& kv : s.Entities()) list.push(EntitySummary(s, kv.first));
    out["entities"] = list;
    return out;
}

Json SimState(Engine& e) {
    Json s = Json::MakeObject();
    s["playing"] = e.Playing();
    s["inPlaySession"] = e.InPlaySession();
    s["frame"] = static_cast<uint64_t>(e.Frame());
    s["revision"] = static_cast<uint64_t>(e.Revision());
    s["dirty"] = e.Dirty();
    s["undo"] = static_cast<uint64_t>(e.UndoDepth());
    s["redo"] = static_cast<uint64_t>(e.RedoDepth());
    s["time"] = e.SimTime();
    s["fixedDt"] = Engine::kFixedDt;
    Json keys = Json::MakeArray();
    for (const std::string& k : e.Input().down) keys.push(k);
    s["keysDown"] = keys;
    return s;
}

struct ViewRequest {
    int width = 640;
    int height = 360;
    RenderView view;
    bool customCamera = false;
};

ViewRequest ParseView(Engine& e, const Json& args) {
    ViewRequest r;
    r.width = std::clamp(args["width"].asInt(640), 16, 4096);
    r.height = std::clamp(args["height"].asInt(360), 16, 4096);
    float aspect = static_cast<float>(r.width) / static_cast<float>(r.height);
    const Json& cam = args["camera"];
    if (cam.isObject()) {
        r.view = MakeLookAtView(ReadVec3(cam["eye"], Vec3(6, 5, 8)), ReadVec3(cam["target"], Vec3(0, 0, 0)), cam["fov"].asFloat(60.0f), aspect);
        RenderView sceneView;
        MakeSceneView(e.GetScene(), aspect, sceneView);
        r.view.clearColor = sceneView.clearColor;
        r.customCamera = true;
    } else {
        MakeSceneView(e.GetScene(), aspect, r.view);
    }
    r.view.drawGrid = args["grid"].asBool(false);
    if (args["highlight"].isNumber()) r.view.highlight = static_cast<EntityId>(args["highlight"].asNumber());
    return r;
}

Json CameraSchema() {
    Json cam = Json::MakeObject();
    cam["type"] = "object";
    cam["description"] = "Optional free camera {eye:[x,y,z], target:[x,y,z], fov:degrees}. Default: the scene's active camera.";
    return cam;
}

Params ViewParams() {
    Params p;
    p.Opt("width", "integer", "Image width in pixels (default 640).")
        .Opt("height", "integer", "Image height in pixels (default 360).")
        .OptWith("camera", CameraSchema())
        .Opt("grid", "boolean", "Draw the editor ground grid (default false).")
        .Opt("highlight", "integer", "Entity id to outline.");
    return p;
}

std::string Guide() {
    return
        "OwnEngine quick guide for agents:\n"
        "1. scene.summary -> list entities (id, name, components).\n"
        "2. component.types -> field schemas for every component.\n"
        "3. entity.create {name, components:{Transform:{position:[0,1,0]}, MeshRenderer:{mesh:\"cube\", color:[1,0,0]}}}.\n"
        "4. component.set {id, type, values} for partial edits. Entities can be referenced by id or unique name.\n"
        "5. render.screenshot -> PNG of the game camera (or pass camera:{eye,target}). Look at it to verify.\n"
        "6. sim.step {frames:60} simulates one second deterministically; input.key {key:\"W\", down:true} injects input.\n"
        "   sim.stop restores the scene from before simulation started.\n"
        "7. scene.save persists to disk (JSON, diff friendly). history.undo reverts the last edit.\n"
        "Use api.batch to send many commands in one round trip.";
}

void Register(CommandRegistry& r, const char* name, const char* summary, Json params, bool mutates,
              std::function<Json(Engine&, const Json&)> fn) {
    Command c;
    c.name = name;
    c.summary = summary;
    c.params = std::move(params);
    c.mutates = mutates;
    c.run = std::move(fn);
    r.Add(std::move(c));
}

}  // namespace

// ---------------------------------------------------------------------------
// Built-in commands
// ---------------------------------------------------------------------------

void RegisterBuiltinCommands(CommandRegistry& r) {
    // ----- engine / api ----------------------------------------------------
    Register(r, "engine.info", "Engine version, platform, project, scene and simulation status.", Params(), false, [](Engine& e, const Json&) {
        Json info = Json::MakeObject();
        info["engine"] = "OwnEngine";
        info["version"] = OE_VERSION;
        info["platform"] = OE_PLATFORM_NAME;
        info["renderer"] = e.Renderer().Name();
        info["project"]["name"] = e.ProjectName();
        info["project"]["dir"] = e.ProjectDir();
        info["scene"]["name"] = e.GetScene().name;
        info["scene"]["path"] = e.ScenePath();
        info["scene"]["entities"] = static_cast<uint64_t>(e.GetScene().Entities().size());
        info["scene"]["dirty"] = e.Dirty();
        info["sim"] = SimState(e);
        info["history"]["undo"] = static_cast<uint64_t>(e.UndoDepth());
        info["history"]["redo"] = static_cast<uint64_t>(e.RedoDepth());
        info["guide"] = Guide();
        return info;
    });

    Register(r, "api.list", "List all commands with their argument schemas.",
             Params().Opt("verbose", "boolean", "Include full JSON schemas (default true)."), false, [](Engine& e, const Json& a) {
                 bool verbose = a["verbose"].asBool(true);
                 Json list = Json::MakeArray();
                 for (const Command& c : e.Commands().All()) {
                     Json j = Json::MakeObject();
                     j["name"] = c.name;
                     j["summary"] = c.summary;
                     j["mutates"] = c.mutates;
                     if (verbose) j["params"] = c.params;
                     list.push(j);
                 }
                 return list;
             });

    Register(r, "api.batch", "Run several commands in order and return every result.",
             Params()
                 .Req("calls", "array", "Array of {command: string, args: object}.")
                 .Opt("stopOnError", "boolean", "Stop at the first failing call (default true)."),
             false, [](Engine& e, const Json& a) {
                 bool stop = a["stopOnError"].asBool(true);
                 Json results = Json::MakeArray();
                 for (const Json& call : a["calls"].items()) {
                     std::string name = call["command"].asString("");
                     if (name == "api.batch") throw ApiError("invalid_argument", "api.batch cannot be nested");
                     Json res = e.Call(name, call["args"]);
                     bool ok = res["ok"].asBool();
                     results.push(std::move(res));
                     if (!ok && stop) break;
                 }
                 return results;
             });

    Register(r, "log.get", "Read engine log entries newer than a sequence number.",
             Params().Opt("since", "integer", "Return entries with seq > since (default 0).").Opt("limit", "integer", "Max entries (default 200)."),
             false, [](Engine&, const Json& a) {
                 Json out = Json::MakeObject();
                 Json list = Json::MakeArray();
                 for (const LogEntry& l : Log::Since(static_cast<uint64_t>(a["since"].asNumber(0)), static_cast<size_t>(a["limit"].asInt(200)))) {
                     Json j = Json::MakeObject();
                     j["seq"] = l.seq;
                     j["time"] = l.time;
                     j["level"] = ToString(l.level);
                     j["category"] = l.category;
                     j["message"] = l.message;
                     list.push(j);
                 }
                 out["entries"] = list;
                 out["lastSeq"] = Log::LastSeq();
                 return out;
             });

    // ----- scene -------------------------------------------------------------
    Register(r, "scene.summary", "Compact list of entities: id, name, parent, component names.", Params(), false,
             [](Engine& e, const Json&) { return SceneSummary(e); });

    Register(r, "scene.get", "Full scene document (the exact JSON that is saved to disk).", Params(), false,
             [](Engine& e, const Json&) { return e.GetScene().ToJson(); });

    Register(r, "scene.new", "Replace the current scene with an empty one (camera + light included by default).",
             Params().Opt("name", "string", "Scene name.").Opt("empty", "boolean", "Skip the default camera and light."), true,
             [](Engine& e, const Json& a) {
                 e.NewScene(a["name"].asString("Untitled"));
                 Scene& s = e.GetScene();
                 if (!a["empty"].asBool(false)) {
                     EntityId cam = s.Create("Main Camera");
                     s.Add<Transform>(cam) = Transform{{0, 3, 8}, {-15, 0, 0}, {1, 1, 1}};
                     s.Add<Camera>(cam);
                     EntityId sun = s.Create("Sun");
                     s.Add<Transform>(sun) = Transform{{0, 5, 0}, {-50, 30, 0}, {1, 1, 1}};
                     s.Add<DirectionalLight>(sun);
                 }
                 return SceneSummary(e);
             });

    Register(r, "scene.load", "Load a scene file (path relative to the project directory).",
             Params().Req("path", "string", "Scene file path, e.g. \"scenes/main.scene.json\"."), false, [](Engine& e, const Json& a) {
                 std::string err;
                 if (!e.LoadScene(e.ResolvePath(a["path"].asString()), &err)) throw ApiError("load_failed", err);
                 return SceneSummary(e);
             });

    Register(r, "scene.save", "Save the scene to disk. While simulating, the pre-simulation scene is saved.",
             Params().Opt("path", "string", "Target path relative to the project (default: current scene path)."), false,
             [](Engine& e, const Json& a) {
                 std::string path = a.has("path") ? e.ResolvePath(a["path"].asString()) : e.ScenePath();
                 if (path.empty()) throw ApiError("no_path", "scene has never been saved", "Pass a path, e.g. {\"path\": \"scenes/main.scene.json\"}.");
                 std::string err;
                 if (!e.SaveScene(path, &err)) throw ApiError("save_failed", err);
                 Json out = Json::MakeObject();
                 out["path"] = path;
                 return out;
             });

    Register(r, "scene.list", "List scene files in the project directory.", Params(), false, [](Engine& e, const Json&) {
        Json list = Json::MakeArray();
        for (const std::string& f : ListFiles(e.ProjectDir(), ".scene.json", true)) list.push(RelativePath(f, e.ProjectDir()));
        return list;
    });

    Register(r, "scene.rename", "Rename the scene.", Params().Req("name", "string", "New scene name."), true, [](Engine& e, const Json& a) {
        e.GetScene().name = a["name"].asString();
        return Json(e.GetScene().name);
    });

    // ----- entities ----------------------------------------------------------
    Register(r, "entity.create", "Create an entity, optionally with components and a parent.",
             Params()
                 .Opt("name", "string", "Entity name (should be unique so it can be referenced by name).")
                 .OptWith("parent", EntityRefSchema("Parent entity id or name."))
                 .Opt("components", "object", "Map of component type -> field values, e.g. {\"Transform\": {\"position\": [0,1,0]}}."),
             true, [](Engine& e, const Json& a) {
                 Scene& s = e.GetScene();
                 Json spec = Json::MakeObject();
                 spec["name"] = a["name"].asString("");
                 if (a.has("parent")) spec["parent"] = RequireEntity(e, a, "parent");
                 Json comps = a["components"].isObject() ? a["components"] : Json::MakeObject();
                 if (!comps.has("Transform")) comps["Transform"] = Json::MakeObject();
                 spec["components"] = comps;
                 std::string err;
                 EntityId id = s.CreateFromJson(spec, false, &err);
                 if (id == kNullEntity) throw ApiError("invalid_component_values", err, "Call component.types to see valid fields.");
                 return s.EntityToJson(id);
             });

    Register(r, "entity.get", "Full data of one entity (all components and values).",
             Params().ReqWith("id", EntityRefSchema("Entity id or name.")), false, [](Engine& e, const Json& a) {
                 return e.GetScene().EntityToJson(RequireEntity(e, a));
             });

    Register(r, "entity.find", "Find entities by name substring, component type and/or tag.",
             Params()
                 .Opt("name", "string", "Case-insensitive substring of the entity name.")
                 .Opt("component", "string", "Only entities that have this component type.")
                 .Opt("tag", "string", "Only entities whose Tag component contains this tag."),
             false, [](Engine& e, const Json& a) {
                 Scene& s = e.GetScene();
                 std::string name = Lower(a["name"].asString(""));
                 const ComponentType* type = a.has("component") ? &RequireType(a["component"]) : nullptr;
                 std::string tag = a["tag"].asString("");
                 Json list = Json::MakeArray();
                 for (const auto& kv : s.Entities()) {
                     if (!name.empty() && Lower(kv.second.name).find(name) == std::string::npos) continue;
                     if (type && !s.GetComponent(kv.first, *type)) continue;
                     if (!tag.empty()) {
                         const Tag* t = s.Get<Tag>(kv.first);
                         if (!t) continue;
                         std::string tags = "," + t->tags + ",";
                         tags.erase(std::remove(tags.begin(), tags.end(), ' '), tags.end());
                         if (tags.find("," + tag + ",") == std::string::npos) continue;
                     }
                     list.push(EntitySummary(s, kv.first));
                 }
                 return list;
             });

    Register(r, "entity.delete", "Delete an entity and all of its children.",
             Params().ReqWith("id", EntityRefSchema("Entity id or name.")), true, [](Engine& e, const Json& a) {
                 Json out = Json::MakeObject();
                 out["deleted"] = e.GetScene().Destroy(RequireEntity(e, a));
                 return out;
             });

    Register(r, "entity.rename", "Rename an entity.",
             Params().ReqWith("id", EntityRefSchema("Entity id or name.")).Req("name", "string", "New name."), true, [](Engine& e, const Json& a) {
                 EntityId id = RequireEntity(e, a);
                 e.GetScene().Record(id)->name = a["name"].asString();
                 return EntitySummary(e.GetScene(), id);
             });

    Register(r, "entity.set_parent", "Re-parent an entity (parent 0 = scene root).",
             Params().ReqWith("id", EntityRefSchema("Entity id or name.")).ReqWith("parent", EntityRefSchema("New parent id/name, or 0 for root.")),
             true, [](Engine& e, const Json& a) {
                 EntityId id = RequireEntity(e, a);
                 EntityId parent = a["parent"].isNumber() && a["parent"].asNumber() == 0 ? kNullEntity : RequireEntity(e, a, "parent");
                 std::string err;
                 if (!e.GetScene().SetParent(id, parent, &err)) throw ApiError("invalid_parent", err);
                 return EntitySummary(e.GetScene(), id);
             });

    Register(r, "entity.duplicate", "Duplicate an entity (and its children) next to the original.",
             Params().ReqWith("id", EntityRefSchema("Entity id or name.")).Opt("name", "string", "Name for the copy."), true,
             [](Engine& e, const Json& a) {
                 Scene& s = e.GetScene();
                 EntityId src = RequireEntity(e, a);
                 std::function<EntityId(EntityId, EntityId)> copy = [&](EntityId from, EntityId parent) {
                     Json j = s.EntityToJson(from);
                     j.erase("id");
                     if (parent != kNullEntity) j["parent"] = parent;
                     std::string err;
                     EntityId id = s.CreateFromJson(j, false, &err);
                     for (EntityId child : s.Children(from)) {
                         if (child != id) copy(child, id);
                     }
                     return id;
                 };
                 EntityId id = copy(src, s.Record(src)->parent);
                 s.Record(id)->name = a.has("name") ? a["name"].asString() : s.Record(src)->name + " Copy";
                 return s.EntityToJson(id);
             });

    // ----- components --------------------------------------------------------
    Register(r, "component.types", "All component types with documentation and JSON schemas of their fields.", Params(), false,
             [](Engine&, const Json&) {
                 Json list = Json::MakeArray();
                 for (const ComponentType& t : TypeRegistry::All()) {
                     Json j = Json::MakeObject();
                     j["name"] = t.name;
                     j["doc"] = t.doc;
                     j["schema"] = ComponentSchema(t);
                     Json defaults = Json::MakeObject();
                     auto pool = t.makePool();
                     defaults = ComponentToJson(t, pool->Add(1));
                     j["defaults"] = defaults;
                     list.push(j);
                 }
                 return list;
             });

    Register(r, "component.add", "Add a component to an entity (values are optional overrides of the defaults).",
             Params().ReqWith("id", EntityRefSchema("Entity id or name.")).Req("type", "string", "Component type name.").Opt("values", "object", "Field values."),
             true, [](Engine& e, const Json& a) {
                 EntityId id = RequireEntity(e, a);
                 const ComponentType& t = RequireType(a["type"]);
                 Scene& s = e.GetScene();
                 bool existed = s.GetComponent(id, t) != nullptr;
                 void* c = s.AddComponent(id, t);
                 std::string err;
                 if (!ApplyComponentJson(t, c, a["values"], &err)) {
                     if (!existed) s.RemoveComponent(id, t);
                     throw ApiError("invalid_component_values", err, "Call component.types to see valid fields.");
                 }
                 return s.EntityToJson(id);
             });

    Register(r, "component.set", "Change some fields of an existing component (partial update).",
             Params().ReqWith("id", EntityRefSchema("Entity id or name.")).Req("type", "string", "Component type name.").Req("values", "object", "Field values to change."),
             true, [](Engine& e, const Json& a) {
                 EntityId id = RequireEntity(e, a);
                 const ComponentType& t = RequireType(a["type"]);
                 Scene& s = e.GetScene();
                 void* c = s.GetComponent(id, t);
                 if (!c) throw ApiError("component_missing", "entity " + std::to_string(id) + " has no " + t.name + " component", "Use component.add to add it first.");
                 // Validate on a copy first so a bad field does not leave a half-applied edit.
                 Json backup = ComponentToJson(t, c);
                 std::string err;
                 if (!ApplyComponentJson(t, c, a["values"], &err)) {
                     std::string ignored;
                     ApplyComponentJson(t, c, backup, &ignored);
                     throw ApiError("invalid_component_values", err, "Call component.types to see valid fields.");
                 }
                 return ComponentToJson(t, c);
             });

    Register(r, "component.remove", "Remove a component from an entity.",
             Params().ReqWith("id", EntityRefSchema("Entity id or name.")).Req("type", "string", "Component type name."), true,
             [](Engine& e, const Json& a) {
                 EntityId id = RequireEntity(e, a);
                 const ComponentType& t = RequireType(a["type"]);
                 if (!e.GetScene().RemoveComponent(id, t)) throw ApiError("component_missing", "entity has no " + t.name + " component");
                 return e.GetScene().EntityToJson(id);
             });

    // ----- simulation ----------------------------------------------------------
    Register(r, "sim.state", "Simulation state: playing, frame, time, keys held.", Params(), false, [](Engine& e, const Json&) { return SimState(e); });
    Register(r, "sim.play", "Start (or resume) real-time simulation. The scene is snapshotted first.", Params(), false, [](Engine& e, const Json&) {
        e.Play();
        return SimState(e);
    });
    Register(r, "sim.pause", "Pause real-time simulation (state is kept).", Params(), false, [](Engine& e, const Json&) {
        e.Pause();
        return SimState(e);
    });
    Register(r, "sim.stop", "Stop simulating and restore the scene from before play/step started.", Params(), false, [](Engine& e, const Json&) {
        e.Stop();
        return SimState(e);
    });
    Register(r, "sim.step", "Advance the simulation by N fixed 1/60 s frames (deterministic).",
             Params().Opt("frames", "integer", "Frames to simulate (default 1, max 36000)."), false, [](Engine& e, const Json& a) {
                 e.Step(std::clamp(a["frames"].asInt(1), 0, 36000));
                 return SimState(e);
             });

    Register(r, "input.key", "Press or release a key (\"W\", \"A\", \"S\", \"D\", \"Space\", \"Left\", ...).",
             Params().Req("key", "string", "Key name.").Opt("down", "boolean", "true = press (default), false = release."), false,
             [](Engine& e, const Json& a) {
                 std::string key = a["key"].asString();
                 if (a["down"].asBool(true)) {
                     if (!e.Input().IsDown(key)) e.Input().pressedThisFrame.insert(key);
                     e.Input().down.insert(key);
                 } else {
                     e.Input().down.erase(key);
                 }
                 return SimState(e);
             });

    Register(r, "input.clear", "Release all keys.", Params(), false, [](Engine& e, const Json&) {
        e.Input().down.clear();
        e.Input().pressedThisFrame.clear();
        return SimState(e);
    });

    // ----- rendering -------------------------------------------------------------
    {
        Params p = ViewParams();
        p.Opt("path", "string", "Also write the PNG to this path (relative to the project).")
            .Opt("inline", "boolean", "Return the PNG as base64 in png_base64 (default true when no path is given).");
        Register(r, "render.screenshot", "Render a frame and return it as PNG (and/or save it). Look at it to verify visual changes.", p, false,
                 [](Engine& e, const Json& a) {
                     ViewRequest vr = ParseView(e, a);
                     RenderTarget target;
                     target.Resize(vr.width, vr.height);
                     RenderStats stats = e.Renderer().Render(e.GetScene(), vr.view, target);
                     Image img = target.ToImage();
                     Json out = Json::MakeObject();
                     out["width"] = vr.width;
                     out["height"] = vr.height;
                     out["hash"] = Format("%016llx", static_cast<unsigned long long>(target.Hash()));
                     out["camera"] = vr.customCamera ? "custom" : (vr.view.cameraEntity ? "scene" : "default (scene has no active camera)");
                     out["stats"]["entities"] = stats.drawnEntities;
                     out["stats"]["triangles"] = stats.triangles;
                     out["stats"]["ms"] = stats.milliseconds;
                     std::vector<uint8_t> png = EncodePng(img);
                     if (a.has("path")) {
                         std::string path = e.ResolvePath(a["path"].asString());
                         CreateDirectories(ParentPath(path));
                         if (!WritePng(path, img)) throw ApiError("write_failed", "cannot write " + path);
                         out["path"] = path;
                     }
                     if (a["inline"].asBool(!a.has("path"))) out["png_base64"] = Base64Encode(png);
                     return out;
                 });
    }
    {
        Params p = ViewParams();
        p.Req("x", "integer", "Pixel x (0 = left).").Req("y", "integer", "Pixel y (0 = top).");
        Register(r, "render.pick", "Return the entity visible at a pixel of a rendered view.", p, false, [](Engine& e, const Json& a) {
            ViewRequest vr = ParseView(e, a);
            RenderTarget target;
            target.Resize(vr.width, vr.height);
            e.Renderer().Render(e.GetScene(), vr.view, target);
            EntityId id = target.IdAt(a["x"].asInt(), a["y"].asInt());
            Json out = Json::MakeObject();
            out["id"] = id;
            if (id != kNullEntity) out["name"] = e.GetScene().Record(id)->name;
            return out;
        });
    }

    Register(r, "render.meshes", "Names of the built-in meshes usable in MeshRenderer.mesh.", Params(), false, [](Engine&, const Json&) {
        Json list = Json::MakeArray();
        for (const std::string& n : BuiltinMeshNames()) list.push(n);
        return list;
    });

    // ----- history -----------------------------------------------------------------
    Register(r, "history.undo", "Undo the last scene edit (not available while simulating).", Params(), false, [](Engine& e, const Json&) {
        if (e.InPlaySession()) throw ApiError("simulating", "cannot undo while simulating", "Call sim.stop first.");
        Json out = Json::MakeObject();
        out["undone"] = e.Undo();
        out["undo"] = static_cast<uint64_t>(e.UndoDepth());
        out["redo"] = static_cast<uint64_t>(e.RedoDepth());
        return out;
    });
    Register(r, "history.redo", "Redo the last undone edit.", Params(), false, [](Engine& e, const Json&) {
        if (e.InPlaySession()) throw ApiError("simulating", "cannot redo while simulating", "Call sim.stop first.");
        Json out = Json::MakeObject();
        out["redone"] = e.Redo();
        out["undo"] = static_cast<uint64_t>(e.UndoDepth());
        out["redo"] = static_cast<uint64_t>(e.RedoDepth());
        return out;
    });
}

}  // namespace oe
