#include "script/ScriptHost.h"

#include <cstring>
#include <exception>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "audio/AudioSystem.h"
#include "physics/PhysicsWorld.h"
#include "scene/Components.h"

// Lua is compiled as C++ (see CMakeLists.txt), so its headers are included
// without extern "C" and Lua errors unwind C++ destructors.
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace oe {

namespace {

constexpr int kHookInterval = 10000;   // instructions between hook calls
constexpr int kMaxHookTicks = 2000;    // 20M instructions per callback
constexpr size_t kMaxErrors = 200;
const char* kScriptMetaKey = "oe.ScriptMeta";
const char* kLoadedKey = "oe.loaded";
std::string PopMessage(lua_State* L);
Vec3 CheckVec3(lua_State* L, int idx);

ScriptHost& Host(lua_State* L) { return **static_cast<ScriptHost**>(lua_getextraspace(L)); }
Scene& SceneOf(lua_State* L) { return Host(L).GetEngine().GetScene(); }

// Runs a binding body, turning C++ exceptions into Lua errors. Lua's own
// errors (luaL_error etc.) are not std::exceptions and pass straight through.
template <class F>
int Guard(lua_State* L, F&& body) {
    std::string error;
    try {
        return body();
    } catch (const std::exception& e) {
        error = e.what();
    }
    return luaL_error(L, "%s", error.c_str());
}

void BudgetHook(lua_State* L, lua_Debug*) {
    if (++Host(L).budgetTicks > kMaxHookTicks) {
        luaL_error(L, "script exceeded its instruction budget (%d instructions) - infinite loop?", kHookInterval * kMaxHookTicks);
    }
}

// ----- JSON <-> Lua -----------------------------------------------------------

void PushJson(lua_State* L, const Json& j, int depth = 0) {
    luaL_checkstack(L, 4, "JSON too deep");
    switch (j.type()) {
        case Json::Type::Null: lua_pushnil(L); break;
        case Json::Type::Bool: lua_pushboolean(L, j.asBool()); break;
        case Json::Type::Number: {
            double v = j.asNumber();
            if (v == static_cast<double>(static_cast<lua_Integer>(v)) && v > -9e15 && v < 9e15) lua_pushinteger(L, static_cast<lua_Integer>(v));
            else lua_pushnumber(L, v);
            break;
        }
        case Json::Type::String: lua_pushlstring(L, j.asString().data(), j.asString().size()); break;
        case Json::Type::Array: {
            lua_createtable(L, static_cast<int>(j.size()), 0);
            for (size_t i = 0; i < j.size(); ++i) {
                PushJson(L, j[i], depth + 1);
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            break;
        }
        case Json::Type::Object: {
            lua_createtable(L, 0, static_cast<int>(j.size()));
            for (const auto& kv : j.members()) {
                PushJson(L, kv.second, depth + 1);
                lua_setfield(L, -2, kv.first.c_str());
            }
            break;
        }
    }
}

Json ToJson(lua_State* L, int idx, int depth = 0) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TNIL: return Json();
        case LUA_TBOOLEAN: return Json(lua_toboolean(L, idx) != 0);
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) return Json(static_cast<int64_t>(lua_tointeger(L, idx)));
            return Json(static_cast<double>(lua_tonumber(L, idx)));
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return Json(std::string(s, len));
        }
        case LUA_TTABLE: {
            if (depth > 32) return Json("<table nested too deep>");
            luaL_checkstack(L, 4, "table too deep");
            // A table is an array when its keys are exactly 1..n.
            lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
            lua_Integer count = 0;
            bool onlyIntKeys = true;
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                ++count;
                if (!lua_isinteger(L, -2)) onlyIntKeys = false;
                lua_pop(L, 1);
            }
            if (count == 0) return Json::MakeObject();
            if (onlyIntKeys && count == n) {
                Json arr = Json::MakeArray();
                for (lua_Integer i = 1; i <= n; ++i) {
                    lua_rawgeti(L, idx, i);
                    arr.push(ToJson(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return arr;
            }
            Json obj = Json::MakeObject();
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    key = lua_tostring(L, -2);
                } else {
                    lua_pushvalue(L, -2);
                    key = luaL_tolstring(L, -1, nullptr);
                    lua_pop(L, 2);
                }
                obj[key] = ToJson(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return obj;
        }
        default: {
            const char* s = luaL_tolstring(L, idx, nullptr);
            Json out(std::string("<") + s + ">");
            lua_pop(L, 1);
            return out;
        }
    }
}

// Components are exposed with vec3 as {x,y,z} and colors as {r,g,b}.
void PushComponent(lua_State* L, const ComponentType& type, const void* c) {
    lua_createtable(L, 0, static_cast<int>(type.fields.size()));
    for (const FieldInfo& f : type.fields) {
        Json v = FieldToJson(f, c);
        if (f.type == FieldType::Vec3 || f.type == FieldType::Color) {
            const char* keys = f.type == FieldType::Vec3 ? "xyz" : "rgb";
            lua_createtable(L, 0, 3);
            for (int i = 0; i < 3; ++i) {
                lua_pushnumber(L, v[i].asNumber());
                lua_setfield(L, -2, std::string(1, keys[i]).c_str());
            }
        } else {
            PushJson(L, v);
        }
        lua_setfield(L, -2, f.name.c_str());
    }
}

EntityId CheckEntity(lua_State* L, int idx) {
    Scene& s = SceneOf(L);
    if (lua_type(L, idx) == LUA_TSTRING) {
        EntityId id = s.FindByName(lua_tostring(L, idx));
        if (id == kNullEntity) luaL_error(L, "no entity named '%s'", lua_tostring(L, idx));
        return id;
    }
    lua_Integer v = luaL_checkinteger(L, idx);
    EntityId id = static_cast<EntityId>(v);
    if (!s.Exists(id)) luaL_error(L, "entity %d does not exist", static_cast<int>(v));
    return id;
}

const ComponentType& CheckType(lua_State* L, int idx) {
    const char* name = luaL_checkstring(L, idx);
    const ComponentType* t = TypeRegistry::Find(name);
    if (!t) {
        std::string names;
        for (const ComponentType& ct : TypeRegistry::All()) names += (names.empty() ? "" : ", ") + ct.name;
        luaL_error(L, "unknown component type '%s' (known: %s)", name, names.c_str());
    }
    return *t;
}

// ----- scene.* ---------------------------------------------------------------

int L_SceneGet(lua_State* L) {
    return Guard(L, [&] {
        EntityId id = CheckEntity(L, 1);
        const ComponentType& t = CheckType(L, 2);
        const void* c = SceneOf(L).GetComponent(id, t);
        if (!c) {
            lua_pushnil(L);
            return 1;
        }
        PushComponent(L, t, c);
        return 1;
    });
}

void ApplyTable(lua_State* L, int idx, const ComponentType& t, void* c) {
    if (lua_isnoneornil(L, idx)) return;
    luaL_checktype(L, idx, LUA_TTABLE);
    std::string err;
    if (!ApplyComponentJson(t, c, ToJson(L, idx), &err)) luaL_error(L, "%s", err.c_str());
}

int L_SceneSet(lua_State* L) {
    return Guard(L, [&] {
        EntityId id = CheckEntity(L, 1);
        const ComponentType& t = CheckType(L, 2);
        void* c = SceneOf(L).GetComponent(id, t);
        if (!c) luaL_error(L, "entity %d has no %s component (use scene.add)", static_cast<int>(id), t.name.c_str());
        ApplyTable(L, 3, t, c);
        return 0;
    });
}

int L_SceneAdd(lua_State* L) {
    return Guard(L, [&] {
        EntityId id = CheckEntity(L, 1);
        const ComponentType& t = CheckType(L, 2);
        ApplyTable(L, 3, t, SceneOf(L).AddComponent(id, t));
        return 0;
    });
}

int L_SceneRemove(lua_State* L) {
    return Guard(L, [&] {
        EntityId id = CheckEntity(L, 1);
        lua_pushboolean(L, SceneOf(L).RemoveComponent(id, CheckType(L, 2)));
        return 1;
    });
}

int L_SceneHas(lua_State* L) {
    return Guard(L, [&] {
        EntityId id = CheckEntity(L, 1);
        lua_pushboolean(L, SceneOf(L).GetComponent(id, CheckType(L, 2)) != nullptr);
        return 1;
    });
}

int L_SceneCreate(lua_State* L) {
    return Guard(L, [&] {
        Json spec = Json::MakeObject();
        spec["name"] = std::string(luaL_optstring(L, 1, ""));
        Json comps = lua_istable(L, 2) ? ToJson(L, 2) : Json::MakeObject();
        if (!comps.isObject()) luaL_error(L, "components must be a table keyed by component type");
        if (!comps.has("Transform")) comps["Transform"] = Json::MakeObject();
        spec["components"] = comps;
        std::string err;
        EntityId id = SceneOf(L).CreateFromJson(spec, false, &err);
        if (id == kNullEntity) luaL_error(L, "%s", err.c_str());
        lua_pushinteger(L, id);
        return 1;
    });
}

// scene.instantiate(path, {name=, position={x,y,z}, parent=id}) -> root id
int L_SceneInstantiate(lua_State* L) {
    return Guard(L, [&] {
        std::string path = luaL_checkstring(L, 1);
        Json opts = lua_istable(L, 2) ? ToJson(L, 2) : Json::MakeObject();
        Engine& e = Host(L).GetEngine();
        EntityId parent = static_cast<EntityId>(opts["parent"].asNumber(0));
        if (parent != kNullEntity && !e.GetScene().Exists(parent)) luaL_error(L, "parent entity %d does not exist", static_cast<int>(parent));
        EntityId root = e.InstantiatePrefabFile(path, parent);
        Scene& s = e.GetScene();
        if (opts["name"].isString()) s.Record(root)->name = opts["name"].asString();
        const Json& p = opts["position"];
        if (p.isObject() || p.isArray()) {
            std::string err;
            ApplyComponentJson(*TypeRegistry::Find("Transform"), &s.Add<Transform>(root), Json(Json::Object{{"position", p}}), &err);
        }
        lua_pushinteger(L, root);
        return 1;
    });
}

int L_SceneDestroy(lua_State* L) {
    return Guard(L, [&] {
        EntityId id = CheckEntity(L, 1);
        lua_pushinteger(L, SceneOf(L).Destroy(id));
        return 1;
    });
}

int L_SceneFind(lua_State* L) {
    EntityId id = SceneOf(L).FindByName(luaL_checkstring(L, 1));
    if (id == kNullEntity) lua_pushnil(L);
    else lua_pushinteger(L, id);
    return 1;
}

int L_SceneExists(lua_State* L) {
    lua_pushboolean(L, lua_isinteger(L, 1) && SceneOf(L).Exists(static_cast<EntityId>(lua_tointeger(L, 1))));
    return 1;
}

int L_SceneName(lua_State* L) {
    EntityId id = CheckEntity(L, 1);
    lua_pushstring(L, SceneOf(L).Record(id)->name.c_str());
    return 1;
}

int L_SceneParent(lua_State* L) {
    EntityId id = CheckEntity(L, 1);
    EntityId p = SceneOf(L).Record(id)->parent;
    if (p == kNullEntity) lua_pushnil(L);
    else lua_pushinteger(L, p);
    return 1;
}

int L_SceneAll(lua_State* L) {
    return Guard(L, [&] {
        Scene& s = SceneOf(L);
        const ComponentType* t = lua_isnoneornil(L, 1) ? nullptr : &CheckType(L, 1);
        lua_newtable(L);
        lua_Integer i = 0;
        for (const auto& kv : s.Entities()) {
            if (t && !s.GetComponent(kv.first, *t)) continue;
            lua_pushinteger(L, kv.first);
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    });
}

int L_SceneWithTag(lua_State* L) {
    std::string tag = luaL_checkstring(L, 1);
    Scene& s = SceneOf(L);
    lua_newtable(L);
    lua_Integer i = 0;
    for (const auto& kv : s.Pool<Tag>()) {
        std::string tags = "," + kv.second.tags + ",";
        std::string clean;
        for (char c : tags) {
            if (c != ' ') clean += c;
        }
        if (clean.find("," + tag + ",") == std::string::npos) continue;
        lua_pushinteger(L, kv.first);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// ----- messaging: scene.send / scene.broadcast ------------------------------------

// Calls `method` on the target's script instance and leaves its result on the
// stack (returns 1), or returns 0 when the entity has no script or no such
// method. Errors fault the target, not the caller.
int CallOn(lua_State* L, EntityId target, const char* method, int firstArg, int nargs) {
    ScriptHost& host = Host(L);
    if (!host.PushInstance(target)) return 0;
    lua_getfield(L, -1, method);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return 0;
    }
    lua_insert(L, -2);  // method, self
    for (int i = 0; i < nargs; ++i) lua_pushvalue(L, firstArg + i);
    if (lua_pcall(L, nargs + 1, 1, 0) != LUA_OK) {
        std::string msg = PopMessage(L);
        host.FaultInstance(target, std::string(method) + ": " + msg);
        return 0;
    }
    return 1;
}

int L_SceneSend(lua_State* L) {
    EntityId target = CheckEntity(L, 1);
    const char* method = luaL_checkstring(L, 2);
    int nargs = lua_gettop(L) - 2;
    if (!CallOn(L, target, method, 3, nargs)) lua_pushnil(L);
    return 1;
}

int L_SceneBroadcast(lua_State* L) {
    const char* method = luaL_checkstring(L, 1);
    int nargs = lua_gettop(L) - 1;
    lua_Integer called = 0;
    for (EntityId id : Host(L).InstanceIds()) {
        if (!SceneOf(L).Exists(id)) continue;
        int top = lua_gettop(L);
        if (CallOn(L, id, method, 2, nargs)) ++called;
        lua_settop(L, top);
    }
    lua_pushinteger(L, called);
    return 1;
}

// ----- game.* ---------------------------------------------------------------------

int L_GameGet(lua_State* L) {
    const Json& data = Host(L).GetEngine().GameData();
    if (lua_isnoneornil(L, 1)) {
        PushJson(L, data);
        return 1;
    }
    PushJson(L, data[std::string(luaL_checkstring(L, 1))]);
    return 1;
}

int L_GameSet(lua_State* L) {
    std::string key = luaL_checkstring(L, 1);
    luaL_checkany(L, 2);
    Json value = ToJson(L, 2);
    Json& data = Host(L).GetEngine().GameData();
    if (value.isNull()) data.erase(key);
    else data[key] = value;
    return 0;
}

int L_GameLoadScene(lua_State* L) {
    std::string path = luaL_checkstring(L, 1);
    Host(L).GetEngine().RequestSceneChange(path);
    return 0;
}

int L_GameScene(lua_State* L) {
    lua_pushstring(L, Host(L).GetEngine().RuntimeScene().c_str());
    return 1;
}

int L_ReportError(lua_State* L) {
    Host(L).RecordError("", kNullEntity, luaL_checkstring(L, 1));
    return 0;
}

// ----- draw.* (debug lines) ---------------------------------------------------------

Color OptColor(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return Color(1, 0.2f, 0.9f);
    lua_getfield(L, idx, "r");
    bool named = lua_isnumber(L, -1);
    lua_pop(L, 1);
    if (named) {
        float c[3];
        const char* k[3] = {"r", "g", "b"};
        for (int i = 0; i < 3; ++i) {
            lua_getfield(L, idx, k[i]);
            c[i] = static_cast<float>(luaL_optnumber(L, -1, 0));
            lua_pop(L, 1);
        }
        return Color(c[0], c[1], c[2]);
    }
    Vec3 v = CheckVec3(L, idx);
    return Color(v.x, v.y, v.z);
}

// draw.line(a, b, color?, seconds?)  seconds: 0 (default) = this frame only
int L_DrawLine(lua_State* L) {
    Host(L).GetEngine().AddDebugLine(CheckVec3(L, 1), CheckVec3(L, 2), OptColor(L, 3), static_cast<float>(luaL_optnumber(L, 4, 0)));
    return 0;
}

int L_DrawBox(lua_State* L) {
    Vec3 c = CheckVec3(L, 1), h = CheckVec3(L, 2) * 0.5f;
    Color col = OptColor(L, 3);
    float secs = static_cast<float>(luaL_optnumber(L, 4, 0));
    Vec3 p[8];
    for (int i = 0; i < 8; ++i) p[i] = c + Vec3(i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z);
    const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) Host(L).GetEngine().AddDebugLine(p[e[0]], p[e[1]], col, secs);
    return 0;
}

int L_DrawSphere(lua_State* L) {
    Vec3 c = CheckVec3(L, 1);
    float r = static_cast<float>(luaL_checknumber(L, 2));
    Color col = OptColor(L, 3);
    float secs = static_cast<float>(luaL_optnumber(L, 4, 0));
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < 24; ++i) {
            auto pt = [&](int k) {
                float ang = 2 * kPi * static_cast<float>(k) / 24, u = std::cos(ang) * r, v = std::sin(ang) * r;
                return axis == 0 ? c + Vec3(0, u, v) : axis == 1 ? c + Vec3(u, 0, v) : c + Vec3(u, v, 0);
            };
            Host(L).GetEngine().AddDebugLine(pt(i), pt(i + 1), col, secs);
        }
    }
    return 0;
}

