#include "api/Commands.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "app/Engine.h"
#include "app/CoordinateQueries.h"
#include "app/Project.h"
#include "app/AssetPreview.h"
#include "app/Vfx.h"
#include "app/VfxExpressionGraph.h"
#include "assets/Assets.h"
#include "scene/SpriteClip.h"
#include "assets/AnimationAsset.h"
#include "assets/AnimationController.h"
#include "audio/AudioSystem.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Log.h"
#include "net/JsonCodec.h"
#include "render/GpuRenderer.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/RenderScene.h"
#include "render/ShaderGraph.h"
#include "render/Font.h"
#include "render/UI.h"
#include "scene/Components.h"
#include "scene/Pose.h"
#include "scene/Retarget.h"
#include "scene/Prefab.h"
#include "scene/Clipboard.h"
#include "nav/Navigation.h"
#include "physics/PhysicsWorld.h"
#include "scene/TileGrid.h"
#include "script/ScriptCheck.h"
#include "script/ScriptHost.h"
#include "script/OfficialModules.h"

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
    } else if (t == "entity") {  // an id or a unique name; MCP clients reject unknown type names
        p["type"] = Json::MakeArray();
        p["type"].push("integer");
        p["type"].push("string");
    } else if (t == "bool") {
        p["type"] = "boolean";
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
    if (type == "integer") return v.isNumber() && std::isfinite(v.asNumber()) &&
        v.asNumber() >= -9223372036854775808.0 && v.asNumber() < 9223372036854775808.0 && std::floor(v.asNumber()) == v.asNumber();
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
    s["axes"] = Json::MakeObject();
    s["rawAxes"] = Json::MakeObject();
    for (const char* name : InputState::kAxisNames) {
        s["axes"][name] = e.Input().Axis(name);
        auto raw = e.Input().axes.find(name);
        s["rawAxes"][name] = raw == e.Input().axes.end() ? 0.0f : raw->second;
    }
    s["mouseLocked"] = e.Input().mouseLocked;
    s["gamePaused"] = e.GamePaused();
    s["scriptErrors"] = static_cast<uint64_t>(e.Scripts().Errors().size());
    s["sceneName"] = e.GetScene().name;
    s["entities"] = static_cast<uint64_t>(e.GetScene().Entities().size());
    return s;
}

struct ViewRequest {
    int width = 640;
    int height = 360;
    RenderView view;
    bool customCamera = false;
};

ViewRequest ParseView(Engine& e, const Json& args, Scene* sceneOverride = nullptr) {
    const Scene& scene = sceneOverride ? *sceneOverride : e.GetScene();
    ViewRequest r;
    r.width = std::clamp(args["width"].asInt(640), 16, 4096);
    r.height = std::clamp(args["height"].asInt(360), 16, 4096);
    float aspect = static_cast<float>(r.width) / static_cast<float>(r.height);
    const Json& cam = args["camera"];
    if (cam.isObject()) {
        r.view = MakeLookAtView(ReadVec3(cam["eye"], Vec3(6, 5, 8)), ReadVec3(cam["target"], Vec3(0, 0, 0)), cam["fov"].asFloat(60.0f), aspect);
        const std::string projection = cam["projection"].asString("perspective");
        if (projection == "orthographic") {
            const float size = cam["orthoSize"].asFloat(5.0f);
            if (!std::isfinite(size) || size <= 0)
                throw ApiError("invalid_argument", "camera.orthoSize must be finite and positive", "Set the half-height in world units.");
            r.view.proj = Mat4::Orthographic(size, aspect, 0.05f, 1000.0f);
        } else if (projection != "perspective") {
            throw ApiError("invalid_argument", "camera.projection must be perspective or orthographic");
        }
        RenderView sceneView;
        MakeSceneView(scene, aspect, sceneView);
        r.view.clearColor = sceneView.clearColor;
        r.customCamera = true;
    } else {
        MakeSceneView(scene, aspect, r.view);
    }
    r.view.drawGrid = args["grid"].asBool(false);
    r.view.shaderTime = static_cast<float>(e.SimTime());
    r.view.playing = e.InPlaySession();
    if (args["colliders"].asBool(false)) {
        TilesetLookup tilesets = e.Assets().Tilesets();
        AppendColliderLines(scene, r.view.lines, &tilesets);
    }
    if (args["navmesh"].asBool(false)) e.Nav().AppendLines(r.view.lines);
    e.AppendDebugLines(r.view.lines);
    if (args.has("ui")) r.view.drawUI = args["ui"].asBool(true);
    if (args["highlight"].isNumber()) r.view.highlight = static_cast<EntityId>(args["highlight"].asNumber());
    return r;
}

Json PointJson(const Vec3& v) { return Json(Json::Array{v.x, v.y, v.z}); }

// The baked mesh, or the error that says why there is none.
Navigation& RequireNav(Engine& e, bool rebake = false) {
    Navigation& nav = e.Nav(rebake);
    if (!nav.Baked())
        throw ApiError("no_navmesh", "the scene has no navigation mesh: " + e.NavInfo()["error"].asString("not baked"),
                       "Give the ground a static Collider (no RigidBody), or add a NavMesh component with source \"meshes\" or \"all\" to bake from "
                       "models; nav.state lists what was collected.");
    return nav;
}

float NavRange(const Json& a) {
    const float range = a["range"].asFloat(Navigation::kSnapRange);
    if (!std::isfinite(range) || range <= 0) throw ApiError("invalid_argument", "range must be positive", "Meters around the point, e.g. 2.");
    return range;
}

Json CameraSchema() {
    Json cam = Json::MakeObject();
    cam["type"] = "object";
    cam["description"] = "Optional free camera {eye:[x,y,z], target:[x,y,z], fov:degrees, projection:perspective|orthographic, orthoSize:half-height}. Free defaults: perspective, fov 60, orthoSize 5, clips 0.05..1000. Omit camera to use the scene's active camera.";
    return cam;
}

