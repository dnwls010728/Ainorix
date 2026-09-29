// Engine self tests. Run: build/bin/oe_tests  (exit code 0 = all passed)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "app/Engine.h"
#include "app/Project.h"
#include "assets/Assets.h"
#include "audio/Wav.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Json.h"
#include "core/Log.h"
#include "render/GpuRenderer.h"
#include "render/UI.h"
#include "scene/Components.h"
#include "script/ScriptHost.h"

using namespace oe;

namespace {

int g_failures = 0;
std::string g_current;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                  \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> t;
    return t;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Tests().push_back({name, std::move(fn)}); }
};

#define TEST(name)                                   \
    static void name();                              \
    static Registrar reg_##name(#name, &name);       \
    static void name()

TEST(JsonRoundTrip) {
    std::string err;
    Json j = Json::parse(R"J({"a": 1, "b": [true, null, 2.5, "x\"y"], "c": {"d": "\u00e9\ud83d\ude00"}, "e": -3e2,})J", &err);
    CHECK(err.empty());
    CHECK(j["a"].asInt() == 1);
    CHECK(j["b"].size() == 4);
    CHECK(j["b"][3].asString() == "x\"y");
    CHECK(j["c"]["d"].asString() == "\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(j["e"].asNumber() == -300);
    Json again = Json::parse(j.dump(2), &err);
    CHECK(err.empty());
    CHECK(again == j);
    Json bad = Json::parse("{\"a\": }", &err);
    CHECK(!err.empty());
    CHECK(err.find("line 1") != std::string::npos);
}

TEST(SceneRoundTrip) {
    Json sceneJson = MakeSampleScene("Test");
    Scene s;
    std::string err;
    CHECK(s.FromJson(sceneJson, &err));
    CHECK(s.Entities().size() == 16);
    CHECK(s.ToJson() == sceneJson);
    EntityId player = s.FindByName("Player");
    CHECK(player != kNullEntity);
    CHECK(s.Get<PlayerController>(player) != nullptr);
}

TEST(ReflectionErrors) {
    Scene s;
    EntityId e = s.Create("E");
    const ComponentType* t = TypeRegistry::Find("MeshRenderer");
    CHECK(t != nullptr);
    void* c = s.AddComponent(e, *t);
    std::string err;
    CHECK(!ApplyComponentJson(*t, c, Json::parse(R"J({"shading": "glossy"})J"), &err));
    CHECK(err.find("smooth") != std::string::npos);
    CHECK(!ApplyComponentJson(*t, c, Json::parse(R"J({"colour": [1,0,0]})J"), &err));
    CHECK(err.find("Valid fields") != std::string::npos);
    CHECK(ApplyComponentJson(*t, c, Json::parse(R"J({"color": "#ff8000"})J"), &err));
    CHECK(static_cast<MeshRenderer*>(c)->color.r == 1.0f);
}

TEST(CommandsAndUndo) {
    Engine engine;
    engine.Call("scene.new", Json());
    size_t before = engine.GetScene().Entities().size();
    Json r = engine.Call("entity.create", Json::parse(R"J({"name": "Box", "components": {"MeshRenderer": {"color": [1,0,0]}}})J"));
    CHECK(r["ok"].asBool());
    CHECK(engine.GetScene().Entities().size() == before + 1);
    Json bad = engine.Call("component.set", Json::parse(R"J({"id": "Box", "type": "MeshRenderer", "values": {"shading": "nope"}})J"));
    CHECK(!bad["ok"].asBool());
    CHECK(bad["error"]["code"].asString() == "invalid_component_values");
    Json unknown = engine.Call("entity.creat", Json());
    CHECK(unknown["error"]["hint"].asString().find("entity.create") != std::string::npos);
    Json missing = engine.Call("entity.get", Json());
    CHECK(missing["error"]["code"].asString() == "missing_argument");
    CHECK(engine.Call("history.undo", Json())["ok"].asBool());
    CHECK(engine.GetScene().Entities().size() == before);
    CHECK(engine.Call("history.redo", Json())["ok"].asBool());
    CHECK(engine.GetScene().FindByName("Box") != kNullEntity);
}

TEST(UndoMergeKey) {
    // Editor drags send many component.set calls with one merge key; they must
    // undo as a single step, and a new key starts a new step.
    Engine engine;
    engine.Call("scene.new", Json());
    CHECK(engine.Call("entity.create", Json::parse(R"J({"name": "Box", "components": {"Transform": {}}})J"))["ok"].asBool());
    size_t base = engine.UndoDepth();
    for (int i = 1; i <= 5; ++i) {
        Json a = Json::parse(R"J({"id": "Box", "type": "Transform", "values": {"position": [0, 0, 0]}, "merge": "drag1"})J");
        a["values"]["position"][0] = static_cast<double>(i);
        CHECK(engine.Call("component.set", a)["ok"].asBool());
    }
    CHECK(engine.UndoDepth() == base + 1);
    CHECK(engine.Call("component.set", Json::parse(R"J({"id": "Box", "type": "Transform", "values": {"position": [9, 0, 0]}, "merge": "drag2"})J"))["ok"].asBool());
    CHECK(engine.UndoDepth() == base + 2);
    CHECK(engine.Call("history.undo", Json())["ok"].asBool());
    CHECK(engine.Call("history.undo", Json())["ok"].asBool());
    Json t = engine.Call("entity.get", Json::parse(R"J({"id": "Box"})J"));
    CHECK(t["result"]["components"]["Transform"]["position"][0].asNumber() == 0.0);
}

TEST(FileCopyAndRemove) {
    // Used by `oe package` to assemble the game folder.
    std::string root = "build/test_fs";
    RemoveAll(root);
    CHECK(WriteTextFile(root + "/src/a.bin", std::string("x\0y", 3)));
    CHECK(CopyFileTo(root + "/src/a.bin", root + "/out/deep/a.bin"));
    std::vector<unsigned char> data;
    CHECK(ReadBinaryFile(root + "/out/deep/a.bin", data) && data.size() == 3 && data[1] == 0);
    CHECK(!CopyFileTo(root + "/missing.bin", root + "/out/m.bin"));
    CHECK(RemoveAll(root));
    CHECK(!FileExists(root + "/src/a.bin"));
}

TEST(SimulationDeterminismAndInput) {
    Json sceneJson = MakeSampleScene("Sim");
    auto run = [&](bool pressW) {
        Engine engine;
        std::string err;
        engine.GetScene().FromJson(sceneJson, &err);
        if (pressW) engine.Call("input.key", Json::parse(R"J({"key": "W"})J"));
        engine.Call("sim.step", Json::parse(R"J({"frames": 60})J"));
        const Transform* t = engine.GetScene().Get<Transform>(engine.GetScene().FindByName("Player"));
        Json out = engine.Call("render.screenshot", Json::parse(R"J({"width": 64, "height": 36, "inline": false})J"));
        return std::make_pair(t->position, out["result"]["hash"].asString());
    };
    auto a = run(false), b = run(false), c = run(true);
    CHECK(a.second == b.second);          // same inputs -> identical frame
    CHECK(a.first == b.first);
    CHECK(c.first.z < a.first.z - 3.5f);  // W moves the player ~4 m forward in 1 s
    // sim.stop restores the edit-time scene.
    Engine engine;
    std::string err;
    engine.GetScene().FromJson(sceneJson, &err);
    engine.Call("sim.step", Json::parse(R"J({"frames": 30})J"));
    engine.Call("sim.stop", Json());
    CHECK(engine.GetScene().ToJson() == sceneJson);
}

TEST(RenderPickAndPng) {
    Engine engine;
    std::string err;
    engine.GetScene().FromJson(MakeSampleScene("Pick"), &err);
    // Look straight down at the player sphere from above.
    Json pick = engine.Call("render.pick", Json::parse(R"J({"x": 32, "y": 32, "width": 64, "height": 64, "camera": {"eye": [0, 6, 2.01], "target": [0, 0.5, 2], "fov": 30}})J"));
    CHECK(pick["ok"].asBool());
    CHECK(pick["result"]["name"].asString() == "Player");
    RenderTarget rt;
    rt.Resize(32, 16);
    engine.RenderGameView(rt);
    std::vector<uint8_t> png = EncodePng(rt.ToImage());
    CHECK(png.size() > 60);
    CHECK(png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
}

TEST(PathSandbox) {
    Engine engine;
    Json r = engine.Call("scene.save", Json::parse(R"J({"path": "../../escape.scene.json"})J"));
    CHECK(!r["ok"].asBool());
    CHECK(r["error"]["code"].asString() == "path_outside_project");
}


// ----- scripting --------------------------------------------------------------

// Fresh project (with the template scripts) in a temp directory.
std::string TempProject(const char* name) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / (std::string("oe_tests_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string d = dir.generic_string();
    std::string err;
    CreateProject(d, name, &err);
    return d;
}

Json Call(Engine& e, const char* cmd, const char* args) { return e.Call(cmd, Json::parse(args)); }

bool Near(const Vec3& a, const Vec3& b, float eps) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

TEST(LuaRotatorMatchesNative) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("rotator"), &err));
    Call(e, "entity.create", R"J({"name": "Native", "components": {"Rotator": {"degreesPerSecond": [20, 60, 0]}}})J");
    Call(e, "entity.create", R"J({"name": "Lua", "components": {"Script": {"path": "scripts/rotator.lua", "params": {"degreesPerSecond": [20, 60, 0]}}}})J");
    Call(e, "sim.step", R"J({"frames": 400})J");
    Scene& s = e.GetScene();
    Vec3 a = s.Get<Transform>(s.FindByName("Native"))->rotation;
    Vec3 b = s.Get<Transform>(s.FindByName("Lua"))->rotation;
    CHECK(a.y > 1.0f);
    CHECK(Near(a, b, 1e-2f));
    CHECK(e.Scripts().Errors().empty());
}

TEST(LuaPlayerControllerMatchesNative) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("player"), &err));
    Call(e, "entity.create", R"J({"name": "Native", "components": {"Transform": {"position": [0, 0.5, 0]}, "PlayerController": {}}})J");
    Call(e, "entity.create", R"J({"name": "Lua", "components": {"Transform": {"position": [10, 0.5, 0]}, "Script": {"path": "scripts/player_controller.lua"}}})J");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "input.key", R"J({"key": "D"})J");
    Call(e, "input.key", R"J({"key": "Space"})J");
    Call(e, "sim.step", R"J({"frames": 20})J");
    Call(e, "input.clear", "{}");
    Call(e, "sim.step", R"J({"frames": 60})J");
    Scene& s = e.GetScene();
    Vec3 a = s.Get<Transform>(s.FindByName("Native"))->position;
    Vec3 b = s.Get<Transform>(s.FindByName("Lua"))->position - Vec3(10, 0, 0);
    CHECK(a.z < -0.9f && a.x > 0.9f);  // moved diagonally ~0.94 m
    CHECK(Near(a, b, 1e-3f));
    CHECK(e.Scripts().Errors().empty());
}