// ----- audio.* -------------------------------------------------------------------

// audio.play(path, {volume=1, pitch=1, loop=false}) -> voice id
int L_AudioPlay(lua_State* L) {
    return Guard(L, [&] {
        std::string path = luaL_checkstring(L, 1);
        Json o = lua_istable(L, 2) ? ToJson(L, 2) : Json::MakeObject();
        int id = Host(L).GetEngine().Audio().Play(path, o["volume"].asFloat(1.0f), o["pitch"].asFloat(1.0f), o["loop"].asBool(false), kNullEntity);
        lua_pushinteger(L, id);
        return 1;
    });
}

int L_AudioStop(lua_State* L) {
    lua_pushboolean(L, Host(L).GetEngine().Audio().Stop(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}

int L_AudioStopAll(lua_State* L) {
    Host(L).GetEngine().Audio().StopAll();
    return 0;
}

// ----- physics.* ---------------------------------------------------------------

Vec3 CheckVec3(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    float v[3] = {0, 0, 0};
    const char* keys[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_getfield(L, idx, keys[i]);
        if (lua_isnumber(L, -1)) {
            v[i] = static_cast<float>(lua_tonumber(L, -1));
        } else {
            lua_pop(L, 1);
            lua_rawgeti(L, idx, i + 1);
            v[i] = static_cast<float>(luaL_optnumber(L, -1, 0));
        }
        lua_pop(L, 1);
    }
    return Vec3(v[0], v[1], v[2]);
}

void PushVec3(lua_State* L, const Vec3& v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z);
    lua_setfield(L, -2, "z");
}

int L_PhysicsRaycast(lua_State* L) {
    return Guard(L, [&] {
        Vec3 origin = CheckVec3(L, 1);
        Vec3 dir = CheckVec3(L, 2);
        float maxDist = static_cast<float>(luaL_optnumber(L, 3, 1000.0));
        Engine& e = Host(L).GetEngine();
        RaycastHit hit = e.Physics().Raycast(e.GetScene(), origin, dir, maxDist);
        if (!hit.hit) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, hit.entity);
        lua_setfield(L, -2, "entity");
        PushVec3(L, hit.point);
        lua_setfield(L, -2, "point");
        PushVec3(L, hit.normal);
        lua_setfield(L, -2, "normal");
        lua_pushnumber(L, hit.distance);
        lua_setfield(L, -2, "distance");
        return 1;
    });
}

