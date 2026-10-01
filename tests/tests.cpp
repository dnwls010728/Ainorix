// Engine self tests. Run: build/bin/oe_tests  (exit code 0 = all passed)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "app/AndroidPackage.h"
#include "app/Engine.h"
#include "app/Project.h"
#include "assets/Assets.h"
#include "audio/Wav.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Zip.h"
#include "editor/EditorMath.h"
#include "render/Font.h"
#include "render/GpuRenderer.h"
#include "render/UI.h"
#include "physics/Physics2D.h"
#include "scene/Components.h"
#include "scene/TileGrid.h"
#include "script/ScriptHost.h"
#if OE_NATIVE_EDITOR
#include "editor/Editor.h"
#include "editor/EditorText.h"
#endif

using namespace oe;

namespace {

// Android test binaries cannot read the build host's source directory.
// Set OE_TEST_SOURCE_DIR to the staged fixtures; other platforms use the source tree.
std::string TestSourceDir() {
    const char* dir = std::getenv("OE_TEST_SOURCE_DIR");
    return dir && *dir ? dir : OE_SOURCE_DIR;
}

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
    CHECK(AbsolutePath("C:/ownengine/game/../assets") == "C:/ownengine/assets");
    CHECK(RelativePath("C:/ownengine/assets/model.glb", "C:/ownengine") == "assets/model.glb");
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
    // Keep fixtures under the staged source tree: Android shell and Node on
    // Windows do not necessarily have a writable POSIX /tmp directory.
    fs::path dir = fs::path(TestSourceDir()) / "build/test_projects" / (std::string("oe_tests_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string d = dir.generic_string();
    std::string err;
    CHECK(CreateProject(d, name, &err));
    if (!err.empty()) std::printf("  project fixture: %s\n", err.c_str());
    return d;
}

Json Call(Engine& e, const char* cmd, const char* args) { return e.Call(cmd, Json::parse(args)); }

bool Near(const Vec3& a, const Vec3& b, float eps) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

TEST(SaveSlotsPersistAndRecover) {
    const std::string project = TempProject("save_slots");
    const std::string directory = JoinPath(project, "saves");
    std::string error;
    {
        Engine e;
        CHECK(e.Open(project, &error));
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.set", R"J({"key":"score","value":17})J")["ok"].asBool());
        CHECK(Call(e, "save.set", R"J({"key":"score","value":{"n":9,"items":[true,null,"한글"]},"slot":"second"})J")["ok"].asBool());
        CHECK(!FileExists(directory));  // lazy I/O; no directory before flush
        CHECK(Call(e, "save.flush", "{}")["ok"].asBool());
        CHECK(Call(e, "save.flush", R"J({"slot":"second"})J")["ok"].asBool());
        CHECK(!FileExists(JoinPath(directory, "slot-default.json.tmp")));
        CHECK(Call(e, "save.set", R"J({"key":"score","value":42})J")["ok"].asBool());
        CHECK(Call(e, "save.flush", "{}")["ok"].asBool());  // replacement, not only first write
        Json deep = 7;
        for (int depth = 0; depth < 32; ++depth) { Json array = Json::MakeArray(); array.push(deep); deep = std::move(array); }
        Json args = Json::MakeObject(); args["key"] = "deep"; args["value"] = deep;
        CHECK(e.Call("save.set", args)["ok"].asBool());
        CHECK(Call(e, "save.flush", "{}")["ok"].asBool());
        CHECK(e.UndoDepth() == 0);
    }
    {
        Engine e;
        CHECK(e.Open(project, &error));
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.state", "{}")["result"]["data"]["score"].asInt() == 42);
        CHECK(Call(e, "save.state", "{}")["result"]["data"]["deep"].isArray());
        CHECK(Call(e, "save.state", R"J({"slot":"second"})J")["result"]["data"]["score"]["n"].asInt() == 9);
        CHECK(!Call(e, "save.state", R"J({"slot":"../escape"})J")["ok"].asBool());
        CHECK(!Call(e, "save.set", R"J({"key":"","value":1})J")["ok"].asBool());
        Call(e, "save.clear", R"J({"key":"score"})J");
        Call(e, "save.flush", "{}");
        e.Saves().Configure(directory);
        CHECK(!Call(e, "save.state", "{}")["result"]["data"].has("score"));
        CHECK(WriteTextFile(JoinPath(directory, "slot-default.json"), "{broken"));
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.state", "{}")["result"]["data"].size() == 0);
        CHECK(WriteTextFile(JoinPath(directory, "slot-default.json"), "[1,2]"));
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.state", "{}")["result"]["data"].size() == 0);
        CHECK(Call(e, "save.set", R"J({"key":"recovered","value":true})J")["ok"].asBool());
        CHECK(Call(e, "save.flush", "{}")["ok"].asBool());
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.state", "{}")["result"]["data"]["recovered"].asBool());
    }
    CHECK(RemoveAll(project));
}

TEST(SaveMemoryAndLuaValidation) {
    const std::string project = TempProject("save_memory");
    Engine e;
    std::string error;
    CHECK(e.Open(project, &error));
    const auto before = ListFiles(project, "", true);
    CHECK(Call(e, "save.state", "{}")["result"]["mode"].asString() == "memory");
    CHECK(Call(e, "script.eval", R"J({"code":"save.set('score', 5) save.set('other', {1, 'two', false}, 'second') save.set('null', nil) save.flush() return save.get('score', 0)"})J")["result"]["value"].asInt() == 5);
    CHECK(Call(e, "script.eval", R"J({"code":"return save.get('absent', 8)"})J")["result"]["value"].asInt() == 8);
    CHECK(Call(e, "script.eval", R"J({"code":"return save.get('null', 8)"})J")["result"]["value"].isNull());
    CHECK(Call(e, "save.state", R"J({"slot":"second"})J")["result"]["data"]["other"].size() == 3);
    for (const char* code : {"save.set('bad', function() end)", "save.set('bad', 0/0)",
                             "save.get('', 0)", "save.set('bad' .. string.char(0), 1)",
                             "save.set('bad', 1, 'default' .. string.char(0))",
                             "local t = {} t.self = t save.set('bad', t)",
                             "save.set('bad', {[1] = 1, named = 2})", "save.set('bad', {[3] = 1})"}) {
        Json args = Json::MakeObject(); args["code"] = code;
        CHECK(!e.Call("script.eval", args)["ok"].asBool());
    }
    CHECK(!Call(e, "save.state", "{}")["result"]["data"].has("bad"));
    e.Step(1);
    e.Stop();
    CHECK(Call(e, "save.state", "{}")["result"]["data"]["score"].asInt() == 5);
    CHECK(Call(e, "script.eval", R"J({"code":"save.delete('score') save.flush()"})J")["ok"].asBool());
    CHECK(!Call(e, "save.state", "{}")["result"]["data"].has("score"));
    CHECK(ListFiles(project, "", true) == before);
    CHECK(RemoveAll(project));
}

TEST(SaveFailedFlushPreservesData) {
    const std::string project = TempProject("save_failure");
    const std::string directory = JoinPath(project, "saves");
    Engine e;
    e.Saves().Configure(directory);
    Call(e, "save.set", R"J({"key":"score","value":7})J");
    CHECK(Call(e, "save.flush", "{}")["ok"].asBool());
    std::string original;
    CHECK(ReadTextFile(JoinPath(directory, "slot-default.json"), original));
    // A directory where the temporary file belongs forces a deterministic write failure.
    CHECK(CreateDirectories(JoinPath(directory, "slot-default.json.tmp")));
    Call(e, "save.set", R"J({"key":"score","value":8})J");
    CHECK(Call(e, "save.flush", "{}")["error"]["code"].asString() == "save_write_failed");
    std::string after;
    CHECK(ReadTextFile(JoinPath(directory, "slot-default.json"), after));
    CHECK(after == original);
    CHECK(Call(e, "save.state", "{}")["result"]["dirty"].asBool());
    CHECK(RemoveAll(project));
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

TEST(MouseLookInput) {
    // Relative mouse motion reaches scripts for exactly one step; the lock flag
    // is set by scripts, visible to tools and released by input.mouse / sim.stop.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("mouselook"), &err));
    Call(e, "script.write", R"J({"path": "scripts/look.lua", "source": "local M = {}\nfunction M:onStart() self.yaw = 0 input.lockMouse() end\nfunction M:onUpdate(dt) local dx, dy = input.mouseDelta() self.yaw = self.yaw - dx * 0.1 self.lastDy = dy end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Looker", "components": {"Script": {"path": "scripts/look.lua"}}})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(Call(e, "sim.state", "{}")["result"]["mouseLocked"].asBool());
    Call(e, "input.mouse", R"J({"dx": 30, "dy": -4})J");
    Call(e, "input.mouse", R"J({"dx": 20})J");  // accumulates until the next step
    Call(e, "sim.step", R"J({"frames": 3})J");
    Json r = Call(e, "script.eval", R"J({"code": "{self.yaw, self.lastDy}", "entity": "Looker"})J");
    CHECK(std::fabs(r["result"]["value"][0].asNumber() + 5.0) < 1e-6);  // 50 px * 0.1, applied once
    CHECK(r["result"]["value"][1].asNumber() == 0.0);                  // cleared after the step
    CHECK(Call(e, "input.mouse", R"J({"locked": false})J")["result"]["mouseLocked"].asBool() == false);
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(!Call(e, "script.eval", R"J({"code": "input.mouseLocked()"})J")["result"]["value"].asBool());
    Call(e, "input.mouse", R"J({"locked": true})J");
    Call(e, "sim.stop", "{}");
    CHECK(!Call(e, "sim.state", "{}")["result"]["mouseLocked"].asBool());
}

TEST(Sprites2DAndTilemaps) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("sprites2d"), &err));
    // 4x2 sheet with two 2x2 frames: frame 0 has only its top-left texel opaque (red),
    // frame 1 is fully opaque green.
    Image sheet;
    sheet.width = 4;
    sheet.height = 2;
    sheet.rgba.assign(4 * 2 * 4, 0);
    auto px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t a) {
        uint8_t* p = &sheet.rgba[static_cast<size_t>((y * 4 + x) * 4)];
        p[0] = r; p[1] = g; p[2] = 0; p[3] = a;
    };
    px(0, 0, 255, 0, 255);
    for (int y = 0; y < 2; ++y)
        for (int x = 2; x < 4; ++x) px(x, y, 0, 255, 255);
    CHECK(WritePng(JoinPath(e.ProjectDir(), "sheet.png"), sheet, true));

    Call(e, "scene.new", "{}");
    Call(e, "component.set", R"J({"id": "Main Camera", "type": "Transform", "values": {"position": [0, 0, 10], "rotation": [0, 0, 0]}})J");
    Call(e, "component.set", R"J({"id": "Main Camera", "type": "Camera", "values": {"projection": "orthographic", "orthoSize": 1}})J");
    Call(e, "entity.create", R"J({"name": "S", "components": {"Transform": {}, "Sprite": {"texture": "sheet.png", "columns": 2, "pixelsPerUnit": 2}}})J");
    EntityId s = e.GetScene().FindByName("S");
    // The 1x1 unit sprite covers pixels 16..48 of a 64x64 view; picking uses the same cut-out rules as drawing.
    auto pick = [&](int x, int y) {
        Json a = Json::MakeObject();
        a["x"] = x; a["y"] = y; a["width"] = 64; a["height"] = 64;
        return static_cast<EntityId>(e.Call("render.pick", a)["result"]["id"].asInt());
    };
    CHECK(pick(20, 20) == s);           // opaque texel of frame 0
    CHECK(pick(44, 44) == kNullEntity); // transparent texel: cut out
    CHECK(pick(44, 20) == kNullEntity);
    Call(e, "component.set", R"J({"id": "S", "type": "Sprite", "values": {"flipX": true}})J");
    CHECK(pick(44, 20) == s && pick(20, 20) == kNullEntity);
    Call(e, "component.set", R"J({"id": "S", "type": "Sprite", "values": {"frame": 1, "flipX": false}})J");
    CHECK(pick(44, 44) == s);

    // SpriteAnimation drives Sprite.frame on simulated time; non-looping clips stop at the end.
    Call(e, "component.add", R"J({"id": "S", "type": "SpriteAnimation", "values": {"clip": "a", "clips": {"a": {"frames": [0, 1], "fps": 10, "loop": false}}}})J");
    Call(e, "sim.step", R"J({"frames": 3})J");
    CHECK(e.GetScene().Get<Sprite>(s)->frame == 0);
    Call(e, "sim.step", R"J({"frames": 4})J");
    CHECK(e.GetScene().Get<Sprite>(s)->frame == 1);
    CHECK(e.GetScene().Get<SpriteAnimation>(s)->finished);
    Call(e, "sim.stop", "{}");

    // Tilemap: rows go down from the entity origin; solid cells collide (a 2D character lands on them).
    Call(e, "entity.create", R"J({"name": "Map", "components": {"Transform": {}, "Tilemap": {"tileset": "sheet.png", "columns": 2,
        "map": ["....", "#..#", "####"], "legend": {"#": 1}, "solid": "#"}}})J");
    Call(e, "entity.create", R"J({"name": "Hero", "components": {"Transform": {"position": [1.5, 0, 0]},
        "CharacterBody": {"shape": "sphere", "radius": 0.3, "plane2D": true, "velocity": [0, 0, 5]}}})J");
    Call(e, "sim.step", R"J({"frames": 90})J");
    const Transform* hero = e.GetScene().Get<Transform>(e.GetScene().FindByName("Hero"));
    CHECK(std::fabs(hero->position.y + 1.7f) < 0.05f);  // resting on row 2 (top at y = -2)
    CHECK(std::fabs(hero->position.x - 1.5f) < 0.01f);
    CHECK(hero->position.z == 0.0f);                    // plane2D ignores velocity.z
    CHECK(e.GetScene().Get<CharacterBody>(e.GetScene().FindByName("Hero"))->grounded);
    Json r = Call(e, "script.eval", R"J({"code": "{tilemap.get('Map', 0, 1), tilemap.solid('Map', 1, 1), tilemap.get('Map', 9, 9)}"})J");
    CHECK(r["result"]["value"][0].asString() == "#" && !r["result"]["value"][1].asBool());
    // Changing a tile from Lua updates collision: a ray down column 2 now stops on row 1.
    Call(e, "script.eval", R"J({"code": "tilemap.set('Map', 2, 1, '#')"})J");
    r = Call(e, "physics.raycast", R"J({"origin": [2.5, 0, 0], "direction": [0, -1, 0]})J");
    CHECK(r["result"]["entity"].asInt() == static_cast<int>(e.GetScene().FindByName("Map")));
    CHECK(std::fabs(r["result"]["point"][1].asNumber() + 1.0) < 0.02);
    r = Call(e, "script.eval", R"J({"code": "local c, r = tilemap.cellAt('Map', {x = 2.5, y = -1.2, z = 0}) return {c, r}"})J");
    CHECK(r["result"]["value"][0].asInt() == 2 && r["result"]["value"][1].asInt() == 1);

    // Jumping into a ceiling ends the upward motion right away.
    Call(e, "script.eval", R"J({"code": "tilemap.set('Map', 1, 0, '#')"})J");
    Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody", "values": {"velocity": [0, 12, 0]}})J");
    Call(e, "sim.step", R"J({"frames": 12})J");
    CHECK(e.GetScene().Get<CharacterBody>(e.GetScene().FindByName("Hero"))->velocity.y <= 0.0f);
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
    Json rewritten = Call(e, "script.write", R"J({"path": "scripts/counter.lua", "source": "local M = {}\nfunction M:onStart() self.count = 0 end\nfunction M:onUpdate(dt) self.count = self.count + 100 end\nreturn M\n"})J");
    CHECK(rewritten["result"]["reloaded"].size() == 1);
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
    CHECK(Call(e, "save.state", "{}")["result"]["data"]["highScore"].asInt() == 7);

    // The button is centered, 60 reference px below the middle: (320, 210) in a 640x360 shot.
    Json click = Call(e, "input.click", R"J({"x": 320, "y": 210})J");
    CHECK(click["result"]["buttonName"].asString() == "Play Again");
    Call(e, "sim.step", R"J({"frames": 2})J");
    CHECK(e.GetScene().name == "Level 1");
    CHECK(Call(e, "game.state", "{}")["result"]["data"]["total"].asInt() == 0);
    CHECK(TextOf(e, "Score")->text.find("Best: 7") != std::string::npos);
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
    CHECK(Call(e, "save.state", "{}")["result"]["data"]["highScore"].asInt() == 7);
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
    Call(e, "entity.create", R"J({"name": "Label", "components": {"UIText": {"text": "HI", "font": "pixel", "x": 0, "y": 0, "size": 80, "color": [1, 1, 1]}}})J");
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