TEST(ScriptEvalAndSandbox) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("eval"), &err));
    Json r = Call(e, "script.eval", R"J({"code": "1 + 2"})J");
    CHECK(r["result"]["value"].asInt() == 3);
    r = Call(e, "script.eval", R"J({"code": "print('hi', 5) return scene.find('Player')"})J");
    CHECK(r["result"]["output"][0].asString() == "hi\t5");
    CHECK(r["result"]["value"].asInt() == static_cast<int>(e.GetScene().FindByName("Player")));
    r = Call(e, "script.eval", R"J({"code": "self:get('Transform').position.y", "entity": "Player"})J");
    CHECK(std::fabs(r["result"]["value"].asNumber() - 0.5) < 1e-6);
    r = Call(e, "script.eval", R"J({"code": "io == nil and os == nil and load == nil and debug == nil"})J");
    CHECK(r["result"]["value"].asBool());
    r = Call(e, "script.eval", R"J({"code": "local x = "})J");
    CHECK(r["error"]["code"].asString() == "script_syntax_error");
    r = Call(e, "script.eval", R"J({"code": "while true do end"})J");
    CHECK(r["error"]["message"].asString().find("instruction budget") != std::string::npos);
    r = Call(e, "script.eval", R"J({"code": "scene.set('Player', 'Transform', {position = {y = 3}})"})J");
    CHECK(r["ok"].asBool());
    const Transform* t = e.GetScene().Get<Transform>(e.GetScene().FindByName("Player"));
    CHECK(t->position.y == 3.0f && t->position.z == 2.0f);  // partial update keeps x/z
    CHECK(Call(e, "history.undo", "{}")["ok"].asBool());    // eval edits are undoable
    CHECK(e.GetScene().Get<Transform>(e.GetScene().FindByName("Player"))->position.y == 0.5f);
}