int L_PhysicsOverlapSphere(lua_State* L) {
    return Guard(L, [&] {
        Vec3 center = CheckVec3(L, 1);
        float radius = static_cast<float>(luaL_checknumber(L, 2));
        Engine& e = Host(L).GetEngine();
        std::vector<EntityId> ids = e.Physics().OverlapSphere(e.GetScene(), center, radius);
        lua_createtable(L, static_cast<int>(ids.size()), 0);
        for (size_t i = 0; i < ids.size(); ++i) {
            lua_pushinteger(L, ids[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    });
}

int L_PhysicsAddImpulse(lua_State* L) {
    EntityId id = CheckEntity(L, 1);
    Host(L).GetEngine().Physics().AddImpulse(id, CheckVec3(L, 2));
    return 0;
}

int L_PhysicsContacts(lua_State* L) {
    EntityId id = CheckEntity(L, 1);
    lua_newtable(L);
    lua_Integer i = 0;
    for (const ContactPair& c : Host(L).GetEngine().Physics().Contacts()) {
        if (c.a != id && c.b != id) continue;
        lua_pushinteger(L, c.a == id ? c.b : c.a);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// ----- input.*, time.*, log.* --------------------------------------------------

int L_InputDown(lua_State* L) {
    lua_pushboolean(L, Host(L).GetEngine().Input().IsDown(luaL_checkstring(L, 1)));
    return 1;
}

int L_InputPressed(lua_State* L) {
    lua_pushboolean(L, Host(L).GetEngine().Input().pressedThisFrame.count(luaL_checkstring(L, 1)) > 0);
    return 1;
}

// input.mouse() -> x, y normalized to the game view (0..1, top-left origin)
int L_InputMouse(lua_State* L) {
    const InputState& in = Host(L).GetEngine().Input();
    lua_pushnumber(L, in.mouseX);
    lua_pushnumber(L, in.mouseY);
    return 2;
}

int L_TimeFrame(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(Host(L).GetEngine().Frame()));
    return 1;
}

int L_TimeNow(lua_State* L) {
    lua_pushnumber(L, Host(L).GetEngine().SimTime());
    return 1;
}

int L_TimeDt(lua_State* L) {
    lua_pushnumber(L, Host(L).currentDt);
    return 1;
}

std::string JoinArgs(lua_State* L, const char* sep) {
    std::string out;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) out += sep;
        out.append(s, len);
        lua_pop(L, 1);
    }
    return out;
}

template <LogLevel Level>
int L_Log(lua_State* L) {
    std::string msg = JoinArgs(L, " ");
    Log::Write(Level, "script", msg);
    Host(L).AppendOutput(msg);
    return 0;
}

int L_Print(lua_State* L) {
    std::string msg = JoinArgs(L, "\t");
    Log::Write(LogLevel::Info, "script", msg);
    Host(L).AppendOutput(msg);
    return 0;
}

// require("lib.util") loads <project>/lib/util.lua once per session.
int L_Require(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' || c == '-';
        if (!ok) luaL_error(L, "invalid module name '%s'", name.c_str());
    }
    if (name.find("..") != std::string::npos) luaL_error(L, "invalid module name '%s'", name.c_str());
    std::string path = name;
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".lua") != 0) {
        for (char& c : path) {
            if (c == '.') c = '/';
        }
        path += ".lua";
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kLoadedKey);
    lua_getfield(L, -1, path.c_str());
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    bool ok = false;
    {
        std::string err;
        try {
            ok = Host(L).LoadFile(path);
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (!err.empty()) luaL_error(L, "%s", err.c_str());
    }
    if (!ok) lua_error(L);  // message is on the stack
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, path.c_str());
    return 1;
}