TEST(MultiTouchButtonKeys) {
    // On-screen controls: UIButton.key holds a key while any finger (or the mouse) is on the button.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("touch"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "script.write", R"J({"path": "scripts/pad.lua", "source": "local M = {}\nfunction M:onUpdate()\n  if input.pressed('Space') then jumps = (jumps or 0) + 1 end\n  left = input.down('Left')\n  fingers = #input.touches()\n  local t = input.touches()[1]\n  firstBegan = t and t.began or false\nend\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Pad", "components": {"Script": {"path": "scripts/pad.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "LeftBtn", "components": {"UIButton": {"anchor": "bottom-left", "x": 10, "y": -10, "width": 100, "height": 100, "key": "Left"}}})J");
    Call(e, "entity.create", R"J({"name": "JumpBtn", "components": {"UIButton": {"anchor": "bottom-right", "x": -10, "y": -10, "width": 100, "height": 100, "key": "Space"}}})J");
    auto eval = [&](const char* code) { return Call(e, "script.eval", (std::string(R"J({"code": ")J") + code + "\"}").c_str())["result"]["value"]; };
    // Finger 1 holds Left, finger 2 taps Jump at the same time (1280x720 view).
    CHECK(Call(e, "input.touch", R"J({"id": 1, "x": 60, "y": 660, "width": 1280, "height": 720})J")["ok"].asBool());
    Call(e, "input.touch", R"J({"id": 2, "x": 1220, "y": 660, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(eval("left").asBool() && eval("jumps").asInt() == 1 && eval("fingers").asInt() == 2 && eval("firstBegan").asBool());
    CHECK(e.GetScene().Get<UIButton>(e.GetScene().FindByName("JumpBtn"))->pressed);
    Call(e, "sim.step", R"J({"frames": 3})J");
    CHECK(eval("jumps").asInt() == 1 && !eval("firstBegan").asBool());  // held: pressed only once
    Call(e, "input.touch", R"J({"id": 2, "down": false})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    Call(e, "input.touch", R"J({"id": 2, "x": 1220, "y": 660, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(eval("jumps").asInt() == 2 && eval("left").asBool());
    // Sliding finger 1 off the button releases Left; lifting everything clears the keys.
    Call(e, "input.touch", R"J({"id": 1, "x": 640, "y": 300, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(!eval("left").asBool());
    Call(e, "input.touch", R"J({"id": 1, "down": false})J");
    Call(e, "input.touch", R"J({"id": 2, "down": false})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(eval("fingers").asInt() == 0 && !e.Input().IsDown("Space") && !e.Input().IsDown("Left"));
    // The mouse works the same way (desktop, editor Game view).
    Call(e, "input.mouse", R"J({"x": 60, "y": 660, "width": 1280, "height": 720, "button": "MouseLeft", "down": true})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(eval("left").asBool());
    CHECK(!Call(e, "input.touch", R"J({"id": 9})J")["ok"].asBool());  // a new finger needs a position
    CHECK(e.Scripts().Errors().empty());
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

// ----- 2D physics (Box2D) & tiles --------------------------------------------------

Tilemap MakeMap(std::initializer_list<const char*> rows, const char* legend, const char* solid = "") {
    Tilemap tm;
    tm.map = Json::MakeArray();
    for (const char* r : rows) tm.map.push(std::string(r));
    tm.legend = Json::parse(legend);
    tm.solid = solid;
    return tm;
}

int FrameAt(const TileFrames& f, int col, int row) { return f.frames[static_cast<size_t>(row * f.width + col)]; }

float LoopArea(const std::vector<TilePoint>& loop) {
    float a = 0;
    for (size_t i = 0; i < loop.size(); ++i) a += loop[i].x * loop[(i + 1) % loop.size()].y - loop[(i + 1) % loop.size()].x * loop[i].y;
    return 0.5f * a;
}

TEST(TileRulesAndAutotiling) {
    // 47 blob patterns: nothing connected is the first, everything the last.
    CHECK(BlobMasks().size() == 47);
    CHECK(BlobIndex(0) == 0 && BlobIndex(255) == 46);
    CHECK(BlobIndex(2) == 0);  // a lone corner without both sides does not count
    // "sides": 4 neighbours (N=1 E=2 S=4 W=8) pick one of 16 frames; outside the map connects by default.
    Tilemap ring = MakeMap({"###", "# #", "###"}, R"J({"#": {"autotile": "sides", "frame": 16}})J");
    TileRules rules = BuildTileRules(ring, nullptr);
    TileFrames f = ResolveFrames(ring, rules);
    CHECK(f.width == 3 && f.height == 3);
    CHECK(FrameAt(f, 1, 1) == -1);             // the hole
    CHECK(FrameAt(f, 0, 0) == 16 + 15);        // N, W outside + E, S
    CHECK(FrameAt(f, 1, 0) == 16 + 1 + 2 + 8); // N outside, E, W (S is the hole)
    ring.legend = Json::parse(R"J({"#": {"autotile": "sides", "frame": 16, "edges": false}})J");
    f = ResolveFrames(ring, BuildTileRules(ring, nullptr));
    CHECK(FrameAt(f, 0, 0) == 16 + 2 + 4);
    // Blob with connects: '=' counts as the same terrain.
    Tilemap blob = MakeMap({"#=", "##"}, R"J({"#": {"autotile": "blob", "frames": [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46], "connects": "=", "edges": false}, "=": 99})J");
    f = ResolveFrames(blob, BuildTileRules(blob, nullptr));
    CHECK(FrameAt(f, 0, 0) == BlobIndex(4 | 8 | 16));  // E ('='), SE, S
    CHECK(FrameAt(f, 1, 0) == 99);
    // Variants: stable per cell, and they do vary.
    Tilemap grass = MakeMap({"..........", ".........."}, R"J({".": {"variants": [5, 6, 7]}})J");
    TileFrames g1 = ResolveFrames(grass, BuildTileRules(grass, nullptr)), g2 = ResolveFrames(grass, BuildTileRules(grass, nullptr));
    CHECK(g1.frames == g2.frames);
    std::set<int> seen(g1.frames.begin(), g1.frames.end());
    CHECK(seen.size() >= 2 && !seen.count(-1) && *seen.begin() >= 5 && *seen.rbegin() <= 7);
    // Rule errors are reported, the rest keeps working.
    Tilemap bad = MakeMap({"#"}, R"J({"#": {"autotile": "blob", "frames": [1, 2]}})J");
    CHECK(BuildTileRules(bad, nullptr).error.find("47") != std::string::npos);

    // Collision: solid outlines (holes wind the other way), one-way runs, shaped cells.
    Tilemap level = MakeMap({"#####", "#   #", "#####", "  ==/", "   /#"}, R"J({"=": {"frame": 1, "collision": "oneway"}, "/": {"collision": "slope-up"}})J", "#");
    TileRules lr = BuildTileRules(level, nullptr);
    std::vector<std::vector<TilePoint>> loops = SolidOutlines(level, lr);
    CHECK(loops.size() == 3);  // ring outside, ring hole, lone block
    int outer = 0, holes = 0;
    for (const auto& l : loops) (LoopArea(l) > 0 ? outer : holes) += 1;
    CHECK(outer == 2 && holes == 1);
    CHECK(loops[0].size() == 4 && std::fabs(LoopArea(loops[0]) - 15.0f) < 1e-4f);  // 5x3 block, collinear corners removed
    std::vector<TileRect> runs = OneWayRuns(level, lr);
    CHECK(runs.size() == 1 && runs[0].col == 2 && runs[0].row == 3 && runs[0].width == 2);
    std::vector<TileShape> shaped = ShapedCells(level, lr);
    CHECK(shaped.size() == 2 && shaped[0].col == 4 && shaped[0].row == 3 && shaped[0].points.size() == 3);
    CHECK(CollisionAt(level, lr, 0, 0) == TileCollision::Solid && CollisionAt(level, lr, 2, 3) == TileCollision::OneWay);
    CHECK(SolidRects(level, lr).size() == 5);

    // Concave polygons are split into convex pieces.
    std::vector<std::vector<TilePoint>> pieces = ConvexPieces({{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 3}, {0, 3}});  // L shape
    float area = 0;
    for (const auto& piece : pieces) area += LoopArea(piece);
    CHECK(pieces.size() == 2 && std::fabs(area - 5.0f) < 1e-4f);
    CHECK(ConvexPieces({{0, 0}, {1, 0}, {1, 1}, {0, 1}}).size() == 1);
}

TEST(TilesetFilesAndPaintCommands) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("tilesets"), &err));
    Call(e, "scene.new", R"J({"empty": true})J");
    // tileset.create validates the rules.
    Json bad = Call(e, "tileset.create", R"J({"path": "tilesets/t.tileset.json", "image": "t.png", "columns": 8, "rows": 8, "tiles": {"#": {"autotile": "sides"}}})J");
    CHECK(!bad["ok"].asBool() && bad["error"]["code"].asString() == "invalid_tileset");
    Json ok = Call(e, "tileset.create", R"J({"path": "tilesets/t.tileset.json", "image": "t.png", "columns": 8, "rows": 8,
        "tiles": {"#": {"autotile": "sides", "frame": 16, "collision": "solid"}, "=": {"frame": 2, "collision": "oneway"}, ".": {"variants": [0, 1]}}})J");
    CHECK(ok["ok"].asBool());
    CHECK(Call(e, "asset.info", R"J({"path": "tilesets/t.tileset.json"})J")["result"]["tiles"].asString() == "#.=");
    Call(e, "entity.create", R"J({"name": "Map", "components": {"Transform": {}, "Tilemap": {"tileset": "tilesets/t.tileset.json", "map": ["", "", ""], "legend": {"=": 3}}}})J");
    // The component's legend overrides a rule's frame and keeps its collision.
    Json info = Call(e, "tilemap.info", R"J({"id": "Map"})J")["result"];
    CHECK(info["columns"].asInt() == 8 && info["image"].asString() == "t.png");
    CHECK(info["tiles"].size() == 3);
    CHECK(info["tiles"][2]["char"].asString() == "=" && info["tiles"][2]["frame"].asInt() == 3 && info["tiles"][2]["collision"].asString() == "oneway");
    CHECK(info["tiles"][0]["frame"].asInt() == 31 && info["tiles"][0]["autotile"].asString() == "sides");

    // Painting: one undo step per merge key.
    size_t depth = e.UndoDepth();
    Call(e, "tilemap.paint", R"J({"id": "Map", "char": "#", "cells": [[0, 2], [1, 2]], "merge": "stroke1"})J");
    Call(e, "tilemap.paint", R"J({"id": "Map", "char": "#", "cells": [[2, 2], [3, 2]], "merge": "stroke1"})J");
    Json fill = Call(e, "tilemap.fill", R"J({"id": "Map", "char": "=", "col": 1, "row": 0, "width": 2, "height": 1})J");
    CHECK(fill["result"]["changed"].asInt() == 2);
    const Tilemap* tm = e.GetScene().Get<Tilemap>(e.GetScene().FindByName("Map"));
    CHECK(tm->map[0].asString() == " ==" && tm->map[2].asString() == "####");
    CHECK(e.UndoDepth() == depth + 2);
    Call(e, "history.undo", "{}");
    tm = e.GetScene().Get<Tilemap>(e.GetScene().FindByName("Map"));
    CHECK(tm->map[0].asString() == "" && tm->map[2].asString() == "####");
    CHECK(!Call(e, "tilemap.paint", R"J({"id": "Map", "char": "##", "cells": [[0, 0]]})J")["ok"].asBool());

    // Tile collision from the file: a 2D ball rests on the painted row (top at y = -2).
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"Transform": {"position": [1.5, 0, 0]}, "Collider2D": {"shape": "circle", "radius": 0.25}, "RigidBody2D": {}}})J");
    Call(e, "sim.step", R"J({"frames": 120})J");
    CHECK(std::fabs(PosOf(e, "Ball").y + 1.75f) < 0.02f);
    // Lua sees the same rules.
    Json r = Call(e, "script.eval", R"J({"code": "local w, h = tilemap.size('Map') return {tilemap.collision('Map', 0, 2), tilemap.solid('Map', 0, 1), w, h}"})J");
    CHECK(r["result"]["value"][0].asString() == "solid" && !r["result"]["value"][1].asBool());
    CHECK(r["result"]["value"][2].asInt() == 4 && r["result"]["value"][3].asInt() == 3);
    CHECK(Call(e, "physics.state", "{}")["result"]["warnings"].size() == 0);
}

void Scene2D(Engine& e) {
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Ground", "components": {"Transform": {"position": [0, -0.5, 0], "scale": [40, 1, 1]}, "Collider2D": {}}})J");
}

TEST(Physics2DBodies) {
    Engine e;
    Scene2D(e);
    Call(e, "entity.create", R"J({"name": "Box", "components": {"Transform": {"position": [0, 3, 0], "rotation": [0, 0, 30]}, "Collider2D": {}, "RigidBody2D": {}}})J");
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"Transform": {"position": [3, 2, 0], "scale": [0.5, 0.5, 0.5]}, "Collider2D": {"shape": "circle"}, "RigidBody2D": {}}})J");
    Call(e, "entity.create", R"J({"name": "Tri", "components": {"Transform": {"position": [-3, 2, 5]}, "Collider2D": {"shape": "polygon", "points": [[-0.5, 0], [0.5, 0], [0, 1], [0, 0.5]]}, "RigidBody2D": {"fixedRotation": true}}})J");
    Call(e, "sim.step", R"J({"frames": 240})J");
    Vec3 box = PosOf(e, "Box");
    const Transform* bt = e.GetScene().Get<Transform>(e.GetScene().FindByName("Box"));
    CHECK(std::fabs(box.y - 0.5f) < 0.02f);                               // settled flat on a face
    CHECK(std::fabs(std::fmod(std::fabs(bt->rotation.z) + 1.0f, 90.0f) - 1.0f) < 1.0f);
    CHECK(bt->rotation.x == 0.0f && bt->rotation.y == 0.0f && box.z == 0.0f);  // only Z rotation, Z position kept
    CHECK(std::fabs(PosOf(e, "Ball").y - 0.25f) < 0.02f);                 // radius scaled by 0.5
    CHECK(std::fabs(PosOf(e, "Tri").y) < 0.02f && PosOf(e, "Tri").z == 5.0f);  // concave outline, z untouched
    Json st = Call(e, "physics.state", "{}")["result"]["world2D"];
    CHECK(st["dynamicBodies"].asInt() == 3 && st["staticBodies"].asInt() == 1);

    // Velocity and impulses from scripts; queries in the plane.
    Call(e, "component.set", R"J({"id": "Ball", "type": "RigidBody2D", "values": {"velocity": [0, 6, 0]}})J");
    Call(e, "sim.step", R"J({"frames": 20})J");
    CHECK(PosOf(e, "Ball").y > 1.5f);
    Json hit = Call(e, "physics.raycast", R"J({"origin": [0, 5, 0], "direction": [0, -1, 0]})J")["result"];
    CHECK(hit["hit"].asBool() && hit["name"].asString() == "Box" && std::fabs(hit["point"][1].asNumber() - 1.0) < 0.03);
    CHECK(std::fabs(hit["normal"][1].asNumber() - 1.0) < 1e-3);
    hit = Call(e, "physics.raycast", R"J({"origin": [-10, 0.5, 3], "direction": [1, 0, 0]})J")["result"];
    CHECK(hit["name"].asString() == "Tri" && hit["point"][2].asNumber() == 3.0);  // 2D hit keeps the ray's z
    Json over = Call(e, "physics.overlap", R"J({"center": [0, 0.5, 0], "radius": 0.2})J")["result"];
    CHECK(over.size() == 1 && over[0]["name"].asString() == "Box");

    // Layers: a body on layer 2 that ignores layer 0 falls through the ground.
    Call(e, "entity.create", R"J({"name": "Ghost", "components": {"Transform": {"position": [8, 1, 0]}, "Collider2D": {"layer": 2, "ignoreLayers": [0]}, "RigidBody2D": {}}})J");
    Call(e, "sim.step", R"J({"frames": 60})J");
    CHECK(PosOf(e, "Ghost").y < -1.0f);
}

TEST(Physics2DCharacters) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("physics2d_char"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // Floor top at y = -4, a wall at x 9..10, a one-way platform (top y = -2) and a slope rising to the right.
    Call(e, "entity.create", R"J({"name": "Level", "components": {"Transform": {}, "Tilemap": {"map": [
        "            ",
        "            ",
        "   ===      ",
        "         /# ",
        "############"], "legend": {"=": {"collision": "oneway"}, "/": {"collision": "slope-up"}}, "solid": "#"}}})J");
    Call(e, "entity.create", R"J({"name": "Hero", "components": {"Transform": {"position": [1, -2, 0]}, "CharacterBody2D": {"radius": 0.3, "height": 1}}})J");
    const CharacterBody2D* cb = [&] { return e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Hero")); }();
    Call(e, "sim.step", R"J({"frames": 60})J");
    CHECK(std::fabs(PosOf(e, "Hero").y + 3.5f) < 0.02f);  // standing on the floor
    cb = e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Hero"));
    CHECK(cb->grounded);

    // Jump up through the one-way platform, land on top of it.
    Call(e, "component.set", R"J({"id": "Hero", "type": "Transform", "values": {"position": [4, -3.5, 0]}})J");
    Call(e, "sim.step", R"J({"frames": 2})J");
    Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody2D", "values": {"velocity": [0, 9, 0]}})J");
    Call(e, "sim.step", R"J({"frames": 150})J");
    cb = e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Hero"));
    CHECK(std::fabs(PosOf(e, "Hero").y + 1.5f) < 0.03f && cb->grounded);
    // Drop through it.
    Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody2D", "values": {"dropThrough": true}})J");
    Call(e, "sim.step", R"J({"frames": 60})J");
    CHECK(std::fabs(PosOf(e, "Hero").y + 3.5f) < 0.03f);
    Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody2D", "values": {"dropThrough": false}})J");

    // Walk right: up the slope onto the block (top y = -3), then into... the map edge is open, so it walks off.
    Call(e, "component.set", R"J({"id": "Hero", "type": "Transform", "values": {"position": [7, -3.5, 0]}})J");
    bool climbed = false;
    for (int i = 0; i < 40; ++i) {
        Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody2D", "values": {"velocity": [3, 0, 0]}})J");
        Call(e, "sim.step", R"J({"frames": 2})J");
        if (PosOf(e, "Hero").x > 10.0f && PosOf(e, "Hero").y > -2.6f) climbed = true;
    }
    CHECK(climbed);
    cb = e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Hero"));
    CHECK(cb->velocity.y <= 0.0f);  // never launched upward by the ramp

    // A wall stops the character; top-down mode has no gravity.
    Call(e, "entity.create", R"J({"name": "Wall", "components": {"Transform": {"position": [3, 5, 0], "scale": [1, 4, 1]}, "Collider2D": {}}})J");
    Call(e, "entity.create", R"J({"name": "Top", "components": {"Transform": {"position": [0, 5, 0]}, "CharacterBody2D": {"mode": "topdown", "shape": "circle", "radius": 0.5}}})J");
    for (int i = 0; i < 30; ++i) {
        Call(e, "component.set", R"J({"id": "Top", "type": "CharacterBody2D", "values": {"velocity": [4, 0, 0]}})J");
        Call(e, "sim.step", R"J({"frames": 2})J");
    }
    Vec3 top = PosOf(e, "Top");
    CHECK(std::fabs(top.x - 2.0f) < 0.03f && std::fabs(top.y - 5.0f) < 1e-3f);
    const CharacterBody2D* tc = e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Top"));
    CHECK(tc->onWall && !tc->grounded);

    // Triggers see characters; collision callbacks fire.
    Call(e, "script.write", R"J({"path": "scripts/coin.lua", "source": "local M = {}\nfunction M:onTriggerEnter(other) coins = (coins or 0) + 1 end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Coin", "components": {"Transform": {"position": [0, 7, 0]}, "Collider2D": {"shape": "circle", "radius": 0.3, "isTrigger": true}, "Script": {"path": "scripts/coin.lua"}}})J");
    for (int i = 0; i < 30; ++i) {
        Call(e, "component.set", R"J({"id": "Top", "type": "CharacterBody2D", "values": {"velocity": [-2, 2, 0]}})J");
        Call(e, "sim.step", R"J({"frames": 2})J");
    }
    CHECK(Call(e, "script.eval", R"J({"code": "coins"})J")["result"]["value"].asInt() == 1);
    CHECK(e.Scripts().Errors().empty());
}