TEST(ScriptErrorsAndHotReload) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("reload"), &err));
    Call(e, "script.write", R"J({"path": "scripts/counter.lua", "source": "local M = {}\nfunction M:onStart() self.count = 0 end\nfunction M:onUpdate(dt)\n  self.count = self.count + 1\n  if self.count == 5 then local t = nil; t.boom = 1 end\nend\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Counter", "components": {"Script": {"path": "scripts/counter.lua"}}})J");
    Call(e, "sim.step", R"J({"frames": 10})J");
    Json errors = Call(e, "script.errors", "{}")["result"];
    CHECK(errors.size() == 1);  // faulted instance reports once, not every frame
    CHECK(errors[0]["message"].asString().find("scripts/counter.lua:5:") != std::string::npos);
    CHECK(errors[0]["entity"].asInt() == static_cast<int>(e.GetScene().FindByName("Counter")));

    // Fix the script: the running instance keeps its state (count = 5) and continues.
    Call(e, "script.write", R"J({"path": "scripts/counter.lua", "source": "local M = {}\nfunction M:onStart() self.count = 0 end\nfunction M:onUpdate(dt) self.count = self.count + 100 end\nreturn M\n"})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    Json r = Call(e, "script.eval", R"J({"code": "self.count", "entity": "Counter"})J");
    CHECK(r["result"]["value"].asInt() == 105);
    CHECK(Call(e, "script.errors", "{}")["result"].size() == 1);

    // A missing script file is reported, not fatal.
    Call(e, "entity.create", R"J({"name": "Ghost", "components": {"Script": {"path": "scripts/missing.lua"}}})J");
    Call(e, "sim.step", R"J({"frames": 3})J");
    CHECK(Call(e, "script.errors", "{}")["result"].size() == 2);

    // sim.stop discards the Lua state.
    Call(e, "sim.stop", "{}");
    CHECK(!e.Scripts().Status()["sessionActive"].asBool());
    CHECK(Call(e, "script.write", R"J({"path": "../escape.lua", "source": ""})J")["error"]["code"].asString() == "path_outside_project");
}

TEST(ScriptedSceneIsDeterministic) {
    std::string dir = TempProject("determinism");
    auto run = [&] {
        Engine e;
        std::string err;
        e.Open(dir, &err);
        Call(e, "script.eval", R"J({"code": "local t = {} for i = 1, 50 do t['k' .. i] = i end local s = '' for k in pairs(t) do s = s .. k end return s"})J");
        Call(e, "sim.step", R"J({"frames": 75})J");
        Json shot = Call(e, "render.screenshot", R"J({"width": 96, "height": 54, "inline": false})J");
        Json order = Call(e, "script.eval", R"J({"code": "local t = {} for i = 1, 50 do t['k' .. i] = i end local s = '' for k in pairs(t) do s = s .. k end return s"})J");
        const Transform* t = e.GetScene().Get<Transform>(e.GetScene().FindByName("Pyramid"));
        return std::make_tuple(shot["result"]["hash"].asString(), order["result"]["value"].asString(), t->position.y);
    };
    auto a = run();
    auto b = run();
    CHECK(std::get<0>(a) == std::get<0>(b));
    CHECK(std::get<1>(a) == std::get<1>(b));  // pairs() order is stable across runs
    CHECK(std::fabs(std::get<2>(a) - 0.75f) > 0.05f);  // bob.lua moved the pyramid
}

// ----- physics -----------------------------------------------------------------

// Empty scene with a 20x1x20 static ground whose top face is at y = 0.
void PhysicsScene(Engine& e) {
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Ground", "components": {"Transform": {"position": [0, -0.5, 0], "scale": [20, 1, 20]}, "Collider": {}}})J");
}

Vec3 PosOf(Engine& e, const char* name) { return e.GetScene().Get<Transform>(e.GetScene().FindByName(name))->position; }

TEST(PhysicsBallRestsOnGround) {
    Engine e;
    PhysicsScene(e);
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"Transform": {"position": [0, 5, 0]}, "Collider": {"shape": "sphere"}, "RigidBody": {}}})J");
    Call(e, "sim.step", R"J({"frames": 30})J");
    CHECK(PosOf(e, "Ball").y < 4.0f);  // falling
    Call(e, "sim.step", R"J({"frames": 270})J");
    Vec3 p = PosOf(e, "Ball");
    CHECK(std::fabs(p.y - 0.5f) < 0.03f);
    const RigidBody* rb = e.GetScene().Get<RigidBody>(e.GetScene().FindByName("Ball"));
    CHECK(Length(rb->velocity) < 0.05f);
    Json state = Call(e, "physics.state", "{}")["result"];
    CHECK(state["dynamicBodies"].asInt() == 1 && state["staticBodies"].asInt() == 1);
}

TEST(PhysicsBoxStackIsStable) {
    Engine e;
    PhysicsScene(e);
    for (int i = 0; i < 3; ++i) {
        Json args = Json::parse(R"J({"components": {"Transform": {}, "Collider": {}, "RigidBody": {}}})J");
        args["name"] = "Box" + std::to_string(i);
        args["components"]["Transform"]["position"] = Json(Json::Array{0.0, 0.5 + i * 1.02, 0.0});
        e.Call("entity.create", args);
    }
    Call(e, "sim.step", R"J({"frames": 300})J");
    for (int i = 0; i < 3; ++i) {
        Vec3 p = PosOf(e, ("Box" + std::to_string(i)).c_str());
        CHECK(std::fabs(p.y - (0.5f + static_cast<float>(i))) < 0.05f);
        CHECK(std::fabs(p.x) < 0.05f && std::fabs(p.z) < 0.05f);
    }
}

TEST(PhysicsCharacterBlockedByWall) {
    Engine e;
    PhysicsScene(e);
    // Wall face at z = -3 (wall spans z -3.5 .. -3).
    Call(e, "entity.create", R"J({"name": "Wall", "components": {"Transform": {"position": [0, 1, -3.25], "scale": [6, 2, 0.5]}, "Collider": {}}})J");
    Call(e, "entity.create", R"J({"name": "Player", "components": {"Transform": {"position": [0, 0.5, 0]}, "CharacterBody": {"shape": "sphere", "radius": 0.5}, "PlayerController": {}}})J");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "sim.step", R"J({"frames": 180})J");  // 3 s at 4 m/s would reach z = -12 without the wall
    Vec3 p = PosOf(e, "Player");
    CHECK(p.z > -2.55f && p.z < -2.4f);  // touching the wall face (z = -3 + radius)
    CHECK(std::fabs(p.y - 0.5f) < 0.05f);
    CHECK(e.GetScene().Get<CharacterBody>(e.GetScene().FindByName("Player"))->grounded);
    // Jump: leaves the ground, then lands again.
    Call(e, "input.clear", "{}");
    Call(e, "input.key", R"J({"key": "Space"})J");
    Call(e, "sim.step", R"J({"frames": 10})J");
    Call(e, "input.clear", "{}");
    CHECK(PosOf(e, "Player").y > 1.0f);
    Call(e, "sim.step", R"J({"frames": 120})J");
    CHECK(std::fabs(PosOf(e, "Player").y - 0.5f) < 0.05f);
}