const char* kPrelude = R"LUA(
-- Base class of every script module: helpers available as self:method().
local Script = {}
function Script:get(t) return scene.get(self.id, t) end
function Script:set(t, v) return scene.set(self.id, t, v) end
function Script:add(t, v) return scene.add(self.id, t, v) end
function Script:has(t) return scene.has(self.id, t) end
function Script:remove(t) return scene.remove(self.id, t) end
function Script:destroy() return scene.destroy(self.id) end
function Script:position() return scene.get(self.id, "Transform").position end
function Script:setPosition(x, y, z) scene.set(self.id, "Transform", {position = {x = x, y = y, z = z}}) end
function Script:translate(x, y, z)
  local p = scene.get(self.id, "Transform").position
  scene.set(self.id, "Transform", {position = {x = p.x + x, y = p.y + y, z = p.z + z}})
end
function Script:rotate(x, y, z)
  local r = scene.get(self.id, "Transform").rotation
  scene.set(self.id, "Transform", {rotation = {x = r.x + x, y = r.y + y, z = r.z + z}})
end
-- Physics helpers: work with CharacterBody (characters) or RigidBody (bodies).
function Script:grounded()
  local c = scene.get(self.id, "CharacterBody")
  return c ~= nil and c.grounded
end
function Script:velocity()
  local c = scene.get(self.id, "CharacterBody") or scene.get(self.id, "RigidBody")
  return c and c.velocity or {x = 0, y = 0, z = 0}