TEST(Physics2DPlatformsAndPushing) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("physics2d_platform"), &err));
    Scene2D(e);
    // A kinematic platform moved by a script carries the character standing on it.
    Call(e, "script.write", R"J({"path": "scripts/mover.lua", "source": "local M = {}\nfunction M:onUpdate(dt)\n  local p = scene.get(self.id, 'Transform').position\n  scene.set(self.id, 'Transform', {position = {x = p.x + 2 * dt, y = p.y, z = 0}})\nend\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Lift", "components": {"Transform": {"position": [-10, 2, 0], "scale": [3, 0.4, 1]}, "Collider2D": {}, "RigidBody2D": {"type": "kinematic"}, "Script": {"path": "scripts/mover.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Rider", "components": {"Transform": {"position": [-10, 3, 0]}, "CharacterBody2D": {"radius": 0.3, "height": 1}}})J");
    Call(e, "sim.step", R"J({"frames": 30})J");
    float x0 = PosOf(e, "Rider").x;
    Call(e, "sim.step", R"J({"frames": 60})J");
    CHECK(std::fabs(PosOf(e, "Rider").x - x0 - 2.0f) < 0.05f);  // moved 2 m with the lift in 1 s
    CHECK(std::fabs(PosOf(e, "Rider").y - 2.7f) < 0.03f);

    // Walking into a crate pushes it; a heavy one barely moves.
    Call(e, "entity.create", R"J({"name": "Crate", "components": {"Transform": {"position": [3, 0.4, 0], "scale": [0.8, 0.8, 1]}, "Collider2D": {}, "RigidBody2D": {}}})J");
    Call(e, "entity.create", R"J({"name": "Safe", "components": {"Transform": {"position": [9, 0.4, 0], "scale": [0.8, 0.8, 1]}, "Collider2D": {}, "RigidBody2D": {"mass": 500}}})J");
    Call(e, "entity.create", R"J({"name": "Pusher", "components": {"Transform": {"position": [1, 0.5, 0]}, "CharacterBody2D": {"radius": 0.3, "height": 1}}})J");
    for (int i = 0; i < 90; ++i) {
        Call(e, "component.set", R"J({"id": "Pusher", "type": "CharacterBody2D", "values": {"velocity": [3, 0, 0]}})J");
        Call(e, "sim.step", R"J({"frames": 1})J");
    }
    CHECK(PosOf(e, "Crate").x > 5.5f);
    CHECK(std::fabs(PosOf(e, "Crate").y - 0.4f) < 0.03f);
    CHECK(PosOf(e, "Safe").x < 9.3f);
    CHECK(Call(e, "physics.contacts", R"J({"id": "Pusher"})J")["result"].size() >= 2);  // ground + a crate
}

TEST(Physics2DIsDeterministic) {
    auto run = [] {
        Engine e;
        Scene2D(e);
        Call(e, "entity.create", R"J({"name": "Slope", "components": {"Transform": {"position": [0, 0, 0]}, "Collider2D": {"shape": "edge", "points": [[-8, 6], [-2, 1], [4, 0]]}}})J");
        for (int i = 0; i < 8; ++i) {
            Json args = Json::parse(R"J({"components": {"Transform": {}, "Collider2D": {}, "RigidBody2D": {}}})J");
            args["name"] = "B" + std::to_string(i);
            args["components"]["Transform"]["position"] = Json(Json::Array{-6.0 + 0.4 * i, 7.0 + 1.1 * i, 0.0});
            args["components"]["Transform"]["rotation"] = Json(Json::Array{0.0, 0.0, 17.0 * i});
            if (i % 2) args["components"]["Collider2D"] = Json::parse(R"J({"shape": "circle", "radius": 0.4, "bounciness": 0.3})J");
            e.Call("entity.create", args);
        }
        Call(e, "entity.create", R"J({"name": "Hero", "components": {"Transform": {"position": [6, 1, 0]}, "CharacterBody2D": {"velocity": [-3, 0, 0]}}})J");
        Call(e, "sim.step", R"J({"frames": 300})J");
        return e.GetScene().ToJson().dump();
    };
    std::string a = run(), b = run();
    CHECK(a == b);
    // Compare this line between builds (native SSE2/NEON vs WebAssembly scalar): Box2D is cross-platform deterministic.
    uint64_t h = 1469598103934665603ull;
    for (char c : a) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    std::printf("  2D scene hash %016llx\n", static_cast<unsigned long long>(h));
}

TEST(DungeonSamplePlays) {
    auto run = [](std::string* summary) {
        Engine e;
        std::string err;
        CHECK(e.Open(TestSourceDir() + "/samples/Dungeon", &err));
        Call(e, "sim.step", R"J({"frames": 5})J");
        auto eval = [&](const char* code) {
            Json a = Json::MakeObject();
            a["code"] = code;
            return e.Call("script.eval", a)["result"]["value"];
        };
        CHECK(eval("return game.get('coinsTotal')").asInt() == 8);
        // The level reads its rules from tilesets/dungeon.tileset.json.
        Json info = Call(e, "tilemap.info", R"J({"id": "Level"})J")["result"];
        CHECK(info["width"].asInt() == 36 && info["height"].asInt() == 20 && !info.has("error"));
        // Walk right, then shoot the slime east of the start twice.
        Call(e, "component.set", R"J({"id": "Player", "type": "Transform", "values": {"position": [22.5, -3.5, 0.1]}})J");
        Call(e, "input.key", R"J({"key": "D", "down": true})J");
        Call(e, "sim.step", R"J({"frames": 2})J");
        Call(e, "input.key", R"J({"key": "D", "down": false})J");
        Call(e, "sim.step", R"J({"frames": 10})J");
        for (int shot = 0; shot < 2; ++shot) {
            Call(e, "input.key", R"J({"key": "Space", "down": true})J");
            Call(e, "sim.step", R"J({"frames": 1})J");
            Call(e, "input.key", R"J({"key": "Space", "down": false})J");
            Call(e, "sim.step", R"J({"frames": 25})J");
        }
        Call(e, "sim.step", R"J({"frames": 15})J");
        CHECK(eval("return #scene.withTag('enemy')").asInt() == 5);
        // Coins are triggers the hero collects.
        Call(e, "component.set", R"J({"id": "Player", "type": "Transform", "values": {"position": [29.5, -1.5, 0.1]}})J");
        Call(e, "sim.step", R"J({"frames": 5})J");
        CHECK(eval("return #scene.withTag('coin')").asInt() == 7);
        CHECK(eval("return scene.get(scene.find('CoinsText'), 'UIText').text").asString() == "COINS 1/8");
        // The stairs refuse to end the level early.
        Call(e, "component.set", R"J({"id": "Player", "type": "Transform", "values": {"position": [28.5, -14.5, 0.1]}})J");
        Call(e, "sim.step", R"J({"frames": 5})J");
        CHECK(eval("return scene.get(scene.find('Note'), 'UIText').text").asString() == "7 COINS LEFT");
        CHECK(e.Scripts().Errors().empty());
        CHECK(Call(e, "physics.state", "{}")["result"]["warnings"].size() == 0);
        Json shot = Call(e, "render.screenshot", R"J({"width": 128, "height": 72, "inline": false})J");
        *summary = shot["result"]["hash"].asString() + e.GetScene().ToJson().dump();
    };
    std::string a, b;
    run(&a);
    run(&b);
    CHECK(a == b);
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

TEST(GltfSkinAndAnimationLoading) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("animation_loader"), &err));
    Json source = Json::parse(R"J({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[1]}],
      "nodes":[{"mesh":0,"skin":0},{"translation":[3,0,0],"children":[0,2]},{"translation":[0,2,0]}],
      "skins":[{"joints":[1,2]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2}}]}],
      "animations":[{"name":"Move","samplers":[{"input":3,"output":4,"interpolation":"STEP"}],
        "channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]},
        {"name":"Turn","samplers":[{"input":3,"output":5}],"channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}]},
        {"name":"Move","samplers":[{"input":3,"output":6,"interpolation":"CUBICSPLINE"}],
        "channels":[{"sampler":0,"target":{"node":2,"path":"scale"}}]}],"bufferViews":[],"accessors":[]})J");
    std::vector<uint8_t> bin;
    auto floats = [&](std::initializer_list<float> values) {
        std::vector<uint8_t> bytes;
        for (float value : values) {
            uint8_t encoded[4];
            std::memcpy(encoded, &value, 4);
            bytes.insert(bytes.end(), encoded, encoded + 4);
        }
        return bytes;
    };
    auto accessor = [&](const std::vector<uint8_t>& bytes, int component, int count, const char* type, bool normalized = false) {
        while (bin.size() % 4) bin.push_back(0);
        Json view = Json::MakeObject();
        view["buffer"] = 0;
        view["byteOffset"] = static_cast<uint64_t>(bin.size());
        view["byteLength"] = static_cast<uint64_t>(bytes.size());
        Json a = Json::MakeObject();
        a["bufferView"] = static_cast<uint64_t>(source["bufferViews"].size());
        a["componentType"] = component;
        a["count"] = count;
        a["type"] = type;
        if (normalized) a["normalized"] = true;
        source["bufferViews"].push(view);
        source["accessors"].push(a);
        bin.insert(bin.end(), bytes.begin(), bytes.end());
    };
    accessor(floats({0,0,0, 1,0,0, 0,1,0}), 5126, 3, "VEC3");
    source["accessors"][0]["min"] = Json::parse("[0,0,0]");
    source["accessors"][0]["max"] = Json::parse("[1,1,0]");
    accessor({0,1,0,0, 1,0,0,0, 0,0,0,0}, 5121, 3, "VEC4");
    accessor(floats({2,2,0,0, 1,0,0,0, 0,0,0,0}), 5126, 3, "VEC4");
    accessor(floats({0,2}), 5126, 2, "SCALAR");
    source["accessors"][3]["min"] = Json::parse("[0]");
    source["accessors"][3]["max"] = Json::parse("[2]");
    accessor(floats({3,0,0, 5,0,0}), 5126, 2, "VEC3");
    accessor({0,0,0,127, 0,0,127,0}, 5120, 2, "VEC4", true);
    accessor(floats({0,0,0, 1,1,1, 0,0,0, 0,0,0, 2,2,2, 0,0,0}), 5126, 6, "VEC3");
    const std::string path = JoinPath(e.ProjectDir(), "assets/models/rig.gltf");
    auto write = [&](Json model, const std::vector<uint8_t>& bytes) {
        Json buffer = Json::MakeObject();
        buffer["byteLength"] = static_cast<uint64_t>(bytes.size());
        buffer["uri"] = "data:application/octet-stream;base64," + Base64Encode(bytes);
        model["buffers"] = Json::MakeArray();
        model["buffers"].push(buffer);
        CHECK(WriteTextFile(path, model.dump()));
    };
    write(source, bin);
    Json info = Call(e, "asset.info", R"J({"path":"assets/models/rig.gltf"})J")["result"];
    CHECK(info["joints"].asInt() == 2 && info["clips"].size() == 3);
    CHECK(info["clips"][0]["name"].asString() == "Move" && info["clips"][2]["name"].asString() == "Move_1");
    CHECK(info["clips"][0]["duration"].asFloat() == 2 && info["clips"][0]["channels"].asInt() == 1);
    Mesh mesh;
    CHECK(LoadModelFile(path, mesh, &err));
    CHECK(mesh.nodes.size() == 3 && mesh.nodes[2].parent == 1 && mesh.joints[1].node == 2);
    CHECK(mesh.positions[0].x == 0 && mesh.skin[0].weights[0] == 0.5f && mesh.skin[0].weights[1] == 0.5f);
    CHECK(mesh.skin[2].weights[0] == 0 && mesh.joints[0].inverseBind.m[0] == 1);
    CHECK(mesh.clips[0].channels[0].interpolation == AnimationInterpolation::Step);
    CHECK(mesh.clips[1].channels[0].values[1].z == 1);
    CHECK(mesh.clips[2].channels[0].interpolation == AnimationInterpolation::CubicSpline);

    auto reject = [&](Json model, std::vector<uint8_t> bytes, const char* message) {
        write(model, bytes);
        CHECK(!LoadModelFile(path, mesh, &err));
        CHECK(err.find(message) != std::string::npos);
    };
    std::vector<uint8_t> broken = bin;
    broken[static_cast<size_t>(source["bufferViews"][1]["byteOffset"].asInt())] = 2;
    reject(source, broken, "joint index is outside");
    broken = bin;
    size_t times = static_cast<size_t>(source["bufferViews"][3]["byteOffset"].asInt());
    std::fill(broken.begin() + times + 4, broken.begin() + times + 8, uint8_t{0});
    reject(source, broken, "strictly increasing");
    broken = bin;
    float negative = -1;
    std::memcpy(broken.data() + source["bufferViews"][2]["byteOffset"].asInt(), &negative, 4);
    reject(source, broken, "finite and nonnegative");
    Json invalid = source;
    invalid["animations"][0]["channels"].push(invalid["animations"][0]["channels"][0]);
    reject(invalid, bin, "duplicate channels");
    invalid = source;
    invalid["meshes"][0]["primitives"][0]["attributes"]["JOINTS_1"] = 1;
    reject(invalid, bin, "four influences");
    invalid = source;
    invalid["skins"][0]["joints"] = Json::MakeArray();
    for (int i = 0; i < 65; ++i) invalid["skins"][0]["joints"].push(1);
    reject(invalid, bin, "64-entry");
    // Rigid child geometry is baked once, then receives a one-joint animation binding.
    invalid = source;
    invalid["nodes"][0].erase("skin");
    invalid["meshes"][0]["primitives"][0]["attributes"].erase("JOINTS_0");
    invalid["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
    write(invalid, bin);
    CHECK(LoadModelFile(path, mesh, &err));
    CHECK(mesh.joints.size() == 1 && mesh.joints[0].node == 0 && mesh.positions[0].x == 3);
    CHECK(mesh.skin[0].weights[0] == 1 && mesh.joints[0].inverseBind.m[12] == -3);
    // Explicit inverse bind matrices survive loading, including their translations.
    accessor(floats({1,0,0,0, 0,1,0,0, 0,0,1,0, -3,0,0,1,
                     1,0,0,0, 0,1,0,0, 0,0,1,0, -3,-2,0,1}), 5126, 2, "MAT4");
    source["skins"][0]["inverseBindMatrices"] = 7;
    write(source, bin);
    CHECK(LoadModelFile(path, mesh, &err));
    CHECK(mesh.joints[1].inverseBind.m[12] == -3 && mesh.joints[1].inverseBind.m[13] == -2);
    invalid = source;
    invalid["accessors"][7]["count"] = 1;
    reject(invalid, bin, "inverse bind matrices");
    // Quantized unsigned skin weights decode and normalize in exactly the same path.
    invalid = source;
    invalid["accessors"][2]["componentType"] = 5121;
    invalid["accessors"][2]["normalized"] = true;
    broken = bin;
    size_t weightOffset = static_cast<size_t>(source["bufferViews"][2]["byteOffset"].asInt());
    for (size_t i = 0; i < 12; ++i) broken[weightOffset + i] = i % 4 < 2 ? 127 : 0;
    write(invalid, broken);
    CHECK(LoadModelFile(path, mesh, &err));
    CHECK(mesh.skin[0].weights[0] == 0.5f && mesh.skin[0].weights[1] == 0.5f);
    invalid["accessors"][2]["normalized"] = false;
    reject(invalid, broken, "normalized unsigned");
}