TEST(PhysicsTriggersAndScriptCallbacks) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("physics_events"), &err));
    PhysicsScene(e);
    Call(e, "script.write", R"J({"path": "scripts/coin.lua", "source": "local M = {}\nfunction M:onTriggerEnter(other)\n  if scene.name(other) == 'Player' then hits = (hits or 0) + 1 end\nend\nreturn M\n"})J");
    Call(e, "script.write", R"J({"path": "scripts/crate.lua", "source": "local M = {}\nfunction M:onCollisionEnter(other) landed = (landed or 0) + 1 lastHit = scene.name(other) end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Coin", "components": {"Transform": {"position": [0, 0.5, -2]}, "Collider": {"shape": "sphere", "radius": 0.4, "isTrigger": true}, "Script": {"path": "scripts/coin.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Crate", "components": {"Transform": {"position": [3, 2, 0]}, "Collider": {}, "RigidBody": {}, "Script": {"path": "scripts/crate.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Player", "components": {"Transform": {"position": [0, 0.5, 0]}, "CharacterBody": {"shape": "sphere", "radius": 0.5}, "PlayerController": {}}})J");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "sim.step", R"J({"frames": 90})J");  // walks through the coin and beyond
    CHECK(Call(e, "script.eval", R"J({"code": "hits"})J")["result"]["value"].asInt() == 1);
    CHECK(Call(e, "script.eval", R"J({"code": "landed"})J")["result"]["value"].asInt() == 1);
    CHECK(Call(e, "script.eval", R"J({"code": "lastHit"})J")["result"]["value"].asString() == "Ground");
    CHECK(e.Scripts().Errors().empty());
    Json contacts = Call(e, "physics.contacts", R"J({"id": "Crate"})J")["result"];
    CHECK(contacts.size() == 1 && contacts[0]["kind"].asString() == "collision");
}

TEST(PhysicsQueries) {
    Engine e;
    PhysicsScene(e);
    Call(e, "entity.create", R"J({"name": "Box", "components": {"Transform": {"position": [2, 0.5, 0]}, "Collider": {}}})J");
    // Works outside simulation too.
    Json hit = Call(e, "physics.raycast", R"J({"origin": [0, 10, 0], "direction": [0, -1, 0]})J")["result"];
    CHECK(hit["hit"].asBool() && hit["name"].asString() == "Ground");
    CHECK(std::fabs(hit["point"][1].asNumber()) < 1e-3 && std::fabs(hit["normal"][1].asNumber() - 1.0) < 1e-3);
    CHECK(std::fabs(hit["distance"].asNumber() - 10.0) < 1e-3);
    hit = Call(e, "physics.raycast", R"J({"origin": [-5, 0.5, 0], "direction": [1, 0, 0]})J")["result"];
    CHECK(hit["name"].asString() == "Box" && std::fabs(hit["distance"].asNumber() - 6.5) < 1e-3);
    Json overlap = Call(e, "physics.overlap", R"J({"center": [2, 1.2, 0], "radius": 0.3})J")["result"];
    CHECK(overlap.size() == 1 && overlap[0]["name"].asString() == "Box");
    // Collider wireframes show up in screenshots.
    Json a = Call(e, "render.screenshot", R"J({"width": 64, "height": 36, "inline": false})J");
    Json b = Call(e, "render.screenshot", R"J({"width": 64, "height": 36, "inline": false, "colliders": true})J");
    CHECK(a["result"]["hash"].asString() != b["result"]["hash"].asString());
}

TEST(LuaCharacterControllerMatchesNative) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("lua_character"), &err));
    PhysicsScene(e);
    Call(e, "entity.create", R"J({"name": "Native", "components": {"Transform": {"position": [-3, 0.5, 0]}, "CharacterBody": {"shape": "sphere", "radius": 0.5}, "PlayerController": {}}})J");
    Call(e, "entity.create", R"J({"name": "Lua", "components": {"Transform": {"position": [3, 0.5, 0]}, "CharacterBody": {"shape": "sphere", "radius": 0.5}, "Script": {"path": "scripts/player_controller.lua"}}})J");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "input.key", R"J({"key": "Space"})J");
    Call(e, "sim.step", R"J({"frames": 5})J");
    Call(e, "input.key", R"J({"key": "Space", "down": false})J");
    Call(e, "sim.step", R"J({"frames": 90})J");
    Vec3 a = PosOf(e, "Native"), b = PosOf(e, "Lua") - Vec3(6, 0, 0);
    CHECK(a.z < -3.0f);
    CHECK(Near(a, b, 1e-3f));
    CHECK(e.Scripts().Errors().empty());
}

const UIText* TextOf(Engine& e, const char* name) {
    EntityId id = e.GetScene().FindByName(name);
    return id ? e.GetScene().Get<UIText>(id) : nullptr;
}

int CountPlayed(Engine& e, const char* path) {
    int n = 0;
    Json state = Call(e, "audio.state", "{}");
    for (const Json& ev : state["result"]["played"].items()) n += ev["path"].asString() == path;
    return n;
}

TEST(TemplateGamePlaythrough) {
    // The `oe new` sample game, played headless like an agent would:
    // level 1 (walk forward over 3 coins) -> level 2 (walk right over 4 coins)
    // -> "You win!" -> click "Play again" -> back to level 1.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("template_game"), &err));
    CHECK(e.GetScene().name == "Level 1");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "sim.step", R"J({"frames": 110})J");
    CHECK(e.GetScene().FindByName("Coin 3") == kNullEntity);
    CHECK(TextOf(e, "Message")->visible && TextOf(e, "Message")->text == "Level clear!");
    CHECK(TextOf(e, "Score")->text.find("Coins: 3/3") == 0);
    CHECK(CountPlayed(e, "sounds/coin.wav") == 3 && CountPlayed(e, "sounds/success.wav") == 1);

    Call(e, "input.clear", "{}");                 // stop walking before level 2 loads
    Call(e, "sim.step", R"J({"frames": 100})J");  // 1.5 s timer -> level 2
    Json state = Call(e, "game.state", "{}")["result"];
    CHECK(state["scene"].asString() == "scenes/level2.scene.json" && state["sceneName"].asString() == "Level 2");
    CHECK(state["data"]["total"].asInt() == 3);

    Call(e, "input.key", R"J({"key": "D"})J");
    Call(e, "sim.step", R"J({"frames": 130})J");
    CHECK(TextOf(e, "Message")->text == "You win!");
    EntityId button = e.GetScene().FindByName("Play Again");
    CHECK(e.GetScene().Get<UIButton>(button)->visible);
    CHECK(Call(e, "game.state", "{}")["result"]["data"]["total"].asInt() == 7);

    // The button is centered, 60 reference px below the middle: (320, 210) in a 640x360 shot.
    Json click = Call(e, "input.click", R"J({"x": 320, "y": 210})J");
    CHECK(click["result"]["buttonName"].asString() == "Play Again");
    Call(e, "sim.step", R"J({"frames": 2})J");
    CHECK(e.GetScene().name == "Level 1");
    CHECK(Call(e, "game.state", "{}")["result"]["data"]["total"].asInt() == 0);
    CHECK(e.Scripts().Errors().empty());

    // Walking off the edge respawns the player at the level start.
    Call(e, "input.clear", "{}");
    Call(e, "input.key", R"J({"key": "S"})J");
    Call(e, "sim.step", R"J({"frames": 110})J");  // past the edge at z = 8
    Call(e, "input.clear", "{}");
    CHECK(PosOf(e, "Player").y < 0.0f);           // falling
    Call(e, "sim.step", R"J({"frames": 150})J");
    Vec3 p = PosOf(e, "Player");
    CHECK(p.y > -1.0f && std::fabs(p.z - 2.0f) < 0.5f);

    Call(e, "sim.stop", "{}");
    CHECK(e.GetScene().name == "Level 1" && e.GetScene().FindByName("Coin 1") != kNullEntity);
}

