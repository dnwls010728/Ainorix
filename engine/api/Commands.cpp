#include "api/Commands.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "app/Engine.h"
#include "app/Project.h"
#include "app/AssetPreview.h"
#include "assets/Assets.h"
#include "audio/AudioSystem.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Log.h"
#include "render/GpuRenderer.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/ShaderGraph.h"
#include "render/UI.h"
#include "scene/Components.h"
#include "scene/Prefab.h"
#include "scene/Clipboard.h"
#include "physics/PhysicsWorld.h"
#include "scene/TileGrid.h"
#include "script/ScriptCheck.h"
#include "script/ScriptHost.h"

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
    r.view.shaderTime = static_cast<float>(e.SimTime());
    if (args["colliders"].asBool(false)) {
        TilesetLookup tilesets = e.Assets().Tilesets();
        AppendColliderLines(e.GetScene(), r.view.lines, &tilesets);
    }
    e.AppendDebugLines(r.view.lines);
    if (args.has("ui")) r.view.drawUI = args["ui"].asBool(true);
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
        .Opt("colliders", "boolean", "Draw collider wireframes: green solid, yellow trigger, cyan character (default false).")
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
    Register(r, "net.serve", "Start a headless dedicated session; slot 1 is the server and ready remote peers start the match automatically.",
             Params().Opt("name", "string", "Server name; default Player.")
                     .Opt("port", "integer", "Listen port: 0..65535; zero chooses an available port.")
                     .Opt("seed", "integer", "Session seed; default 1.")
                     .Opt("minPlayers", "integer", "Minimum ready remote players: 1..63; default 1."), false,
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
        if (includePose) {
            out["jointMatrices"] = Json::MakeArray();
            if (mesh) for (const Mat4& matrix : EvaluateAnimationPose(*mesh, clip, animator->time)) {
                Json values = Json::MakeArray();
                for (float value : matrix.m) values.push(value);
                out["jointMatrices"].push(values);
            }
        }
        return out;
    };
    Register(r, "animation.state", "Animator playback state and optional model-space joint matrices (column-major).",
             Params().ReqWith("id", EntityRefSchema("Entity with an Animator.")).Opt("pose", "boolean", "Include evaluated jointMatrices."),
             false, [animatorState](Engine& e, const Json& a) { return animatorState(e, RequireEntity(e, a), a["pose"].asBool()); });
    Register(r, "animation.play", "Play a named model clip; add Animator if needed. Defaults to restarting at the beginning (end for reverse playback).",
             Params().ReqWith("id", EntityRefSchema("Entity with a MeshRenderer.")).Req("clip", "string", "Exact clip name from asset.info.")
                 .Opt("speed", "number", "Playback multiplier; negative = reverse. Keeps existing speed when omitted.")
                 .Opt("loop", "boolean", "Repeat the clip. Keeps existing loop setting when omitted.")
                 .Opt("restart", "boolean", "Reset time (default true); false preserves time."),
             true, [animatorState](Engine& e, const Json& a) {
                 EntityId id = RequireEntity(e, a);
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
                 if (a.has("speed")) next.speed = a["speed"].asFloat();
                 if (!std::isfinite(next.speed)) throw ApiError("invalid_argument", "speed must be finite", "Use a finite playback multiplier.");
                 if (a.has("loop")) next.loop = a["loop"].asBool();
                 if (a["restart"].asBool(true)) next.time = next.speed < 0 ? clip->duration : 0;
                 next.playing = true;
                 e.GetScene().Add<Animator>(id) = next;
                 return animatorState(e, id, false);
             });
    auto requireEmitter = [](Engine& e, const Json& args) -> ParticleEmitter& {
        EntityId id = RequireEntity(e, args);
        ParticleEmitter* emitter = e.GetScene().Get<ParticleEmitter>(id);
        if (!emitter) throw ApiError("missing_component", "entity has no ParticleEmitter", "Add ParticleEmitter with component.add first.");
        return *emitter;
    };
    Register(r, "particles.state", "Live particle count, accepted births and optional particle snapshots in their local/world coordinates.",
             Params().ReqWith("id", EntityRefSchema("ParticleEmitter entity.")).Opt("particles", "boolean", "Include age, lifetime, position, velocity and worldSpace."),
             false, [requireEmitter](Engine& e, const Json& a) {
                 const ParticleEmitter& emitter = requireEmitter(e, a);
                 Json out = Json::MakeObject();
                 out["count"] = static_cast<uint64_t>(emitter.particles.size());
                 out["emitted"] = emitter.emitted;
                 out["playing"] = emitter.playing;
                 out["loop"] = emitter.loop;
                 out["maxParticles"] = emitter.maxParticles;
                 if (a["particles"].asBool()) {
                     out["particles"] = Json::MakeArray();
                     for (const Particle& particle : emitter.particles) {
                         Json p = Json::MakeObject();
                         p["age"] = particle.age;
                         p["lifetime"] = particle.lifetime;
                         p["position"] = Json(Json::Array{particle.position.x, particle.position.y, particle.position.z});
                         p["velocity"] = Json(Json::Array{particle.velocity.x, particle.velocity.y, particle.velocity.z});
                         p["worldSpace"] = particle.worldSpace;
                         out["particles"].push(p);
                     }
                 }
                 return out;
             });
    Register(r, "particles.burst", "Emit particles immediately, including on paused emitters. Excess births are dropped at capacity; runtime only.",
             Params().ReqWith("id", EntityRefSchema("ParticleEmitter entity.")).Req("count", "integer", "Number to emit, 0..10000."),
             false, [requireEmitter](Engine& e, const Json& a) {
                 const ParticleEmitter& emitter = requireEmitter(e, a);
                 if (!ParticleSettingsValid(emitter))
                     throw ApiError("invalid_emitter", "particle settings must be finite", "Correct direction/gravity/colors and numeric fields with component.set.");
                 double requested = a["count"].asNumber();
                 if (requested < 0 || requested > 10000) throw ApiError("invalid_argument", "count must be 0..10000", "Use a bounded burst; maxParticles also limits accepted births.");
                 int count = static_cast<int>(requested);
                 Json out = Json::MakeObject();
                 out["accepted"] = BurstParticles(e.GetScene(), RequireEntity(e, a), count);
                 return out;
             });
    Register(r, "particles.clear", "Clear live particles and reset the random stream/counters. Optionally re-arm the initial burst; runtime only.",
             Params().ReqWith("id", EntityRefSchema("ParticleEmitter entity.")).Opt("restart", "boolean", "Re-arm initial burst for next playing step (default false)."),
             false, [requireEmitter](Engine& e, const Json& a) {
                 ParticleEmitter& emitter = requireEmitter(e, a);
                 emitter.particles.clear();
                 emitter.randomState = 0;
                 emitter.carry = 0;
                 emitter.emitted = 0;
                 emitter.initialBurstEmitted = !a["restart"].asBool(false);
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

    Register(r, "ui.layout", "Where every visible UI element is on a screen of the given size (pixels, top-left origin), in draw order. Use the centers with input.click.",
             Params()
                 .Opt("width", "integer", "Screen width (default 640, like render.screenshot).")
                 .Opt("height", "integer", "Screen height (default 360).")
                 .Opt("interactable", "boolean", "Only buttons and sliders that accept clicks."),
             false, [](Engine& e, const Json& a) {
                 int w = std::max(1, a["width"].asInt(640)), h = std::max(1, a["height"].asInt(360));
                 bool onlyInteractable = a["interactable"].asBool(false);
                 auto r1 = [](float v) { return std::round(static_cast<double>(v) * 10.0) / 10.0; };
                 static const char* kKinds[] = {"UIText", "UIPanel", "UIButton", "UIImage", "UISlider"};
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
        Register(r, "render.screenshot", "Render a frame and return it as PNG (and/or save it). Look at it to verify visual changes.", p, false,
                 [](Engine& e, const Json& a) {
                     ViewRequest vr = ParseView(e, a);
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
                     RenderStats stats = renderer->Render(e.GetScene(), vr.view, target);
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

    Register(r, "script.reload", "Reload all loaded Lua modules now (running instances keep their state).", Params(), false,
             [](Engine& e, const Json&) {
                 Json list = Json::MakeArray();
                 for (const std::string& p : e.Scripts().ReloadAll()) list.push(p);
                 Json out = Json::MakeObject();
                 out["reloaded"] = list;
                 return out;
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

    Register(r, "script.list", "Lua files in the project, which entities use them, and the running script state.", Params(), false,
             [](Engine& e, const Json&) {
                 Json files = Json::MakeArray();
                 for (const std::string& f : ListFiles(e.ProjectDir(), ".lua", true)) {
                     std::string rel = RelativePath(f, e.ProjectDir());
                     if (rel.rfind("build/", 0) == 0) continue;
                     Json j = Json::MakeObject();
                     j["path"] = rel;
                     Json users = Json::MakeArray();
                     for (const auto& kv : e.GetScene().Pool<Script>()) {
                         if (kv.second.path == rel) users.push(kv.first);
                     }
                     j["entities"] = users;
                     files.push(j);
                 }
                 Json out = e.Scripts().Status();
                 out["files"] = files;
                 return out;
             });

    Register(r, "script.read", "Read a Lua file from the project.", Params().Req("path", "string", "e.g. \"scripts/player.lua\"."), false,
             [](Engine& e, const Json& a) {
                 std::string path = e.ResolvePath(a["path"].asString());
                 std::string text;
                 if (!ReadTextFile(path, text)) throw ApiError("not_found", "cannot read " + a["path"].asString(), "Call script.list to see script files.");
                 Json out = Json::MakeObject();
                 out["path"] = a["path"].asString();
                 out["source"] = text;
                 return out;
             });

    // Source for script.check / script.params: `source` text, else the file at `path`.
    auto scriptSource = [](Engine& e, const Json& a, std::string& chunk) {
        chunk = a["path"].asString("source");
        if (a.has("source")) return a["source"].asString("");
        if (!a.has("path")) throw ApiError("missing_argument", "pass path or source", "e.g. {\"path\": \"scripts/player.lua\"}");
        std::string text;
        if (!ReadTextFile(e.ResolvePath(chunk), text)) throw ApiError("not_found", "cannot read " + chunk, "Call script.list to see script files.");
        return text;
    };

    Register(r, "script.check", "Check a Lua script without running it: syntax errors and suspicious globals (missing local, typos), with line numbers.",
             Params()
                 .Opt("path", "string", "Script file, e.g. \"scripts/player.lua\" (also names the chunk in messages).")
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
                 .Opt("path", "string", "Script file, e.g. \"scripts/player.lua\".")
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
                 std::string path = e.ResolvePath(rel);
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

    Register(r, "game.state", "Runtime scene and game data (game.set values) of the current play session.", Params(), false,
             [](Engine& e, const Json&) {
                 Json out = Json::MakeObject();
                 out["inPlaySession"] = e.InPlaySession();
                 out["scene"] = e.RuntimeScene();
                 out["sceneName"] = e.GetScene().name;
                 out["data"] = e.GameData();
                 out["frame"] = static_cast<uint64_t>(e.Frame());
                 return out;
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
                 .Opt("maxDistance", "number", "Maximum distance in meters (default 1000)."),
             false, [](Engine& e, const Json& a) {
                 RaycastHit hit = e.Physics().Raycast(e.GetScene(), ReadVec3(a["origin"], Vec3()), ReadVec3(a["direction"], Vec3(0, -1, 0)),
                                                      a["maxDistance"].asFloat(1000.0f));
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