TEST(AnimationPoseInterpolation) {
    Mesh mesh;
    mesh.nodes.resize(2);
    mesh.nodes[0].parent = 1;  // parents may occur after children in the source
    mesh.nodes[0].scale = Vec3(2, 1, 1);
    mesh.nodes[1].usesMatrix = true;
    mesh.nodes[1].matrix = Mat4::Translation(Vec3(3, 0, 0));
    mesh.joints.push_back({0, Mat4::Translation(Vec3(-1, 0, 0))});
    AnimationChannel channel;
    channel.node = 0;
    channel.times = {0, 2};
    channel.values = {Vec4(0,0,0,0), Vec4(4,0,0,0)};
    AnimationClip clip;
    clip.channels.push_back(channel);
    auto sample = [&](float time) { return EvaluateAnimationPose(mesh, &clip, time)[0]; };
    CHECK(Near(sample(1).TransformPoint(Vec3(1,0,0)), Vec3(5,0,0), 1e-5f));
    CHECK(Near(sample(-8).TransformPoint(Vec3(1,0,0)), Vec3(3,0,0), 1e-5f));
    CHECK(Near(sample(8).TransformPoint(Vec3(1,0,0)), Vec3(7,0,0), 1e-5f));
    CHECK(Near(sample(1).TransformDir(Vec3(1,0,0)), Vec3(2,0,0), 1e-5f));  // unanimated scale retained
    clip.channels[0].interpolation = AnimationInterpolation::Step;
    CHECK(Near(sample(1.999f).TransformPoint(Vec3(1,0,0)), Vec3(3,0,0), 1e-5f));
    CHECK(Near(sample(2).TransformPoint(Vec3(1,0,0)), Vec3(7,0,0), 1e-5f));
    clip.channels[0].interpolation = AnimationInterpolation::CubicSpline;
    clip.channels[0].values = {Vec4(), Vec4(), Vec4(2,0,0,0), Vec4(), Vec4(4,0,0,0), Vec4()};
    CHECK(Near(sample(1).TransformPoint(Vec3(1,0,0)), Vec3(5.5f,0,0), 1e-5f));  // tangent scaled by two seconds
    clip.channels[0].path = AnimationPath::Rotation;
    clip.channels[0].interpolation = AnimationInterpolation::Linear;
    clip.channels[0].values = {Vec4(0,0,0,1), Vec4(0,0,1,0)};
    CHECK(Near(sample(1).TransformDir(Vec3(1,0,0)), Vec3(0,2,0), 1e-5f));
    // Quaternion signs represent the same rotation; interpolation takes the shortest arc.
    clip.channels[0].values[1] = Vec4(0,0,0,-1);
    CHECK(Near(sample(1).TransformDir(Vec3(1,0,0)), Vec3(2,0,0), 1e-5f));
    clip.channels[0].interpolation = AnimationInterpolation::CubicSpline;
    clip.channels[0].values = {Vec4(), Vec4(0,0,0,1), Vec4(), Vec4(), Vec4(0,0,1,0), Vec4()};
    CHECK(Near(sample(1).TransformDir(Vec3(1,0,0)), Vec3(0,2,0), 1e-5f));  // interpolated quaternion normalized
    clip.channels[0].path = AnimationPath::Scale;
    clip.channels[0].interpolation = AnimationInterpolation::Linear;
    clip.channels[0].values = {Vec4(1,1,1,0), Vec4(3,3,3,0)};
    CHECK(Near(sample(1).TransformDir(Vec3(0,1,0)), Vec3(0,2,0), 1e-5f));
    CHECK(Near(EvaluateAnimationPose(mesh, nullptr, 0)[0].TransformPoint(Vec3(1,0,0)), Vec3(3,0,0), 1e-5f));
}

TEST(AnimatorCommandsAndLua) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("animator"), &err));
    CHECK(CopyFileTo(TestSourceDir() + "/samples/Showcase/assets/models/fox.glb", JoinPath(e.ProjectDir(), "assets/models/fox.glb")));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Fox","components":{"MeshRenderer":{"mesh":"assets/models/fox.glb"}}})J");
    EntityId id = e.GetScene().FindByName("Fox");
    CHECK(Call(e, "animation.play", R"J({"id":"Fox","clip":"missing"})J")["error"]["code"].asString() == "animation_not_found");
    CHECK(e.GetScene().Get<Animator>(id) == nullptr);
    CHECK(Call(e, "animation.play", R"J({"id":"Fox","clip":"Walk"})J")["ok"].asBool());
    Json saved = e.GetScene().ToJson();
    Scene restored;
    CHECK(restored.FromJson(saved, &err) && restored.Get<Animator>(id)->clip == "Walk");
    CHECK(Call(e, "history.undo", "{}")["ok"].asBool());
    CHECK(e.GetScene().Get<Animator>(id) == nullptr);
    CHECK(Call(e, "history.redo", "{}")["ok"].asBool());
    CHECK(e.GetScene().Get<Animator>(id)->clip == "Walk");
    Call(e, "sim.step", R"J({"frames":12})J");
    Json state = Call(e, "animation.state", R"J({"id":"Fox","pose":true})J")["result"];
    CHECK(std::fabs(state["time"].asFloat() - 0.2f) < 1e-6f && state["validClip"].asBool());
    CHECK(state["jointMatrices"].size() == 24 && state["jointMatrices"][0].size() == 16);
    Json pose = state["jointMatrices"];
    Call(e, "component.set", R"J({"id":"Fox","type":"Animator","values":{"playing":false}})J");
    Call(e, "sim.step", R"J({"frames":10})J");
    CHECK(Call(e, "animation.state", R"J({"id":"Fox","pose":true})J")["result"]["jointMatrices"] == pose);
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Walk","loop":false})J");
    Call(e, "sim.step", R"J({"frames":60})J");
    state = Call(e, "animation.state", R"J({"id":"Fox"})J")["result"];
    CHECK(!state["playing"].asBool() && state["time"] == state["duration"]);
    CHECK(Call(e, "script.eval", R"J({"code":"animation.play('Fox', 'Walk', {speed=-1, loop=false})"})J")["ok"].asBool());
    Call(e, "sim.step", R"J({"frames":60})J");
    state = Call(e, "animation.state", R"J({"id":"Fox"})J")["result"];
    CHECK(!state["playing"].asBool() && state["time"].asFloat() == 0);
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Run","speed":-1,"loop":true})J");
    Call(e, "component.set", R"J({"id":"Fox","type":"Animator","values":{"time":0}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    state = Call(e, "animation.state", R"J({"id":"Fox"})J")["result"];
    CHECK(std::fabs(state["time"].asFloat() - state["duration"].asFloat() + 1.0f/60) < 1e-6f);
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Run","speed":1})J");
    Call(e, "sim.step", R"J({"frames":24})J");
    Json first = Call(e, "animation.state", R"J({"id":"Fox","pose":true})J")["result"];
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Run","restart":false})J");
    CHECK(e.GetScene().Get<Animator>(id)->time == first["time"].asFloat());
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Run"})J");
    Call(e, "sim.step", R"J({"frames":24})J");
    CHECK(Call(e, "animation.state", R"J({"id":"Fox","pose":true})J")["result"] == first);
    CHECK(Call(e, "script.eval", R"J({"code":"local ok = pcall(animation.play, 'Fox', 'invalid'); assert(not ok)"})J")["ok"].asBool());
    // self:play follows the same command path from a real script instance.
    CHECK(WriteTextFile(JoinPath(e.ProjectDir(), "scripts/animate.lua"), "return {onStart=function(self) self:play('Survey', {speed=0}) end}"));
    Call(e, "component.add", R"J({"id":"Fox","type":"Script","values":{"path":"scripts/animate.lua"}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    CHECK(e.GetScene().Get<Animator>(id)->clip == "Survey" && e.GetScene().Get<Animator>(id)->time == 0);
    CHECK(e.Scripts().Errors().empty());
}

TEST(ParticleSimulationAndCommands) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("particles"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "entity.create", R"J({"name":"Effect","components":{"Transform":{"position":[3,4,0]},"ParticleEmitter":{"rate":0,"burst":2,"lifetime":0.5,"speed":2,"spread":0,"gravity":[0,-2,0],"maxParticles":3,"dimensions":2,"loop":false}}})J")["ok"].asBool());
    EntityId id = e.GetScene().FindByName("Effect");
    Call(e, "sim.step", R"J({"frames":1})J");
    Json state = Call(e, "particles.state", R"J({"id":"Effect","particles":true})J")["result"];
    CHECK(state["count"].asInt() == 2 && state["emitted"].asInt() == 2);
    CHECK(state["particles"][0]["age"].asFloat() == 0 && state["particles"][0]["position"][1].asFloat() == 0);
    CHECK(state["particles"][0]["velocity"][1].asFloat() == 2 && state["particles"][0]["velocity"][2].asFloat() == 0);
    CHECK(Call(e, "particles.burst", R"J({"id":"Effect","count":10})J")["result"]["accepted"].asInt() == 1);
    CHECK(Call(e, "particles.burst", R"J({"id":"Effect","count":1})J")["result"]["accepted"].asInt() == 0);
    CHECK(!Call(e, "particles.burst", R"J({"id":"Effect","count":10001})J")["ok"].asBool());
    CHECK(!Call(e, "particles.burst", R"J({"id":"Effect","count":-1})J")["ok"].asBool());
    CHECK(!Call(e, "particles.burst", R"J({"id":"Effect","count":1.5})J")["ok"].asBool());
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"playing":false}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    state = Call(e, "particles.state", R"J({"id":"Effect","particles":true})J")["result"];
    CHECK(std::fabs(state["particles"][0]["age"].asFloat() - 1.0f/60) < 1e-6f);
    CHECK(std::fabs(state["particles"][0]["velocity"][1].asFloat() - (2 - 2.0f/60)) < 1e-6f);
    CHECK(state["particles"][0]["position"][1].asFloat() > 0);
    Call(e, "sim.step", R"J({"frames":31})J");
    CHECK(Call(e, "particles.state", R"J({"id":"Effect"})J")["result"]["count"].asInt() == 0);
    Call(e, "particles.clear", R"J({"id":"Effect","restart":true})J");
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"space":"world","playing":true}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    state = Call(e, "particles.state", R"J({"id":"Effect","particles":true})J")["result"];
    CHECK(state["particles"][0]["worldSpace"].asBool() && state["particles"][0]["position"][0].asFloat() == 3);
    Call(e, "component.set", R"J({"id":"Effect","type":"Transform","values":{"position":[8,4,0]}})J");
    CHECK(e.GetScene().Get<ParticleEmitter>(id)->particles[0].position.x == 3);
    Json serialized = e.GetScene().ToJson();
    Scene restored;
    CHECK(restored.FromJson(serialized, &err));
    CHECK(restored.Get<ParticleEmitter>(id)->particles.empty() && restored.Get<ParticleEmitter>(id)->burst == 2);
    Call(e, "particles.clear", R"J({"id":"Effect"})J");
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"maxParticles":10,"loop":true,"rate":6,"lifetime":10}})J");
    Call(e, "sim.step", R"J({"frames":60})J");
    CHECK(Call(e, "particles.state", R"J({"id":"Effect"})J")["result"]["count"].asInt() == 6);
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"maxParticles":1}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    CHECK(Call(e, "particles.state", R"J({"id":"Effect"})J")["result"]["count"].asInt() == 1);
    CHECK(Call(e, "script.eval", R"J({"code":"assert(particles.burst('Effect', 5) == 0); assert(not pcall(particles.burst, 'Effect', -1))"})J")["ok"].asBool());
    Call(e, "particles.clear", R"J({"id":"Effect"})J");
    CHECK(WriteTextFile(JoinPath(e.ProjectDir(), "scripts/burst.lua"), "return {onStart=function(self) assert(self:burst(1) == 1) end}"));
    Call(e, "component.add", R"J({"id":"Effect","type":"Script","values":{"path":"scripts/burst.lua"}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    CHECK(Call(e, "particles.state", R"J({"id":"Effect"})J")["result"]["count"].asInt() == 1 && e.Scripts().Errors().empty());
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"gravity":[0,1e100,0]}})J");
    CHECK(Call(e, "particles.burst", R"J({"id":"Effect","count":1})J")["error"]["code"].asString() == "invalid_emitter");
}