end
function Script:setVelocity(x, y, z)
  local t = scene.has(self.id, "CharacterBody") and "CharacterBody" or "RigidBody"
  scene.set(self.id, t, {velocity = {x = x, y = y, z = z}})
end
function Script:addImpulse(x, y, z) physics.addImpulse(self.id, {x = x, y = y, z = z}) end
function Script:contacts() return physics.contacts(self.id) end
function Script:send(target, method, ...) return scene.send(target, method, ...) end

-- Timers: fire at the start of a frame, in creation order, on simulated time.
local timers, nextTimer = {}, 1
timer = {}
function timer.after(seconds, fn)
  local id = nextTimer; nextTimer = id + 1
  timers[#timers + 1] = {id = id, at = time.now() + seconds, fn = fn}
  return id
end
function timer.every(seconds, fn)
  local id = nextTimer; nextTimer = id + 1
  timers[#timers + 1] = {id = id, at = time.now() + seconds, every = math.max(seconds, 1e-3), fn = fn}
  return id
end
function timer.cancel(id)
  for i, t in ipairs(timers) do
    if t.id == id then table.remove(timers, i) return true end
  end
  return false
end
function __oe_update_timers(now)
  local due = {}
  for _, t in ipairs(timers) do
    if now + 1e-9 >= t.at then due[#due + 1] = t end
  end
  for _, t in ipairs(due) do
    if t.every then t.at = t.at + t.every else timer.cancel(t.id) end
    local ok, err = pcall(t.fn)
    if not ok then __oe_error("timer " .. t.id .. ": " .. tostring(err)) end
  end
end
return {__index = Script}
)LUA";

void SetFuncs(lua_State* L, const char* table, const luaL_Reg* funcs) {
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_setglobal(L, table);
}

std::string PopMessage(lua_State* L) {
    const char* s = lua_tostring(L, -1);
    std::string msg = s ? s : "(error object is not a string)";
    lua_pop(L, 1);
    return msg;
}

}  // namespace

// ----- ScriptHost -------------------------------------------------------------

ScriptHost::ScriptHost(Engine& engine) : engine_(engine) {}

ScriptHost::~ScriptHost() {
    if (L_) lua_close(L_);
}

lua_State* ScriptHost::State() {
    if (!L_) Open();
    return L_;
}

void ScriptHost::Open() {
    L_ = luaL_newstate();
    lua_State* L = L_;
    *static_cast<ScriptHost**>(lua_getextraspace(L)) = this;

    // Sandbox: no io, os, package, debug; no loading code from files/strings.
    const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                             {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine}};
    for (const luaL_Reg& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    for (const char* unsafe : {"dofile", "loadfile", "load"}) {
        lua_pushnil(L);
        lua_setglobal(L, unsafe);
    }
    // Deterministic random numbers.
    lua_getglobal(L, "math");
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, 0);
    lua_call(L, 1, 0);
    lua_pop(L, 1);

    lua_register(L, "print", L_Print);
    lua_register(L, "require", L_Require);
    const luaL_Reg sceneFuncs[] = {{"get", L_SceneGet},     {"set", L_SceneSet},       {"add", L_SceneAdd},       {"remove", L_SceneRemove},
                                   {"has", L_SceneHas},     {"create", L_SceneCreate}, {"instantiate", L_SceneInstantiate}, {"send", L_SceneSend}, {"broadcast", L_SceneBroadcast}, {"destroy", L_SceneDestroy}, {"find", L_SceneFind},
                                   {"exists", L_SceneExists}, {"name", L_SceneName},   {"parent", L_SceneParent}, {"all", L_SceneAll},
                                   {"withTag", L_SceneWithTag}, {nullptr, nullptr}};
    SetFuncs(L, "scene", sceneFuncs);
    const luaL_Reg inputFuncs[] = {{"down", L_InputDown}, {"pressed", L_InputPressed}, {"mouse", L_InputMouse}, {nullptr, nullptr}};
    SetFuncs(L, "input", inputFuncs);
    const luaL_Reg timeFuncs[] = {{"frame", L_TimeFrame}, {"now", L_TimeNow}, {"dt", L_TimeDt}, {nullptr, nullptr}};
    SetFuncs(L, "time", timeFuncs);
    const luaL_Reg drawFuncs[] = {{"line", L_DrawLine}, {"box", L_DrawBox}, {"sphere", L_DrawSphere}, {nullptr, nullptr}};
    SetFuncs(L, "draw", drawFuncs);
    const luaL_Reg audioFuncs[] = {{"play", L_AudioPlay}, {"stop", L_AudioStop}, {"stopAll", L_AudioStopAll}, {nullptr, nullptr}};
    SetFuncs(L, "audio", audioFuncs);
    const luaL_Reg gameFuncs[] = {{"get", L_GameGet}, {"set", L_GameSet}, {"loadScene", L_GameLoadScene}, {"scene", L_GameScene}, {nullptr, nullptr}};
    SetFuncs(L, "game", gameFuncs);
    lua_register(L, "__oe_error", L_ReportError);
    const luaL_Reg physicsFuncs[] = {{"raycast", L_PhysicsRaycast}, {"overlapSphere", L_PhysicsOverlapSphere},
                                     {"addImpulse", L_PhysicsAddImpulse}, {"contacts", L_PhysicsContacts}, {nullptr, nullptr}};
    SetFuncs(L, "physics", physicsFuncs);
    const luaL_Reg logFuncs[] = {{"info", L_Log<LogLevel::Info>}, {"warn", L_Log<LogLevel::Warn>}, {"error", L_Log<LogLevel::Error>}, {nullptr, nullptr}};
    SetFuncs(L, "log", logFuncs);

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kLoadedKey);

    if (luaL_loadbufferx(L, kPrelude, std::strlen(kPrelude), "=prelude", "t") != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        OE_LOG_ERROR("script", "prelude failed: %s", PopMessage(L).c_str());
        lua_newtable(L);
    }
    lua_setfield(L, LUA_REGISTRYINDEX, kScriptMetaKey);

    lua_sethook(L, BudgetHook, LUA_MASKCOUNT, kHookInterval);
}