TEST(PrefabsCreateAndInstantiate) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("prefabs"), &err));
    Call(e, "entity.create", R"J({"name": "Tower", "components": {"MeshRenderer": {}}})J");
    Call(e, "entity.create", R"J({"name": "Flag", "parent": "Tower", "components": {"Transform": {"position": [0, 1, 0]}, "Tag": {"tags": "flag"}}})J");
    Json made = Call(e, "prefab.create", R"J({"id": "Tower", "path": "prefabs/tower.prefab.json"})J");
    CHECK(made["result"]["entities"].asInt() == 2);
    Json a = Call(e, "prefab.instantiate", R"J({"path": "prefabs/tower.prefab.json", "name": "Tower A", "position": [5, 0, 0]})J");
    CHECK(a["ok"].asBool());
    EntityId root = static_cast<EntityId>(a["result"]["id"].asNumber());
    CHECK(e.GetScene().Get<Prefab>(root)->path == "prefabs/tower.prefab.json");
    CHECK(e.GetScene().Get<Transform>(root)->position.x == 5.0f);
    std::vector<EntityId> kids = e.GetScene().Children(root);
    CHECK(kids.size() == 1 && e.GetScene().Get<Tag>(kids[0])->tags == "flag");
    // From Lua, during simulation.
    Json r = Call(e, "script.eval", R"J({"code": "return scene.instantiate('prefabs/coin.prefab.json', {name = 'Spawned', position = {x = 1, y = 2, z = 3}})"})J");
    EntityId spawned = static_cast<EntityId>(r["result"]["value"].asNumber());
    CHECK(e.GetScene().Record(spawned)->name == "Spawned" && e.GetScene().Get<Transform>(spawned)->position.z == 3.0f);
    CHECK(Call(e, "prefab.list", "{}")["result"].size() == 2);
    CHECK(Call(e, "prefab.instantiate", R"J({"path": "prefabs/missing.prefab.json"})J")["error"]["code"].asString() == "not_found");
}

TEST(TimersMessagesAndGameData) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("timers"), &err));
    PhysicsScene(e);
    Call(e, "script.write", R"J({"path": "scripts/receiver.lua", "source": "local M = {}\nfunction M:onStart() self.got = 0 end\nfunction M:ping(n) self.got = self.got + n return self.got end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "A", "components": {"Script": {"path": "scripts/receiver.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "B", "components": {"Script": {"path": "scripts/receiver.lua"}}})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(Call(e, "script.eval", R"J({"code": "scene.send(scene.find('A'), 'ping', 5)"})J")["result"]["value"].asInt() == 5);
    CHECK(Call(e, "script.eval", R"J({"code": "scene.broadcast('ping', 1)"})J")["result"]["value"].asInt() == 2);
    CHECK(Call(e, "script.eval", R"J({"code": "self.got", "entity": "A"})J")["result"]["value"].asInt() == 6);
    CHECK(Call(e, "script.eval", R"J({"code": "scene.send(scene.find('Ground'), 'ping', 1)"})J")["result"]["value"].isNull());

    Call(e, "script.eval", R"J({"code": "fired = 0 ticks = 0 timer.after(0.5, function() fired = time.frame() end) local id = timer.every(0.25, function() ticks = ticks + 1 end) cancelMe = timer.after(0.1, function() fired = -1 end) timer.cancel(cancelMe)"})J");
    Call(e, "sim.step", R"J({"frames": 60})J");
    int fired = Call(e, "script.eval", R"J({"code": "fired"})J")["result"]["value"].asInt();
    CHECK(fired >= 30 && fired <= 32);
    CHECK(Call(e, "script.eval", R"J({"code": "ticks"})J")["result"]["value"].asInt() == 3);  // at +0.25, +0.5, +0.75 s

    Call(e, "script.eval", R"J({"code": "game.set('lives', 3) game.set('name', 'hero')"})J");
    Json data = Call(e, "game.state", "{}")["result"]["data"];
    CHECK(data["lives"].asInt() == 3 && data["name"].asString() == "hero");
    Call(e, "sim.stop", "{}");
    CHECK(Call(e, "game.state", "{}")["result"]["data"].size() == 0);  // reset with the session
    CHECK(e.Scripts().Errors().empty());
}

TEST(UIRenderingAndClicks) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "script.write", R"J({"path": "scripts/button.lua", "source": "local M = {}\nfunction M:onClick() clicks = (clicks or 0) + 1 end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Label", "components": {"UIText": {"text": "HI", "x": 0, "y": 0, "size": 80, "color": [1, 1, 1]}}})J");
    Call(e, "entity.create", R"J({"name": "Btn", "components": {"UIButton": {"anchor": "bottom-right", "x": -10, "y": -10, "width": 200, "height": 100}, "Script": {"path": "scripts/button.lua"}}})J");
    // Text pixels: the 'H' left column starts at the top-left corner.
    RenderTarget rt;
    rt.Resize(1280, 720);
    RenderView view;
    MakeSceneView(e.GetScene(), 1280.0f / 720.0f, view);
    e.Renderer().Render(e.GetScene(), view, rt);
    CHECK(rt.color[static_cast<size_t>(20) * 1280 + 3] == 0xFFFFFFFFu);  // inside the H stem
    CHECK(rt.IdAt(3, 20) == e.GetScene().FindByName("Label"));
    // Hidden elements are not drawn; free cameras skip UI.
    Json shot = Call(e, "render.screenshot", R"J({"width": 64, "height": 36, "inline": false})J");
    Json free = Call(e, "render.screenshot", R"J({"width": 64, "height": 36, "inline": false, "camera": {"eye": [0, 5, 5], "target": [0, 0, 0]}})J");
    CHECK(shot["result"]["hash"].asString() != free["result"]["hash"].asString());
    // Click inside / outside the button (bottom-right 200x100 at 10 px margin in a 1280x720 view).
    Json hit = Call(e, "input.click", R"J({"x": 1200, "y": 650, "width": 1280, "height": 720})J");
    CHECK(hit["result"]["buttonName"].asString() == "Btn");
    Call(e, "sim.step", R"J({"frames": 1})J");
    Call(e, "input.click", R"J({"x": 100, "y": 650, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(Call(e, "script.eval", R"J({"code": "clicks"})J")["result"]["value"].asInt() == 1);
}

TEST(AudioMixingIsCapturable) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("audio"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    CHECK(Call(e, "audio.generate", R"J({"path": "sounds/blip.wav", "preset": "blip"})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name": "Music", "components": {"AudioSource": {"clip": "sounds/coin.wav", "loop": true, "volume": 0.5}}})J");
    Call(e, "audio.capture", R"J({"action": "start"})J");
    Call(e, "sim.step", R"J({"frames": 30})J");
    Call(e, "script.eval", R"J({"code": "audio.play('sounds/blip.wav')"})J");
    Call(e, "sim.step", R"J({"frames": 30})J");
    Json stats = Call(e, "audio.capture", R"J({"action": "stop", "path": "out.wav"})J")["result"];
    CHECK(std::fabs(stats["seconds"].asNumber() - 1.0) < 1e-6);
    CHECK(stats["peak"].asNumber() > 0.1 && stats["rms"].asNumber() > 0.01);
    Json st = Call(e, "audio.state", "{}")["result"];
    CHECK(st["voices"].size() == 1 && st["voices"][0]["loop"].asBool());  // blip finished, music loops
    CHECK(st["played"].size() == 2 && st["played"][1]["frame"].asInt() == 30);
    // WAV round trip through the decoder.
    std::vector<unsigned char> bytes;
    CHECK(ReadBinaryFile(JoinPath(e.ProjectDir(), "out.wav"), bytes));
    AudioClip clip;
    CHECK(DecodeWav(bytes, clip, &err) && clip.Frames() == 48000);
    // Silence is silent; removing the source stops its voice.
    Call(e, "component.remove", R"J({"id": "Music", "type": "AudioSource"})J");
    Call(e, "audio.capture", R"J({"action": "start"})J");
    Call(e, "sim.step", R"J({"frames": 10})J");
    CHECK(Call(e, "audio.capture", R"J({"action": "stop"})J")["result"]["peak"].asNumber() == 0.0);
    CHECK(Call(e, "audio.play", R"J({"path": "sounds/nope.wav"})J")["error"]["code"].asString() == "not_found");
}

TEST(PhysicsIsDeterministic) {
    auto run = [] {
        Engine e;
        PhysicsScene(e);
        for (int i = 0; i < 6; ++i) {
            Json args = Json::parse(R"J({"components": {"Transform": {}, "Collider": {}, "RigidBody": {}}})J");
            args["name"] = "Box" + std::to_string(i);
            args["components"]["Transform"]["position"] = Json(Json::Array{0.3 * i, 1.0 + 1.3 * i, 0.2 * i});
            args["components"]["Transform"]["rotation"] = Json(Json::Array{10.0 * i, 20.0 * i, 0.0});
            e.Call("entity.create", args);
        }
        Call(e, "entity.create", R"J({"name": "Player", "components": {"Transform": {"position": [0, 0.5, 4]}, "CharacterBody": {}, "PlayerController": {}}})J");
        Call(e, "input.key", R"J({"key": "W"})J");
        Call(e, "sim.step", R"J({"frames": 240})J");
        Json shot = Call(e, "render.screenshot", R"J({"width": 96, "height": 54, "inline": false, "camera": {"eye": [6, 5, 8], "target": [0, 1, 0]}})J");
        return std::make_pair(shot["result"]["hash"].asString(), e.GetScene().ToJson().dump());
    };
    auto a = run(), b = run();
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);  // every transform and velocity identical
}