TEST(ParticleRandomStreamIsDeterministic) {
    auto simulate = [](int seed) {
        Engine e;
        Call(e, "scene.new", R"J({"empty":true})J");
        Json args = Json::parse(R"J({"name":"Effect","components":{"ParticleEmitter":{"rate":0,"burst":10,"loop":false,"spread":90,"gravity":[0,0,0],"lifetime":10}}})J");
        args["components"]["ParticleEmitter"]["seed"] = seed;
        e.Call("entity.create", args);
        Call(e, "sim.step", R"J({"frames":15})J");
        return Call(e, "particles.state", R"J({"id":"Effect","particles":true})J")["result"];
    };
    Json first = simulate(7);
    CHECK(first == simulate(7) && first != simulate(8));
    CHECK(first["count"].asInt() == 10);
    for (const Json& particle : first["particles"].items()) {
        const Json& v = particle["velocity"];
        float length = std::sqrt(v[0].asFloat()*v[0].asFloat() + v[1].asFloat()*v[1].asFloat() + v[2].asFloat()*v[2].asFloat());
        CHECK(std::fabs(length - 1) < 1e-5f && v[1].asFloat() >= -1e-6f);
    }
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
    CHECK(e.Open(TestSourceDir() + "/samples/Showcase", &err));
    Json info = Call(e, "asset.info", R"J({"path": "assets/models/fox.glb"})J")["result"];
    CHECK(info["triangles"].asInt() == 576 && info["textures"][0][0].asInt() == 1024);
    CHECK(info["joints"].asInt() == 24 && info["clips"].size() == 3);
    CHECK(info["clips"][0]["name"].asString() == "Survey" && info["clips"][1]["name"].asString() == "Walk" &&
          info["clips"][2]["name"].asString() == "Run");
    CHECK(info["clips"][1]["duration"].asFloat() > 0.7f && info["clips"][1]["channels"].asInt() == 21);
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

TEST(ShowcaseAnimationControls) {
    auto play = []() {
        Engine e;
        std::string err;
        CHECK(e.Open(TestSourceDir() + "/samples/Showcase", &err));
        EntityId fox = e.GetScene().FindByName("Fox");
        Call(e, "sim.step", R"J({"frames":1})J");
        CHECK(e.GetScene().Get<Animator>(fox)->clip == "Survey");
        Call(e, "input.key", R"J({"key":"W","down":true})J");
        Call(e, "sim.step", R"J({"frames":12})J");
        CHECK(e.GetScene().Get<Animator>(fox)->clip == "Walk");
        float walkTime = e.GetScene().Get<Animator>(fox)->time;
        Call(e, "sim.step", R"J({"frames":1})J");
        CHECK(e.GetScene().Get<Animator>(fox)->time > walkTime);  // no restart each update
        Call(e, "input.key", R"J({"key":"Shift","down":true})J");
        Call(e, "sim.step", R"J({"frames":12})J");
        CHECK(e.GetScene().Get<Animator>(fox)->clip == "Run");
        RenderTarget rt;
        rt.Resize(320,180);
        e.RenderGameView(rt);
        uint64_t runHash = rt.Hash();
        Call(e, "input.key", R"J({"key":"W","down":false})J");
        Call(e, "input.key", R"J({"key":"Shift","down":false})J");
        Call(e, "sim.step", R"J({"frames":1})J");
        CHECK(e.GetScene().Get<Animator>(fox)->clip == "Survey");
        CHECK(e.Scripts().Errors().empty());
        return runHash;
    };
    uint64_t first = play();
    CHECK(first == play());
    std::printf("  animated Showcase hash %016llx\n", static_cast<unsigned long long>(first));
}

TEST(UIQuadsAreWhatSoftwareDraws) {
    // Both renderers draw UI from BuildUIQuads; the software result is the reference.
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Showcase", &err));
    std::vector<UIQuad> quads = BuildUIQuads(e.GetScene(), 640, 360, &e.Assets());
    CHECK(quads.size() > 50);  // one textured quad per glyph
    RenderTarget rt;
    rt.Resize(640, 360);
    e.RenderGameView(rt);
    std::set<EntityId> drawn;
    for (const UIQuad& q : quads) {
        CHECK(q.x1 > q.x0 && q.y1 > q.y0);
        CHECK(q.texture != nullptr);  // Showcase UI is all text in the default font
        for (int y = std::max(0, q.y0); y < std::min(360, q.y1); ++y) {
            for (int x = std::max(0, q.x0); x < std::min(640, q.x1); ++x) {
                Color c;
                float a;
                ShadeUIQuad(q, x + 0.5f, y + 0.5f, &c, &a);
                if (a >= 0.5f * q.alpha && rt.IdAt(x, y) == q.entity) drawn.insert(q.entity);
            }
        }
    }
    for (const auto& kv : e.GetScene().Pool<UIText>()) CHECK(!kv.second.visible || drawn.count(kv.first));
}

TEST(UITextFontsAndRichText) {
    // UTF-8 decoding and the embedded default font.
    std::vector<uint32_t> cps = DecodeUtf8("A\xea\xb0\x80\xff");
    CHECK(cps.size() == 3 && cps[0] == 'A' && cps[1] == 0xAC00 && cps[2] == 0xFFFD);
    std::shared_ptr<FontFace> roboto = FontFace::Default();
    CHECK(roboto && roboto->familyName == "Roboto" && roboto->HasGlyph('g') && !roboto->HasGlyph(0xAC00));
    FontFace::Glyph g = roboto->GetGlyph('H', 32, false);
    CHECK(g.page == 0 && g.w > 10 && g.h > 18 && g.y0 < 0 && g.advance > 15.0f);
    CHECK(roboto->GetGlyph('H', 32, true).w > g.w);  // synthetic bold is wider
    float w1, h1, w2, h2, wp, hp;
    MeasureText("Hello", 32, &w1, &h1, "default");
    MeasureText("Hello\nWorld", 32, &w2, &h2, "default");
    MeasureText("Hello", 32, &wp, &hp, "pixel");
    CHECK(w1 > 60 && w1 < 90 && std::fabs(w2 - w1) < 20 && h2 > h1 * 1.8f);
    CHECK(wp == (5 * 6 - 1) * 4.0f && hp == 7 * 4.0f);  // pixel font: 6x8 cells of 4 px

    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui_text"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // A font file in the project (the embedded Roboto written out).
    CreateDirectories(JoinPath(e.ProjectDir(), "assets/fonts"));
    CHECK(CopyFileTo(TestSourceDir() + "/third_party/fonts/Roboto-Regular.ttf", JoinPath(e.ProjectDir(), "assets/fonts/body.ttf")));
    Json info = Call(e, "asset.info", R"J({"path": "assets/fonts/body.ttf"})J")["result"];
    CHECK(info["kind"].asString() == "font" && info["family"].asString() == "Roboto");
    Call(e, "entity.create", R"J({"name": "Red", "components": {"UIText": {"text": "<color=#ff0000>WW</color>WW", "font": "assets/fonts/body.ttf", "x": 0, "y": 0, "size": 72}}})J");
    Call(e, "entity.create", R"J({"name": "Raw", "components": {"UIText": {"text": "<b>", "richText": false, "x": 0, "y": 200, "size": 40}}})J");
    Call(e, "entity.create", R"J({"name": "Wrapped", "components": {"UIText": {"text": "one two three four five six", "x": 0, "y": 400, "width": 120, "size": 24}}})J");
    Call(e, "entity.create", R"J({"name": "Typed", "components": {"UIText": {"text": "abc", "x": 600, "y": 0, "visibleCharacters": 0}}})J");
    RenderTarget rt;
    rt.Resize(1280, 720);
    RenderView view;
    MakeSceneView(e.GetScene(), 1280.0f / 720.0f, view);
    e.Renderer().Render(e.GetScene(), view, rt);
    int red = 0, white = 0;
    EntityId redId = e.GetScene().FindByName("Red");
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 400; ++x) {
            uint32_t c = rt.color[static_cast<size_t>(y) * 1280 + static_cast<size_t>(x)];
            if (rt.IdAt(x, y) != redId) continue;
            if ((c & 0xFF) > 200 && ((c >> 8) & 0xFF) < 60) ++red;
            if ((c & 0xFF) > 200 && ((c >> 8) & 0xFF) > 200) ++white;
        }
    }
    CHECK(red > 200 && white > 200);
    Json layout = Call(e, "ui.layout", R"J({"width": 1280, "height": 720})J")["result"]["elements"];
    CHECK(layout.size() == 4);
    CHECK(layout[1]["rect"][2].asNumber() > 40);   // "<b>" drawn literally: three glyphs
    CHECK(layout[2]["rect"][2].asNumber() == 120);  // the box width
    CHECK(layout[2]["rect"][3].asNumber() > 70);    // wrapped onto 3 lines
    for (const UIQuad& q : BuildUIQuads(e.GetScene(), 1280, 720, &e.Assets())) CHECK(q.entity != e.GetScene().FindByName("Typed"));
}

TEST(UILayoutAnchorsAndClipping) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui_layout"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // A fitted vertical menu: padding 10, spacing 5, children 100x40 and 120x30.
    Call(e, "entity.create", R"J({"name": "Menu", "components": {"UIPanel": {"anchor": "top-left", "x": 100, "y": 50, "width": 0, "height": 0}, "UILayout": {"direction": "vertical", "padding": 10, "spacing": 5, "fit": true}}})J");
    Call(e, "entity.create", R"J({"name": "A", "parent": "Menu", "components": {"UIButton": {"width": 100, "height": 40}}})J");
    Call(e, "entity.create", R"J({"name": "B", "parent": "Menu", "components": {"UIButton": {"width": 120, "height": 30, "order": -5}}})J");
    // Stretch inside a clipping panel; a child outside the clip cannot be clicked.
    Call(e, "entity.create", R"J({"name": "Frame", "components": {"UIPanel": {"anchor": "bottom-right", "x": -20, "y": -20, "width": 200, "height": 100, "clip": true}}})J");
    Call(e, "entity.create", R"J({"name": "Fill", "parent": "Frame", "components": {"UIImage": {"anchor": "stretch", "width": -20, "height": -20}}})J");
    Call(e, "entity.create", R"J({"name": "Hidden", "parent": "Frame", "components": {"UIButton": {"anchor": "top-left", "x": 150, "y": 0, "width": 100, "height": 50}}})J");
    Json els = Call(e, "ui.layout", R"J({"width": 1280, "height": 720})J")["result"]["elements"];
    std::map<std::string, Json> by;
    for (const Json& j : els.items()) by[j["name"].asString()] = j["rect"];
    auto rect = [&](const char* n, double x, double y, double w, double h) {
        const Json& r = by[n];
        return std::fabs(r[0].asNumber() - x) < 0.01 && std::fabs(r[1].asNumber() - y) < 0.01 && std::fabs(r[2].asNumber() - w) < 0.01 && std::fabs(r[3].asNumber() - h) < 0.01;
    };
    CHECK(rect("Menu", 100, 50, 140, 95));
    CHECK(rect("A", 110, 60, 100, 40));  // hierarchy order: A first although B has a lower draw order
    CHECK(rect("B", 110, 105, 120, 30));
    CHECK(rect("Frame", 1060, 600, 200, 100));
    CHECK(rect("Fill", 1070, 610, 180, 80));
    CHECK(Call(e, "input.click", R"J({"x": 1270, "y": 620, "width": 1280, "height": 720})J")["result"]["button"].asInt() == 0);  // clipped part
    CHECK(Call(e, "input.click", R"J({"x": 1220, "y": 620, "width": 1280, "height": 720})J")["result"]["buttonName"].asString() == "Hidden");
    // Horizontal row, centered, and a grid.
    Call(e, "component.set", R"J({"id": "Menu", "type": "UILayout", "values": {"direction": "horizontal", "crossAlign": "center"}})J");
    els = Call(e, "ui.layout", R"J({"width": 1280, "height": 720})J")["result"]["elements"];
    for (const Json& j : els.items()) by[j["name"].asString()] = j["rect"];
    CHECK(rect("Menu", 100, 50, 245, 60));
    CHECK(rect("B", 215, 65, 120, 30));
    // UICanvas: author for 1920x1080, match width.
    Call(e, "entity.create", R"J({"name": "Canvas", "components": {"UICanvas": {"referenceWidth": 1920, "referenceHeight": 1080, "match": 0}}})J");
    CHECK(std::fabs(Call(e, "ui.layout", R"J({"width": 960, "height": 720})J")["result"]["scale"].asNumber() - 0.5) < 1e-6);
}