Params ViewParams() {
    Params p;
    p.Opt("width", "integer", "Image width in pixels (default 640).")
        .Opt("height", "integer", "Image height in pixels (default 360).")
        .OptWith("camera", CameraSchema())
        .Opt("grid", "boolean", "Draw the editor ground grid (default false).")
        .Opt("colliders", "boolean", "Draw collider wireframes: green solid, yellow trigger, cyan character (default false).")
        .Opt("navmesh", "boolean", "Draw the navigation mesh: light blue borders, darker lines between its polygons (default false).")
        .Opt("ui", "boolean", "Draw the in-game UI (default: true with the scene camera, false with a free camera).")
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
        "8. Attached to a human's editor: editor.state says what they selected and have open; editor.select points at entities.\n"
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
    RegisterAnimationAssetCommands(r);
    RegisterAnimationControllerCommands(r);
    Register(r, "vfx.edit", "Open an isolated Effects document shared by editor and attached agents; scene history is unchanged.",
             Params().Req("path", "string", "Project .vfx.json path.").Opt("new", "boolean", "Start an unsaved asset; path must not exist.")
                 .Opt("preset", "string", "Preset for a new document (default fire).").Opt("discard", "boolean", "Explicitly discard previous unsaved document."), false,
             [](Engine& e, const Json& a) { return e.VfxDocumentCall("edit", a); });
    Register(r, "vfx.edit_replace", "Validate and replace the complete isolated document in one document history step.",
             Params().Req("vfx", "object", "Complete oe.vfx document.").Opt("merge", "string", "Repeated edits with the same nonempty key coalesce."), false,
             [](Engine& e, const Json& a) { return e.VfxDocumentCall("edit_replace", a); });
    for (const char* operation : {"edit_state", "undo", "redo", "save", "close"}) {
        const std::string op = operation; Params params;
        if (op == "close") params.Opt("discard", "boolean", "Explicitly discard unsaved document changes.");
        Register(r, ("vfx." + op).c_str(), "Inspect, undo/redo, atomically save or close the isolated Effects document.", params, false,
                 [op](Engine& e, const Json& a) { return e.VfxDocumentCall(op == "edit_state" ? "state" : op, a); });
    }
    for (const char* operation : {"create", "get", "list", "set", "diff", "from_entity"}) {
        Params params;
        const std::string op = operation;
        if (op != "list") params.Req("path", "string", "Project-relative .vfx.json asset path.");
        if (op == "create") params.Opt("vfx", "object", "Version 1 oe.vfx document; provide exactly one of vfx or preset.")
            .Opt("preset", "string", "Name from vfx.presets; installs bundled dependencies and refuses existing asset paths.");
        if (op == "set") params.Opt("emitter", "string", "Emitter block name.").Opt("trail", "string", "Trail block name.")
            .Opt("values", "object", "Partial block field patch.").Opt("params", "object", "Replace parameter definitions.")
            .Opt("merge", "string", "Coalesce a drag in the open document history; ignored for direct file edits.");
        if (op == "diff") params.Opt("against", "string", "Reference .vfx.json; omitted compares named blocks with component defaults.");
        if (op == "from_entity") params.ReqWith("id", EntityRefSchema("Root of a VFXSystem/Trail subtree to pack."));
        Register(r, ("vfx." + op).c_str(), "Read or atomically author a reusable VFX asset; file edits do not enter scene undo.", params, false,
                 [op](Engine& e, const Json& a) { return VfxAssetCall(e, op, a); });
    }
    Register(r, "vfx.spawn", "Create a linked VFXSystem with validated parameters in one scene undo step.",
             Params().Req("path", "string", "Project .vfx.json asset.").Opt("name", "string", "Owner entity name.")
                 .OptWith("parent", EntityRefSchema("Optional parent; position is local to parent."))
                 .Opt("position", "array", "Local [x,y,z].").Opt("params", "object", "Named parameter overrides.")
                 .Opt("autoDestroy", "boolean", "Destroy owner after all emission and particles/trails finish (default true)."), true,
             [](Engine& e, const Json& a) { return VfxRuntimeCall(e, "spawn", 0, a); });
    for (const char* operation : {"play", "stop", "burst", "clear", "set_param", "state"}) {
        const std::string op = operation; Params params;
        params.ReqWith("id", EntityRefSchema("VFXSystem owner id or name."));
        if (op == "play") params.Opt("restart", "boolean", "Restart all blocks (default true); false resumes their clocks.");
        if (op == "stop") params.Opt("kill", "boolean", "Clear particles and trails immediately (default false: drain).");
        if (op == "burst") params.Opt("emitter", "string", "One named emitter; omitted targets all in asset order.")
            .Opt("count", "integer", "Births per selected emitter, 0..10000; omitted uses each configured burst.");
        if (op == "clear") params.Opt("restart", "boolean", "If supplied, reset clocks/RNG/counters; true also re-arms automatic births. Omitted only clears live particles/trails.");
        if (op == "state") params.Opt("particles", "boolean", "Include captured/evaluated particle snapshots per emitter.");
        if (op == "set_param") params.Req("name", "string", "Parameter name.").ReqWith("value", Json::MakeObject());
        Register(r, ("vfx." + op).c_str(), "Control or inspect linked VFX runtime; controls do not create scene undo entries.", params, false,
                 [op](Engine& e, const Json& a) { return VfxRuntimeCall(e, op, RequireEntity(e, a), a); });
    }
    Register(r,"vfx.expression_eval","Compile and evaluate a bounded VFX expression without changing the scene; returns its type, value and instruction cost.",
        Params().Req("source","string","Expression source, 1..1024 bytes.").Opt("type","string","scalar (default) or vector.")
            .Opt("inputs","object","Finite scalar/vector inputs from vfx.schema.expressions; omitted values use deterministic defaults.")
            .Opt("params","object","Numeric scalar/three-vector parameter values, up to 32 entries."),false,
        [](Engine&,const Json& a){return EvaluateVfxExpressionCall(a);});
    Register(r,"vfx.expression_graph","Convert expression text to transient typed value nodes; unsupported conversion preserves source documents.",
        Params().Req("source","string","Expression text, 1..1024 bytes.").Opt("type","string","scalar (default) or vector.")
            .Opt("params","object","Numeric parameter defaults, up to 32 entries."),false,
        [](Engine&,const Json& a){return VfxExpressionGraphCall(a,false);});
    Register(r,"vfx.expression_source","Compile a transient value graph back to expression text; rejects cycles, invalid types and exceeded budgets.",
        Params().Req("graph","object","oe.vfx.expression_graph version 1; nodes with op, args, literal/name/axis and integer output.")
            .Opt("type","string","scalar (default) or vector.").Opt("params","object","Numeric parameter defaults, up to 32 entries."),false,
        [](Engine&,const Json& a){return VfxExpressionGraphCall(a,true);});
    Register(r, "vfx.schema", "Discover particle/trail fields, fixed-order modules, defaults, ranges and authoring limits.", Params(), false,
             [](Engine&, const Json&) { return VfxSchema(); });
    Register(r, "vfx.presets", "List portable effect presets with intent sentences and tags.", Params().Opt("tag", "string", "Filter by an exact tag."), false,
             [](Engine&, const Json& a) { return VfxPresets(a["tag"].asString()); });
    Register(r, "vfx.apply_preset", "Apply a preset asset and attach a VFXSystem in one scene undo step; installed files survive undo.",
             Params().ReqWith("id", EntityRefSchema("Target entity id or name.")).Req("preset", "string", "Name from vfx.presets.")
                 .Opt("mode", "string", "replace resets the affected component (default); merge preserves unspecified settings; bundled presets require replace."), true,
             [](Engine& e, const Json& a) { return ApplyVfxPreset(e, RequireEntity(e, a), a); });
    Register(r, "vfx.check", "Validate settings in an effect subtree and report actionable warnings without stepping it.",
             Params().OptWith("id", EntityRefSchema("Effect root id or name; includes descendants.")).Opt("path", "string", "Alternative .vfx.json asset source."), false,
             [](Engine& e, const Json& a) { if (a.has("id") == a.has("path")) throw ApiError("invalid_vfx", "provide exactly one id or path"); return a.has("path") ? CheckVfxAsset(e, a["path"].asString()) : CheckVfx(e, RequireEntity(e, a)); });
    Register(r, "vfx.preview", "Restart an isolated effect and render a contact sheet with one shared camera; does not run gameplay scripts.",
             Params().OptWith("id", EntityRefSchema("Effect root id or name.")).Opt("path", "string", "Alternative .vfx.json asset source.")
                 .Opt("times", "array", "1..16 increasing seconds within 0..60; rounded to 60 Hz. Default [0.1,0.3,0.6,1].")
                 .Opt("size", "integer", "Square tile size 32..512 (default 256); up to four columns.")
                 .Opt("renderer", "string", "software (default, deterministic) or gpu (hardware dependent).")
                 .Opt("camera", "object", "Optional eye/target vec3, fov, projection and orthoSize; otherwise fit the sampled effect.")
                 .Opt("motion", "array", "2..32 keys [[seconds,[x,y,z]],...] of root offsets in parent coordinates; no gameplay scripts.")
                 .Opt("out", "string", "Write contact-sheet PNG inside the project.")
                 .Opt("inline", "boolean", "Return png_base64 (default true when out is absent)."), false,
             [](Engine& e, const Json& a) { if (a.has("id") == a.has("path")) throw ApiError("invalid_vfx", "provide exactly one id or path"); return PreviewVfx(e, a.has("id") ? RequireEntity(e, a) : 0, a); });
    Register(r, "vfx.analyze", "Measure a restarted isolated effect at fixed steps; peaks and completion examine every frame.",
             Params().OptWith("id", EntityRefSchema("Effect root id or name.")).Opt("path", "string", "Alternative .vfx.json asset source.")
                 .Opt("duration", "number", "1/60..60 seconds (default 5), rounded to 60 Hz.")
                 .Opt("step", "number", "Sample interval >=1/60 second (default 0.25); at most 241 samples.")
                 .Opt("render", "boolean", "Include mean luminance of 128x128 software frames (default false).")
                 .Opt("camera", "object", "Optional eye/target vec3, fov, projection and orthoSize for luminance frames.")
                 .Opt("motion", "array", "Root offsets [[seconds,[x,y,z]],...] for trail/moving-owner analysis."), false,
             [](Engine& e, const Json& a) { if (a.has("id") == a.has("path")) throw ApiError("invalid_vfx", "provide exactly one id or path"); return AnalyzeVfx(e, a.has("id") ? RequireEntity(e, a) : 0, a); });
    const struct { const char* name; const char* summary; } netQueries[] = {
        {"state", "Read network configuration, role, lobby state and connection error; none mode starts no networking."},
        {"players", "List lobby players in id order, or the local player in none mode."},
        {"stats", "Read peer RTT in milliseconds, loss, channel bytes, pending messages and selected transport route."},
        {"leave", "Leave the session gracefully within 30 simulated frames; none mode is unchanged."}
    };
    for (const auto& query : netQueries) {
        std::string command = query.name;
        std::string full = "net." + command;
        Register(r, full.c_str(), query.summary, Params(), false,
                 [command](Engine& e, const Json& a) { return e.NetworkCall(command, a); });
    }
    Register(r, "net.simulate", "Inspect or change shared loopback faults; future sends use the supplied seeded configuration.",
             Params().Opt("seed", "integer", "Fault RNG seed: 0..4294967295.")
                     .Opt("latencyFrames", "integer", "One-way delay: 0..3600 frames.")
                     .Opt("jitterFrames", "integer", "Uniform +/- jitter: 0..3600 frames.")
                     .Opt("lossPermille", "integer", "Loss: 0..1000 parts per thousand.")
                     .Opt("duplicatePermille", "integer", "Duplication: 0..1000 parts per thousand.")
                     .Opt("reorderFrames", "integer", "Extra random delay: 0..3600 frames."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("simulate", a); });
    Register(r, "net.spawn_local_peers", "Play an idle network project with additional in-process loopback peers; host stepping drives the group and auto-starts when ready.",
             Params().Req("count", "integer", "Additional peers: 1..7; host also occupies a player slot.")
                     .Opt("seed", "integer", "Session seed; default 1."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("spawn_local_peers", a); });
    Register(r, "net.local_peers", "Read preview peer indices, frames, states, stats and desync reports.", Params(), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("local_peers", a); });
    Register(r, "net.peer_call", "Route input or diagnostic commands to a preview peer; returns that command's JSON envelope.",
             Params().Req("peer", "integer", "Preview index: 0 host, 1..7 additional peers.")
                     .Req("command", "string", "input.*, state/diagnostic query or render.screenshot.")
                     .Opt("args", "object", "Command arguments."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("peer_call", a); });
    Register(r, "net.serve", "Start a headless dedicated session; slot 1 is the server and ready remote peers start the match automatically. "
             "With network.lateJoin the world starts at once, ready players enter it while it runs and it keeps running empty.",
             Params().Opt("name", "string", "Server name; default Player.")
                     .Opt("port", "integer", "Listen port: 0..65535; zero chooses an available port.")
                     .Opt("seed", "integer", "Session seed; default 1.")
                     .Opt("room", "string", "Loopback transport: room name; default the project's gameId.")
                     .Opt("minPlayers", "integer", "Minimum ready remote players before the match starts: 1..63, default 1 (with network.lateJoin: 0..63, default 0)."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("serve", a); });
    Register(r, "net.entities", "Inspect authoritative network entity ids, local ids, owners and received replication fields.", Params(), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("entities", a); });
    Register(r, "net.host", "Host a lobby; net.start begins lockstep, rollback or authoritative simulation.",
             Params().Opt("name", "string", "Player name: 1..64 printable bytes.")
                     .Opt("seed", "integer", "Session seed: 0..4294967295; default 1.")
                     .Opt("room", "string", "Loopback room in this process; default gameId.")
                     .Opt("port", "integer", "Listen port override: 0..65535; zero chooses an available port."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("host", a); });
    Register(r, "net.join", "Asynchronously join a lobby; advance both engines with sim.step until lobby.",
             Params().Opt("name", "string", "Player name: 1..64 printable bytes.")
                     .Opt("address", "string", "Numeric host IPv4, ws/wss URL, or loopback room name.")
                     .Opt("port", "integer", "TCP/UDP host port: 1..65535; default project network.port."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("join", a); });
    Register(r, "net.ready", "Set local lobby readiness; does not start synchronized simulation.",
             Params().Req("ready", "boolean", "Local player readiness."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("ready", a); });
    Register(r, "net.kick", "Host removes a remote player with a bounded graceful close.",
             Params().Req("player", "integer", "Remote player id from net.players.").Opt("reason", "string", "1..64 printable bytes; default kicked."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("kick", a); });
    Register(r, "net.move", "Server with several worlds (oe serve-game --scenes): put a connected player into the world of another scene. Its connection stays; the world it was in sees it leave, the other sees it join with `data` (Lua net.move, onPlayerJoined(id, data)).",
             Params().Req("player", "integer", "Remote player id from net.players.").Req("scene", "string", "The scene of the world to move it to (net.state().worlds).")
                     .Opt("data", "object", "Handed to the world it enters: at most 8 KiB of JSON, depth <=8."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("move", a); });
    Register(r, "net.admit", "Server with network.admission \"script\": let a ready lobby player into the running world (Lua net.admit).",
             Params().Req("player", "integer", "Remote player id from net.players."), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("admit", a); });
    Register(r, "net.rpc", "Send a bounded reliable RPC, or call its Lua handler locally in none mode.",
             Params().Req("name", "string", "Handler name: 1..64 printable bytes.")
                     .Opt("target", "string", "server/owner/all/others or decimal player id; default server.")
                     .Opt("args", "array", "At most 16 bounded JSON arguments, depth <=8, <=8192 serialized bytes.")
                     .OptWith("entity", EntityRefSchema("Entity id/name for target owner; alternatively pass id/name as first argument.")), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("rpc", a); });
    Register(r, "net.start", "Host starts a ready lockstep, rollback or authoritative match with a frozen roster and frame-zero barrier.", Params(), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("start", a); });
    Register(r, "net.desync_report", "First confirmed hash mismatch: frame, player, hashes and bounded host/peer scene JSON.", Params(), false,
             [](Engine& e, const Json& a) { return e.NetworkCall("desync_report", a); });
    Register(r, "input.player", "Inspect synchronized declared actions and raw quantized axes for one player.",
             Params().Req("player", "integer", "Player id."), false, [](Engine& e, const Json& a) {
                 double player = a["player"].asNumber();
                 if (player < 1 || player >= 4294967294.0 || std::floor(player) != player) throw ApiError("invalid_argument", "invalid player id");
                 const auto& input = e.PlayerInput(static_cast<uint32_t>(player)); Json out = Json::MakeObject();
                 out["down"] = Json::MakeArray(); out["pressed"] = Json::MakeArray(); out["axes"] = Json::MakeObject();
                 for (const auto& key : input.down) out["down"].push(key);
                 for (const auto& key : input.pressedThisFrame) out["pressed"].push(key);
                 for (const auto& axis : input.axes) out["axes"][axis.first] = axis.second;
                 return out;
             });
    Register(r, "sim.record_state", "Enable native full-state snapshots before frame zero; project resources must stay immutable.", Params(), false,
             [](Engine& e, const Json& a) { return e.SnapshotCall("record", a); });
    Register(r, "sim.save_state", "Save a full recorded simulation in one of eight in-memory slots.",
             Params().Opt("slot", "string", "Slot name, default default."), false,
             [](Engine& e, const Json& a) { return e.SnapshotCall("save", a); });
    Register(r, "sim.load_state", "Restore native Lua, physics and audio state; refuses changed resources or an active match.",
             Params().Opt("slot", "string", "Previously saved slot."), false,
             [](Engine& e, const Json& a) { return e.SnapshotCall("load", a); });
    auto animatorState = [](Engine& e, EntityId id, bool includePose) {
        if (const SpriteAnimation* sprite = e.GetScene().Get<SpriteAnimation>(id)) {
            const Json& clip = SpriteAnimationClips(*sprite, e.Assets())[sprite->clip];
            SpriteClipTiming timing; const bool valid = ParseSpriteClipTiming(clip, timing, nullptr);
            Json out = Json::MakeObject();
            out["id"] = id; out["type"] = "SpriteAnimation"; out["clip"] = sprite->clip;
            out["speed"] = sprite->speed; out["playing"] = sprite->playing && !sprite->finished;
            out["loop"] = SpriteAnimationLoop(*sprite, clip); out["asset"] = sprite->asset; out["time"] = sprite->time; out["finished"] = sprite->finished;
            out["validClip"] = valid;
            out["duration"] = valid ? (SpriteAnimationLoop(*sprite, clip) ? timing.duration : timing.end) : 0;
            out["variableTiming"] = timing.variable;
            if (const Sprite* renderer = e.GetScene().Get<Sprite>(id)) out["frame"] = renderer->frame;
            return out;
        }
        const Animator* animator = e.GetScene().Get<Animator>(id);
        if (!animator) throw ApiError("missing_component", "entity has no Animator", "Call animation.play or component.add with type Animator.");
        Json out = Json::MakeObject();
        out["id"] = id;
        out["clip"] = animator->clip;
        out["speed"] = animator->speed;
        out["loop"] = animator->loop;
        out["playing"] = animator->playing;
        out["time"] = animator->time;
        const MeshRenderer* renderer = e.GetScene().Get<MeshRenderer>(id);
        std::shared_ptr<const Mesh> mesh = renderer ? e.Assets().GetMesh(renderer->mesh) : nullptr;
        const AnimationClip* clip = mesh ? FindAnimationClip(*mesh, animator->clip) : nullptr;
        out["validClip"] = clip != nullptr;
        out["duration"] = clip ? clip->duration : 0;
        if (clip) {
            out["displayName"] = clip->displayName.empty() ? clip->name : clip->displayName;
            out["loopRange"] = Json::MakeArray(); out["loopRange"].push(clip->loopStart); out["loopRange"].push(clip->loopEnd < 0 ? clip->duration : clip->loopEnd);
        }
        if (includePose) {
            out["jointMatrices"] = Json::MakeArray();
            if (mesh) for (const Mat4& matrix : JointPalette(*mesh, EvaluateEntityPose(e.GetScene(), id, *mesh))) {
                Json values = Json::MakeArray();
                for (float value : matrix.m) values.push(value);
                out["jointMatrices"].push(values);
            }
        }
        return out;
    };
    Register(r, "animation.preview", "Open/control an isolated stopped-scene animation preview; no scripts, physics, events, scene edits or revision changes.",
             Params().OptWith("id", EntityRefSchema("Animated entity; required when opening."))
                 .Opt("clip", "string", "Preview clip.").Opt("time", "number", "Seek in seconds; clamps to the preview timeline.")
                 .Opt("playing", "boolean", "Editor auto-advance flag.").Opt("speed", "number", "Finite playback multiplier; negative reverses.")
                 .Opt("loop", "boolean", "Repeat; defaults to the clip setting.")
                 .Opt("ik", "boolean", "Apply the target model's stored BoneIK goals (default false); scripts do not move them during preview.")
                 .Opt("frames", "integer", "Advance 0..3600 fixed 1/60 s steps, even when paused."), false,
             [](Engine& e, const Json& a) { return e.AnimationPreviewCall("preview", a); });
    Register(r, "animation.preview_state", "Read isolated preview state, clip list and event marks; never changes the authored scene.", Params(), false,
             [](Engine& e, const Json& a) { return e.AnimationPreviewCall("state", a); });
    Register(r, "animation.preview_close", "Close isolated animation preview and restore the authored pose in editor views.", Params(), false,
             [](Engine& e, const Json& a) { return e.AnimationPreviewCall("close", a); });
    Register(r, "animation.state", "SpriteAnimation or Animator playback state; optional model-space joint matrices (column-major).",
             Params().ReqWith("id", EntityRefSchema("Entity with SpriteAnimation or Animator.")).Opt("pose", "boolean", "Include model jointMatrices."),
             false, [animatorState](Engine& e, const Json& a) { return animatorState(e, RequireEntity(e, a), a["pose"].asBool()); });
    Register(r, "animation.play", "Play a named sprite/model clip; add Animator for a model if needed. Restart defaults to the beginning (last frame/end for reverse).",
             Params().ReqWith("id", EntityRefSchema("Entity with SpriteAnimation and Sprite, or MeshRenderer.")).Req("clip", "string", "Exact name in SpriteAnimation.clips or asset.info.")
                 .Opt("speed", "number", "Playback multiplier; negative = reverse. Keeps existing speed when omitted.")
                 .Opt("loop", "boolean", "Repeat the clip. Keeps existing loop setting when omitted.")
                 .Opt("restart", "boolean", "Reset time (default true); false preserves time.")
                 .Opt("blend", "number", "Model crossfade seconds 0..5; sprites switch immediately."),
             true, [animatorState](Engine& e, const Json& a) {
                 EntityId id = RequireEntity(e, a);
                 if (a.has("blend") && (!std::isfinite(a["blend"].asNumber()) || a["blend"].asNumber() < 0 || a["blend"].asNumber() > 5))
                     throw ApiError("invalid_argument", "blend must be in 0..5 seconds");
                 if (const SpriteAnimation* existing = e.GetScene().Get<SpriteAnimation>(id)) {
                     if (!e.GetScene().Get<Sprite>(id)) throw ApiError("missing_component", "entity has no Sprite", "Add a Sprite component first.");
                     SpriteAnimation next = *existing;
                     const std::string name = a["clip"].asString();
                     const Json& clip = SpriteAnimationClips(next, e.Assets())[name];
                     if (!clip.isObject() || !clip["frames"].isArray() || clip["frames"].size() == 0)
                         throw ApiError("animation_not_found", "sprite has no clip '" + name + "'", "Define SpriteAnimation.clips with a nonempty frames array.");
                     if (a.has("speed")) next.speed = a["speed"].asFloat();
                     if (!std::isfinite(next.speed)) throw ApiError("invalid_argument", "speed must be finite", "Use a finite playback multiplier.");
                     if (a.has("loop")) {
                         if (next.asset.empty()) next.clips[name]["loop"] = a["loop"].asBool();
                         else next.loopOverride = a["loop"].asBool() ? 1 : 0;
                     }
                     next.clip = next.current = name; next.currentAsset = next.asset;
                     if (a["restart"].asBool(true)) {
                         SpriteClipTiming timing; std::string error;
                         if (!ParseSpriteClipTiming(clip, timing, &error)) throw ApiError("invalid_animation", error);
                         next.time = next.speed < 0 ? static_cast<float>(timing.variable && SpriteAnimationLoop(next, clip) ? timing.duration : timing.end) : 0;
                         next.eventsStarted = false;
                     }
                     next.finished = false; next.playing = true;
                     e.GetScene().Add<SpriteAnimation>(id) = next;
                     return animatorState(e, id, false);
                 }
                 const MeshRenderer* renderer = e.GetScene().Get<MeshRenderer>(id);
                 if (!renderer) throw ApiError("missing_component", "entity has no MeshRenderer", "Add MeshRenderer with a glTF model first.");
                 std::string error;
                 std::shared_ptr<const Mesh> mesh = e.Assets().GetMesh(renderer->mesh, &error);
                 if (!mesh) throw ApiError("asset_error", error, "Check MeshRenderer.mesh with asset.info.");
                 const AnimationClip* clip = FindAnimationClip(*mesh, a["clip"].asString());
                 if (!clip) throw ApiError("animation_not_found", "model has no clip '" + a["clip"].asString() + "'", "Call asset.info to list clips.");
                 const Animator* existing = e.GetScene().Get<Animator>(id);
                 Animator next = existing ? *existing : Animator{};
                 next.clip = clip->name;
                 if (a.has("blend")) next.blend = a["blend"].asFloat();
                 if (a.has("speed")) next.speed = a["speed"].asFloat();
                 if (!std::isfinite(next.speed)) throw ApiError("invalid_argument", "speed must be finite", "Use a finite playback multiplier.");
                 if (a.has("loop")) next.loop = a["loop"].asBool();
                 if (a["restart"].asBool(true)) { next.time = next.speed < 0 ? (next.loop && clip->loopEnd >= 0 ? clip->loopEnd : clip->duration) : (next.loop ? clip->loopStart : 0); next.eventsStarted = false; }
                 next.playing = true;
                 e.GetScene().Add<Animator>(id) = next;
                 return animatorState(e, id, false);
             });
    auto animationEvents = [](Engine& e, const Json& a, bool write) {
        const EntityId id = RequireEntity(e, a);
        const SpriteAnimation* sprite = e.GetScene().Get<SpriteAnimation>(id);
        const Animator* animator = e.GetScene().Get<Animator>(id);
        if (!sprite && !animator && (!a.has("clip") || !e.GetScene().Get<MeshRenderer>(id)))
            throw ApiError("missing_component", "entity has no animation component", "Add SpriteAnimation/Animator, or pass clip for a model with MeshRenderer.");
        const std::string name = a.has("clip") ? a["clip"].asString() : sprite ? sprite->clip : animator->clip;
        Json markers = Json::MakeArray();
        std::string source;
        double duration = 0, fps = 0;
        size_t frames = 0;
        SpriteClipTiming timing;
        if (sprite) {
            const Json& clip = SpriteAnimationClips(*sprite, e.Assets())[name];
            frames = clip["frames"].isArray() ? clip["frames"].size() : 0;
            fps = std::max(0.001, clip["fps"].asNumber(8));
            if (!frames || !std::isfinite(fps)) throw ApiError("animation_not_found", "invalid sprite clip '" + name + "'", "Define frames and finite fps in SpriteAnimation.clips.");
            std::string timingError;
            if (!ParseSpriteClipTiming(clip, timing, &timingError)) throw ApiError("invalid_animation", timingError);
            duration = SpriteAnimationLoop(*sprite, clip) ? timing.duration : timing.end;
            if (clip.has("events")) markers = clip["events"];
            source = sprite->asset.empty() ? "SpriteAnimation.clips" : sprite->asset;
        } else {
            const MeshRenderer* renderer = e.GetScene().Get<MeshRenderer>(id);
            std::string error;
            auto mesh = renderer ? e.Assets().GetMesh(renderer->mesh, &error) : nullptr;
            if (!mesh) throw ApiError("asset_error", error.empty() ? "entity has no model" : error, "Check MeshRenderer.mesh and its .events.json sidecar.");
            const AnimationClip* clip = FindAnimationClip(*mesh, name);
            if (!clip) throw ApiError("animation_not_found", "model has no clip '" + name + "'", "Use asset.info to list clips.");
            duration = clip->duration; source = renderer->mesh + (clip->metadataSidecar ? ".clips.json" : ".events.json");
            for (const AnimationMarker& marker : clip->events) {
                Json item = Json::MakeObject(); item["time"] = marker.time; item["name"] = marker.name; markers.push(item);
            }
        }
        if (write) markers = a["events"];
        std::vector<AnimationMarker> parsed;
        std::string error;
        if (!(sprite ? SpriteClipMarkers(markers, timing, parsed, &error) : ParseAnimationMarkers(markers, duration, 0, 0, parsed, &error)))
            throw ApiError("invalid_animation_events", error, "Use [{frame: index, name: string}] for sprites or [{time: seconds, name: string}] for models; finished is reserved.");
        if (write) {
            if (sprite && sprite->asset.empty()) e.GetScene().Get<SpriteAnimation>(id)->clips[name]["events"] = markers;
            else if (sprite) {
                Json args = Json::MakeObject(); args["path"] = sprite->asset; args["clip"] = name; args["values"]["events"] = markers;
                Json result = e.Call("animation.set", args);
                if (!result["ok"].asBool()) throw ApiError(result["error"]["code"].asString(), result["error"]["message"].asString());
            }
            else {
                const std::string path = e.ResolvePath(source);
                Json sidecar = Json::MakeObject(); std::string text;
                if (FileExists(path)) {
                    if (!ReadTextFile(path, text)) throw ApiError("read_failed", "cannot read " + source);
                    sidecar = Json::parse(text);
                }
                if (source.size() >= 11 && source.substr(source.size() - 11) == ".clips.json") sidecar["metadata"][name]["events"] = markers;
                else sidecar["clips"][name] = markers;
                if (!WriteTextFile(path, sidecar.dump(2) + "\n")) throw ApiError("write_failed", "cannot write " + source);
                e.Assets().Clear();
            }
        }
        Json out = Json::MakeObject(); out["id"] = id; out["clip"] = name; out["source"] = source;
        out["duration"] = duration; out["events"] = Json::MakeArray(); out["completion"] = "finished";
        for (const AnimationMarker& marker : parsed) {
            Json item = Json::MakeObject(); item["time"] = marker.time; item["name"] = marker.name;
            if (sprite) item["frame"] = static_cast<int>(timing.Index(marker.time));
            out["events"].push(item);
        }
        return out;
    };
    Register(r, "animation.events", "List timeline markers for a sprite/model clip; non-looping completion calls onAnimationEvent(\"finished\", clip).",
             Params().ReqWith("id", EntityRefSchema("Animated entity.")).Opt("clip", "string", "Clip name; default current clip."), false,
             [animationEvents](Engine& e, const Json& a) { return animationEvents(e, a, false); });
    Register(r, "animation.set_events", "Replace clip markers. Inline sprites store scene events; linked sprites write .anim.json; models write clips.json metadata when present, otherwise legacy events.json. Shared file writes are not undoable.",
             Params().ReqWith("id", EntityRefSchema("Animated entity.")).Opt("clip", "string", "Clip name; default current clip.")
                 .Req("events", "array", "At most 256 {frame: sequenceIndex, name} for sprites or {time: seconds, name} for models. Empty clears markers.")
                 .Opt("merge", "string", "Undo group key for a sprite marker drag; model file writes remain outside undo."), true,
             [animationEvents](Engine& e, const Json& a) { return animationEvents(e, a, true); });
    Register(r, "animation.retarget", "Move animation clips from one model's skeleton to another's (different bone names, proportions and rest pose, e.g. mocap for a T-pose rig onto a character in an A-pose) and write them as a clip file. With attach (default) the file is listed in '<target>.clips.json', which adds its clips to the target model: play them with Animator / animation.play like the model's own.",
             Params().Req("source", "string", "Model file with the clips, e.g. \"assets/anims/mocap.glb\".")
                 .Req("target", "string", "Model file of the character that should play them.")
                 .Req("bones", "object", "Target bone name -> source bone name for the body: hips, spine, neck, head, shoulders, arms, hands, legs, feet. Bones left out keep their rest pose (fingers, hair, cloth). Names: animation.bones / asset.info.")
                 .Req("out", "string", "Clip file to write, e.g. \"assets/anims/hero_clips.gltf\".")
                 .Opt("hips", "string", "Target bone that also takes the source's movement (bob and sway of the body), scaled to the target's size. Usually the pelvis.")
                 .Opt("ends", "array", "Target bones that keep their rest orientation relative to the limb above instead of being laid along their source bone: feet and toes (rigs disagree on where the toe bone sits, and heels tip the foot). Without it shoes can tilt in every clip.")
                 .Opt("clips", "object", "New clip name -> source clip name. Default: every clip under its own name.")
                 .Opt("rate", "number", "Keys per second, 1..120 (default 30).")
                 .Opt("attach", "boolean", "List the file in '<target>.clips.json' so the target model has the clips (default true)."),
             false, [](Engine& e, const Json& a) {
                 auto load = [&](const std::string& path) {
                     std::string error;
                     std::shared_ptr<const Mesh> mesh = e.Assets().GetMesh(path, &error);
                     if (!mesh) throw ApiError("asset_error", path + ": " + error, "Pass project-relative model paths (.glb/.gltf/.fbx); check them with asset.info.");
                     return mesh;
                 };
                 const std::string out = a["out"].asString();
                 if (out.size() < 6 || Lower(out).compare(out.size() - 5, 5, ".gltf") != 0) throw ApiError("invalid_path", "out must end with .gltf", "e.g. assets/anims/hero_clips.gltf");
                 const std::shared_ptr<const Mesh> source = load(a["source"].asString()), target = load(a["target"].asString());
                 RetargetOptions options;
                 for (const auto& pair : a["bones"].members()) options.bones[pair.first] = pair.second.asString("");
                 for (const auto& pair : a["clips"].members()) options.clips[pair.first] = pair.second.asString("");
                 options.hips = a["hips"].asString("");
                 for (const Json& name : a["ends"].items()) options.ends.push_back(name.asString(""));
                 options.rate = a["rate"].asFloat(30);
                 Json gltf, report;
                 std::string error;
                 if (!RetargetClips(*source, *target, options, gltf, report, &error))
                     throw ApiError("retarget_failed", error, "Check the bone names of both models with animation.bones / asset.info and the clip names with asset.info.");
                 const std::string full = e.ResolvePath(out);
                 CreateDirectories(ParentPath(full));
                 if (!WriteTextFile(full, gltf.dump())) throw ApiError("write_failed", "cannot write " + out);
                 report["out"] = out;
                 if (a["attach"].asBool(true)) {
                     // Paths in the list are relative to the model file.
                     const std::string model = e.ResolvePath(a["target"].asString()), list = model + ".clips.json";
                     std::vector<std::string> from, to;
                     auto split = [](const std::string& path, std::vector<std::string>& parts) {
                         std::string part;
                         for (char c : path) {
                             if (c == '/' || c == '\\') { if (!part.empty()) parts.push_back(part); part.clear(); }
                             else part += c;
                         }
                         if (!part.empty()) parts.push_back(part);
                     };
                     split(ParentPath(model), from);
                     split(full, to);
                     size_t common = 0;
                     while (common < from.size() && common + 1 < to.size() && from[common] == to[common]) ++common;
                     std::string relative;
                     for (size_t i = common; i < from.size(); ++i) relative += "../";
                     for (size_t i = common; i < to.size(); ++i) relative += (i > common ? "/" : "") + to[i];
                     Json sidecar = Json::MakeObject();
                     std::string text;
                     if (FileExists(list) && ReadTextFile(list, text)) {
                         try { sidecar = Json::parse(text); } catch (const std::exception&) { sidecar = Json::MakeObject(); }
                     }
                     if (!sidecar.isObject()) sidecar = Json::MakeObject();
                     if (!sidecar["clips"].isArray()) sidecar["clips"] = Json::MakeArray();
                     bool listed = false;
                     for (const Json& entry : sidecar["clips"].items()) listed = listed || entry.asString("") == relative;
                     if (!listed) sidecar["clips"].push(relative);
                     if (!WriteTextFile(list, sidecar.dump(2))) throw ApiError("write_failed", "cannot write " + a["target"].asString() + ".clips.json");
                     report["attached"] = a["target"].asString() + ".clips.json";
                 }
                 e.Assets().Clear();  // the target model now has the clips
                 return report;
             });
    Register(r, "animation.retarget_map", "Guess the bone map for animation.retarget from the bone names of two humanoid models (Mixamo, 3ds Max Biped, Unreal and Unity/VRM style names): {bones: {target: source}, hips, ends (feet and toes), unmatched (body parts not found in both)}. Check it, complete it by hand where a rig uses other names, then pass it on.",
             Params().Req("source", "string", "Model file with the clips.").Req("target", "string", "Model file of the character."),
             false, [](Engine& e, const Json& a) {
                 auto load = [&](const std::string& path) {
                     std::string error;
                     std::shared_ptr<const Mesh> mesh = e.Assets().GetMesh(path, &error);
                     if (!mesh) throw ApiError("asset_error", path + ": " + error, "Pass project-relative model paths (.glb/.gltf/.fbx); check them with asset.info.");
                     return mesh;
                 };
                 const std::shared_ptr<const Mesh> source = load(a["source"].asString()), target = load(a["target"].asString());
                 return GuessRetargetMap(*source, *target);
             });
    Register(r, "animation.bones", "Nodes (bones) of an entity's model in its current pose - Animator clip, BoneIK goals, SpringBone swing - with world position/rotation and modelPosition (the Transform.position a child entity needs to stand on the bone), and how its BoneIK / BoneAttachment / SpringBone children resolve. Use it for bone names and to check IK and springs.",
             Params().ReqWith("id", EntityRefSchema("Entity with a MeshRenderer model.")).Opt("bone", "string", "Only this node (exact name).")
                 .Opt("ik", "boolean", "false = the clip's pose before BoneIK goals and SpringBone swing: where the animation alone puts a bone (foot placement adds the ground height to it). Default true."),
             false, [](Engine& e, const Json& a) {
                 const EntityId id = RequireEntity(e, a);
                 const Scene& scene = e.GetScene();
                 const MeshRenderer* renderer = scene.Get<MeshRenderer>(id);
                 if (!renderer) throw ApiError("missing_component", "entity has no MeshRenderer", "Add MeshRenderer with a glTF or FBX model first.");
                 std::string error;
                 std::shared_ptr<const Mesh> mesh = e.Assets().GetMesh(renderer->mesh, &error);
                 if (!mesh) throw ApiError("asset_error", error, "Check MeshRenderer.mesh with asset.info.");
                 const std::string only = a["bone"].asString("");
                 if (!only.empty() && FindModelNode(*mesh, only) < 0)
                     throw ApiError("bone_not_found", "model has no node '" + only + "'", "Call animation.bones without bone to list the names.");
                 const std::vector<Mat4> nodes = EvaluateEntityPose(scene, id, *mesh, a["ik"].asBool(true));
                 const Mat4 world = scene.WorldMatrix(id);
                 auto vec3 = [](const Vec3& v) { return Json(Json::Array{v.x, v.y, v.z}); };
                 auto worldPosition = [&](int node) { return (world * nodes[static_cast<size_t>(node)]).TransformPoint(Vec3(0, 0, 0)); };
                 // What each bone moves: how many vertices it is the strongest influence of, and where they are
                 // in the default pose (model space), so a skirt or hair bone can be told from a body bone.
                 std::vector<int> skinned(mesh->nodes.size(), 0);
                 std::vector<Vec3> lowest(mesh->nodes.size()), highest(mesh->nodes.size());
                 // Only for the whole list: a script asking for one bone every frame should not pay for it.
                 for (size_t v = 0; only.empty() && v < mesh->skin.size() && v < mesh->positions.size(); ++v) {
                     size_t strongest = 0;
                     for (size_t k = 1; k < 4; ++k) if (mesh->skin[v].weights[k] > mesh->skin[v].weights[strongest]) strongest = k;
                     if (!(mesh->skin[v].weights[strongest] > 0) || mesh->skin[v].joints[strongest] >= mesh->joints.size()) continue;
                     const size_t owner = static_cast<size_t>(mesh->joints[mesh->skin[v].joints[strongest]].node);
                     const Vec3& p = mesh->positions[v];
                     if (skinned[owner]++ == 0) lowest[owner] = highest[owner] = p;
                     lowest[owner] = Vec3(std::min(lowest[owner].x, p.x), std::min(lowest[owner].y, p.y), std::min(lowest[owner].z, p.z));
                     highest[owner] = Vec3(std::max(highest[owner].x, p.x), std::max(highest[owner].y, p.y), std::max(highest[owner].z, p.z));
                 }
                 Json out = Json::MakeObject();
                 out["bones"] = Json::MakeArray();
                 for (size_t i = 0; i < mesh->nodes.size(); ++i) {
                     const ModelNode& node = mesh->nodes[i];
                     if (node.name.empty() || (!only.empty() && node.name != only)) continue;
                     Vec3 position, rotation, scale;
                     DecomposeTRS(world * nodes[i], position, rotation, scale);
                     Json bone = Json::MakeObject();
                     bone["name"] = node.name;
                     bone["parent"] = node.parent >= 0 ? mesh->nodes[static_cast<size_t>(node.parent)].name : std::string();
                     bone["position"] = vec3(position);
                     bone["rotation"] = vec3(rotation);
                     // Where a child entity of the model would stand to be on the bone (BoneIK goals, Transform.position).
                     bone["modelPosition"] = vec3(nodes[i].TransformPoint(Vec3(0, 0, 0)));
                     if (only.empty()) bone["vertices"] = skinned[i];
                     if (skinned[i] > 0) bone["vertexMin"] = vec3(lowest[i]), bone["vertexMax"] = vec3(highest[i]);
                     out["bones"].push(std::move(bone));
                 }
                 out["goals"] = Json::MakeArray();
                 out["attachments"] = Json::MakeArray();
                 out["springs"] = Json::MakeArray();
                 out["colliders"] = Json::MakeArray();
                 for (EntityId child : scene.Children(id)) {
                     const EntityRecord* record = scene.Record(child);
                     if (const BoneIK* ik = scene.Get<BoneIK>(child)) {
                         const int node = FindModelNode(*mesh, ik->bone);
                         Json goal = Json::MakeObject();
                         goal["id"] = child;
                         goal["name"] = record->name;
                         goal["bone"] = ik->bone;
                         goal["found"] = node >= 0;
                         // How far the bone ends up from the goal: 0 when reached, the shortfall when out of reach.
                         if (node >= 0) goal["distance"] = Length(worldPosition(node) - scene.WorldMatrix(child).TransformPoint(Vec3(0, 0, 0)));
                         out["goals"].push(std::move(goal));
                     }
                     if (const BoneAttachment* attachment = scene.Get<BoneAttachment>(child)) {
                         Json item = Json::MakeObject();
                         item["id"] = child;
                         item["name"] = record->name;
                         item["bone"] = attachment->bone;
                         item["found"] = FindModelNode(*mesh, attachment->bone) >= 0;
                         out["attachments"].push(std::move(item));
                     }
                     if (const SpringBone* spring = scene.Get<SpringBone>(child)) {
                         const int count = SpringBoneCount(*mesh, spring->bone, spring->endLength);
                         Json item = Json::MakeObject();
                         item["id"] = child;
                         item["name"] = record->name;
                         item["bone"] = spring->bone;
                         item["found"] = count > 0 || FindModelNode(*mesh, spring->bone) >= 0;
                         item["bones"] = count;  // how many bones swing; 0 = nothing below the bone
                         item["simulating"] = count > 0 && spring->simulated == spring->bone && spring->tails.size() == static_cast<size_t>(count);
                         out["springs"].push(std::move(item));
                     }
                     if (const SpringCollider* collider = scene.Get<SpringCollider>(child)) {
                         Json item = Json::MakeObject();
                         item["id"] = child;
                         item["name"] = record->name;
                         item["bone"] = collider->bone;
                         item["tail"] = collider->tail;
                         item["found"] = FindModelNode(*mesh, collider->bone) >= 0 && (collider->tail.empty() || FindModelNode(*mesh, collider->tail) >= 0);
                         out["colliders"].push(std::move(item));
                     }
                 }
                 return out;
             });
    auto requireTrail = [](Engine& e, const Json& args) -> Trail& {
        EntityId id = RequireEntity(e, args);
        Trail* trail = e.GetScene().Get<Trail>(id);
        if (!trail) throw ApiError("missing_component", "entity has no Trail", "Add Trail with component.add first.");
        return *trail;
    };
    Register(r, "trail.state", "Path of a Trail: point count, emitting and optionally the points (world position, age), oldest first.",
             Params().ReqWith("id", EntityRefSchema("Trail entity.")).Opt("points", "boolean", "Include position and age of every point."),
             false, [requireTrail](Engine& e, const Json& a) {
                 const Trail& trail = requireTrail(e, a);
                 Json out = Json::MakeObject();
                 out["count"] = static_cast<uint64_t>(trail.points.size());
                 out["emitting"] = TrailEmitting(trail);
                 out["maxPoints"] = trail.maxPoints;
                 if (a["points"].asBool()) {
                     out["points"] = Json::MakeArray();
                     for (const TrailPoint& point : trail.points) {
                         Json p = Json::MakeObject();
                         p["position"] = Json(Json::Array{point.position.x, point.position.y, point.position.z});
                         p["age"] = point.age;
                         if (trail.mode == "ribbon") p["tip"] = Json(Json::Array{point.tip.x, point.tip.y, point.tip.z});
                         out["points"].push(p);
                     }
                 }
                 return out;
             });
    Register(r, "trail.clear", "Forget a Trail's path so no ribbon joins the old and new place (teleport, respawn); runtime only.",
             Params().ReqWith("id", EntityRefSchema("Trail entity.")), false, [requireTrail](Engine& e, const Json& a) {
                 ClearTrail(requireTrail(e, a));
                 Json out = Json::MakeObject();
                 out["count"] = 0;
                 return out;
             });
    // ----- engine / api ----------------------------------------------------
    Register(r, "engine.info", "Engine version, platform, project, scene and simulation status.", Params(), false, [](Engine& e, const Json&) {
        Json info = Json::MakeObject();
        info["engine"] = "OwnEngine";
        info["version"] = OE_VERSION;
        info["platform"] = OE_PLATFORM_NAME;
        info["renderer"] = e.Renderer().Name();  // screenshots, hashes, picking
        info["displayRenderer"] = e.DisplayRenderer().Name();  // editor viewport and game window
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
    Register(r, "scene.epoch", "Read the opaque runtime world epoch (not a portable identity or gameplay value).", Params(), false,
             [](Engine& e, const Json&) {
                 Json out = Json::MakeObject();
                 out["epoch"] = e.GetScene().EpochString();
                 return out;
             });

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

    Register(r, "entity.copy", "Copy selected subtrees into a portable clipboard document. Remap reflected entity references; external references become null.",
             Params().Req("ids", "array", "Selected entity ids or names. Descendants included once; JSON script params are copied verbatim."),
             false, [](Engine& e, const Json& a) {
                 if (a["ids"].size() == 0 || a["ids"].size() > 10000)
                     throw ApiError("invalid_argument", "select 1..10000 entities", "Pass ids:[1,2] or entity names.");
                 std::vector<EntityId> ids;
                 for (const Json& ref : a["ids"].items()) {
                     if (ref.isNumber() && (ref.asNumber() <= 0 || ref.asNumber() > std::numeric_limits<EntityId>::max() ||
                                            std::floor(ref.asNumber()) != ref.asNumber()))
                         throw ApiError("invalid_argument", "entity ids must be positive uint32 integers", "Use entity ids or unique names.");
                     Json args = Json::MakeObject(); args["id"] = ref;
                     ids.push_back(RequireEntity(e, args));
                 }
                 Json document = CopyEntities(e.GetScene(), ids);
                 if (document["entities"].size() > 10000 || document.dump().size() > 8 * 1024 * 1024)
                     throw ApiError("clipboard_limit", "selected subtrees exceed clipboard bounds", "Copy fewer entities (10000 entities / 8 MiB).");
                 return document;
             });
    Register(r, "entity.paste", "Atomically paste a clipboard document with fresh ids and unique names. Root transforms stay local to parent; one undo step.",
             Params().Req("document", "object", "Document returned by entity.copy, up to 8 MiB/10000 entities.")
                 .OptWith("parent", EntityRefSchema("Optional destination parent; 0 or omitted means root.")),
             true, [](Engine& e, const Json& a) {
                 EntityId parent = a.has("parent") && a["parent"].asNumber(-1) != 0 ? RequireEntity(e, a, "parent") : kNullEntity;
                 std::vector<EntityId> roots;
                 std::string error;
                 if (!PasteEntities(e.GetScene(), a["document"], parent, roots, &error))
                     throw ApiError("invalid_clipboard", error, "Copy entities using entity.copy, then pass its result as document.");
                 Json result = Json::MakeObject(), list = Json::MakeArray();
                 for (EntityId id : roots) list.push(id);
                 result["roots"] = list;
                 return result;
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
                 if (t.name == "VFXSystem") ValidateVfxSystem(e, s.GetComponent(id, t), a.has("values") ? a["values"] : Json::MakeObject());
                 ValidateVfxSettings(t, s.GetComponent(id, t), a.has("values") ? a["values"] : Json::MakeObject());
                 void* c = s.AddComponent(id, t);
                 std::string err;
                 if (!ApplyComponentJson(t, c, a["values"], &err)) {
                     if (!existed) s.RemoveComponent(id, t);
                     throw ApiError("invalid_component_values", err, "Call component.types to see valid fields.");
                 }
                 return s.EntityToJson(id);
             });

    Register(r, "component.set", "Change some fields of an existing component (partial update).",
             Params().ReqWith("id", EntityRefSchema("Entity id or name.")).Req("type", "string", "Component type name.").Req("values", "object", "Field values to change.")
                 .Opt("merge", "string", "Undo group key: consecutive edits with the same key become one undo step (used for editor drags)."),
             true, [](Engine& e, const Json& a) {
                 EntityId id = RequireEntity(e, a);
                 const ComponentType& t = RequireType(a["type"]);
                 Scene& s = e.GetScene();
                 void* c = s.GetComponent(id, t);
                 if (!c) throw ApiError("component_missing", "entity " + std::to_string(id) + " has no " + t.name + " component", "Use component.add to add it first.");
                 if (t.name == "VFXSystem") ValidateVfxSystem(e, c, a["values"]);
                 ValidateVfxSettings(t, c, a["values"]);
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

    Json axisNameSchema = Json::MakeObject();
    axisNameSchema["type"] = "string";
    axisNameSchema["enum"] = Json::MakeArray();
    for (const char* name : InputState::kAxisNames) axisNameSchema["enum"].push(name);
    axisNameSchema["description"] = "Logical gamepad axis; positive X right, positive Y up, LT/RT nonnegative.";
    Register(r, "input.axis", "Inject a raw gamepad axis. Reads use a 0.15 scalar dead zone, rescaled to full range.",
             Params().ReqWith("name", axisNameSchema).Req("value", "number", "Raw stick [-1,1] or trigger [0,1] value."), false,
             [](Engine& e, const Json& a) {
                 std::string name = a["name"].asString();
                 double value = a["value"].asNumber();
                 double minimum = name == "LT" || name == "RT" ? 0 : -1;
                 if (!std::isfinite(value) || value < minimum || value > 1)
                     throw ApiError("invalid_argument", "axis value is outside its finite range",
                                    "Use -1..1 for sticks and 0..1 for LT/RT; input.axis reads apply the 0.15 dead zone.");
                 if (!e.Input().SetAxis(name, static_cast<float>(value)))
                     throw ApiError("invalid_argument", "unknown gamepad axis", "Use LeftX, LeftY, RightX, RightY, LT or RT.");
                 return SimState(e);
             });

    Register(r, "input.key", "Press or release a key (\"W\", \"A\", \"Space\", \"Left\", ...) or logical Gamepad button.",
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

    Register(r, "input.text", "Type characters, as a keyboard would: the focused UIInput gets them on the next simulated frame "
             "(focus it with input.click or ui.focus; Backspace and Enter are keys: input.key). `composing` is what an input method "
             "(Korean, Japanese, Chinese) shows before a character is typed: the field draws it underlined at the caret.",
             Params()
                 .Opt("text", "string", "Characters to type (UTF-8).")
                 .Opt("composing", "string", "Text being composed, replacing the previous one (\"\" = none). Omit to leave it."),
             false, [](Engine& e, const Json& a) {
                 for (uint32_t c : DecodeUtf8(a["text"].asString(""))) e.Input().AddText(c);
                 if (a.has("composing")) e.Input().composing = a["composing"].asString("");
                 return SimState(e);
             });

    Register(r, "ui.clipboard", "Clipboard used by UIInput copy, cut and paste (Ctrl+C / Ctrl+X / Ctrl+V): read it, or set it before pasting. "
             "Tool sessions keep their own; a game window uses the system clipboard.",
             Params().Opt("text", "string", "New clipboard text. Omit to read."), false, [](Engine& e, const Json& a) {
                 if (a.has("text")) e.SetClipboardText(a["text"].asString());
                 Json out = Json::MakeObject();
                 out["text"] = e.ClipboardText();
                 return out;
             });

    Register(r, "input.click", "Click the game view at a pixel (as seen in a screenshot of the given size). Triggers UIButton onClick (or sets a UISlider) on the next simulated frame.",
             Params()
                 .Req("x", "number", "Pixel x (0 = left).")
                 .Req("y", "number", "Pixel y (0 = top).")
                 .Opt("width", "integer", "Width of the image the coordinates refer to (default 640, like render.screenshot).")
                 .Opt("height", "integer", "Height of that image (default 360)."),
             false, [](Engine& e, const Json& a) {
                 InputState& in = e.Input();
                 int w = std::max(1, a["width"].asInt(640)), h = std::max(1, a["height"].asInt(360));
                 in.mouseX = a["x"].asFloat() / static_cast<float>(w);
                 in.mouseY = a["y"].asFloat() / static_cast<float>(h);
                 in.viewWidth = w;
                 in.viewHeight = h;
                 in.pressedThisFrame.insert("MouseLeft");
                 Json out = SimState(e);
                 EntityId hit = HitTestButton(e.GetScene(), a["x"].asFloat(), a["y"].asFloat(), w, h, &e.Assets());
                 out["button"] = hit;
                 if (hit != kNullEntity) out["buttonName"] = e.GetScene().Record(hit)->name;
                 return out;
             });

    for (const std::string operation : {"camera.worldToScreen", "camera.screenRay", "camera.screenToPlane", "ui.screenToCanvas"}) {
        Params params;
        if (operation != "ui.screenToCanvas") params.Req("camera", "entity", "Explicit Camera entity id or unique name.");
        else params.Opt("canvas", "entity", "Single effective UICanvas id/name; omitted uses first canvas or fallback.");
        if (operation == "camera.worldToScreen") params.Req("position", "object", "Finite world point {x,y,z}.");
        else params.Req("screen", "object", "Logical view pixel point {x,y}, top-left origin; not clamped.");
        if (operation == "camera.screenToPlane") params.Req("plane", "object", "World plane {point:{x,y,z},normal:{x,y,z}}; nonzero normal.");
        params.Opt("view", "object", "{width,height} integers 1..16384; omitted uses local input.viewSize state.");
        Register(r, operation.c_str(), "Read-only presentation coordinate query; local view dependent. See SCRIPTING.md for result schema and simulation restrictions.",
                 params, false, [operation](Engine& e, const Json& a) { return QueryCoordinates(e, operation, a); });
    }

    Register(r, "ui.measureText", "Read-only text layout in reference pixels at effective view scale; actual renderer shaping, excluding paint effects and fitting.",
             Params().Req("text", "string", "UTF-8 string, at most 65536 bytes.")
                 .Opt("style", "object", "font, size, letterSpacing, lineSpacing, bold, rich; defaults match UIText.")
                 .Opt("wrapWidth", "number", "Reference pixels; omitted/zero = no wrap; maximum 65536.")
                 .Opt("view", "object", "Logical {width,height}; omitted uses local input.viewSize().")
                 .Opt("canvas", "entity", "Single effective UICanvas; omitted uses first canvas/fallback."),
             false, [](Engine& e, const Json& a) { return QueryCoordinates(e, "ui.measureText", a); });

    Register(r, "ui.layout", "Where every visible UI element is on a screen of the given size (pixels, top-left origin), in draw order. Use the centers with input.click.",
             Params()
                 .Opt("width", "integer", "Screen width (default 640, like render.screenshot).")
                 .Opt("height", "integer", "Screen height (default 360).")
                 .Opt("interactable", "boolean", "Only buttons and sliders that accept clicks."),
             false, [](Engine& e, const Json& a) {
                 int w = std::max(1, a["width"].asInt(640)), h = std::max(1, a["height"].asInt(360));
                 bool onlyInteractable = a["interactable"].asBool(false);
                 auto r1 = [](float v) { return std::round(static_cast<double>(v) * 10.0) / 10.0; };
                 static const char* kKinds[] = {"UIText", "UIPanel", "UIButton", "UIImage", "UISlider", "UIInput", "UIDropdown", "UIJoystick"};
                 Json list = Json::MakeArray();
                 for (const UIRect& rc : LayoutUI(e.GetScene(), w, h, &e.Assets())) {
                     if (onlyInteractable && !rc.interactable) continue;
                     Json j = Json::MakeObject();
                     j["id"] = rc.entity;
                     j["name"] = e.GetScene().Record(rc.entity)->name;
                     j["type"] = kKinds[static_cast<int>(rc.kind)];
                     j["rect"] = Json(Json::Array{r1(rc.x), r1(rc.y), r1(rc.w), r1(rc.h)});
                     j["center"] = Json(Json::Array{r1(rc.x + rc.w * 0.5f), r1(rc.y + rc.h * 0.5f)});
                     if (rc.parent != kNullEntity) j["parent"] = rc.parent;
                     if (rc.opacity < 1.0f) j["opacity"] = r1(rc.opacity * 100.0f) / 100.0;
                     if (rc.interactable) j["interactable"] = true;
                     if (rc.kind == UIRect::Kind::Button) {
                         const UIButton& b = *e.GetScene().Get<UIButton>(rc.entity);
                         if (b.toggle) j["on"] = b.on;
                     }
                     if (rc.kind == UIRect::Kind::Dropdown) {
                         if (rc.item >= 0) j["item"] = rc.item;  // a row of the open list
                         else if (e.GetScene().Get<UIDropdown>(rc.entity)->open) j["open"] = true;
                     }
                     if (rc.interactable && rc.item < 0 && rc.entity == e.UIFocus()) j["focused"] = true;
                     if (rc.clipped) j["clip"] = Json(Json::Array{r1(rc.clip[0]), r1(rc.clip[1]), r1(rc.clip[2]), r1(rc.clip[3])});
                     list.push(j);
                 }
                 Json out = Json::MakeObject();
                 out["width"] = w;
                 out["height"] = h;
                 out["scale"] = std::round(static_cast<double>(UIScale(e.GetScene(), w, h)) * 1e5) / 1e5;
                 out["elements"] = list;
                 return out;
             });

    Register(r, "ui.focus", "Keyboard/gamepad focus of the UI (UICanvas.navigation): give it to a button or slider, clear it, or read it. "
             "Arrows / D-pad then move it (input.key), Enter / GamepadA clicks the focused button.",
             Params().OptWith("id", EntityRefSchema("Button, slider, text field or drop-down to focus (id or name); 0 clears the focus. Omit to read.")), false, [](Engine& e, const Json& a) {
                 if (a.has("id")) {
                     const bool clear = a["id"].isNumber() && a["id"].asInt() == 0;
                     const EntityId id = clear ? kNullEntity : RequireEntity(e, a);
                     const Scene& scene = e.GetScene();
                     if (id != kNullEntity && !scene.Get<UIButton>(id) && !scene.Get<UISlider>(id) && !scene.Get<UIInput>(id) && !scene.Get<UIDropdown>(id))
                         throw ApiError("invalid_argument", "only a UIButton, UISlider, UIInput or UIDropdown can have the focus",
                                        "ui.layout {interactable: true} lists them.");
                     e.SetUIFocus(id);
                 }
                 Json out = Json::MakeObject();
                 out["focus"] = e.UIFocus();
                 if (e.UIFocus() != kNullEntity && e.GetScene().Exists(e.UIFocus())) out["name"] = e.GetScene().Record(e.UIFocus())->name;
                 return out;
             });

    Register(r, "input.touch", "Put a finger on the game view, move it or lift it (multi-touch). Scripts read fingers with input.touches(); UIButtons with a `key` hold that key while touched. Unlike a real device, this does not move the mouse.",
             Params()
                 .Opt("id", "integer", "Finger id (default 0); use different ids for several fingers.")
                 .Opt("x", "number", "Pixel x (required to put a finger down).")
                 .Opt("y", "number", "Pixel y.")
                 .Opt("down", "boolean", "true = touch / move (default), false = lift the finger.")
                 .Opt("width", "integer", "Width of the image the coordinates refer to (default 640).")
                 .Opt("height", "integer", "Height of that image (default 360)."),
             false, [](Engine& e, const Json& a) {
                 InputState& in = e.Input();
                 const int id = a["id"].asInt(0);
                 auto it = std::find_if(in.touches.begin(), in.touches.end(), [&](const InputState::Touch& t) { return t.id == id; });
                 if (!a["down"].asBool(true)) {
                     if (it != in.touches.end()) in.touches.erase(it);
                 } else {
                     if (!a.has("x") || !a.has("y")) {
                         if (it == in.touches.end()) throw ApiError("missing_argument", "x and y are needed to put a finger down", "Pass {id, x, y} in screenshot pixels.");
                     }
                     int w = std::max(1, a["width"].asInt(640)), h = std::max(1, a["height"].asInt(360));
                     if (it == in.touches.end()) {
                         InputState::Touch t;
                         t.id = id;
                         t.began = true;
                         in.touches.push_back(t);
                         it = in.touches.end() - 1;
                     }
                     it->x = a["x"].asFloat(it->x * static_cast<float>(w)) / static_cast<float>(w);
                     it->y = a["y"].asFloat(it->y * static_cast<float>(h)) / static_cast<float>(h);
                     in.viewWidth = w;
                     in.viewHeight = h;
                 }
                 Json out = SimState(e);
                 Json list = Json::MakeArray();
                 for (const InputState::Touch& t : in.touches) list.push(t.id);
                 out["touches"] = list;
                 return out;
             });

    Register(r, "input.mouse", "Move the mouse over the game view, add relative motion (mouse look) and optionally press/release a button.",
             Params()
                 .Opt("x", "number", "Pixel x (keeps the current position when omitted).")
                 .Opt("y", "number", "Pixel y.")
                 .Opt("dx", "number", "Relative motion in pixels, read by scripts with input.mouseDelta() on the next step (mouse look).")
                 .Opt("dy", "number", "Relative vertical motion in pixels (positive = down).")
                 .Opt("wheel", "number", "Mouse wheel notches for the next step (positive = away from the user; scrolls UIScroll panels, input.wheel()).")
                 .Opt("locked", "boolean", "Set the mouse lock state (what input.lockMouse() does; the editor clears it when the player presses Escape).")
                 .Opt("width", "integer", "Width of the image the coordinates refer to (default 640).")
                 .Opt("height", "integer", "Height of that image (default 360).")
                 .Opt("button", "string", "MouseLeft or MouseRight.")
                 .Opt("down", "boolean", "Press (true) or release (false) the button."),
             false, [](Engine& e, const Json& a) {
                 InputState& in = e.Input();
                 int w = std::max(1, a["width"].asInt(640)), h = std::max(1, a["height"].asInt(360));
                 if (a.has("x") || a.has("y")) {
                     in.mouseX = a["x"].asFloat(in.mouseX * static_cast<float>(w)) / static_cast<float>(w);
                     in.mouseY = a["y"].asFloat(in.mouseY * static_cast<float>(h)) / static_cast<float>(h);
                     in.viewWidth = w;
                     in.viewHeight = h;
                 }
                 in.mouseDX += a["dx"].asFloat(0.0f);
                 in.mouseDY += a["dy"].asFloat(0.0f);
                 in.wheel += a["wheel"].asFloat(0.0f);
                 if (a.has("locked")) in.mouseLocked = a["locked"].asBool();
                 if (a.has("button")) {
                     std::string b = a["button"].asString();
                     if (a["down"].asBool(true)) {
                         if (!in.IsDown(b)) in.pressedThisFrame.insert(b);
                         in.down.insert(b);
                     } else {
                         in.down.erase(b);
                     }
                 }
                 Json out = SimState(e);
                 if (a.has("button") && a["down"].asBool(true) && in.viewWidth > 0 && in.viewHeight > 0 && !in.mouseLocked) {
                     EntityId hit = HitTestButton(e.GetScene(), in.mouseX * static_cast<float>(in.viewWidth), in.mouseY * static_cast<float>(in.viewHeight),
                                                  in.viewWidth, in.viewHeight, &e.Assets());
                     if (hit != kNullEntity && e.GetScene().Get<UIButton>(hit)) {
                         out["button"] = hit;
                         out["buttonName"] = e.GetScene().Record(hit)->name;
                     }
                 }
                 return out;
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
        Json rendererSchema = Json::MakeObject();
        rendererSchema["type"] = "string";
        rendererSchema["enum"] = Json::MakeArray();
        rendererSchema["enum"].push("software");
        rendererSchema["enum"].push("gpu");
        rendererSchema["description"] = "software (default: deterministic, the hash is stable across machines) or gpu (what the game window "
                                        "and editor show: MSAA, filtered shadows, mipmaps; the hash depends on the GPU and driver).";
        p.OptWith("renderer", rendererSchema);
        p.Opt("animationPreview", "boolean", "Render the isolated animation preview instead of the authored scene (default false).");
        Register(r, "render.screenshot", "Render a frame and return it as PNG (and/or save it). Look at it to verify visual changes.", p, false,
                 [](Engine& e, const Json& a) {
                     Scene& scene = a["animationPreview"].asBool() ? e.AnimationRenderScene() : e.GetScene();
                     ViewRequest vr = ParseView(e, a, &scene);
                     RenderTarget target;
                     target.Resize(vr.width, vr.height);
                     IRenderer* renderer = &e.Renderer();
                     std::string which = a["renderer"].asString("software");
                     if (which == "gpu") {
                         std::string err;
                         if (!e.EnableGpu(nullptr, &err)) {
                             throw ApiError("gpu_unavailable", "no GPU renderer: " + err, "Use the default software renderer (omit `renderer`).");
                         }
                         renderer = e.Gpu();
                     } else if (which != "software") {
                         throw ApiError("invalid_argument", "renderer must be software or gpu", "Omit it for the software renderer.");
                     }
                     RenderStats stats = renderer->Render(scene, vr.view, target);
                     Image img = target.ToImage();
                     Json out = Json::MakeObject();
                     out["width"] = vr.width;
                     out["height"] = vr.height;
                     out["hash"] = Format("%016llx", static_cast<unsigned long long>(target.Hash()));
                     out["renderer"] = renderer->Name();
                     out["camera"] = vr.customCamera ? "custom" : (vr.view.cameraEntity ? "scene" : "default (scene has no active camera)");
                     out["stats"]["entities"] = stats.drawnEntities;
                     out["stats"]["triangles"] = stats.triangles;
                     out["stats"]["ms"] = stats.milliseconds;
                     out["stats"]["drawCalls"] = stats.drawCalls;
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
        Register(r, "render.pick",
                 "Return the entity visible at a pixel of a rendered view, and where its surface is: point (world position), "
                 "normal (facing the camera) and distance from the camera. point is absent where nothing is drawn.",
                 p, false, [](Engine& e, const Json& a) {
            ViewRequest vr = ParseView(e, a);
            RenderTarget target;
            target.Resize(vr.width, vr.height);
            e.Renderer().Render(e.GetScene(), vr.view, target);
            const int x = a["x"].asInt(), y = a["y"].asInt();
            EntityId id = target.IdAt(x, y);
            Json out = Json::MakeObject();
            out["id"] = id;
            if (id != kNullEntity) out["name"] = e.GetScene().Record(id)->name;
            // The depth buffer holds NDC z mapped to 0..1 (1 = cleared): unproject the pixel center.
            const Mat4 inverse = (vr.view.proj * vr.view.view).Inverse();
            auto surface = [&](int px, int py, Vec3& world) {
                if (px < 0 || py < 0 || px >= target.width || py >= target.height) return false;
                const float depth = target.depth[static_cast<size_t>(py) * static_cast<size_t>(target.width) + static_cast<size_t>(px)];
                if (depth >= 1.0f) return false;
                const Vec4 ndc((static_cast<float>(px) + 0.5f) / static_cast<float>(target.width) * 2.0f - 1.0f,
                               1.0f - (static_cast<float>(py) + 0.5f) / static_cast<float>(target.height) * 2.0f, depth * 2.0f - 1.0f, 1.0f);
                const Vec4 h = inverse * ndc;
                if (std::fabs(h.w) < 1e-12f) return false;
                world = Vec3(h.x / h.w, h.y / h.w, h.z / h.w);
                return true;
            };
            Vec3 point;
            if (surface(x, y, point)) {
                auto round = [](const Vec3& v) {  // millimetres, as doubles so the JSON reads 0.009, not 0.00900000054
                    Json json = Json::MakeArray();
                    for (float c : {v.x, v.y, v.z}) json.push(Json(std::round(static_cast<double>(c) * 1000.0) / 1000.0 + 0.0));
                    return json;
                };
                out["point"] = round(point);
                out["distance"] = std::round(static_cast<double>(Length(point - vr.view.eye)) * 1000.0) / 1000.0;
                // Normal from the neighbouring pixels of the same surface, turned toward the camera.
                Vec3 right, down;
                const bool hasRight = surface(x + 1, y, right) && target.IdAt(x + 1, y) == id;
                const bool hasDown = surface(x, y + 1, down) && target.IdAt(x, y + 1) == id;
                Vec3 left, up;
                const bool hasLeft = !hasRight && surface(x - 1, y, left) && target.IdAt(x - 1, y) == id;
                const bool hasUp = !hasDown && surface(x, y - 1, up) && target.IdAt(x, y - 1) == id;
                if ((hasRight || hasLeft) && (hasDown || hasUp)) {
                    const Vec3 du = hasRight ? right - point : point - left;
                    const Vec3 dv = hasDown ? down - point : point - up;
                    Vec3 normal = Cross(du, dv);
                    if (Length(normal) > 1e-9f) {
                        normal = Normalize(normal);
                        if (Dot(normal, vr.view.eye - point) < 0) normal = normal * -1.0f;
                        out["normal"] = round(normal);
                    }
                }
            }
            return out;
        });
    }

    Register(r, "render.meshes", "Values usable in MeshRenderer.mesh: built-in shapes and the project's model files.", Params(), false, [](Engine& e, const Json&) {
        Json list = Json::MakeArray();
        for (const std::string& n : BuiltinMeshNames()) list.push(n);
        for (const std::string& f : ListFiles(e.ProjectDir(), "", true)) {
            std::string rel = RelativePath(f, e.ProjectDir());
            if (rel.rfind("build/", 0) != 0 && AssetManager::KindOf(rel) == "model") list.push(rel);
        }
        return list;
    });

    // ----- scripting -------------------------------------------------------------
    Register(r, "script.eval", "Run Lua code in the game's script state and return the result (expressions are returned directly).",
             Params()
                 .Req("code", "string", "Lua source, e.g. \"scene.find('Player')\" or a block with return.")
                 .OptWith("entity", EntityRefSchema("Bind `self` to this entity's script instance (or a plain entity handle).")),
             true, [](Engine& e, const Json& a) {
                 EntityId id = a.has("entity") ? RequireEntity(e, a, "entity") : kNullEntity;
                 return e.Scripts().Eval(a["code"].asString(), id);
             });

    Register(r, "script.reload", "Reload project dependencies and attached modules transactionally; keep instance state and official exports.", Params(), false,
             [](Engine& e, const Json&) {
                 e.Scripts().ReloadAll();
                 return e.Scripts().ReloadReport();
             });

    Register(r, "script.errors", "Script errors (load, runtime, eval) with file:line, entity and frame.",
             Params().Opt("clear", "boolean", "Clear the list after returning it."), false, [](Engine& e, const Json& a) {
                 Json list = Json::MakeArray();
                 for (const ScriptError& err : e.Scripts().Errors()) {
                     Json j = Json::MakeObject();
                     j["script"] = err.script;
                     j["entity"] = err.entity;
                     j["message"] = err.message;
                     j["frame"] = static_cast<uint64_t>(err.frame);
                     list.push(j);
                 }
                 if (a["clear"].asBool(false)) e.Scripts().ClearErrors();
                 return list;
             });

    Register(r, "script.list", "Project and embedded Lua sources, users, runtime state, and reserved namespace conflicts.", Params(), false,
             [](Engine& e, const Json&) {
                 Json files = Json::MakeArray(), conflicts = Json::MakeArray();
                 for (const std::string& f : ListFiles(e.ProjectDir(), ".lua", true)) {
                     std::string rel = RelativePath(f, e.ProjectDir());
                     if (rel.rfind("build/", 0) == 0) continue;
                     if (IsOfficialRoot(rel)) {
                         Json conflict = Json::MakeObject();
                         conflict["path"] = rel;
                         conflict["hint"] = "Reserved oe/ namespace; move project helpers under scripts/.";
                         conflicts.push(conflict);
                         continue;
                     }
                     Json j = Json::MakeObject();
                     j["path"] = rel;
                     Json users = Json::MakeArray();
                     for (const auto& kv : e.GetScene().Pool<Script>()) {
                         if (kv.second.path == rel) users.push(kv.first);
                     }
                     j["entities"] = users;
                     files.push(j);
                 }
                 for (const auto& module : OfficialManifest()) {
                     Json j = Json::MakeObject();
                     j["path"] = module.path; j["origin"] = "embedded"; j["readOnly"] = true;
                     j["entities"] = Json::MakeArray(); files.push(j);
                 }
                 Json out = e.Scripts().Status();
                 out["files"] = files;
                 out["reservedConflicts"] = conflicts;
                 return out;
             });

    Register(r, "script.read", "Read project or embedded Lua source without executing it.",
             Params().Req("path", "string", "Project path or canonical virtual path, e.g. @oe/ui.lua."), false,
             [](Engine& e, const Json& a) {
                 ScriptSource source = ResolveScriptSource(e, a["path"].asString());
                 Json out = Json::MakeObject();
                 out["path"] = source.path;
                 out["source"] = source.source;
                 if (source.embedded) { out["origin"] = "embedded"; out["readOnly"] = true; }
                 return out;
             });

    // Source for script.check / script.params: `source` text, else the file at `path`.
    auto scriptSource = [](Engine& e, const Json& a, std::string& chunk) {
        chunk = a["path"].asString("source");
        if (a.has("source")) {
            if (a.has("path")) {
                if (!chunk.empty() && chunk[0] == '@') ResolveOfficialSource(chunk);
                else ResolveProjectScriptPath(e, chunk);
            }
            return a["source"].asString("");
        }
        if (!a.has("path")) throw ApiError("missing_argument", "pass path or source", "e.g. {\"path\": \"scripts/player.lua\"}");
        ScriptSource source = ResolveScriptSource(e, chunk);
        chunk = source.path;
        return source.source;
    };

    Register(r, "script.check", "Check a Lua script without running it: syntax errors and suspicious globals (missing local, typos), with line numbers.",
             Params()
                 .Opt("path", "string", "Project or canonical virtual source path, e.g. @oe/ui.lua (also names the diagnostic chunk).")
                 .Opt("source", "string", "Lua source to check instead of the file (e.g. unsaved edits)."),
             false, [scriptSource](Engine& e, const Json& a) {
                 std::string chunk;
                 std::string source = scriptSource(e, a, chunk);
                 Json list = Json::MakeArray();
                 int errors = 0, warnings = 0;
                 for (const LuaDiagnostic& d : CheckLuaSource(source, chunk, ScriptHost::SandboxGlobals(e))) {
                     Json j = Json::MakeObject();
                     j["line"] = d.line;
                     j["severity"] = d.severity;
                     j["message"] = d.message;
                     list.push(j);
                     (d.severity == "error" ? errors : warnings) += 1;
                 }
                 Json out = Json::MakeObject();
                 out["ok"] = errors == 0;
                 out["errors"] = errors;
                 out["warnings"] = warnings;
                 out["diagnostics"] = list;
                 return out;
             });

    Register(r, "script.params", "Parameters a script reads from its Script component (self.params.x or default): name, type, default, options, description.",
             Params()
                 .Opt("path", "string", "Project or canonical virtual source path, e.g. @oe/ui.lua.")
                 .Opt("source", "string", "Lua source to analyze instead of the file."),
             false, [scriptSource](Engine& e, const Json& a) {
                 std::string chunk;
                 std::string source = scriptSource(e, a, chunk);
                 Json list = Json::MakeArray();
                 for (const ScriptParam& p : InferScriptParams(source)) {
                     Json j = Json::MakeObject();
                     j["name"] = p.name;
                     j["type"] = p.type;
                     j["default"] = p.defaultValue;
                     if (!p.options.empty()) {
                         Json opts = Json::MakeArray();
                         for (const std::string& o : p.options) opts.push(o);
                         j["options"] = opts;
                     }
                     if (!p.description.empty()) j["description"] = p.description;
                     j["line"] = p.line;
                     list.push(j);
                 }
                 return list;
             });

    Register(r, "script.write", "Create or overwrite a Lua file in the project (hot-reloaded if it is running).",
             Params().Req("path", "string", "Must end with .lua, e.g. \"scripts/enemy.lua\".").Req("source", "string", "Lua source code."), false,
             [](Engine& e, const Json& a) {
                 std::string rel = a["path"].asString();
                 if (rel.size() < 5 || rel.compare(rel.size() - 4, 4, ".lua") != 0) throw ApiError("invalid_path", "script path must end with .lua");
                 std::string path = ResolveProjectScriptPath(e, rel);
                 if (!WriteTextFile(path, a["source"].asString())) throw ApiError("write_failed", "cannot write " + rel);
                 Json out = Json::MakeObject();
                 out["path"] = rel;
                 Json reloaded = Json::MakeArray();
                 for (const std::string& p : e.Scripts().PollHotReload(rel)) reloaded.push(p);
                 out["reloaded"] = reloaded;
                 return out;
             });

    // ----- game runtime ------------------------------------------------------------
    Json saveValueSchema = Json::MakeObject();
    saveValueSchema["description"] = "Finite JSON value.";
    Register(r, "save.state", "Read a save slot (memory-only unless persistence was explicitly enabled).",
             Params().Opt("slot", "string", "Slot name; default is default."), false,
             [](Engine& e, const Json& a) { return e.Saves().State(a["slot"].asString("default")); });
    Register(r, "save.set", "Set a JSON save value. Use save.flush to persist it; independent of scene undo.",
             Params().Req("key", "string", "Nonempty key.").ReqWith("value", saveValueSchema)
                     .Opt("slot", "string", "Slot name; default is default."), false,
             [](Engine& e, const Json& a) {
                 std::string slot = a["slot"].asString("default");
                 e.Saves().Set(a["key"].asString(), a["value"], slot);
                 e.Touch();
                 return e.Saves().State(slot);
             });
    Register(r, "save.clear", "Delete a save key, or clear the slot when key is omitted. Use save.flush to persist.",
             Params().Opt("key", "string", "Key to delete; omit for the whole slot.")
                     .Opt("slot", "string", "Slot name; default is default."), false,
             [](Engine& e, const Json& a) {
                 std::string slot = a["slot"].asString("default");
                 e.Saves().Clear(a["key"].asString(), slot);
                 e.Touch();
                 return e.Saves().State(slot);
             });
    Register(r, "save.flush", "Flush one save slot; memory mode never touches the filesystem.",
             Params().Opt("slot", "string", "Slot name; default is default."), false,
             [](Engine& e, const Json& a) {
                 std::string slot = a["slot"].asString("default");
                 e.Saves().Flush(slot);
                 return e.Saves().State(slot);
             });

    Register(r, "time.date", "Calendar date scripts see through time.date() (daily rewards). set fixes it for tests; an empty set returns to the clock.",
             Params().Opt("set", "string", "YYYY-MM-DD to pin the date, or \"\" to use the machine's clock again."), false,
             [](Engine& e, const Json& a) {
                 if (a.has("set")) {
                     const std::string date = a["set"].asString();
                     const bool shape = date.size() == 10 && date[4] == '-' && date[7] == '-';
                     bool digits = shape;
                     for (size_t i = 0; digits && i < date.size(); ++i)
                         if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) digits = false;
                     if (!date.empty() && !digits) throw ApiError("bad_date", "set must look like 2026-01-31", "Pass YYYY-MM-DD, or \"\" to clear the override.");
                     e.SetDateOverride(date);
                 }
                 Json out = Json::MakeObject();
                 out["date"] = e.Today();
                 out["overridden"] = !e.DateOverride().empty();
                 return out;
             });

    auto hostSettings = [](Engine& e, Json& out) {
        Json accept = Json::MakeArray();
        for (const std::string& name : e.HostAccept()) accept.push(name);
        out["accept"] = accept;
        out["language"] = e.UserLanguage();
    };

    Register(r, "host.state", "The host bridge (Lua host.call / host.poll): the calls the game made to its embedding app (ads, purchases), "
                              "messages waiting for host.poll(), the names tools accept and the user language.",
             Params(), false, [hostSettings](Engine& e, const Json&) {
                 Json out = Json::MakeObject();
                 Json calls = Json::MakeArray();
                 for (const Engine::HostCallRecord& c : e.HostCalls()) {
                     Json item = Json::MakeObject();
                     item["name"] = c.name;
                     item["data"] = c.data;
                     item["frame"] = static_cast<double>(c.frame);
                     item["handled"] = c.handled;
                     calls.push(item);
                 }
                 out["calls"] = calls;
                 out["pending"] = static_cast<int>(e.PendingHostMessages());
                 hostSettings(e, out);
                 return out;
             });

    Register(r, "host.configure", "Stand in for the embedding app in a tool session: which host.call names report as handled, and the user language.",
             Params()
                 .Opt("accept", "array", "Names host.call returns true for; [\"*\"] accepts every name, [] none (the default: games use their fallback).")
                 .Opt("language", "string", "ISO 639-1 code host.language() returns, e.g. \"ko\"."),
             false, [hostSettings](Engine& e, const Json& a) {
                 if (a.has("accept")) {
                     if (!a["accept"].isArray()) throw ApiError("bad_accept", "accept must be an array of names", "Pass e.g. {\"accept\":[\"ads.rewarded\"]}.");
                     std::set<std::string> names;
                     for (size_t i = 0; i < a["accept"].size(); ++i) names.insert(a["accept"][i].asString());
                     e.SetHostAccept(std::move(names));
                 }
                 if (a.has("language")) e.SetUserLanguage(a["language"].asString());
                 Json out = Json::MakeObject();
                 hostSettings(e, out);
                 return out;
             });

    Register(r, "host.reply", "Deliver a message from the embedding app to the game, as its ad SDK or store would (the game reads it with host.poll()).",
             Params()
                 .Req("name", "string", "Message name, e.g. \"ads.result\".")
                 .Opt("data", "object", "Payload the game receives as message.data."),
             false, [](Engine& e, const Json& a) {
                 const std::string name = a["name"].asString();
                 if (name.empty()) throw ApiError("bad_name", "name must not be empty", "Pass the message name the game expects, e.g. \"ads.result\".");
                 e.PushHostMessage(name, a.has("data") ? a["data"] : Json::MakeObject());
                 Json out = Json::MakeObject();
                 out["pending"] = static_cast<int>(e.PendingHostMessages());
                 return out;
             });

    Register(r, "game.state", "Runtime scene and game data (game.set values) of the current play session.", Params(), false,
             [](Engine& e, const Json&) {
                 Json out = Json::MakeObject();
                 out["inPlaySession"] = e.InPlaySession();
                 out["scene"] = e.RuntimeScene();
                 out["sceneName"] = e.GetScene().name;
                 out["data"] = e.GameData();
                 out["frame"] = static_cast<uint64_t>(e.Frame());
                 out["quitRequested"] = e.QuitRequested();
                 return out;
             });

    Register(r, "game.quit", "Ask the game window to close, like game.quit() in Lua: oe run and the packaged player end, "
             "tools and the editor only report it as game.state.quitRequested (cleared by sim.stop).",
             Params(), false, [](Engine& e, const Json&) {
                 e.RequestQuit();
                 Json out = Json::MakeObject();
                 out["quitRequested"] = true;
                 return out;
             });

    Register(r, "game.args", "Launch parameters of this process (--set name=value; Lua game.arg(name)): which server it is, where its "
             "services are. They survive sim.stop and match starts. Omit set to read them.",
             Params().Opt("set", "object", "Names (letters, digits, _ . -; 1..64) to string values of at most 4096 bytes."), false,
             [](Engine& e, const Json& a) {
                 if (a.has("set")) {
                     if (!a["set"].isObject()) throw ApiError("bad_args", "set must be an object of strings", "Pass e.g. {\"set\":{\"channel\":\"1\"}}.");
                     for (const auto& item : a["set"].members()) {
                         std::string error;
                         const std::string value = item.second.isString() ? item.second.asString() : item.second.dump();
                         if (!e.SetLaunchArg(item.first, value, &error)) throw ApiError("bad_args", error, "Use short names and string values.");
                     }
                 }
                 Json out = Json::MakeObject();
                 for (const auto& item : e.LaunchArgs()) out[item.first] = item.second;
                 return out;
             });

    // ----- Links: framed TCP to services outside the engine session ---------
    auto linkId = [](const Json& a) {
        const double id = a["id"].asNumber(0);
        return id >= 1 && id <= 4294967295.0 && std::floor(id) == id ? static_cast<uint32_t>(id) : 0u;
    };
    auto linkState = [](Engine& e, uint32_t id) {
        e.Links().Pump();
        if (e.Links().State(id).empty()) throw ApiError("link_unknown", "no such link", "Use the id from link.open; links close on sim.stop and when a match starts.");
        Json out = Json::MakeObject();
        out["id"] = id;
        out["state"] = e.Links().State(id);
        out["error"] = e.Links().Error(id);
        return out;
    };
    Json linkValueSchema = Json::MakeObject();  // any JSON type
    linkValueSchema["description"] = "JSON value encoded as binary Json (what Lua link.send does with a table).";
    Register(r, "link.open", "Open an outbound framed TCP connection (Lua link.open) to a service that is not an engine session, e.g. a login "
             "or lobby server. Wire: u32 length, u16 type, payload (little endian). Connecting is asynchronous: poll for the state.",
             Params().Opt("address", "string", "Numeric IPv4 address; default 127.0.0.1.").Req("port", "integer", "TCP port: 1..65535."), false,
             [linkState](Engine& e, const Json& a) {
                 const double port = a["port"].asNumber(0);
                 return linkState(e, e.OpenLink(a["address"].asString("127.0.0.1"), port >= 1 && port <= 65535 && std::floor(port) == port ? static_cast<uint32_t>(port) : 0));
             });
    Register(r, "link.send", "Queue one message on a link. The payload is `value` (any JSON, sent as the engine's binary Json), `text` (raw bytes "
             "of the string) or `hex`; none sends an empty payload.",
             Params().Req("id", "integer", "Link id from link.open.").Req("type", "integer", "Message type: 0..65535.")
                     .OptWith("value", linkValueSchema)
                     .Opt("text", "string", "Raw payload bytes.").Opt("hex", "string", "Raw payload bytes as hexadecimal digits."), false,
             [linkId, linkState](Engine& e, const Json& a) {
                 const double type = a["type"].asNumber(-1);
                 if (type < 0 || type > 65535 || std::floor(type) != type) throw ApiError("link_send", "type must be 0..65535", "Message types are 16-bit numbers agreed with the service.");
                 std::vector<uint8_t> bytes;
                 if (a.has("hex")) {
                     const std::string hex = a["hex"].asString();
                     auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
                     if (hex.size() % 2) throw ApiError("link_send", "hex needs an even number of digits", "Two digits per byte, e.g. \"01ff\".");
                     for (size_t i = 0; i < hex.size(); i += 2) {
                         const int high = digit(hex[i]), low = digit(hex[i + 1]);
                         if (high < 0 || low < 0) throw ApiError("link_send", "hex holds a character that is not a hexadecimal digit", "Two digits per byte, e.g. \"01ff\".");
                         bytes.push_back(static_cast<uint8_t>(high * 16 + low));
                     }
                 } else if (a.has("text")) {
                     const std::string text = a["text"].asString();
                     bytes.assign(text.begin(), text.end());
                 } else if (a.has("value") && !EncodeJson(a["value"], bytes, LinkSet::kMaxPayloadBytes)) {
                     throw ApiError("link_send", "value exceeds the binary Json bounds", "At most 65534 bytes, depth 16, finite numbers.");
                 }
                 std::string error;
                 const uint32_t id = linkId(a);
                 if (!e.Links().Send(id, static_cast<uint16_t>(type), bytes.data(), bytes.size(), &error))
                     throw ApiError("link_send", error, "Check link.state: the link must be connecting or open, the payload at most 65534 bytes.");
                 Json out = linkState(e, id);
                 out["bytes"] = static_cast<uint64_t>(bytes.size());
                 return out;
             });
    Register(r, "link.poll", "State of a link (connecting, open, closed + error) and the messages received since the last poll (Lua link.poll). "
             "Each message has type, size, hex and, when the payload is binary Json, value.",
             Params().Req("id", "integer", "Link id from link.open."), false, [linkId, linkState](Engine& e, const Json& a) {
                 const uint32_t id = linkId(a);
                 Json out = linkState(e, id);
                 Json messages = Json::MakeArray();
                 for (const LinkSet::Message& message : e.Links().Take(id)) {
                     Json item = Json::MakeObject();
                     item["type"] = message.type;
                     item["size"] = static_cast<uint64_t>(message.payload.size());
                     std::string hex;
                     for (uint8_t byte : message.payload) { hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15]; }
                     item["hex"] = hex;
                     Json value;
                     if (!message.payload.empty() && DecodeJson(message.payload.data(), message.payload.size(), value)) item["value"] = value;
                     messages.push(item);
                 }
                 out["messages"] = messages;
                 return out;
             });
    Register(r, "link.close", "Close and forget a link (Lua link.close); unsent bytes are dropped.",
             Params().Req("id", "integer", "Link id from link.open."), false, [linkId](Engine& e, const Json& a) {
                 Json out = Json::MakeObject();
                 out["closed"] = e.Links().Close(linkId(a));
                 return out;
             });
    Register(r, "link.state", "Every link of this engine: id, address, port, state, error, unread messages, unsent bytes.", Params(), false,
             [](Engine& e, const Json&) {
                 e.Links().Pump();
                 return e.Links().Status();
             });

    Register(r, "game.pause", "Gameplay pause of the play session (like game.pause in Lua): scripts and UI keep running with dt = 0, "
             "physics, systems, timers and particles wait. Omit paused to read the state.",
             Params().Opt("paused", "bool", "true pauses, false resumes."), false, [](Engine& e, const Json& a) {
                 if (a.has("paused")) e.SetGamePaused(a["paused"].asBool());
                 Json out = Json::MakeObject();
                 out["paused"] = e.GamePaused();
                 return out;
             });

    Register(r, "game.load_scene", "Switch to another scene during a play session (like game.loadScene in Lua).",
             Params().Req("path", "string", "Scene file relative to the project."), false, [](Engine& e, const Json& a) {
                 if (!e.InPlaySession()) throw ApiError("not_simulating", "no play session", "Use scene.load to edit another scene, or sim.play/sim.step first.");
                 e.ReadProjectJson(a["path"].asString());  // validate now for a clear error
                 e.RequestSceneChange(a["path"].asString());
                 Json out = Json::MakeObject();
                 out["pending"] = a["path"].asString();
                 out["hint"] = "The scene changes at the end of the next simulated frame (sim.step).";
                 return out;
             });

    // ----- audio -------------------------------------------------------------------
    {
        Json presets = Json::MakeArray();
        for (const std::string& p : SoundPresets()) presets.push(p);
        Json presetSchema = Json::MakeObject();
        presetSchema["type"] = "string";
        presetSchema["enum"] = presets;
        presetSchema["description"] = "Sound style.";
        Register(r, "audio.generate", "Synthesize a sound effect WAV into the project (for games without audio assets).",
                 Params()
                     .Req("path", "string", "Output .wav path, e.g. \"sounds/coin.wav\".")
                     .ReqWith("preset", presetSchema)
                     .Opt("seed", "integer", "0 = canonical sound; other values vary the pitch slightly."),
                 false, [](Engine& e, const Json& a) {
                     std::string rel = a["path"].asString();
                     if (rel.size() < 5 || rel.compare(rel.size() - 4, 4, ".wav") != 0) throw ApiError("invalid_path", "audio path must end with .wav");
                     std::vector<float> mono;
                     if (!GenerateSound(a["preset"].asString(), static_cast<uint32_t>(a["seed"].asInt(0)), mono)) throw ApiError("invalid_preset", "unknown preset");
                     if (!WriteWav(e.ResolvePath(rel), mono, 1, kAudioSampleRate)) throw ApiError("write_failed", "cannot write " + rel);
                     Json out = Json::MakeObject();
                     out["path"] = rel;
                     out["seconds"] = static_cast<double>(mono.size()) / kAudioSampleRate;
                     return out;
                 });
    }

    Register(r, "audio.play", "Play a WAV clip now (mixed while simulating).",
             Params().Req("path", "string", "WAV file relative to the project.").Opt("volume", "number", "0..2 (default 1).").Opt("loop", "boolean", "Repeat forever."),
             false, [](Engine& e, const Json& a) {
                 Json out = Json::MakeObject();
                 out["voice"] = e.Audio().Play(a["path"].asString(), a["volume"].asFloat(1.0f), 1.0f, a["loop"].asBool(false), kNullEntity);
                 return out;
             });

    Register(r, "audio.stop", "Stop one voice (by id) or all sounds.", Params().Opt("voice", "integer", "Voice id from audio.play; omit to stop everything."), false,
             [](Engine& e, const Json& a) {
                 Json out = Json::MakeObject();
                 if (a.has("voice")) {
                     out["stopped"] = e.Audio().Stop(a["voice"].asInt());
                 } else {
                     e.Audio().StopAll();
                     out["stopped"] = true;
                 }
                 return out;
             });

    Register(r, "audio.state", "Playing voices, sounds played this session (with frame numbers) and capture status.", Params(), false,
             [](Engine& e, const Json&) { return e.Audio().State(); });

    Register(r, "audio.capture", "Record the mixed audio of simulated frames: start, sim.step, then stop to get peak/RMS and optionally a WAV.",
             Params()
                 .Req("action", "string", "\"start\" or \"stop\".")
                 .Opt("path", "string", "On stop: also write the capture to this .wav path."),
             false, [](Engine& e, const Json& a) {
                 std::string action = a["action"].asString();
                 if (action == "start") {
                     e.Audio().StartCapture();
                     Json out = Json::MakeObject();
                     out["capturing"] = true;
                     return out;
                 }
                 if (action != "stop") throw ApiError("invalid_argument", "action must be \"start\" or \"stop\"");
                 if (!e.Audio().Capturing()) throw ApiError("not_capturing", "no capture in progress", "Call audio.capture {action:\"start\"} first.");
                 return e.Audio().StopCapture(a.has("path") ? e.ResolvePath(a["path"].asString()) : std::string());
             });

    // ----- prefabs ---------------------------------------------------------------
    Register(r, "prefab.edit", "Open a prefab source in an isolated edit scene. Preserve the original scene and history; play is disabled.",
             Params().Req("path", "string", "Project-relative .prefab.json source."), false, [](Engine& e, const Json& a) {
                 std::string error;
                 if (!e.BeginPrefabEdit(a["path"].asString(), &error))
                     throw ApiError("prefab_edit_failed", error, "Stop play and close the current prefab; use a valid prefab with one root.");
                 Json result = Json::MakeObject(); result["path"] = e.EditingPrefab(); return result;
             });
    Register(r, "prefab.save", "Atomically save the isolated prefab source. Existing instances are not changed.", Params(), false,
             [](Engine& e, const Json&) {
                 std::string error;
                 if (!e.SavePrefabEdit(&error)) throw ApiError("prefab_save_failed", error, "Call prefab.edit and keep exactly one root.");
                 Json result = Json::MakeObject(); result["path"] = e.EditingPrefab(); return result;
             });
    Register(r, "prefab.close", "Restore the scene and undo history from before prefab editing. Unsaved edits require discard:true.",
             Params().Opt("discard", "boolean", "Explicitly discard unsaved prefab edits (default false)."), false,
             [](Engine& e, const Json& a) {
                 std::string error;
                 if (!e.EndPrefabEdit(a["discard"].asBool(), &error))
                     throw ApiError("prefab_close_failed", error, "Call prefab.save first, or prefab.close {discard:true}.");
                 return e.HistoryState();
             });
    Register(r, "prefab.state", "Current isolated prefab source path and unsaved status; empty path outside prefab mode.", Params(), false,
             [](Engine& e, const Json&) {
                 Json result = Json::MakeObject(); result["path"] = e.EditingPrefab();
                 result["dirty"] = !e.EditingPrefab().empty() && e.Dirty(); return result;
             });
    Register(r, "prefab.create", "Save an entity and its children as a reusable prefab file.",
             Params().ReqWith("id", EntityRefSchema("Root entity id or name.")).Req("path", "string", "Must end with .prefab.json, e.g. \"prefabs/coin.prefab.json\"."),
             false, [](Engine& e, const Json& a) {
                 EntityId id = RequireEntity(e, a);
                 std::string rel = a["path"].asString();
                 if (rel.size() < 12 || rel.compare(rel.size() - 12, 12, ".prefab.json") != 0) throw ApiError("invalid_path", "prefab path must end with .prefab.json");
                 Json prefab = MakePrefab(e.GetScene(), id);
                 if (!WriteTextFile(e.ResolvePath(rel), prefab.dump(2) + "\n")) throw ApiError("write_failed", "cannot write " + rel);
                 Json out = Json::MakeObject();
                 out["path"] = rel;
                 out["entities"] = static_cast<uint64_t>(prefab["entities"].size());
                 return out;
             });

    Register(r, "prefab.instantiate", "Create a copy of a prefab in the scene. Returns the new root entity.",
             Params()
                 .Req("path", "string", "Prefab file, e.g. \"prefabs/coin.prefab.json\".")
                 .Opt("name", "string", "Name for the new root entity.")
                 .OptWith("parent", EntityRefSchema("Parent entity id or name."))
                 .Opt("position", "vec3", "Root position (local to the parent)."),
             true, [](Engine& e, const Json& a) {
                 EntityId parent = a.has("parent") ? RequireEntity(e, a, "parent") : kNullEntity;
                 EntityId root = e.InstantiatePrefabFile(a["path"].asString(), parent);
                 Scene& s = e.GetScene();
                 if (a.has("name")) s.Record(root)->name = a["name"].asString();
                 if (a.has("position")) s.Add<Transform>(root).position = ReadVec3(a["position"], Vec3());
                 return s.EntityToJson(root);
             });

    Register(r, "prefab.list", "Prefab files in the project and how many instances the scene has of each.", Params(), false,
             [](Engine& e, const Json&) {
                 Json list = Json::MakeArray();
                 for (const std::string& f : ListFiles(e.ProjectDir(), ".prefab.json", true)) {
                     std::string rel = RelativePath(f, e.ProjectDir());
                     if (rel.rfind("build/", 0) == 0) continue;
                     Json j = Json::MakeObject();
                     j["path"] = rel;
                     Json users = Json::MakeArray();
                     for (const auto& kv : e.GetScene().Pool<Prefab>()) {
                         if (kv.second.path == rel) users.push(kv.first);
                     }
                     j["instances"] = users;
                     list.push(j);
                 }
                 return list;
             });

    // ----- tilemaps ----------------------------------------------------------------
    auto requireTilemap = [](Engine& e, const Json& a) -> Tilemap& {
        EntityId id = RequireEntity(e, a);
        Tilemap* tm = e.GetScene().Get<Tilemap>(id);
        if (!tm) throw ApiError("component_missing", "entity " + std::to_string(id) + " has no Tilemap", "Add one with component.add {type: \"Tilemap\"}.");
        return *tm;
    };
    auto tileChar = [](const Json& a) {
        std::string c = a["char"].asString();
        if (c.size() != 1) throw ApiError("invalid_argument", "char must be exactly one ASCII character", "Use \" \" to erase.");
        return c[0];
    };
    Register(r, "tilemap.info",
             "A tilemap's size and tile rules: every character with its frame (for autotiles the fully connected one), autotile mode, variants "
             "and collision; the tileset image and grid. Use it to pick characters for tilemap.paint.",
             Params().ReqWith("id", EntityRefSchema("Tilemap entity id or name.")), false, [requireTilemap](Engine& e, const Json& a) {
                 Tilemap& tm = requireTilemap(e, a);
                 TilesetLookup lookup = e.Assets().Tilesets();
                 TileRules rules = BuildTileRules(tm, &lookup);
                 int w = 0, h = 0;
                 MapSize(tm, w, h);
                 Json out = Json::MakeObject();
                 out["width"] = w;
                 out["height"] = h;
                 out["tileSize"] = tm.tileSize;
                 out["image"] = rules.image;
                 out["columns"] = rules.columns;
                 out["rows"] = rules.rows;
                 Json tiles = Json::MakeArray();
                 for (const auto& kv : rules.tiles) {
                     const TileDef& d = kv.second;
                     Json t = Json::MakeObject();
                     t["char"] = std::string(1, kv.first);
                     int frame = d.frame;
                     if (d.autotile == AutoTile::Sides && d.autoFrames.size() == 16) frame = d.autoFrames[15];
                     else if (d.autotile == AutoTile::Blob && d.autoFrames.size() == static_cast<size_t>(kBlobTileCount)) frame = d.autoFrames[static_cast<size_t>(BlobIndex(255))];
                     else if (!d.variants.empty()) frame = d.variants[0];
                     t["frame"] = frame;
                     if (d.autotile != AutoTile::None) t["autotile"] = d.autotile == AutoTile::Sides ? "sides" : "blob";
                     if (!d.variants.empty()) t["variants"] = static_cast<uint64_t>(d.variants.size());
                     const char* col[] = {"none", "solid", "oneway", "shape"};
                     t["collision"] = col[static_cast<int>(d.collision)];
                     tiles.push(t);
                 }
                 out["tiles"] = tiles;
                 if (!rules.error.empty()) out["error"] = rules.error;
                 out["hint"] = "Paint with tilemap.paint {id, char, cells: [[col, row], ...]} or tilemap.fill; \" \" erases. Row 0 is the top row.";
                 return out;
             });
    Register(r, "tilemap.paint", "Set tiles at cells (col, row from the top-left; the map grows as needed). Autotiles pick their frames from the new neighbours.",
             Params()
                 .ReqWith("id", EntityRefSchema("Tilemap entity id or name."))
                 .Req("char", "string", "Tile character (see tilemap.info); \" \" erases.")
                 .Req("cells", "array", "Cells [[col, row], ...].")
                 .Opt("merge", "string", "Undo group key: consecutive paints with the same key become one undo step (editor brush strokes)."),
             true, [requireTilemap, tileChar](Engine& e, const Json& a) {
                 Tilemap& tm = requireTilemap(e, a);
                 char c = tileChar(a);
                 int changed = 0;
                 for (const Json& cell : a["cells"].items()) {
                     if (!cell.isArray() || cell.size() != 2) throw ApiError("invalid_argument", "cells must be [[col, row], ...]");
                     int col = cell[0].asInt(-1), row = cell[1].asInt(-1);
                     if (TileAt(tm, col, row) == c) continue;
                     if (c == ' ' && TileAt(tm, col, row) == '\0') continue;
                     if (!SetTile(tm, col, row, c)) throw ApiError("invalid_argument", "cell [" + std::to_string(col) + ", " + std::to_string(row) + "] is outside 0..4096");
                     ++changed;
                 }
                 Json out = Json::MakeObject();
                 out["changed"] = changed;
                 return out;
             });
    Register(r, "tilemap.fill", "Fill a rectangle of cells with one tile character (\" \" erases).",
             Params()
                 .ReqWith("id", EntityRefSchema("Tilemap entity id or name."))
                 .Req("char", "string", "Tile character.")
                 .Req("col", "integer", "Left column.")
                 .Req("row", "integer", "Top row.")
                 .Req("width", "integer", "Columns.")
                 .Req("height", "integer", "Rows.")
                 .Opt("merge", "string", "Undo group key."),
             true, [requireTilemap, tileChar](Engine& e, const Json& a) {
                 Tilemap& tm = requireTilemap(e, a);
                 char c = tileChar(a);
                 int col = a["col"].asInt(), row = a["row"].asInt(), w = a["width"].asInt(), h = a["height"].asInt();
                 if (w <= 0 || h <= 0 || col < 0 || row < 0 || col + w > 4096 || row + h > 4096) throw ApiError("invalid_argument", "rectangle must be inside 0..4096 with a positive size");
                 int changed = 0;
                 for (int rr = row; rr < row + h; ++rr) {
                     for (int cc = col; cc < col + w; ++cc) {
                         if (TileAt(tm, cc, rr) == c) continue;
                         SetTile(tm, cc, rr, c);
                         ++changed;
                     }
                 }
                 Json out = Json::MakeObject();
                 out["changed"] = changed;
                 return out;
             });
    Register(r, "tileset.create",
             "Create or replace a tileset file (*.tileset.json): image + grid + tile rules shared by tilemaps. Rules: frame, variants, autotile "
             "(\"sides\" = 16 frames, \"blob\" = 47 frames), connects, edges, collision (none, solid, oneway, slope-up, slope-down, half-bottom, half-top or a polygon).",
             Params()
                 .Req("path", "string", "e.g. \"tilesets/terrain.tileset.json\".")
                 .Req("image", "string", "Tileset image (PNG) path.")
                 .Req("columns", "integer", "Tiles per row in the image.")
                 .Req("rows", "integer", "Tile rows in the image.")
                 .Req("tiles", "object", "Character -> rule, e.g. {\"#\": {\"autotile\": \"blob\", \"frame\": 0, \"collision\": \"solid\"}, \"=\": {\"frame\": 47, \"collision\": \"oneway\"}}.")
                 .Opt("overwrite", "boolean", "Replace an existing file."),
             false, [](Engine& e, const Json& a) {
                 std::string path = a["path"].asString();
                 if (AssetManager::KindOf(path) != "tileset") throw ApiError("invalid_path", "tileset path must end with .tileset.json", "e.g. \"tilesets/terrain.tileset.json\"");
                 std::string full = e.ResolvePath(path);
                 if (FileExists(full) && !a["overwrite"].asBool(false)) throw ApiError("already_exists", "'" + path + "' exists", "Pass overwrite: true to replace it.");
                 Json j = Json::MakeObject();
                 j["image"] = a["image"];
                 j["columns"] = a["columns"];
                 j["rows"] = a["rows"];
                 j["tiles"] = a["tiles"];
                 Tileset check;
                 std::string err;
                 if (!ParseTileset(j, check, &err)) throw ApiError("invalid_tileset", err, "See docs/2D.md (Tilesets) for the rule format.");
                 CreateDirectories(ParentPath(full));
                 if (!WriteTextFile(full, j.dump(2) + "\n")) throw ApiError("io_error", "cannot write '" + path + "'");
                 e.Assets().Forget(path);
                 Json out = Json::MakeObject();
                 out["path"] = path;
                 out["tiles"] = static_cast<uint64_t>(check.tiles.size());
                 out["hint"] = "Use it with Tilemap {tileset: \"" + path + "\"}; the map characters are the keys of tiles.";
                 return out;
             });

    // ----- physics ---------------------------------------------------------------
    Register(r, "physics.raycast", "Cast a ray against colliders and characters; returns the first hit (triggers are ignored).",
             Params()
                 .Req("origin", "vec3", "Ray start [x,y,z].")
                 .Req("direction", "vec3", "Ray direction (normalized automatically).")
                 .Opt("maxDistance", "number", "Maximum distance in meters (default 1000).")
                 .Opt("ignore", "entity", "Entity the ray passes through, e.g. the character it starts inside of."),
             false, [](Engine& e, const Json& a) {
                 const EntityId ignore = a.has("ignore") ? RequireEntity(e, a, "ignore") : kNullEntity;
                 RaycastHit hit = e.Physics().Raycast(e.GetScene(), ReadVec3(a["origin"], Vec3()), ReadVec3(a["direction"], Vec3(0, -1, 0)),
                                                      a["maxDistance"].asFloat(1000.0f), ignore);
                 Json out = Json::MakeObject();
                 out["hit"] = hit.hit;
                 if (hit.hit) {
                     out["entity"] = hit.entity;
                     if (const EntityRecord* rec = e.GetScene().Record(hit.entity)) out["name"] = rec->name;
                     out["point"] = Json(Json::Array{hit.point.x, hit.point.y, hit.point.z});
                     out["normal"] = Json(Json::Array{hit.normal.x, hit.normal.y, hit.normal.z});
                     out["distance"] = hit.distance;
                 }
                 return out;
             });

    Register(r, "physics.overlap", "Entities whose colliders/characters intersect a sphere (triggers are ignored).",
             Params().Req("center", "vec3", "Sphere center [x,y,z].").Req("radius", "number", "Sphere radius in meters."), false,
             [](Engine& e, const Json& a) {
                 Json list = Json::MakeArray();
                 for (EntityId id : e.Physics().OverlapSphere(e.GetScene(), ReadVec3(a["center"], Vec3()), a["radius"].asFloat(1.0f))) {
                     list.push(EntitySummary(e.GetScene(), id));
                 }
                 return list;
             });

    Register(r, "physics.contacts", "Pairs touching after the last simulation step (collisions and trigger overlaps).",
             Params().OptWith("id", EntityRefSchema("Only pairs involving this entity.")), false, [](Engine& e, const Json& a) {
                 EntityId filter = a.has("id") ? RequireEntity(e, a) : kNullEntity;
                 Json list = Json::MakeArray();
                 for (const ContactPair& c : e.Physics().Contacts()) {
                     if (filter != kNullEntity && c.a != filter && c.b != filter) continue;
                     if (!e.GetScene().Exists(c.a) || !e.GetScene().Exists(c.b)) continue;  // destroyed since the last step
                     Json j = Json::MakeObject();
                     j["a"] = c.a;
                     j["b"] = c.b;
                     const EntityRecord* ra = e.GetScene().Record(c.a);
                     const EntityRecord* rb = e.GetScene().Record(c.b);
                     j["names"] = Json(Json::Array{ra ? ra->name : "?", rb ? rb->name : "?"});
                     j["kind"] = c.trigger ? "trigger" : "collision";
                     list.push(j);
                 }
                 return list;
             });

    Register(r, "physics.joints", "Joint and Joint2D components as the physics worlds hold them after the last simulation step: `active` (false = a body is missing, see physics.state warnings, or it broke), `broken`, and `force` in N (what the joint held during the last step; compare with breakForce).",
             Params().OptWith("id", EntityRefSchema("Only the joint on this entity, or joints connected to it.")), false, [](Engine& e, const Json& a) {
                 EntityId filter = a.has("id") ? RequireEntity(e, a) : kNullEntity;
                 const Scene& scene = e.GetScene();
                 std::map<std::pair<EntityId, bool>, JointInfo> held;
                 for (const JointInfo& info : e.Physics().Joints()) held[{info.id, info.is2D}] = info;
                 Json list = Json::MakeArray();
                 auto add = [&](EntityId id, bool is2D, const std::string& type, EntityId connected, bool broken) {
                     if (filter != kNullEntity && id != filter && connected != filter) return;
                     auto it = held.find({id, is2D});
                     const EntityRecord* record = scene.Record(id);
                     const EntityRecord* other = connected != kNullEntity ? scene.Record(connected) : nullptr;
                     Json j = Json::MakeObject();
                     j["id"] = id;
                     j["name"] = record ? record->name : "?";
                     j["component"] = is2D ? "Joint2D" : "Joint";
                     j["type"] = type;
                     j["connected"] = connected;
                     j["connectedName"] = connected == kNullEntity ? "(world)" : other ? other->name : "?";
                     j["active"] = it != held.end() && it->second.active;
                     j["broken"] = broken;
                     j["force"] = it != held.end() ? it->second.force : 0.0f;
                     list.push(j);
                 };
                 for (const auto& kv : scene.Pool<Joint>()) add(kv.first, false, kv.second.type, kv.second.connected, kv.second.broken);
                 for (const auto& kv : scene.Pool<Joint2D>()) add(kv.first, true, kv.second.type, kv.second.connected, kv.second.broken);
                 return list;
             });

    Register(r, "physics.state", "Physics backend, gravity, body counts and warnings (e.g. invalid shapes).", Params(), false,
             [](Engine& e, const Json&) { return e.Physics().Stats(); });

    // ----- assets ----------------------------------------------------------------
    // ----- Portable surface graphs (*.shader.json)
    auto shaderInfo = [](const ShaderGraph& graph) {
        Json result = Json::MakeObject();
        result["nodes"] = static_cast<int>(graph.instructions.size());
        result["color"] = graph.color;
        result["emissive"] = graph.emissive;
        result["normal"] = graph.normal;
        result["offset"] = graph.offset;
        if (graph.maxOffset >= 0) result["maxOffset"] = graph.maxOffset;
        result["uniforms"] = Json::MakeArray();
        for (const std::string& name : graph.uniformNames) result["uniforms"].push(name);
        return result;
    };
    Register(r, "shader.create", "Create a validated portable surface shader graph (*.shader.json).",
             Params().Req("path", "string", "Project-relative .shader.json file.")
                 .Req("graph", "object", "Ordered nodes, color output, optional emissive/normal/offset outputs and named uniform defaults.")
                 .Opt("overwrite", "boolean", "Replace an existing graph."), false,
             [shaderInfo](Engine& e, const Json& a) {
                 const std::string path = a["path"].asString(), full = e.ResolvePath(path);
                 if (AssetManager::KindOf(path) != "shader") throw ApiError("invalid_path", "shader path must end with .shader.json");
                 if (FileExists(full) && !a["overwrite"].asBool()) throw ApiError("already_exists", path + " exists", "Use overwrite: true.");
                 ShaderGraph graph;
                 std::string error;
                 if (!CompileShaderGraph(a["graph"], graph, &error))
                     throw ApiError("invalid_shader", error, "Use ordered nodes with prior-node args and a valid color output; see docs/POSTPROCESS.md.");
                 CreateDirectories(ParentPath(full));
                 if (!WriteTextFile(full, a["graph"].dump(2) + "\n")) throw ApiError("io_error", "cannot write " + path);
                 e.Assets().Forget(path);
                 Json result = shaderInfo(graph);
                 result["path"] = path;
                 return result;
             });
    Register(r, "shader.check", "Validate a surface shader graph and list its instructions/outputs/uniform names.",
             Params().Req("path", "string", "Project-relative .shader.json file."), false,
             [shaderInfo](Engine& e, const Json& a) {
                 const std::string path = a["path"].asString(), full = e.ResolvePath(path);
                 if (AssetManager::KindOf(path) != "shader") throw ApiError("invalid_path", "shader path must end with .shader.json");
                 std::string text, error;
                 if (!ReadTextFile(full, text)) throw ApiError("not_found", "cannot read " + path, "Create it with shader.create.");
                 const Json json = Json::parse(text, &error);
                 if (!error.empty()) throw ApiError("invalid_shader", error, "Fix the graph JSON syntax.");
                 ShaderGraph graph;
                 if (!CompileShaderGraph(json, graph, &error)) throw ApiError("invalid_shader", error, "Fix the named graph field.");
                 Json result = shaderInfo(graph);
                 result["path"] = path;
                 return result;
             });

    // ----- Materials (*.mat.json)
    auto writeMaterial = [](Engine& e, const std::string& path, const Json& values, bool create, bool overwrite) {
        if (AssetManager::KindOf(path) != "material") throw ApiError("invalid_path", "material path must end with .mat.json", "e.g. \"materials/gold.mat.json\"");
        std::string full = e.ResolvePath(path);
        Json j = DefaultMaterialJson();
        if (!create || (FileExists(full) && !overwrite)) {
            if (create) throw ApiError("already_exists", "'" + path + "' exists", "Use material.set to change it, or overwrite: true.");
            std::string text, err;
            if (!ReadTextFile(full, text)) throw ApiError("not_found", "no material '" + path + "'", "Create it with material.create.");
            j = Json::parse(text, &err);
            if (!err.empty() || !j.isObject()) throw ApiError("invalid_material", path + ": " + (err.empty() ? "not a JSON object" : err));
        }
        if (!values.isNull() && !values.isObject()) throw ApiError("invalid_argument", "values must be an object of material fields");
        for (const auto& kv : values.members()) j[kv.first] = kv.second;
        // An opacity below 1 only shows with blending.
        if (values.has("opacity") && !values.has("alphaMode") && values["opacity"].asFloat(1.0f) < 1.0f && j["alphaMode"].asString("opaque") == "opaque") j["alphaMode"] = "blend";
        Material check;
        std::string err;
        TextureLoader load = [&e](const std::string& p, std::string* er) { return e.Assets().GetTexture(p, er); };
        if (!MaterialFromJson(j, load, check, &err,
            [&e](const std::string& p, std::string* er) { return e.Assets().GetShader(p, er); })) {
            Json docs = MaterialFieldDocs();
            std::string names;
            for (const auto& kv : docs.members()) names += (names.empty() ? "" : ", ") + kv.first;
            throw ApiError("invalid_material", err, "Fields: " + names + ".");
        }
        CreateDirectories(ParentPath(full));
        if (!WriteTextFile(full, j.dump(2) + "\n")) throw ApiError("io_error", "cannot write '" + path + "'");
        e.Assets().Forget(path);
        Json out = Json::MakeObject();
        out["path"] = path;
        out["values"] = j;
        out["hint"] = "Use it with MeshRenderer {material: \"" + path + "\"}. Files reload automatically when edited.";
        return out;
    };
    Register(r, "material.create",
             "Create a material file (*.mat.json): PBR baseColor/opacity/metallic/roughness, base/normal/metallicRoughness/occlusion/emissive textures, "
             "alphaMode (opaque, mask, blend), doubleSided, unlit, tiling. Assign it with MeshRenderer.material.",
             Params()
                 .Req("path", "string", "e.g. \"materials/glass.mat.json\".")
                 .Opt("values", "object", "Fields to set, e.g. {\"baseColor\": [1, 0.8, 0.2], \"metallic\": 1, \"roughness\": 0.3}. Others keep defaults.")
                 .Opt("overwrite", "boolean", "Replace an existing file."),
             false, [writeMaterial](Engine& e, const Json& a) { return writeMaterial(e, a["path"].asString(), a["values"], true, a["overwrite"].asBool(false)); });
    Register(r, "material.set", "Change fields of a material file (other fields keep their values). Every mesh using it updates.",
             Params().Req("path", "string", "Material file.").Req("values", "object", "Fields to change."),
             false, [writeMaterial](Engine& e, const Json& a) { return writeMaterial(e, a["path"].asString(), a["values"], false, false); });

    Register(r, "asset.preview", "Render a static, automatically framed software thumbnail without changing the scene.",
             Params().Req("path", "string", "Texture/model/material/prefab/scene asset inside the project.")
                 .Opt("size", "integer", "Square output size 16..256, default 64.")
                 .Opt("pixels", "boolean", "Include packed RGBA8 pixels for editor consumers.")
                 .Opt("out", "string", "Optional PNG path inside the project."), false,
             [](Engine& e, const Json& a) {
                 RenderTarget target;
                 std::string error;
                 if (!RenderAssetPreview(e, a["path"].asString(), a["size"].asInt(64), target, &error))
                     throw ApiError("preview_failed", error, "Use a renderable asset and size 16..256.");
                 const auto png = EncodePng(target.ToImage());
                 if (a.has("out") && !WritePng(e.ResolvePath(a["out"].asString()), target.ToImage()))
                     throw ApiError("write_failed", "cannot write preview PNG");
                 Json result = Json::MakeObject(); result["width"] = target.width; result["height"] = target.height;
                 result["mimeType"] = "image/png"; result["data"] = Base64Encode(png);
                 if (a["pixels"].asBool()) {
                     Json pixels = Json::MakeArray(); for (uint32_t pixel : target.color) pixels.push(static_cast<uint64_t>(pixel));
                     result["pixels"] = pixels;
                 }
                 return result;
             });
    Register(r, "asset.import", "Import an external file into the project using the same rules as oe import. The explicit source path may be outside the project.",
             Params().Req("source", "string", "UTF-8 source file path; import copies to a project-owned asset path."), false,
             [](Engine& e, const Json& a) {
                 Json result = Json::MakeObject(); result["path"] = ImportAssetFile(e, a["source"].asString()); return result;
             });
    Register(r, "asset.list", "Project files by kind (model, texture, material, shader, audio, font, script, prefab, scene).",
             Params().Opt("kind", "string", "Only this kind."), false, [](Engine& e, const Json& a) {
                 std::string kind = a["kind"].asString("");
                 Json list = Json::MakeArray();
                 for (const std::string& f : ListFiles(e.ProjectDir(), "", true)) {
                     std::string rel = RelativePath(f, e.ProjectDir());
                     if (rel.rfind("build/", 0) == 0 || rel.rfind(".", 0) == 0) continue;
                     std::string k = AssetManager::KindOf(rel);
                     if (k == "other" || (!kind.empty() && k != kind)) continue;
                     std::vector<unsigned char> bytes;
                     Json j = Json::MakeObject();
                     j["path"] = rel;
                     j["kind"] = k;
                     list.push(j);
                 }
                 return list;
             });

    Register(r, "asset.info", "Details of a project file: model vertices/triangles/materials/textures/bounds, joints and clips (+ a scale hint), image size, sound length.",
             Params().Req("path", "string", "Project-relative path, or a built-in mesh name."), false,
             [](Engine& e, const Json& a) { return e.Assets().Info(a["path"].asString()); });

    Register(r, "asset.preload", "Load project files before they are needed, so their first use does not stall a frame: effects (.vfx.json, with the textures, materials and meshes of their emitters), materials, textures, models, shader graphs, fonts and sounds. With the GPU renderer, textures and meshes are uploaded and graph shaders compiled too. Like game.preload(paths) in Lua. Returns {loaded: [{path, kind}], errors: [{path, message}], gpu}.",
             Params().Req("paths", "array", "Project-relative paths, e.g. [\"effects/slash.vfx.json\", \"assets/sounds/slash.wav\"]."), false,
             [](Engine& e, const Json& a) {
                 std::vector<std::string> paths;
                 for (size_t i = 0; i < a["paths"].size(); ++i) {
                     if (!a["paths"][i].isString()) throw ApiError("invalid_argument", "paths must be an array of strings", "Example: {\"paths\": [\"effects/slash.vfx.json\"]}");
                     paths.push_back(a["paths"][i].asString());
                 }
                 return e.Preload(paths);
             });

    Register(r, "asset.reload", "Forget cached models/textures so they are reloaded from disk.", Params(), false, [](Engine& e, const Json&) {
        e.Assets().Clear();
        Json out = Json::MakeObject();
        out["cleared"] = true;
        return out;
    });

    Register(r, "asset.generate_texture", "Create a PNG texture procedurally (checker, grid, bricks, gradient, noise).",
             Params()
                 .Req("path", "string", "Output .png path, e.g. \"assets/textures/floor.png\".")
                 .Req("pattern", "string", "checker | grid | bricks | gradient | noise")
                 .Opt("size", "integer", "Width and height in pixels (default 256).")
                 .Opt("color1", "vec3", "First color [r,g,b] 0..1.")
                 .Opt("color2", "vec3", "Second color [r,g,b] 0..1.")
                 .Opt("cells", "integer", "Pattern repeats across the image (default 8)."),
             false, [](Engine& e, const Json& a) {
                 std::string rel = a["path"].asString();
                 if (rel.size() < 5 || rel.compare(rel.size() - 4, 4, ".png") != 0) throw ApiError("invalid_path", "texture path must end with .png");
                 std::string pattern = a["pattern"].asString();
                 int size = std::clamp(a["size"].asInt(256), 8, 2048);
                 int cells = std::clamp(a["cells"].asInt(8), 1, 256);
                 Vec3 c1 = ReadVec3(a["color1"], Vec3(0.85f, 0.85f, 0.85f)), c2 = ReadVec3(a["color2"], Vec3(0.35f, 0.35f, 0.4f));
                 Image img;
                 img.width = img.height = size;
                 img.rgba.resize(static_cast<size_t>(size) * size * 4);
                 uint32_t rng = 12345;
                 auto noise = [&]() { rng = rng * 1664525u + 1013904223u; return static_cast<float>(rng >> 8) / 16777216.0f; };
                 float cell = static_cast<float>(size) / static_cast<float>(cells);
                 for (int y = 0; y < size; ++y) {
                     for (int x = 0; x < size; ++x) {
                         float t = 0;
                         float fx = static_cast<float>(x) / cell, fy = static_cast<float>(y) / cell;
                         if (pattern == "checker") t = ((static_cast<int>(fx) + static_cast<int>(fy)) & 1) ? 1.0f : 0.0f;
                         else if (pattern == "grid") t = (fx - std::floor(fx) < 0.06f || fy - std::floor(fy) < 0.06f) ? 1.0f : 0.0f;
                         else if (pattern == "bricks") {
                             float by = fy * 2.0f;
                             float bx = fx + ((static_cast<int>(by) & 1) ? 0.5f : 0.0f);
                             t = (by - std::floor(by) < 0.1f || bx - std::floor(bx) < 0.05f) ? 1.0f : 0.0f;
                         } else if (pattern == "gradient") t = static_cast<float>(y) / static_cast<float>(size - 1);
                         else if (pattern == "noise") t = noise();
                         else throw ApiError("invalid_argument", "unknown pattern '" + pattern + "'", "Use checker, grid, bricks, gradient or noise.");
                         Vec3 c = c1 * (1 - t) + c2 * t;
                         uint8_t* px = &img.rgba[(static_cast<size_t>(y) * size + x) * 4];
                         px[0] = static_cast<uint8_t>(Clamp(c.x, 0, 1) * 255);
                         px[1] = static_cast<uint8_t>(Clamp(c.y, 0, 1) * 255);
                         px[2] = static_cast<uint8_t>(Clamp(c.z, 0, 1) * 255);
                         px[3] = 255;
                     }
                 }
                 std::string full = e.ResolvePath(rel);
                 CreateDirectories(ParentPath(full));
                 if (!WritePng(full, img)) throw ApiError("write_failed", "cannot write " + rel);
                 Json out = Json::MakeObject();
                 out["path"] = rel;
                 out["size"] = size;
                 return out;
             });

    // ----- navigation ------------------------------------------------------------
    Register(r, "nav.bake", "Build the navigation mesh again from the scene's static colliders/meshes (NavMesh component settings) and report it. "
                            "Queries bake by themselves; call this during play after the level changed (a door opened, a bridge fell).",
             Params(), false, [](Engine& e, const Json&) {
                 RequireNav(e, true);
                 return e.NavInfo();
             });

    Register(r, "probe.bake", "Take the picture of every ReflectionProbe again now and report them (as probe.state). Pictures are taken by "
                              "themselves when a scene loads, play starts and after edits while stopped; call this during play after the "
                              "scenery or its lighting changed (a door opened, the lights went out). Lua: game.bakeProbes().",
             Params(), false, [](Engine& e, const Json&) { return e.ProbeState(true); });

    Register(r, "probe.state", "The scene's reflection probes: {probes: [{id, name, position, min, max, resolution, captured, width, height, levels}], max}. "
                               "captured = its picture exists (it is taken when a frame first uses the probe); max = probes one frame uses "
                               "(the nearest to the camera). With id and out, writes that probe's picture (equirectangular, taken now if needed) as a PNG to look at.",
             Params().OptWith("id", EntityRefSchema("Probe whose picture to write (needs out).")).Opt("out", "string", "PNG path inside the project, e.g. \"build/probe.png\"."),
             false, [](Engine& e, const Json& a) {
                 Json out = e.ProbeState(false);
                 if (!a.has("out")) return out;
                 const EntityId id = RequireEntity(e, a);
                 const RenderLights lights = GatherRenderLights(e.GetScene(), &e.Assets());
                 for (const RenderProbe& p : lights.probes) {
                     if (p.id != id) continue;
                     const std::shared_ptr<const EnvironmentMap> map = CaptureProbe(e.GetScene(), &e.Assets(), p);
                     if (!map || map->levels.empty()) throw ApiError("capture_failed", "the probe's picture could not be taken");
                     const EnvironmentMap::Level& level = map->levels[0];
                     Image img;
                     img.width = level.width;
                     img.height = level.height;
                     img.rgba.resize(static_cast<size_t>(level.width) * static_cast<size_t>(level.height) * 4, 255);
                     for (size_t i = 0; i < static_cast<size_t>(level.width) * static_cast<size_t>(level.height); ++i)
                         for (size_t c = 0; c < 3; ++c) img.rgba[i * 4 + c] = static_cast<uint8_t>(Clamp(level.rgb[i * 3 + c], 0.0f, 1.0f) * 255.0f + 0.5f);
                     const std::string full = e.ResolvePath(a["out"].asString());
                     CreateDirectories(ParentPath(full));
                     if (!WritePng(full, img)) throw ApiError("write_failed", "cannot write " + full);
                     out = e.ProbeState(false);
                     out["path"] = full;
                     return out;
                 }
                 throw ApiError("not_a_probe", "that entity has no ReflectionProbe", "probe.state without arguments lists the probes.");
             });

    Register(r, "nav.state", "The navigation mesh as baked: {baked, polygons, area, bounds, colliders, meshes, triangles, warnings, settings}.",
             Params(), false, [](Engine& e, const Json&) {
                 const Navigation& nav = e.Nav();
                 Json out = e.NavInfo();
                 const NavSettings settings = ReadNavSettings(e.GetScene());
                 Json s = Json::MakeObject();
                 s["agentRadius"] = settings.agentRadius;
                 s["agentHeight"] = settings.agentHeight;
                 s["maxSlope"] = settings.maxSlope;
                 s["stepHeight"] = settings.stepHeight;
                 s["cellSize"] = settings.cellSize;
                 s["minRegion"] = settings.minRegion;
                 out["settings"] = std::move(s);
                 if (!nav.Baked())
                     out["hint"] = "Give the ground a static Collider (no RigidBody), or add a NavMesh component with source \"meshes\" or \"all\".";
                 return out;
             });

    Register(r, "nav.path", "Shortest walk on the navigation mesh between two points: {found, partial, points:[[x,y,z],...], length}. "
                            "partial = the goal cannot be reached and the path ends as close as it gets.",
             Params()
                 .Req("from", "vec3", "Start [x,y,z] (snapped to the mesh).")
                 .Req("to", "vec3", "Goal [x,y,z] (snapped to the mesh).")
                 .Opt("range", "number", "How far from each point the mesh is searched, meters (default 2; twice that vertically)."),
             false, [](Engine& e, const Json& a) {
                 const Navigation& nav = RequireNav(e);
                 const float range = NavRange(a);
                 const Vec3 from = ReadVec3(a["from"], Vec3()), to = ReadVec3(a["to"], Vec3());
                 const NavPath path = nav.FindPath(from, to, range);
                 Json out = Json::MakeObject();
                 out["found"] = path.found;
                 if (!path.found) {
                     out["reason"] = std::string(nav.Nearest(from, range).found ? "to" : "from") + " is not within range of the navigation mesh";
                     out["hint"] = "Raise range, or check the mesh with render.screenshot {navmesh:true}.";
                     return out;
                 }
                 out["partial"] = path.partial;
                 Json points = Json::MakeArray();
                 for (const Vec3& p : path.points) points.push(PointJson(p));
                 out["points"] = std::move(points);
                 out["length"] = path.length;
                 return out;
             });

    Register(r, "nav.nearest", "The closest point of the navigation mesh to a point: {found, point, distance}.",
             Params().Req("point", "vec3", "Point [x,y,z].").Opt("range", "number", "Search distance in meters (default 2; twice that vertically)."),
             false, [](Engine& e, const Json& a) {
                 const Vec3 point = ReadVec3(a["point"], Vec3());
                 const NavPoint nearest = RequireNav(e).Nearest(point, NavRange(a));
                 Json out = Json::MakeObject();
                 out["found"] = nearest.found;
                 if (nearest.found) {
                     out["point"] = PointJson(nearest.point);
                     out["distance"] = Length(nearest.point - point);
                 }
                 return out;
             });

    Register(r, "nav.raycast", "Walk straight from one point toward another on the navigation mesh: {onMesh, hit, point, normal, distance}. "
                               "hit = an edge of the mesh (a wall, a drop) is in the way; point is where the walk stops.",
             Params()
                 .Req("from", "vec3", "Start [x,y,z] (snapped to the mesh).")
                 .Req("to", "vec3", "End [x,y,z].")
                 .Opt("range", "number", "How far from the start the mesh is searched, meters (default 2)."),
             false, [](Engine& e, const Json& a) {
                 const NavHit hit = RequireNav(e).Raycast(ReadVec3(a["from"], Vec3()), ReadVec3(a["to"], Vec3()), NavRange(a));
                 Json out = Json::MakeObject();
                 out["onMesh"] = hit.valid;
                 out["hit"] = hit.hit;
                 if (hit.valid) {
                     out["point"] = PointJson(hit.point);
                     if (hit.hit) out["normal"] = PointJson(hit.normal);
                     out["distance"] = hit.distance;
                 }
                 return out;
             });

    // ----- debug drawing -----------------------------------------------------------
    Register(r, "debug.draw", "Draw lines/boxes/spheres in every view (visible in screenshots) to mark points, paths or areas.",
             Params()
                 .Opt("lines", "array", "[{a:[x,y,z], b:[x,y,z], color?:[r,g,b]}]")
                 .Opt("boxes", "array", "[{center:[x,y,z], size:[x,y,z], color?}]")
                 .Opt("spheres", "array", "[{center:[x,y,z], radius:number, color?}]")
                 .Opt("seconds", "number", "Lifetime in simulated seconds (default: until debug.clear)."),
             false, [](Engine& e, const Json& a) {
                 float seconds = a.has("seconds") ? a["seconds"].asFloat() : -1.0f;
                 size_t before = e.DebugLineCount();
                 auto color = [](const Json& j) {
                     Vec3 c = ReadVec3(j["color"], Vec3(1, 0.2f, 0.9f));
                     return Color(c.x, c.y, c.z);
                 };
                 for (const Json& l : a["lines"].items()) e.AddDebugLine(ReadVec3(l["a"], Vec3()), ReadVec3(l["b"], Vec3()), color(l), seconds);
                 for (const Json& b : a["boxes"].items()) {
                     Vec3 c = ReadVec3(b["center"], Vec3()), h = ReadVec3(b["size"], Vec3(1, 1, 1)) * 0.5f;
                     Vec3 p[8];
                     for (int i = 0; i < 8; ++i) p[i] = c + Vec3(i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z);
                     const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                     for (const auto& ed : edges) e.AddDebugLine(p[ed[0]], p[ed[1]], color(b), seconds);
                 }
                 for (const Json& s : a["spheres"].items()) {
                     Vec3 c = ReadVec3(s["center"], Vec3());
                     float r = s["radius"].asFloat(0.5f);
                     for (int axis = 0; axis < 3; ++axis) {
                         for (int i = 0; i < 24; ++i) {
                             float a0 = 2 * kPi * static_cast<float>(i) / 24, a1 = 2 * kPi * static_cast<float>(i + 1) / 24;
                             auto pt = [&](float ang) {
                                 float u = std::cos(ang) * r, v = std::sin(ang) * r;
                                 return axis == 0 ? c + Vec3(0, u, v) : axis == 1 ? c + Vec3(u, 0, v) : c + Vec3(u, v, 0);
                             };
                             e.AddDebugLine(pt(a0), pt(a1), color(s), seconds);
                         }
                     }
                 }
                 Json out = Json::MakeObject();
                 out["added"] = static_cast<uint64_t>(e.DebugLineCount() - before);
                 out["total"] = static_cast<uint64_t>(e.DebugLineCount());
                 return out;
             });

    Register(r, "debug.clear", "Remove all debug lines.", Params(), false, [](Engine& e, const Json&) {
        e.ClearDebugLines();
        Json out = Json::MakeObject();
        out["cleared"] = true;
        return out;
    });

    // ----- history -----------------------------------------------------------------
    // What the person in the editor is looking at, so "this", "the selected one" or "line 12" can be resolved.
    Register(r, "editor.state",
             "What the human sees in the open editor: selected entities, focused and visible panels, Scene view camera, "
             "the script tab and cursor/selection, the selected asset, open shader/effect, play state. {open:false} without an editor.",
             Params(), false, [](Engine& e, const Json& a) {
                 if (const Engine::EditorHandler& editor = e.GetEditorHandler()) return editor("state", a);
                 Json out = Json::MakeObject();
                 out["open"] = false;
                 out["hint"] = "No editor shows this engine. The human runs `oe editor <project>`; agents attach with `oe mcp --connect 7777`.";
                 return out;
             });
    Register(r, "editor.animation", "Open/focus or close the Animation panel in the attached native editor; no scene edit.",
             Params().Opt("open", "boolean", "true opens/focuses (default); false closes the panel and its preview.").Opt("frames", "boolean", "Show sprite frame authoring instead of the preview timeline.").Opt("controller", "string", "Open a .controller.json graph; empty returns to clip preview."), false,
             [](Engine& e, const Json& a) {
                 if (const Engine::EditorHandler& editor = e.GetEditorHandler()) return editor("animation", a);
                 throw ApiError("editor_unavailable", "no native editor is attached", "Run oe editor and attach with --connect; animation.preview also works without an editor.");
                 return Json();
             });
    Register(r, "editor.select",
             "Select entities in the open editor (Hierarchy, Inspector, gizmo) to point the human at them; frame:true also moves the Scene view to the first one. Not an edit (no undo entry).",
             Params().Req("ids", "array", "Entity ids or unique names; [] clears the selection.")
                 .Opt("frame", "boolean", "Move the Scene view camera to the first entity (default false)."),
             false, [](Engine& e, const Json& a) {
                 const Engine::EditorHandler& editor = e.GetEditorHandler();
                 if (!editor) throw ApiError("no_editor", "no editor shows this engine",
                                             "The human runs `oe editor <project>`; attach to it with `oe mcp --connect 7777`.");
                 Json ids = Json::MakeArray();
                 for (const Json& ref : a["ids"].items()) {
                     Json one = Json::MakeObject();
                     one["id"] = ref;
                     ids.push(Json(static_cast<uint64_t>(RequireEntity(e, one))));
                 }
                 Json args = Json::MakeObject();
                 args["ids"] = ids;
                 args["frame"] = a["frame"].asBool(false);
                 return editor("select", args);
             });
    Register(r, "history.list", "Scene edit commands and arguments in chronological order, with applied cursor. Consecutive merge groups share one entry.",
             Params(), false, [](Engine& e, const Json&) { return e.HistoryState(); });
    Register(r, "history.go", "Move to an applied history cursor by undoing/redoing edits. Retains the redo branch until a new edit.",
             Params().Req("cursor", "integer", "Applied entry count from 0 to history.list entries.size."), false,
             [](Engine& e, const Json& a) {
                 if (e.InPlaySession()) throw ApiError("simulating", "cannot change history while simulating", "Call sim.stop first.");
                 const int cursor = a["cursor"].asInt(-1);
                 if (cursor < 0 || static_cast<size_t>(cursor) > e.UndoDepth() + e.RedoDepth())
                     throw ApiError("invalid_argument", "history cursor is out of range", "Read history.list for the current entry count.");
                 while (e.UndoDepth() > static_cast<size_t>(cursor)) e.Undo();
                 while (e.UndoDepth() < static_cast<size_t>(cursor)) e.Redo();
                 return e.HistoryState();
             });
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