// ----- assets & rendering ---------------------------------------------------------

size_t CountId(const RenderTarget& rt, EntityId id) {
    size_t n = 0;
    for (EntityId v : rt.ids) n += v == id;
    return n;
}

double MeanLuma(const RenderTarget& rt) {
    double sum = 0;
    for (uint32_t c : rt.color) sum += (c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF);
    return sum / (3.0 * static_cast<double>(rt.color.size()));
}

RenderTarget RenderLook(Engine& e, Vec3 eye, Vec3 target, int w = 160, int h = 90) {
    RenderTarget rt;
    rt.Resize(w, h);
    RenderView v = MakeLookAtView(eye, target, 50.0f, static_cast<float>(w) / static_cast<float>(h));
    e.Renderer().Render(e.GetScene(), v, rt);
    return rt;
}

TEST(TexturesAndGltfModels) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("assets"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // PNG encode -> decode round trip.
    Image img;
    img.width = 3;
    img.height = 2;
    img.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255};
    std::vector<uint8_t> png = EncodePng(img);
    Texture tex;
    CHECK(DecodeImage(png.data(), png.size(), tex, &err) && tex.width == 3 && tex.height == 2);
    CHECK(tex.texels[1] == 0xFF00FF00u && tex.texels[5] == 0xFF5A5046u);
    CHECK(Call(e, "asset.generate_texture", R"J({"path": "assets/textures/c.png", "pattern": "checker", "size": 64})J")["ok"].asBool());
    CHECK(Call(e, "asset.info", R"J({"path": "assets/textures/c.png"})J")["result"]["width"].asInt() == 64);

    // A 1x1 m quad glTF with an embedded 1x1 red PNG (data URIs), written by hand.
    std::vector<uint8_t> bin;
    auto putF = [&](float f) { uint8_t b[4]; std::memcpy(b, &f, 4); bin.insert(bin.end(), b, b + 4); };
    for (float v : {-0.5f, -0.5f, 0.f, 0.5f, -0.5f, 0.f, 0.5f, 0.5f, 0.f, -0.5f, 0.5f, 0.f}) putF(v);  // positions (48 bytes)
    for (float v : {0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f}) putF(v);                                   // uvs (32 bytes)
    for (int i : {0, 1, 2, 0, 2, 3}) { bin.push_back(static_cast<uint8_t>(i)); bin.push_back(0); }  // indices (12 bytes)
    Image red;
    red.width = red.height = 1;
    red.rgba = {255, 0, 0, 255};
    std::string gltf = std::string(R"J({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],
      "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],
      "textures":[{"source":0}],"images":[{"uri":"data:image/png;base64,)J") + Base64Encode(EncodePng(red)) +
                       R"J("}],"buffers":[{"byteLength":92,"uri":"data:application/octet-stream;base64,)J" + Base64Encode(bin) + R"J("}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":32},{"buffer":0,"byteOffset":80,"byteLength":12}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-0.5,-0.5,0],"max":[0.5,0.5,0]},
                   {"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},
                   {"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}]})J";
    CHECK(WriteTextFile(JoinPath(e.ProjectDir(), "assets/models/quad.gltf"), gltf));
    Json info = Call(e, "asset.info", R"J({"path": "assets/models/quad.gltf"})J")["result"];
    CHECK(info["triangles"].asInt() == 2 && info["textures"].size() == 1 && info["submeshes"][0]["texture"].asInt() == 0);
    CHECK(std::fabs(info["size"][0].asNumber() - 1.0) < 1e-6);
    Call(e, "entity.create", R"J({"name": "Quad", "components": {"MeshRenderer": {"mesh": "assets/models/quad.gltf", "color": [1, 1, 1], "unlit": true}}})J");
    RenderTarget rt = RenderLook(e, Vec3(0, 0, 2), Vec3(0, 0, 0));
    CHECK(rt.color[45 * 160 + 80] == 0xFF0000FFu);  // textured red, unlit
    // A texture override replaces the model's texture.
    Call(e, "component.set", R"J({"id": "Quad", "type": "MeshRenderer", "values": {"texture": "assets/textures/c.png"}})J");
    rt = RenderLook(e, Vec3(0, 0, 2), Vec3(0, 0, 0));
    CHECK(rt.color[45 * 160 + 80] != 0xFF0000FFu);
    // Missing models render as a magenta error cube instead of disappearing.
    Call(e, "component.set", R"J({"id": "Quad", "type": "MeshRenderer", "values": {"mesh": "assets/models/missing.glb"}})J");
    rt = RenderLook(e, Vec3(0, 0, 3), Vec3(0, 0, 0));
    CHECK(rt.color[45 * 160 + 80] == 0xFFFF00FFu);
    CHECK(Call(e, "asset.list", R"J({"kind": "model"})J")["result"].size() == 1);
}

TEST(ShadingShadowsAndLights) {
    Engine e;
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Sun", "components": {"Transform": {"rotation": [-70, 20, 0]}, "DirectionalLight": {"shadowStrength": 1}}})J");
    Call(e, "entity.create", R"J({"name": "Ground", "components": {"Transform": {"scale": [10, 1, 10]}, "MeshRenderer": {"mesh": "plane"}}})J");
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"Transform": {"position": [0, 1, 0], "scale": [1.5, 1.5, 1.5]}, "MeshRenderer": {"mesh": "sphere"}}})J");
    Vec3 eye(0, 6, 7), at(0, 0, 0);
    RenderTarget withShadow = RenderLook(e, eye, at);
    Call(e, "component.set", R"J({"id": "Sun", "type": "DirectionalLight", "values": {"shadows": false}})J");
    RenderTarget noShadow = RenderLook(e, eye, at);
    CHECK(MeanLuma(withShadow) < MeanLuma(noShadow) - 0.5);
    Call(e, "component.set", R"J({"id": "Ball", "type": "MeshRenderer", "values": {"shading": "flat"}})J");
    RenderTarget flat = RenderLook(e, eye, at);
    CHECK(flat.Hash() != noShadow.Hash());
    Call(e, "entity.create", R"J({"name": "Lamp", "components": {"Transform": {"position": [2, 0.5, 2]}, "PointLight": {"intensity": 3, "range": 4}}})J");
    RenderTarget lamp = RenderLook(e, eye, at);
    CHECK(MeanLuma(lamp) > MeanLuma(flat) + 1.0);
}