TEST(UIInteractionHoverSliderAndDisabled) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui_input"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "script.write", R"J({"path": "scripts/ui_log.lua", "source": "local M = {}
log_ = log_ or {}
function M:onPointerEnter() table.insert(log_, 'enter') end
function M:onPointerExit() table.insert(log_, 'exit') end
function M:onClick() table.insert(log_, 'click') end
function M:onValueChanged(v) table.insert(log_, string.format('value %.2f', v)) end
return M
"})J");
    Call(e, "entity.create", R"J({"name": "Btn", "components": {"UIButton": {"anchor": "top-left", "x": 0, "y": 0, "width": 200, "height": 100}, "Script": {"path": "scripts/ui_log.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Off", "components": {"UIButton": {"anchor": "top-left", "x": 300, "y": 0, "width": 200, "height": 100, "interactable": false}, "Script": {"path": "scripts/ui_log.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Vol", "components": {"UISlider": {"anchor": "top-left", "x": 0, "y": 200, "width": 400, "height": 40, "value": 0, "step": 0.25}, "Script": {"path": "scripts/ui_log.lua"}}})J");
    auto logText = [&] {
        return Call(e, "script.eval", R"J({"code": "table.concat(log_ or {}, ',')"})J")["result"]["value"].asString();
    };
    Call(e, "input.mouse", R"J({"x": 100, "y": 50, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(e.GetScene().Get<UIButton>(e.GetScene().FindByName("Btn"))->hovered);
    Call(e, "input.mouse", R"J({"x": 400, "y": 50, "width": 1280, "height": 720, "button": "MouseLeft", "down": true})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    Call(e, "input.mouse", R"J({"button": "MouseLeft", "down": false})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(logText() == "enter,exit");  // the disabled button neither hovers nor clicks
    // Press on the slider at 60 % and drag past its end: snapped values, callback per change.
    Call(e, "input.mouse", R"J({"x": 240, "y": 220, "width": 1280, "height": 720, "button": "MouseLeft", "down": true})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    Call(e, "input.mouse", R"J({"x": 900, "y": 500, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(e.GetScene().Get<UISlider>(e.GetScene().FindByName("Vol"))->value == 1.0f);
    Call(e, "input.mouse", R"J({"button": "MouseLeft", "down": false})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(logText() == "enter,exit,enter,value 0.50,exit,value 1.00");
    Call(e, "input.click", R"J({"x": 100, "y": 50, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(logText() == "enter,exit,enter,value 0.50,exit,value 1.00,enter,click");
    CHECK(e.Scripts().Errors().empty());
    Call(e, "sim.stop", "{}");
    CHECK(e.GetScene().Get<UISlider>(e.GetScene().FindByName("Vol"))->value == 0.0f);
}

// Mean color of a pixel block in a rendered target.
Color MeanColor(const RenderTarget& rt, int x0, int y0, int x1, int y1) {
    double r = 0, g = 0, b = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            uint32_t c = rt.color[static_cast<size_t>(y) * static_cast<size_t>(rt.width) + static_cast<size_t>(x)];
            r += c & 0xFF;
            g += (c >> 8) & 0xFF;
            b += (c >> 16) & 0xFF;
            ++n;
        }
    }
    return Color(static_cast<float>(r / n / 255), static_cast<float>(g / n / 255), static_cast<float>(b / n / 255));
}

TEST(MaterialsFilesAndPbr) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("materials"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // Files: create, validation errors with hints, set, listing, info.
    Json made = Call(e, "material.create", R"J({"path": "materials/gold.mat.json", "values": {"baseColor": [1, 0.8, 0.2], "metallic": 1, "roughness": 0.3}})J");
    CHECK(made["ok"].asBool() && made["result"]["values"]["roughness"].asNumber() > 0.29);
    CHECK(Call(e, "material.create", R"J({"path": "materials/gold.mat.json"})J")["error"]["code"].asString() == "already_exists");
    Json bad = Call(e, "material.create", R"J({"path": "materials/bad.mat.json", "values": {"shininess": 3}})J");
    CHECK(bad["error"]["code"].asString() == "invalid_material" && bad["error"]["hint"].asString().find("roughness") != std::string::npos);
    CHECK(Call(e, "material.create", R"J({"path": "materials/x.json"})J")["error"]["code"].asString() == "invalid_path");
    CHECK(Call(e, "material.create", R"J({"path": "materials/m.mat.json", "values": {"baseTexture": "missing.png"}})J")["error"]["code"].asString() == "invalid_material");
    Json glass = Call(e, "material.create", R"J({"path": "materials/glass.mat.json", "values": {"opacity": 0.4}})J");
    CHECK(glass["result"]["values"]["alphaMode"].asString() == "blend");
    CHECK(Call(e, "asset.list", R"J({"kind": "material"})J")["result"].size() == 2);
    CHECK(Call(e, "asset.info", R"J({"path": "materials/glass.mat.json"})J")["result"]["alphaMode"].asString() == "blend");

    // A sphere in front of the camera, lit head-on.
    Call(e, "entity.create", R"J({"name": "Cam", "components": {"Transform": {"position": [0, 0, 3]}, "Camera": {"clearColor": [0, 0, 0]}}})J");
    Call(e, "entity.create", R"J({"name": "Sun", "components": {"Transform": {"rotation": [0, 0, 0]}, "DirectionalLight": {"ambient": [0.2, 0.2, 0.2]}}})J");
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"MeshRenderer": {"mesh": "sphere", "color": [1, 1, 1], "material": "materials/gold.mat.json"}, "Transform": {"scale": [2, 2, 2]}}})J");
    RenderView view;
    MakeSceneView(e.GetScene(), 1.0f, view);
    RenderTarget rt;
    rt.Resize(128, 128);
    e.Renderer().Render(e.GetScene(), view, rt);
    Color center = MeanColor(rt, 60, 60, 68, 68);  // the specular highlight of a smooth metal
    Color rim = MeanColor(rt, 63, 36, 65, 38);
    CHECK(center.r > 0.9f && center.g > 0.7f);  // bright highlight
    CHECK(rim.r < center.r && rim.r > rim.b + 0.1f);  // gold-tinted reflections away from it
    // Rougher: a dimmer, wider highlight. Materials reload when set.
    Call(e, "material.set", R"J({"path": "materials/gold.mat.json", "values": {"roughness": 0.9, "metallic": 0}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    Color rough = MeanColor(rt, 60, 60, 68, 68);
    CHECK(rough.r < center.r + 0.01f && rough.b < 0.35f);
    // Emissive glows without light; unlit ignores it.
    Call(e, "material.create", R"J({"path": "materials/neon.mat.json", "values": {"baseColor": [0, 0, 0], "emissive": [0, 1, 0]}})J");
    Call(e, "component.set", R"J({"id": "Ball", "type": "MeshRenderer", "values": {"material": "materials/neon.mat.json"}})J");
    Call(e, "component.set", R"J({"id": "Sun", "type": "DirectionalLight", "values": {"intensity": 0, "ambient": [0, 0, 0]}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    Color glow = MeanColor(rt, 60, 60, 68, 68);
    CHECK(glow.g > 0.95f && glow.r < 0.05f);
    // A broken material file renders magenta (like a missing mesh).
    Call(e, "component.set", R"J({"id": "Ball", "type": "MeshRenderer", "values": {"material": "materials/none.mat.json"}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    Color magenta = MeanColor(rt, 60, 60, 68, 68);
    CHECK(magenta.r > 0.95f && magenta.g < 0.05f && magenta.b > 0.95f);
}

TEST(TransparencyNormalMapsAndDoubleSided) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("transparency"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Cam", "components": {"Transform": {"position": [0, 0, 5]}, "Camera": {"clearColor": [0, 0, 0]}}})J");
    // Created front to back on purpose: the draw list sorts transparent parts back to front.
    Call(e, "entity.create", R"J({"name": "Front", "components": {"MeshRenderer": {"mesh": "quad", "color": [0, 0, 1], "unlit": true, "opacity": 0.5}, "Transform": {"position": [0, 0, 1], "scale": [2, 2, 1]}}})J");
    Call(e, "entity.create", R"J({"name": "Middle", "components": {"MeshRenderer": {"mesh": "quad", "color": [0, 1, 0], "unlit": true, "opacity": 0.5}, "Transform": {"position": [0, 0, 0.5], "scale": [2, 2, 1]}}})J");
    Call(e, "entity.create", R"J({"name": "Back", "components": {"MeshRenderer": {"mesh": "quad", "color": [1, 0, 0], "unlit": true}, "Transform": {"scale": [2, 2, 1]}}})J");
    RenderView view;
    MakeSceneView(e.GetScene(), 1.0f, view);
    RenderTarget rt;
    rt.Resize(64, 64);
    e.Renderer().Render(e.GetScene(), view, rt);
    Color c = MeanColor(rt, 30, 30, 34, 34);
    // red, then green at 50 %, then blue at 50 %: (0.25, 0.25, 0.5)
    CHECK(std::fabs(c.r - 0.25f) < 0.02f && std::fabs(c.g - 0.25f) < 0.02f && std::fabs(c.b - 0.5f) < 0.02f);
    CHECK(rt.IdAt(32, 32) == e.GetScene().FindByName("Front"));  // at least half opaque: pickable
    // An opaque surface in front hides transparent ones behind it (depth test, no depth write for glass).
    Call(e, "component.set", R"J({"id": "Back", "type": "Transform", "values": {"position": [0, 0, 2]}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    c = MeanColor(rt, 30, 30, 34, 34);
    CHECK(c.r > 0.98f && c.g < 0.02f && c.b < 0.02f);
    // Sprites: alphaCutoff 0 blends the image's alpha.
    Call(e, "component.set", R"J({"id": "Back", "type": "Transform", "values": {"position": [0, 0, -1]}})J");
    Call(e, "component.set", R"J({"id": "Front", "type": "MeshRenderer", "values": {"visible": false}})J");
    Call(e, "component.set", R"J({"id": "Middle", "type": "MeshRenderer", "values": {"visible": false}})J");
    Image img;
    img.width = img.height = 4;
    img.rgba.assign(64, 255);
    for (size_t i = 3; i < 64; i += 4) img.rgba[i] = 128;  // white at 50 % alpha
    CHECK(WritePng(JoinPath(e.ProjectDir(), "soft.png"), img, true));
    Call(e, "entity.create", R"J({"name": "Smoke", "components": {"Sprite": {"texture": "soft.png", "alphaCutoff": 0, "pixelsPerUnit": 1, "pixelArt": true}}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    c = MeanColor(rt, 30, 30, 34, 34);
    CHECK(std::fabs(c.r - 1.0f) < 0.02f && std::fabs(c.g - 0.5f) < 0.03f);  // white over red at ~50 %

    // Normal maps: a map tilted towards +u makes a plane lit from +x brighter.
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Cam", "components": {"Transform": {"position": [0, 4, 0], "rotation": [-90, 0, 0]}, "Camera": {"clearColor": [0, 0, 0]}}})J");
    Call(e, "entity.create", R"J({"name": "Sun", "components": {"Transform": {"rotation": [-30, 90, 0]}, "DirectionalLight": {"ambient": [0, 0, 0]}}})J");
    Call(e, "entity.create", R"J({"name": "Floor", "components": {"MeshRenderer": {"mesh": "plane", "color": [1, 1, 1]}, "Transform": {"scale": [4, 1, 4]}}})J");
    MakeSceneView(e.GetScene(), 1.0f, view);
    e.Renderer().Render(e.GetScene(), view, rt);
    Color flat = MeanColor(rt, 28, 28, 36, 36);
    Image nm;
    nm.width = nm.height = 4;
    nm.rgba.clear();
    for (int i = 0; i < 16; ++i) nm.rgba.insert(nm.rgba.end(), {218, 128, 218, 255});  // normal leaning to +u (+x on the plane)
    CHECK(WritePng(JoinPath(e.ProjectDir(), "tilt.png"), nm, true));
    Call(e, "material.create", R"J({"path": "tilt.mat.json", "values": {"normalTexture": "tilt.png", "roughness": 1}})J");
    Call(e, "component.set", R"J({"id": "Floor", "type": "MeshRenderer", "values": {"material": "tilt.mat.json"}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    Color bumped = MeanColor(rt, 28, 28, 36, 36);
    CHECK(bumped.r > flat.r + 0.1f);
    // Double-sided: seen from below, a plane only shows when its material is double-sided.
    Call(e, "component.set", R"J({"id": "Cam", "type": "Transform", "values": {"position": [0, -4, 0], "rotation": [90, 0, 0]}})J");
    Call(e, "component.set", R"J({"id": "Floor", "type": "MeshRenderer", "values": {"material": "", "unlit": true}})J");
    MakeSceneView(e.GetScene(), 1.0f, view);
    e.Renderer().Render(e.GetScene(), view, rt);
    CHECK(rt.IdAt(32, 32) == kNullEntity);
    Call(e, "material.create", R"J({"path": "two.mat.json", "values": {"doubleSided": true}})J");
    Call(e, "component.set", R"J({"id": "Floor", "type": "MeshRenderer", "values": {"material": "two.mat.json"}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    CHECK(rt.IdAt(32, 32) == e.GetScene().FindByName("Floor"));
}

TEST(GltfMaterialsAreLoaded) {
    // A glTF with a full metallic-roughness material: factors, alpha mode, double sided, emissive strength.
    std::string dir = TempProject("gltf_materials");
    const char* gltf = R"J({
      "asset": {"version": "2.0"}, "extensionsUsed": ["KHR_materials_emissive_strength"],
      "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
      "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.4, 0.6, 0.5], "metallicFactor": 0.25, "roughnessFactor": 0.75},
                     "emissiveFactor": [1, 0.5, 0], "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 3}},
                     "alphaMode": "BLEND", "doubleSided": true}],
      "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
      "bufferViews": [{"buffer": 0, "byteLength": 36}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]}]
    })J";
    CHECK(WriteTextFile(JoinPath(dir, "tri.gltf"), gltf));
    Mesh mesh;
    std::string err;
    CHECK(LoadModelFile(JoinPath(dir, "tri.gltf"), mesh, &err));
    CHECK(mesh.materials.size() == 1 && mesh.submeshes.size() == 1 && mesh.submeshes[0].material == 0);
    const Material& m = mesh.materials[0];
    CHECK(std::fabs(m.baseColor.g - 0.4f) < 1e-6f && std::fabs(m.opacity - 0.5f) < 1e-6f);
    CHECK(std::fabs(m.metallic - 0.25f) < 1e-6f && std::fabs(m.roughness - 0.75f) < 1e-6f);
    CHECK(m.alphaMode == AlphaMode::Blend && m.doubleSided && m.emissiveIntensity == 3.0f && m.emissive.g == 0.5f);
    CHECK(mesh.tangents.size() == mesh.positions.size());
}

TEST(UIGpuMatchesSoftware) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui_gpu"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Win", "components": {"UIPanel": {"anchor": "center", "x": 0, "y": 0, "width": 700, "height": 500, "radius": 24, "borderWidth": 4, "color": [0.1, 0.1, 0.2], "opacity": 0.9}, "UILayout": {"padding": 30, "spacing": 16, "crossAlign": "stretch"}}})J");
    Call(e, "entity.create", R"J({"name": "T", "parent": "Win", "components": {"UIText": {"text": "Title <b>bold</b> <color=orange>orange</color>", "size": 40, "outlineWidth": 2}}})J");
    Call(e, "entity.create", R"J({"name": "S", "parent": "Win", "components": {"UISlider": {"value": 0.4}}})J");
    Call(e, "entity.create", R"J({"name": "I", "parent": "Win", "components": {"UIImage": {"height": 60, "color": [0.9, 0.3, 0.3], "radius": 30, "fill": 0.6}}})J");
    Call(e, "entity.create", R"J({"name": "B", "parent": "Win", "components": {"UIButton": {"text": "OK"}}})J");
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    const int w = 640, h = 360;
    RenderView view;
    MakeSceneView(e.GetScene(), static_cast<float>(w) / h, view);
    RenderTarget sw, gpu;
    sw.Resize(w, h);
    gpu.Resize(w, h);
    e.Renderer().Render(e.GetScene(), view, sw);
    e.Gpu()->Render(e.GetScene(), view, gpu);
    // The frame is opaque: UI text/panels blended over it must not punch alpha
    // holes (they showed as dark boxes behind glyphs in the native editor).
    bool opaque = true;
    for (uint32_t c : gpu.color) opaque = opaque && (c >> 24) == 0xFF;
    CHECK(opaque);
    double total = 0;
    int outliers = 0;
    for (size_t i = 0; i < sw.color.size(); ++i) {
        int worst = 0;
        for (int k = 0; k < 3; ++k) {
            int d = std::abs(static_cast<int>((sw.color[i] >> (8 * k)) & 0xFF) - static_cast<int>((gpu.color[i] >> (8 * k)) & 0xFF));
            total += d;
            worst = std::max(worst, d);
        }
        outliers += worst > 24;
    }
    double mean = total / (sw.color.size() * 3.0);
    std::printf("  UI mean channel difference %.3f, outliers %d\n", mean, outliers);
    CHECK(mean < 0.5 && outliers < 50);
}

TEST(CameraPostProcessing) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("postprocess"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},"Camera":{"projection":"orthographic","clearColor":[1,1,1]}}})J");
    Call(e, "entity.create", R"J({"name":"UI","components":{"UIPanel":{"anchor":"top-left","x":0,"y":0,"width":200,"height":200,"color":[1,0,0],"opacity":1}}})J");
    RenderView view;
    CHECK(MakeSceneView(e.GetScene(), 128.0f / 72, view));
    RenderTarget original, neutral, effect;
    for (RenderTarget* target : {&original, &neutral, &effect}) target->Resize(128, 72);
    e.Renderer().Render(e.GetScene(), view, original);
    Call(e, "component.add", R"J({"id":"Camera","type":"PostProcess"})J");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, neutral);
    CHECK(original.Hash() == neutral.Hash());  // merely adding default settings never changes an existing frame
    Call(e, "component.set", R"J({"id":"Camera","type":"PostProcess","values":{"vignette":0.8,"vignetteRadius":0.3,"vignetteSoftness":0.5}})J");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    SetMaxRenderThreads(1);
    e.Renderer().Render(e.GetScene(), view, effect);
    SetMaxRenderThreads(4);
    e.Renderer().Render(e.GetScene(), view, neutral);
    SetMaxRenderThreads(16);
    CHECK(effect.Hash() == neutral.Hash() && effect.Hash() != original.Hash());
    CHECK((effect.color.back() & 255) < 60);  // edges darken, the center and overlaid UI retain their colors
    CHECK(effect.color[36 * 128 + 64] == original.color[36 * 128 + 64]);
    CHECK(effect.color[10 * 128 + 10] == original.color[10 * 128 + 10]);  // opaque UI retains its own color
    CHECK((effect.color[10 * 128 + 10] & 255) == 255);
    CHECK(effect.ids == original.ids && effect.depth == original.depth);
    Scene restored;
    CHECK(restored.FromJson(e.GetScene().ToJson(), &err));
    CHECK(restored.Get<PostProcess>(restored.FindByName("Camera"))->vignette == 0.8f);
    Call(e, "history.undo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, neutral);
    CHECK(neutral.Hash() == original.Hash());
    Call(e, "history.redo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    if (e.EnableGpu(nullptr, &err)) {
        RenderTarget gpu;
        gpu.Resize(128, 72);
        e.Gpu()->Render(e.GetScene(), view, gpu);
        double error = 0;
        for (size_t i = 0; i < effect.color.size(); ++i) for (int shift : {0, 8, 16}) {
            int a = static_cast<int>((effect.color[i] >> shift) & 255);
            int b = static_cast<int>((gpu.color[i] >> shift) & 255);
            error += std::abs(a - b);
        }
        double mean = error / static_cast<double>(effect.color.size() * 3);
        std::printf("  vignette software/GPU mean difference %.4f\n", mean);
        CHECK(mean < 1);
    } else std::printf("  SKIP postprocess GPU comparison (%s)\n", err.c_str());
    Call(e, "entity.create", R"J({"name":"Cube","components":{"Transform":{"position":[-4,3,0]},"MeshRenderer":{"unlit":true,"color":[0.2,0.4,0.6]}}})J");
    view.highlight = e.GetScene().FindByName("Cube");
    view.postProcess.vignette = 0;
    e.Renderer().Render(e.GetScene(), view, original);
    view.postProcess.vignette = 0.8f;
    e.Renderer().Render(e.GetScene(), view, effect);
    int outlinePixels = 0;
    for (size_t i = 0; i < original.color.size(); ++i) {
        uint32_t c = original.color[i];
        if ((c & 255) == 255 && ((c >> 8) & 255) == 158 && ((c >> 16) & 255) == 26) {
            ++outlinePixels;
            CHECK(effect.color[i] == c);  // the vignette never darkens editor selection feedback
        }
    }
    CHECK(outlinePixels > 0);
    // Reusing a view for another camera must not carry over the first camera's effects.
    Call(e, "component.set", R"J({"id":"Camera","type":"Camera","values":{"active":false}})J");
    Call(e, "entity.create", R"J({"name":"OtherCamera","components":{"Camera":{}}})J");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    CHECK(view.postProcess.vignette == 0);
    CHECK(MakeLookAtView(Vec3(0,0,10), Vec3(0,0,0), 60, 1).postProcess.vignette == 0);
}