void ScriptHost::ResetInstances() {
    if (L_) {
        for (auto& kv : instances_) {
            if (kv.second.ref >= 0) luaL_unref(L_, LUA_REGISTRYINDEX, kv.second.ref);
        }
    }
    instances_.clear();
}

bool ScriptHost::PushInstance(EntityId id) {
    auto it = instances_.find(id);
    if (!L_ || it == instances_.end() || it->second.ref < 0 || it->second.faulted) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second.ref);
    return true;
}

void ScriptHost::FaultInstance(EntityId id, const std::string& message) {
    auto it = instances_.find(id);
    RecordError(it != instances_.end() ? it->second.path : "", id, message);
    if (it != instances_.end()) it->second.faulted = true;
}

std::vector<EntityId> ScriptHost::InstanceIds() const {
    std::vector<EntityId> ids;
    for (const auto& kv : instances_) ids.push_back(kv.first);
    return ids;
}

void ScriptHost::Reset() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
    modules_.clear();
    instances_.clear();
}

bool ScriptHost::LoadFile(const std::string& path) {
    lua_State* L = State();
    std::string full = engine_.ResolvePath(path);
    std::string source;
    if (!ReadTextFile(full, source)) {
        lua_pushfstring(L, "cannot read script '%s'", path.c_str());
        return false;
    }
    std::string chunkName = "@" + RelativePath(full, engine_.ProjectDir());
    if (luaL_loadbufferx(L, source.data(), source.size(), chunkName.c_str(), "t") != LUA_OK) return false;
    ArmBudget();
    return lua_pcall(L, 0, 1, 0) == LUA_OK;
}