TEST(OrthographicAndFollowCamera) {
    Engine e;
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Cam", "components": {"Transform": {"position": [0, 0, 5]}, "Camera": {"projection": "orthographic", "orthoSize": 3}}})J");
    Call(e, "entity.create", R"J({"name": "Box", "components": {"MeshRenderer": {}}})J");
    EntityId box = e.GetScene().FindByName("Box");
    auto coverage = [&]() {
        RenderTarget rt;
        rt.Resize(160, 90);
        e.RenderGameView(rt);
        return CountId(rt, box);
    };
    size_t near1 = coverage();
    Call(e, "component.set", R"J({"id": "Box", "type": "Transform", "values": {"position": [0, 0, -10]}})J");
    size_t far1 = coverage();
    CHECK(near1 > 200 && near1 == far1);  // orthographic: size does not depend on distance
    Call(e, "component.set", R"J({"id": "Cam", "type": "Camera", "values": {"projection": "perspective"}})J");
    CHECK(coverage() < far1 / 4);

    // CameraFollow keeps the camera at target + offset and aims at the target.
    Call(e, "entity.create", R"J({"name": "Runner", "components": {"Transform": {"position": [0, 0.5, 0]}, "Velocity": {"linear": [3, 0, 0]}}})J");
    EntityId runner = e.GetScene().FindByName("Runner");
    Json args = Json::parse(R"J({"id": "Cam", "type": "CameraFollow", "values": {"offset": [0, 3, 6], "smoothing": 0}})J");
    args["values"]["target"] = runner;
    e.Call("component.add", args);
    Call(e, "sim.step", R"J({"frames": 60})J");
    Vec3 cam = PosOf(e, "Cam"), run = PosOf(e, "Runner");
    CHECK(Near(cam, run + Vec3(0, 3, 6), 1e-3f));
    Vec3 fwd = ForwardFromEuler(e.GetScene().Get<Transform>(e.GetScene().FindByName("Cam"))->rotation);
    CHECK(Dot(fwd, Normalize(run + Vec3(0, 0.5f, 0) - cam)) > 0.999f);
}

TEST(DebugDrawing) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("debugdraw"), &err));
    e.Call("scene.new", Json());
    auto hash = [&]() { return Call(e, "render.screenshot", R"J({"width": 96, "height": 54, "inline": false})J")["result"]["hash"].asString(); };
    Call(e, "sim.step", R"J({"frames": 1})J");
    std::string clean = hash();
    CHECK(Call(e, "debug.draw", R"J({"boxes": [{"center": [0, 1, 0], "size": [2, 2, 2], "color": [1, 0, 0]}], "seconds": 0.5})J")["result"]["added"].asInt() == 12);
    CHECK(hash() != clean);
    Call(e, "sim.step", R"J({"frames": 40})J");  // expired
    CHECK(hash() == clean);
    // Lua: 0-second lines last one simulated frame.
    Call(e, "script.eval", R"J({"code": "draw.sphere({x = 0, y = 1, z = 0}, 1, {1, 1, 0})"})J");
    CHECK(e.DebugLineCount() == 72);
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(e.DebugLineCount() == 0);
}