TEST(GpuRendererMatchesSoftware) {
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Showcase", &err));
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
    // Clear and restore selection on reused targets: no stale outline in game frames.
    EntityId highlight = view.highlight;
    view.highlight = kNullEntity;
    e.Gpu()->Render(e.GetScene(), view, again);
    int orangeWithoutSelection = 0;
    for (uint32_t c : again.color) orangeWithoutSelection += orange(c);
    CHECK(orangeWithoutSelection < orangeGpu);
    view.highlight = highlight;
    e.Gpu()->Render(e.GetScene(), view, again);
    CHECK(again.Hash() == gpu.Hash());
    // render.screenshot {renderer: gpu} goes through the same renderer.
    Json shot = Call(e, "render.screenshot", R"J({"renderer": "gpu", "width": 64, "height": 36, "inline": false})J");
    CHECK(shot["ok"].asBool() && shot["result"]["renderer"].asString() == e.Gpu()->Name());
    CHECK(!Call(e, "render.screenshot", R"J({"renderer": "vulkan", "inline": false})J")["ok"].asBool());
}

TEST(AnimatedSkinRenderers) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("skin_renderers"), &err));
    CHECK(CopyFileTo(TestSourceDir() + "/samples/Showcase/assets/models/fox.glb", JoinPath(e.ProjectDir(), "assets/models/fox.glb")));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Fox","components":{"Transform":{"scale":[0.012,0.012,0.012]},"MeshRenderer":{"mesh":"assets/models/fox.glb","color":[1,1,1]},"Animator":{"clip":"Walk","playing":false}}})J");
    Call(e, "entity.create", R"J({"name":"Sun","components":{"Transform":{"rotation":[-65,25,0]},"DirectionalLight":{"shadowStrength":0.8}}})J");
    Call(e, "entity.create", R"J({"name":"Ground","components":{"Transform":{"scale":[8,1,8],"position":[0,-0.01,0]},"MeshRenderer":{"mesh":"plane","color":[0.5,0.5,0.5]}}})J");
    EntityId fox = e.GetScene().FindByName("Fox");
    RenderView view = MakeLookAtView(Vec3(2,1.5f,2), Vec3(0,0.45f,0), 45, 320.0f/180);
    view.drawUI = false;
    RenderTarget sw, again, gpu;
    sw.Resize(320,180);
    again.Resize(320,180);
    gpu.Resize(320,180);
    e.Renderer().Render(e.GetScene(), view, sw);
    uint64_t start = sw.Hash();
    Call(e, "component.set", R"J({"id":"Fox","type":"Animator","values":{"time":0.25}})J");
    SetMaxRenderThreads(1);
    e.Renderer().Render(e.GetScene(), view, sw);
    SetMaxRenderThreads(16);
    e.Renderer().Render(e.GetScene(), view, again);
    CHECK(sw.Hash() != start && sw.Hash() == again.Hash() && CountId(sw, fox) > 500);
    CHECK(WritePng("build/animated-walk-software.png", sw.ToImage(), true));
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    for (const char* clip : {"Survey", "Walk", "Run"}) {
        Json args = Json::parse(R"J({"id":"Fox","type":"Animator","values":{"time":0.25,"playing":false}})J");
        args["values"]["clip"] = clip;
        e.Call("component.set", args);
        for (const char* shading : {"smooth", "flat"}) {
            Json material = Json::parse(R"J({"id":"Fox","type":"MeshRenderer","values":{}})J");
            material["values"]["shading"] = shading;
            e.Call("component.set", material);
            e.Renderer().Render(e.GetScene(), view, sw);
            e.Gpu()->Render(e.GetScene(), view, gpu);
            double total = 0, interior = 0;
            int interiorCount = 0, outliers = 0;
            for (int y = 1; y < 179; ++y) for (int x = 1; x < 319; ++x) {
                size_t i = static_cast<size_t>(y)*320 + static_cast<size_t>(x);
                bool inside = sw.ids[i] == fox && sw.ids[i-1] == fox && sw.ids[i+1] == fox &&
                              sw.ids[i-320] == fox && sw.ids[i+320] == fox;
                int worst = 0;
                for (int ch = 0; ch < 3; ++ch) {
                    int diff = std::abs(static_cast<int>((sw.color[i] >> (8*ch)) & 255) - static_cast<int>((gpu.color[i] >> (8*ch)) & 255));
                    total += diff;
                    if (inside) interior += diff;
                    worst = std::max(worst, diff);
                }
                interiorCount += inside;
                outliers += worst > 64;
            }
            double mean = total / (sw.color.size()*3.0);
            double foxMean = interiorCount ? interior / (interiorCount*3.0) : 255;
            std::printf("  skin %s/%s: mean %.2f, Fox interior %.2f, outliers %d\n", clip, shading, mean, foxMean, outliers);
            CHECK(mean < 2 && foxMean < 8 && outliers < static_cast<int>(sw.color.size())/100);
            e.Gpu()->Render(e.GetScene(), view, again);
            CHECK(again.Hash() == gpu.Hash());
            if (std::string(clip) == "Walk" && std::string(shading) == "smooth")
                CHECK(WritePng("build/animated-walk-gpu.png", gpu.ToImage(), true));
        }
    }
    view.highlight = fox;
    e.Renderer().Render(e.GetScene(), view, sw);
    e.Gpu()->Render(e.GetScene(), view, gpu);
    auto orange = [](uint32_t c) { return (c & 255) > 230 && ((c >> 8) & 255) > 140 && ((c >> 8) & 255) < 175 && ((c >> 16) & 255) < 50; };
    CHECK(std::count_if(sw.color.begin(), sw.color.end(), orange) > 30);
    CHECK(std::count_if(gpu.color.begin(), gpu.color.end(), orange) > 30);
}

TEST(WebGamePak) {
    // `oe package --web` data: the page unpacks game.pak into /game.
    std::string dir = TestSourceDir() + "/samples/Hello";
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

TEST(AndroidGamePakExtract) {
    // The Android player unpacks assets/game.pak into its data folder.
    std::string dir = TestSourceDir() + "/samples/Hello";
    std::vector<std::string> files = GameFiles(dir);
    std::string err;
    CHECK(WriteGamePak(dir, files, "build/test_android/game.pak", &err));
    std::vector<unsigned char> pak;
    CHECK(ReadBinaryFile("build/test_android/game.pak", pak));
    CHECK(ExtractGamePak(pak, "build/test_android/game", &err));
    for (const std::string& f : files) {
        std::vector<unsigned char> a, b;
        CHECK(ReadBinaryFile(dir + "/" + f, a) && ReadBinaryFile("build/test_android/game/" + f, b) && a == b);
    }
    Engine e;
    CHECK(e.Open("build/test_android/game", &err));
    // Paths that would leave the folder are refused.
    auto pakWith = [](const std::string& path) {
        std::string index = "{\"files\":[{\"path\":\"" + path + "\",\"offset\":0,\"size\":1}]}";
        std::vector<unsigned char> p = {'O', 'E', 'P', 'A', 'K', '0', '0', '1'};
        for (int i = 0; i < 4; ++i) p.push_back(static_cast<unsigned char>((index.size() >> (8 * i)) & 0xFF));
        p.insert(p.end(), index.begin(), index.end());
        p.push_back('x');
        return p;
    };
    CHECK(ExtractGamePak(pakWith("ok/file.txt"), "build/test_android/x", &err));
    for (const char* bad : {"../evil.txt", "a/../../evil.txt", "/abs.txt", "c:/evil.txt", ".."}) {
        CHECK(!ExtractGamePak(pakWith(bad), "build/test_android/x", &err));
    }
    CHECK(!ExtractGamePak(std::vector<unsigned char>{'n', 'o', 'p', 'e'}, "build/test_android/x", &err));
    RemoveAll("build/test_android");
}

TEST(ZipWriterAlignment) {
    // APK rules: stored entries 4-byte aligned, native libraries page aligned.
    ZipWriter zip;
    const std::string a = "hello", b(1000, 'b');
    zip.AddStored("a.txt", reinterpret_cast<const unsigned char*>(a.data()), a.size(), 4);
    zip.AddStored("lib/arm64-v8a/libx.so", reinterpret_cast<const unsigned char*>(b.data()), b.size(), 16384);
    zip.AddStored("empty", nullptr, 0, 4);
    std::vector<unsigned char> bytes = zip.Finish();
    std::vector<ZipEntry> entries;
    std::string err;
    CHECK(ReadZip(bytes, entries, &err) && entries.size() == 3);
    if (entries.size() == 3) {
        CHECK(entries[0].name == "a.txt" && std::string(entries[0].data.begin(), entries[0].data.end()) == a);
        CHECK(entries[0].crc == Crc32(reinterpret_cast<const unsigned char*>(a.data()), a.size()));
        CHECK(entries[1].size == b.size() && entries[2].data.empty());
        // Data offsets: find each entry's bytes in the archive.
        auto offsetOf = [&](const ZipEntry& e) {
            auto it = std::search(bytes.begin(), bytes.end(), e.data.begin(), e.data.end());
            return static_cast<size_t>(it - bytes.begin());
        };
        CHECK(offsetOf(entries[0]) % 4 == 0);
        CHECK(offsetOf(entries[1]) % 16384 == 0);
        // Raw copies keep the data (as when merging archives).
        ZipWriter copy;
        for (const ZipEntry& e : entries) copy.AddRaw(e);
        std::vector<ZipEntry> again;
        CHECK(ReadZip(copy.Finish(), again, &err) && again.size() == 3 && again[1].data == entries[1].data);
    }
    CHECK(Crc32(reinterpret_cast<const unsigned char*>("123456789"), 9) == 0xCBF43926u);
    CHECK(!ReadZip(std::vector<unsigned char>(30, 0), entries, &err));
}

TEST(AndroidApk) {
    CHECK(IsValidAndroidPackageName("com.example.game") && IsValidAndroidPackageName("a.b_2"));
    CHECK(!IsValidAndroidPackageName("game") && !IsValidAndroidPackageName("com.1game") && !IsValidAndroidPackageName("com..x") &&
          !IsValidAndroidPackageName("com.my-game") && !IsValidAndroidPackageName("com.x."));
    CHECK(DefaultAndroidPackageName("My Game!") == "com.ownengine.mygame");
    CHECK(DefaultAndroidPackageName("2048") == "com.ownengine.game2048");
    CHECK(IsValidAndroidPackageName(DefaultAndroidPackageName("한글")));

    AndroidAppInfo app;
    app.packageName = "com.example.hello";
    app.label = "Hello \xEC\x95\x88\xEB\x85\x95";  // UTF-8 label -> UTF-16 in the string pool
    app.orientation = "portrait";
    app.hasIcon = true;
    std::vector<unsigned char> manifest = BuildAndroidManifest(app);
    CHECK(manifest.size() > 8 && manifest[0] == 0x03 && manifest[1] == 0x00);  // RES_XML_TYPE
    CHECK((static_cast<size_t>(manifest[4]) | (static_cast<size_t>(manifest[5]) << 8) | (static_cast<size_t>(manifest[6]) << 16) |
           (static_cast<size_t>(manifest[7]) << 24)) == manifest.size());
    auto hasUtf16 = [](const std::vector<unsigned char>& data, const std::u16string& text) {
        std::vector<unsigned char> needle;
        for (char16_t c : text) {
            needle.push_back(static_cast<unsigned char>(c & 0xFF));
            needle.push_back(static_cast<unsigned char>(c >> 8));
        }
        return std::search(data.begin(), data.end(), needle.begin(), needle.end()) != data.end();
    };
    CHECK(hasUtf16(manifest, u"android.app.NativeActivity") && hasUtf16(manifest, u"oe_player") && hasUtf16(manifest, u"com.example.hello"));
    CHECK(hasUtf16(manifest, u"Hello \uC548\uB155"));
    std::vector<unsigned char> arsc = BuildAndroidResources(app.packageName);
    CHECK(arsc.size() > 12 && arsc[0] == 0x02 && hasUtf16(arsc, u"res/drawable/icon.png"));

    // A complete (unsigned) APK with a stand-in library.
    std::string err;
    std::vector<unsigned char> so(5000, 0x7f);
    CreateDirectories("build/test_apk/arm64-v8a");
    FILE* f = std::fopen("build/test_apk/arm64-v8a/liboe_player.so", "wb");
    CHECK(f && std::fwrite(so.data(), 1, so.size(), f) == so.size());
    if (f) std::fclose(f);
    ApkContents contents;
    contents.app = app;
    contents.nativeLibs = {{"arm64-v8a", "build/test_apk/arm64-v8a/liboe_player.so"}};
    contents.gamePak = {'O', 'E', 'P', 'A', 'K', '0', '0', '1', 0, 0, 0, 0};
    CHECK(WriteUnsignedApk(contents, "build/test_apk/out.apk", &err));
    std::vector<unsigned char> apk;
    std::vector<ZipEntry> entries;
    CHECK(ReadBinaryFile("build/test_apk/out.apk", apk) && ReadZip(apk, entries, &err));
    std::set<std::string> names;
    for (const ZipEntry& e : entries) names.insert(e.name);
    for (const char* n : {"AndroidManifest.xml", "resources.arsc", "res/drawable/icon.png", "lib/arm64-v8a/liboe_player.so", "assets/game.pak", "assets/game.id"}) {
        CHECK(names.count(n) == 1);
    }
    CHECK(entries.size() >= 4 && entries[3].data == so);
    // The same contents as an app bundle (Google Play): proto manifest + resources in base/.
    CHECK(WriteUnsignedAppBundle(contents, "build/test_apk/out.aab", &err));
    std::vector<unsigned char> aab;
    CHECK(ReadBinaryFile("build/test_apk/out.aab", aab) && ReadZip(aab, entries, &err));
    names.clear();
    for (const ZipEntry& e : entries) names.insert(e.name);
    for (const char* n : {"BundleConfig.pb", "base/manifest/AndroidManifest.xml", "base/resources.pb", "base/res/drawable/icon.png",
                          "base/lib/arm64-v8a/liboe_player.so", "base/assets/game.pak", "base/assets/game.id"}) {
        CHECK(names.count(n) == 1);
    }
    auto has = [](const std::vector<unsigned char>& data, const std::string& text) {
        return std::search(data.begin(), data.end(), text.begin(), text.end(),
                           [](unsigned char x, char y) { return x == static_cast<unsigned char>(y); }) != data.end();
    };
    std::vector<unsigned char> proto = BuildAndroidManifestProto(app);
    CHECK(proto.size() > 2 && proto[0] == 0x0A);  // XmlNode.element, length-delimited
    CHECK(has(proto, "android.app.NativeActivity") && has(proto, "oe_ANativeActivity_onCreate") && has(proto, app.label));
    CHECK(has(BuildAndroidResourcesProto(app.packageName), "res/drawable/icon.png"));
    contents.app.packageName = "nope";
    CHECK(!WriteUnsignedApk(contents, "build/test_apk/bad.apk", &err));
    CHECK(!WriteUnsignedAppBundle(contents, "build/test_apk/bad.aab", &err));
    RemoveAll("build/test_apk");
}

TEST(AgentInstructionsInSync) {
    // AGENTS.md (read by most coding agents) and CLAUDE.md must say the same thing (docs/DESIGN.md).
    std::string agents, claude;
    CHECK(ReadTextFile(TestSourceDir() + "/AGENTS.md", agents) && ReadTextFile(TestSourceDir() + "/CLAUDE.md", claude));
    CHECK(!agents.empty() && agents == claude);
    CHECK(claude.find("docs/DESIGN.md") != std::string::npos);
}

TEST(ScriptCheckAndParams) {
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/FPS", &err));
    // Syntax error: line and message, nothing runs.
    Json r = e.Call("script.check", Json::parse(R"J({"path": "scripts/bad.lua", "source": "local T = {}\nfunction T:onUpdate()\n  print(1\nend\nreturn T"})J"));
    CHECK(r["ok"].asBool() && !r["result"]["ok"].asBool());
    CHECK(r["result"]["diagnostics"][0]["line"].asInt() == 4 && r["result"]["diagnostics"][0]["severity"].asString() == "error");
    // Compiles, but a global is assigned and an unknown one is read.
    r = e.Call("script.check", Json::parse(R"J({"source": "local T = {}\nfunction T:onUpdate(dt)\n  hits = 1\n  lgo.info(math.floor(dt))\n  log.info(scene.find('A'))\nend\nreturn T"})J"));
    CHECK(r["result"]["ok"].asBool() && r["result"]["warnings"].asInt() == 2);
    CHECK(r["result"]["diagnostics"][0]["line"].asInt() == 3 && r["result"]["diagnostics"][1]["line"].asInt() == 4);
    // Every sample script is clean.
    for (const char* path : {"scripts/fps_player.lua", "scripts/target.lua"}) {
        r = e.Call("script.check", Json(Json::Object{{"path", Json(path)}}));
        CHECK(r["ok"].asBool() && r["result"]["errors"].asInt() == 0 && r["result"]["warnings"].asInt() == 0);
    }
    // Params: name, type, default, options from comparisons, trailing comment.
    r = e.Call("script.params", Json::parse(R"J({"path": "scripts/target.lua"})J"));
    CHECK(r["ok"].asBool() && r["result"].size() == 5);
    const Json& move = r["result"][0];
    CHECK(move["name"].asString() == "move" && move["type"].asString() == "string" && move["default"].asString() == "none");
    CHECK(move["options"].size() == 3);
    r = e.Call("script.params", Json::parse(R"J({"source": "local R = {}\nfunction R:onStart()\n  local p = self.params\n  self.v = p.spin or {0, 45, 0} -- deg/s\n  self.on = self.params.enabled or true\nend\nfunction R:onUpdate()\n  local p = self:position()\n  print(p.x)\nend\nreturn R"})J"));
    CHECK(r["result"].size() == 2);  // p.x in another function is not a param
    CHECK(r["result"][0]["type"].asString() == "vec3" && r["result"][0]["description"].asString() == "deg/s");
    CHECK(r["result"][1]["type"].asString() == "boolean");
}

TEST(EditorMathDecompose) {
    // Gizmo edits turn a world matrix back into Transform fields.
    const Vec3 cases[][3] = {
        {{1, 2, 3}, {10, 20, 30}, {1, 1, 1}},
        {{-4, 0.5f, 7}, {-80, 135, -170}, {2, 0.5f, 3}},
        {{0, 0, 0}, {0, 90, 0}, {1, 2, 1}},  // gimbal lock
        {{0, 0, 0}, {45, -90, 10}, {1, 1, 1}},
    };
    for (const auto& c : cases) {
        Mat4 m = Mat4::TRS(c[0], c[1], c[2]);
        Vec3 p, r, sc;
        DecomposeTRS(m, p, r, sc);
        Mat4 back = Mat4::TRS(p, r, sc);
        float worst = 0;
        for (int i = 0; i < 16; ++i) worst = std::max(worst, std::fabs(back.m[i] - m.m[i]));
        CHECK(worst < 1e-4f);
        CHECK(Near(p, c[0], 1e-5f));
        CHECK(Near(sc, c[2], 1e-4f));
    }
    CHECK(std::fabs(NearestAngle(-181.0f, 179.0f) - 179.0f) < 1e-4f);
    CHECK(std::fabs(NearestAngle(350.0f, 0.0f) + 10.0f) < 1e-4f);
}

TEST(EditorCameraRays) {
    EditorCamera cam;
    cam.target = Vec3(1, 0, -2);
    cam.Orbit(30, -10);
    CHECK(Near(cam.Eye() + cam.Forward() * cam.distance, cam.target, 1e-4f));
    RenderView v = MakeLookAtView(cam.Eye(), cam.target, cam.fov, 16.0f / 9.0f);
    Vec3 o, d, hit;
    ScreenRay(v.view, v.proj, 320, 180, 640, 360, o, d);  // center pixel looks at the target
    CHECK(Length(Cross(d, cam.Forward())) < 1e-3f);
    CHECK(RayPlane(o, d, Vec3(0, 0, 0), Vec3(0, 1, 0), hit) && Near(hit, cam.target, 1e-3f));
    Vec3 eye = cam.Eye();
    cam.Look(20, 5);  // fly look keeps the eye in place
    CHECK(Near(cam.Eye(), eye, 1e-3f));
    cam.Set2D(true);
    CHECK(Near(cam.Forward(), Vec3(0, 0, -1), 1e-6f));
}

#if OE_NATIVE_EDITOR
WindowEvent KeyEvent(WindowKey key, bool down, bool ctrl = false) {
    WindowEvent e;
    e.type = WindowEvent::Type::Key;
    e.key = key;
    e.down = down;
    e.ctrl = ctrl;
    return e;
}

std::vector<WindowEvent> CtrlChord(WindowKey key) {
    return {KeyEvent(WindowKey::Control, true, true), KeyEvent(key, true, true), KeyEvent(key, false, true), KeyEvent(WindowKey::Control, false, false)};
}

// String literals in `text`, with C escapes (\" \\ \n) resolved.
std::vector<std::string> StringLiterals(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        std::string lit;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
                lit += text[i] == 'n' ? '\n' : text[i];
            } else {
                lit += text[i];
            }
        }
        out.push_back(lit);
    }
    return out;
}