bool ScriptHost::ReloadModule(const std::string& path, Module& module) {
    lua_State* L = State();
    int top = lua_gettop(L);
    try {
        module.mtime = FileModifiedTime(engine_.ResolvePath(path));
    } catch (const std::exception&) {
        module.mtime = 0;
    }
    bool ok = false;
    std::string error;
    try {
        ok = LoadFile(path);
        if (!ok) error = PopMessage(L);
    } catch (const std::exception& e) {
        error = e.what();
    }
    if (ok && !lua_istable(L, -1)) {
        ok = false;
        error = path + ": script must return a table, e.g. `local M = {} function M:onUpdate(dt) end return M`";
    }
    if (!ok) {
        lua_settop(L, top);
        RecordError(path, kNullEntity, error);
        return false;
    }
    int fresh = lua_gettop(L);
    if (module.ref >= 0) {
        // Hot reload: refill the existing table so live instances keep their
        // metatable (and their state) but see the new functions.
        lua_rawgeti(L, LUA_REGISTRYINDEX, module.ref);
        int old = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, old) != 0) {
            lua_pop(L, 1);
            lua_pushvalue(L, -1);
            lua_pushnil(L);
            lua_rawset(L, old);
        }
        lua_pushnil(L);
        while (lua_next(L, fresh) != 0) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, old);
        }
        lua_settop(L, old);
    }
    int tableIdx = lua_gettop(L);
    lua_pushvalue(L, tableIdx);
    lua_setfield(L, tableIdx, "__index");
    lua_getfield(L, LUA_REGISTRYINDEX, kScriptMetaKey);
    lua_setmetatable(L, tableIdx);
    if (module.ref < 0) module.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_settop(L, top);
    return true;
}

int ScriptHost::GetModule(const std::string& path) {
    auto it = modules_.find(path);
    if (it != modules_.end()) return it->second.ref;
    Module m;
    ReloadModule(path, m);
    modules_[path] = m;
    return m.ref;
}

std::vector<std::string> ScriptHost::PollHotReload() {
    std::vector<std::string> reloaded;
    if (!L_) return reloaded;
    for (auto& kv : modules_) {
        int64_t mtime = 0;
        try {
            mtime = FileModifiedTime(engine_.ResolvePath(kv.first));
        } catch (const std::exception&) {
        }
        if (mtime == kv.second.mtime) continue;
        if (ReloadModule(kv.first, kv.second)) {
            reloaded.push_back(kv.first);
            OE_LOG_INFO("script", "reloaded %s", kv.first.c_str());
        }
    }
    // Give instances of reloaded modules another chance.
    for (auto it = instances_.begin(); it != instances_.end();) {
        bool hit = false;
        for (const std::string& p : reloaded) hit = hit || p == it->second.path;
        if (hit && it->second.ref < 0) {
            it = instances_.erase(it);
            continue;
        }
        if (hit) it->second.faulted = false;
        ++it;
    }
    return reloaded;
}

std::vector<std::string> ScriptHost::ReloadAll() {
    for (auto& kv : modules_) kv.second.mtime = -1;
    return PollHotReload();
}

bool ScriptHost::CallMethod(EntityId id, Instance& inst, const char* name, float dt, bool withDt, EntityId other) {
    lua_State* L = State();
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst.ref);
    lua_getfield(L, -1, name);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return true;
    }
    lua_insert(L, -2);  // function, self
    int nargs = 1;
    if (withDt) {
        lua_pushnumber(L, dt);
        nargs = 2;
    } else if (other != kNullEntity) {
        lua_pushinteger(L, other);
        nargs = 2;
    }
    ArmBudget();
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        std::string msg = PopMessage(L);
        lua_settop(L, top);
        RecordError(inst.path, id, std::string(name) + ": " + msg);
        inst.faulted = true;
        return false;
    }
    lua_settop(L, top);
    return true;
}

void ScriptHost::DispatchPhysicsEvents(const std::vector<PhysicsEvent>& events) {
    if (!L_) return;
    for (const PhysicsEvent& ev : events) {
        const char* callback = ToString(ev.kind);
        for (int side = 0; side < 2; ++side) {
            EntityId self = side == 0 ? ev.a : ev.b;
            EntityId other = side == 0 ? ev.b : ev.a;
            auto it = instances_.find(self);
            if (it == instances_.end() || it->second.ref < 0 || it->second.faulted) continue;
            if (!engine_.GetScene().Exists(self)) continue;
            CallMethod(self, it->second, callback, 0, false, other);
        }
    }
}

void ScriptHost::Notify(EntityId id, const char* method) {
    auto it = instances_.find(id);
    if (!L_ || it == instances_.end() || it->second.ref < 0 || it->second.faulted) return;
    CallMethod(id, it->second, method, 0, false);
}

void ScriptHost::DestroyInstance(EntityId id, Instance& inst, bool callOnDestroy) {
    if (inst.ref >= 0) {
        if (callOnDestroy && !inst.faulted) CallMethod(id, inst, "onDestroy", 0, false);
        luaL_unref(State(), LUA_REGISTRYINDEX, inst.ref);
        inst.ref = -1;
    }
}