TEST(ShowcaseFoxModel) {
    // Completion check for rendering: the external glTF character (samples/Showcase) is drawn with its texture.
    Engine e;
    std::string err;
    CHECK(e.Open(std::string(OE_SOURCE_DIR) + "/samples/Showcase", &err));
    Json info = Call(e, "asset.info", R"J({"path": "assets/models/fox.glb"})J")["result"];
    CHECK(info["triangles"].asInt() == 576 && info["textures"][0][0].asInt() == 1024);
    RenderTarget rt;
    rt.Resize(320, 180);
    e.RenderGameView(rt);
    EntityId fox = e.GetScene().FindByName("Fox");
    CHECK(CountId(rt, fox) > 100);
    // Textured: the fox's pixels are not all one color.
    std::set<uint32_t> colors;
    for (size_t i = 0; i < rt.ids.size(); ++i) {
        if (rt.ids[i] == fox) colors.insert(rt.color[i]);
    }
    CHECK(colors.size() > 20);
    CHECK(e.Scripts().Errors().empty());
    // Multi-threaded rendering is bit-identical to a single thread.
    SetMaxRenderThreads(1);
    RenderTarget single;
    single.Resize(320, 180);
    e.RenderGameView(single);
    SetMaxRenderThreads(16);
    CHECK(single.Hash() == rt.Hash());
}

TEST(UIQuadsAreWhatSoftwareDraws) {
    // Both renderers draw UI from BuildUIQuads; the software result is the reference.
    Engine e;
    std::string err;
    CHECK(e.Open(std::string(OE_SOURCE_DIR) + "/samples/Showcase", &err));
    std::vector<UIQuad> quads = BuildUIQuads(e.GetScene(), 640, 360);
    CHECK(quads.size() > 100);  // every lit font pixel is a quad
    RenderTarget rt;
    rt.Resize(640, 360);
    e.RenderGameView(rt);
    for (const UIQuad& q : quads) {
        CHECK(q.x1 > q.x0 && q.y1 > q.y0);
        CHECK(rt.IdAt(q.x0, q.y0) == q.entity);
    }
}

TEST(GpuRendererMatchesSoftware) {
    Engine e;
    std::string err;
    CHECK(e.Open(std::string(OE_SOURCE_DIR) + "/samples/Showcase", &err));
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    CHECK(&e.DisplayRenderer() == e.Gpu());
    const int w = 320, h = 180;
    RenderView view;
    MakeSceneView(e.GetScene(), static_cast<float>(w) / h, view);
    view.highlight = e.GetScene().FindByName("Crate 1");
    RenderTarget sw, gpu;
    sw.Resize(w, h);
    gpu.Resize(w, h);
    e.Renderer().Render(e.GetScene(), view, sw);
    e.Gpu()->Render(e.GetScene(), view, gpu);
    // Same image up to anti-aliasing and filtering: small mean difference, few outliers.
    double total = 0;
    int outliers = 0, orangeSw = 0, orangeGpu = 0;
    auto orange = [](uint32_t c) { return (c & 0xFF) > 230 && ((c >> 8) & 0xFF) > 140 && ((c >> 8) & 0xFF) < 175 && ((c >> 16) & 0xFF) < 50; };
    for (size_t i = 0; i < sw.color.size(); ++i) {
        int worst = 0;
        for (int ch = 0; ch < 3; ++ch) {
            int d = std::abs(static_cast<int>((sw.color[i] >> (8 * ch)) & 0xFF) - static_cast<int>((gpu.color[i] >> (8 * ch)) & 0xFF));
            total += d;
            worst = std::max(worst, d);
        }
        outliers += worst > 64;
        orangeSw += orange(sw.color[i]);
        orangeGpu += orange(gpu.color[i]);
    }
    double mean = total / (static_cast<double>(sw.color.size()) * 3.0);
    std::printf("  %s: mean channel difference %.2f, outliers %d of %zu\n", e.Gpu()->Name(), mean, outliers, sw.color.size());
    CHECK(mean < 6.0);
    CHECK(outliers < static_cast<int>(sw.color.size()) / 25);
    CHECK(orangeSw > 30 && orangeGpu > 30);  // selection outline on both
    // Rendering again (cached meshes/textures) gives the same frame.
    RenderTarget again;
    again.Resize(w, h);
    e.Gpu()->Render(e.GetScene(), view, again);
    CHECK(again.Hash() == gpu.Hash());
    // render.screenshot {renderer: gpu} goes through the same renderer.
    Json shot = Call(e, "render.screenshot", R"J({"renderer": "gpu", "width": 64, "height": 36, "inline": false})J");
    CHECK(shot["ok"].asBool() && shot["result"]["renderer"].asString() == e.Gpu()->Name());
    CHECK(!Call(e, "render.screenshot", R"J({"renderer": "vulkan", "inline": false})J")["ok"].asBool());
}

TEST(WebGamePak) {
    // `oe package --web` data: the page unpacks game.pak into /game.
    std::string dir = std::string(OE_SOURCE_DIR) + "/samples/Hello";
    std::vector<std::string> files = GameFiles(dir);
    CHECK(std::find(files.begin(), files.end(), "project.json") != files.end());
    CHECK(std::find(files.begin(), files.end(), "AGENTS.md") == files.end());
    std::string pak = "build/test_web/game.pak";
    std::string err;
    CHECK(WriteGamePak(dir, files, pak, &err));
    std::vector<unsigned char> bytes;
    CHECK(ReadBinaryFile(pak, bytes) && bytes.size() > 12);
    CHECK(std::string(bytes.begin(), bytes.begin() + 8) == "OEPAK001");
    uint32_t n = bytes[8] | (bytes[9] << 8) | (bytes[10] << 16) | (static_cast<uint32_t>(bytes[11]) << 24);
    Json index = Json::parse(std::string(bytes.begin() + 12, bytes.begin() + 12 + n), &err);
    CHECK(err.empty() && index["files"].size() == files.size());
    for (const Json& f : index["files"].items()) {
        std::vector<unsigned char> original;
        CHECK(ReadBinaryFile(dir + "/" + f["path"].asString(), original));
        size_t at = 12 + n + static_cast<size_t>(f["offset"].asNumber());
        CHECK(original.size() == static_cast<size_t>(f["size"].asNumber()));
        CHECK(at + original.size() <= bytes.size() && std::equal(original.begin(), original.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at)));
    }
    RemoveAll("build/test_web");
}

TEST(JpegEncoder) {
    // The editor viewport stream sends JPEG frames.
    Image img;
    img.width = 32;
    img.height = 16;
    img.rgba.assign(32 * 16 * 4, 200);
    std::vector<uint8_t> jpg = EncodeJpeg(img, 85);
    CHECK(jpg.size() > 100 && jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[jpg.size() - 2] == 0xFF && jpg.back() == 0xD9);
}

}  // namespace

int main() {
    Log::SetEcho(false);
    for (const TestCase& t : Tests()) {
        int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
    }
    std::printf("\n%zu tests, %d failed checks\n", Tests().size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