TEST(EditorTranslations) {
    CHECK(EditorCatalogProblems().empty());
    for (const std::string& p : EditorCatalogProblems()) std::printf("  catalog: %s\n", p.c_str());
    CHECK(ParseEditorLanguage("ko-KR") == EditorLanguage::Korean);
    CHECK(ParseEditorLanguage("ja_JP.UTF-8") == EditorLanguage::Japanese);
    CHECK(ParseEditorLanguage("fr") == EditorLanguage::English);
    // Every string the editor code passes to Tr()/TrId() has a translation.
    std::set<std::string> missing;
    for (const std::string& file : ListFiles(TestSourceDir() + "/engine/editor", ".cpp", false)) {
        if (file.find("EditorText.cpp") != std::string::npos) continue;
        std::string src;
        CHECK(ReadTextFile(file, src));
        for (size_t at = src.find("Tr"); at != std::string::npos; at = src.find("Tr", at + 2)) {
            bool call = src.compare(at, 3, "Tr(") == 0 || src.compare(at, 5, "TrId(") == 0;
            if (!call || (at > 0 && (std::isalnum(static_cast<unsigned char>(src[at - 1])) || src[at - 1] == '_'))) continue;
            // Arguments up to the matching ')', skipping string literals.
            size_t open = src.find('(', at), end = open;
            int depth = 0;
            for (; end < src.size(); ++end) {
                char c = src[end];
                if (c == '"') {
                    for (++end; end < src.size() && src[end] != '"'; ++end) {
                        if (src[end] == '\\') ++end;
                    }
                } else if (c == '(') {
                    ++depth;
                } else if (c == ')' && --depth == 0) {
                    break;
                }
            }
            for (const std::string& lit : StringLiterals(src.substr(open, end - open))) {
                if (!EditorCatalogHas(lit)) missing.insert(lit);
            }
        }
        // Create menu presets: {"key", "Group", "Label", ...}
        size_t presets = src.find("const Preset kPresets[]");
        if (presets != std::string::npos) {
            std::string block = src.substr(presets, src.find("};", presets) - presets);
            for (size_t line = block.find("\n    {\""); line != std::string::npos; line = block.find("\n    {\"", line + 1)) {
                std::vector<std::string> lits = StringLiterals(block.substr(line, block.find('\n', line + 1) - line));
                for (size_t k = 1; k < 3 && k < lits.size(); ++k) {
                    if (lits[k] != "2D" && lits[k] != "UI" && !EditorCatalogHas(lits[k])) missing.insert(lits[k]);
                }
            }
        }
    }
    for (const std::string& m : missing) std::printf("  no translation: %s\n", m.c_str());
    CHECK(missing.empty());
    SetEditorLanguage(EditorLanguage::Korean);
    CHECK(std::string(Tr("Save")) == "저장" && TrId("Hierarchy") == "계층###Hierarchy");
    SetEditorLanguage(EditorLanguage::Japanese);
    CHECK(std::string(Tr("Save")) == "保存" && std::string(Tr("not in the catalog")) == "not in the catalog");
    SetEditorLanguage(EditorLanguage::English);
}

TEST(NativeEditorScriptProblems) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("editor_scripts"), &err));
    Call(e, "script.write", R"J({"path": "scripts/warn.lua", "source": "local T = {}\nfunction T:onStart()\n  score = 0\n  lgo.info('x')\nend\nreturn T"})J");
    Call(e, "script.write", R"J({"path": "scripts/broken.lua", "source": "local T = {}\nfunction T:onStart()\n  print(1\nend\nreturn T"})J");
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    NativeEditor::Options options;
    options.language = "en";
    NativeEditor ed(e, nullptr, options);
    CHECK(ed.Init(&err));
    RenderTarget img;
    auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) {
            ed.Update({}, 1280, 720, 1.0f, Engine::kFixedDt);
            CHECK(ed.DrawToImage(img));
        }
    };
    frames(3);
    ed.OpenScript("scripts/warn.lua");
    ed.OpenScript("scripts/broken.lua");
    frames(2);
    std::vector<std::string> warn = ed.ScriptProblems("scripts/warn.lua");
    CHECK(warn.size() == 2 && warn[0].rfind("warning 3:", 0) == 0 && warn[1].rfind("warning 4:", 0) == 0);
    std::vector<std::string> broken = ed.ScriptProblems("scripts/broken.lua");
    CHECK(broken.size() == 1 && broken[0].rfind("error 4:", 0) == 0);
}

TEST(NativeEditorHeadless) {
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Hello", &err));
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    NativeEditor::Options options;
    options.language = "en";
    NativeEditor ed(e, nullptr, options);
    CHECK(ed.Init(&err));
    RenderTarget img;
    // Events are queued: Dear ImGui applies at most one press/release of a key per frame.
    auto frames = [&](int n, std::vector<WindowEvent> events = {}) {
        for (int i = 0; i < n; ++i) {
            ed.Update(i == 0 ? events : std::vector<WindowEvent>(), 1280, 720, 1.0f, Engine::kFixedDt);
            CHECK(ed.DrawToImage(img));
        }
    };
    frames(4);
    CHECK(img.width == 1280 && img.height == 720);
    std::set<uint32_t> colors;
    for (size_t i = 0; i < img.color.size(); i += 101) colors.insert(img.color[i]);
    CHECK(colors.size() > 40);  // panels, text and the rendered scene, not a blank frame

    Scene& s = e.GetScene();
    EntityId player = s.FindByName("Player");
    ed.Select(player);
    frames(1);
    CHECK(ed.Selected() == player);
    Vec3 start = s.Get<Transform>(player)->position;

    // Delete goes through entity.delete, Ctrl+Z through history.undo.
    frames(3, {KeyEvent(WindowKey::Delete, true), KeyEvent(WindowKey::Delete, false)});
    CHECK(!s.Exists(player));
    frames(6, CtrlChord(WindowKey::Z));
    CHECK(s.Exists(player));

    // Edits arriving through the API (an agent via HTTP/MCP) are announced.
    auto posted = e.PostCall("component.set", Json::parse(R"J({"id": "Player", "type": "MeshRenderer", "values": {"color": [1, 0, 0]}})J"));
    e.RunPostedJobs();
    CHECK(posted.get()["ok"].asBool());
    frames(1);
    CHECK(ed.LastNotice().rfind("API: component.set Player", 0) == 0);

    // Ctrl+P plays and gives the Game view the keyboard: W walks the player forward.
    frames(6, CtrlChord(WindowKey::P));
    CHECK(e.InPlaySession());
    CHECK(ed.GameViewFocused());
    frames(30, {KeyEvent(WindowKey::W, true)});
    frames(1, {KeyEvent(WindowKey::W, false)});
    CHECK(s.Exists(player) && s.Get<Transform>(player)->position.z < start.z - 0.5f);

    // Ctrl+P again stops and restores the edit-time scene.
    frames(6, CtrlChord(WindowKey::P));
    CHECK(!e.InPlaySession());
    player = s.FindByName("Player");
    CHECK(player != kNullEntity && Near(s.Get<Transform>(player)->position, start, 1e-5f));
    CHECK(!ed.GameViewFocused());

    // Closing the window with unsaved edits asks first instead of quitting.
    WindowEvent close;
    close.type = WindowEvent::Type::Close;
    frames(2, {close});
    CHECK(!ed.QuitRequested());
}

TEST(NativeEditorTilePainting) {
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Dungeon", &err));
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    NativeEditor::Options options;
    options.language = "en";
    NativeEditor ed(e, nullptr, options);
    CHECK(ed.Init(&err));
    RenderTarget img;
    auto frames = [&](int n, std::vector<WindowEvent> events = {}) {
        for (int i = 0; i < n; ++i) {
            ed.Update(i == 0 ? events : std::vector<WindowEvent>(), 1280, 720, 1.0f, Engine::kFixedDt);
            CHECK(ed.DrawToImage(img));
        }
    };
    auto mouse = [](WindowEvent::Type type, float x, float y, int button = 0, bool down = false) {
        WindowEvent ev;
        ev.type = type;
        ev.x = x;
        ev.y = y;
        ev.button = button;
        ev.down = down;
        return ev;
    };
    frames(3);
    Scene& s = e.GetScene();
    EntityId level = s.FindByName("Level");
    ed.Select(level);
    ed.SetTileBrush(true, '~');
    frames(3);
    auto count = [&](char c) {
        int n = 0;
        for (const Json& row : s.Get<Tilemap>(level)->map.items()) n += static_cast<int>(std::count(row.asString().begin(), row.asString().end(), c));
        return n;
    };
    const int water = count('~');
    std::array<float, 4> view = ed.SceneViewRect();
    CHECK(view[2] > 100 && view[3] > 100);
    float cx = view[0] + view[2] * 0.5f, cy = view[1] + view[3] * 0.5f;
    // A left drag paints a line of cells.
    frames(2, {mouse(WindowEvent::Type::MouseMove, cx, cy)});
    frames(2, {mouse(WindowEvent::Type::MouseButton, cx, cy, 0, true)});
    frames(2, {mouse(WindowEvent::Type::MouseMove, cx + 60, cy)});
    frames(2, {mouse(WindowEvent::Type::MouseButton, cx + 60, cy, 0, false)});
    const int painted = count('~') - water;
    CHECK(painted >= 2);
    const size_t undo = e.UndoDepth();
    // A right drag erases (one more undo step).
    frames(2, {mouse(WindowEvent::Type::MouseButton, cx, cy, 1, true)});
    frames(2, {mouse(WindowEvent::Type::MouseButton, cx, cy, 1, false)});
    CHECK(count('~') == water + painted - 1);
    CHECK(e.UndoDepth() == undo + 1);
    // Ctrl+Z undoes the erase, then the whole stroke.
    frames(4, CtrlChord(WindowKey::Z));
    CHECK(count('~') == water + painted);
    frames(4, CtrlChord(WindowKey::Z));
    CHECK(count('~') == water);
    // Brush off: clicks select again.
    ed.SetTileBrush(false, '~');
    frames(2);
    CHECK(ed.Selected() == level);
}
#endif

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