void ScriptHost::Update(float dt) {
    Scene& scene = engine_.GetScene();
    currentDt = dt;

    // Timers created with timer.after/every fire at the start of the frame.
    if (L_) {
        int top = lua_gettop(L_);
        lua_getglobal(L_, "__oe_update_timers");
        if (lua_isfunction(L_, -1)) {
            lua_pushnumber(L_, engine_.SimTime());
            ArmBudget();
            if (lua_pcall(L_, 1, 0, 0) != LUA_OK) RecordError("", kNullEntity, "timer: " + PopMessage(L_));
        }
        lua_settop(L_, top);
    }

    // Drop instances whose entity or Script component went away.
    for (auto it = instances_.begin(); it != instances_.end();) {
        const Script* sc = scene.Get<Script>(it->first);
        if (!sc || !sc->enabled) {
            DestroyInstance(it->first, it->second, scene.Exists(it->first));
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
    if (scene.Pool<Script>().empty()) return;

    lua_State* L = State();
    std::vector<EntityId> ids;
    for (const auto& kv : scene.Pool<Script>()) ids.push_back(kv.first);

    for (EntityId id : ids) {
        const Script* sc = scene.Get<Script>(id);  // scripts may destroy entities
        if (!sc || !sc->enabled || sc->path.empty()) continue;
        std::string params = sc->params.dump();
        auto it = instances_.find(id);
        if (it != instances_.end() && (it->second.path != sc->path || it->second.params != params)) {
            DestroyInstance(id, it->second, true);
            instances_.erase(it);
            it = instances_.end();
        }
        if (it == instances_.end()) {
            Instance inst;
            inst.path = sc->path;
            inst.params = params;
            Json paramsJson = sc->params;
            std::string entityName = scene.Record(id)->name;
            int module = GetModule(inst.path);
            if (module < 0) {
                inst.faulted = true;
                instances_[id] = inst;
                continue;
            }
            lua_createtable(L, 0, 4);
            lua_pushinteger(L, id);
            lua_setfield(L, -2, "id");
            lua_pushstring(L, entityName.c_str());
            lua_setfield(L, -2, "name");
            PushJson(L, paramsJson);
            lua_setfield(L, -2, "params");
            lua_rawgeti(L, LUA_REGISTRYINDEX, module);
            lua_setmetatable(L, -2);
            inst.ref = luaL_ref(L, LUA_REGISTRYINDEX);
            Instance& stored = instances_[id] = inst;
            CallMethod(id, stored, "onStart", 0, false);
            if (!scene.Exists(id)) continue;
        }
        Instance& inst = instances_[id];
        if (!inst.faulted) CallMethod(id, inst, "onUpdate", dt, true);
    }
}

Json ScriptHost::Eval(const std::string& code, EntityId entity) {
    lua_State* L = State();
    int top = lua_gettop(L);
    std::string asExpr = "return " + code;
    if (luaL_loadbufferx(L, asExpr.data(), asExpr.size(), "=eval", "t") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbufferx(L, code.data(), code.size(), "=eval", "t") != LUA_OK) {
            std::string msg = PopMessage(L);
            lua_settop(L, top);
            throw ApiError("script_syntax_error", msg, "Check the Lua syntax; the line number is after \"eval:\".");
        }
    }
    if (entity != kNullEntity) {
        auto it = instances_.find(entity);
        if (it != instances_.end() && it->second.ref >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.ref);
        } else {
            lua_createtable(L, 0, 2);
            lua_pushinteger(L, entity);
            lua_setfield(L, -2, "id");
            lua_pushstring(L, engine_.GetScene().Record(entity)->name.c_str());
            lua_setfield(L, -2, "name");
            lua_getfield(L, LUA_REGISTRYINDEX, kScriptMetaKey);
            lua_setmetatable(L, -2);
        }
        lua_setglobal(L, "self");
    }
    std::vector<std::string> output;
    output_ = &output;
    ArmBudget();
    int status = lua_pcall(L, 0, LUA_MULTRET, 0);
    output_ = nullptr;
    if (entity != kNullEntity) {
        lua_pushnil(L);
        lua_setglobal(L, "self");
    }
    if (status != LUA_OK) {
        std::string msg = PopMessage(L);
        lua_settop(L, top);
        RecordError("", entity, msg);
        throw ApiError("script_error", msg);
    }
    int n = lua_gettop(L) - top;
    Json value;
    if (n == 1) {
        value = ToJson(L, top + 1);
    } else if (n > 1) {
        value = Json::MakeArray();
        for (int i = 1; i <= n; ++i) value.push(ToJson(L, top + i));
    }
    lua_settop(L, top);
    Json out = Json::MakeObject();
    out["value"] = value;
    Json lines = Json::MakeArray();
    for (const std::string& l : output) lines.push(l);
    out["output"] = lines;
    return out;
}

void ScriptHost::RecordError(const std::string& script, EntityId entity, const std::string& message) {
    ScriptError e;
    e.script = script;
    e.entity = entity;
    e.message = message;
    e.frame = engine_.Frame();
    errors_.push_back(e);
    if (errors_.size() > kMaxErrors) errors_.erase(errors_.begin());
    OE_LOG_ERROR("script", "%s%s", entity != kNullEntity ? Format("entity %u: ", entity).c_str() : "", message.c_str());
}

void ScriptHost::AppendOutput(const std::string& line) {
    if (output_) output_->push_back(line);
}

Json ScriptHost::Status() const {
    Json out = Json::MakeObject();
    Json mods = Json::MakeArray();
    for (const auto& kv : modules_) {
        Json m = Json::MakeObject();
        m["path"] = kv.first;
        m["loaded"] = kv.second.ref >= 0;
        mods.push(m);
    }
    out["modules"] = mods;
    Json insts = Json::MakeArray();
    for (const auto& kv : instances_) {
        Json i = Json::MakeObject();
        i["entity"] = kv.first;
        i["path"] = kv.second.path;
        i["running"] = kv.second.ref >= 0 && !kv.second.faulted;
        insts.push(i);
    }
    out["instances"] = insts;
    out["errors"] = static_cast<uint64_t>(errors_.size());
    out["sessionActive"] = L_ != nullptr;
    return out;
}

}  // namespace oe
