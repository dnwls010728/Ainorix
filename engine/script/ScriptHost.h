#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/Json.h"
#include "scene/Reflect.h"

struct lua_State;

namespace oe {

class Engine;
struct PhysicsEvent;

struct ScriptError {
    std::string script;  // project-relative path ("" for script.eval)
    EntityId entity = kNullEntity;
    std::string message;  // includes "file:line:" when Lua knows it
    uint64_t frame = 0;
};

// Runs Lua scripts attached through the Script component.
//
// One sandboxed Lua state lives for one play session: Reset() (on sim.stop,
// scene load, ...) discards it and the next simulation step starts fresh,
// so runs are reproducible. Scripts only see the engine through the
// `scene`, `input`, `time` and `log` tables (see docs/SCRIPTING.md).
class ScriptHost {
public:
    explicit ScriptHost(Engine& engine);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Called once per fixed simulation step, before built-in systems.
    void Update(float dt);
    // Destroys every script instance and the Lua state.
    void Reset();
    // Destroys the instances but keeps the Lua state (globals, timers,
    // loaded modules). Used when a game changes scene.
    void ResetInstances();
    // Reloads modules whose files changed on disk. Existing instances keep
    // their state and pick up the new functions. changedPath forces a known
    // project-relative API write to reload even if its timestamp is unchanged.
    // Returns reloaded paths.
    std::vector<std::string> PollHotReload(const std::string& changedPath = {});
    // Calls onCollisionEnter/Exit and onTriggerEnter/Exit(self, otherId) on
    // both entities of every event.
    void DispatchPhysicsEvents(const std::vector<PhysicsEvent>& events);
    // Calls `method(self)` on the entity's script instance if it has one.
    void Notify(EntityId id, const char* method);
    // Same with a number argument: `method(self, value)`.
    void Notify(EntityId id, const char* method, float value);
    // Reloads every loaded module regardless of timestamps.
    std::vector<std::string> ReloadAll();

    // Runs a chunk of Lua. Expressions are returned ("1 + 2" -> 3).
    // `self` is bound to the entity's script instance (or a plain entity
    // handle) when `entity` is given. Throws ApiError on failure.
    Json Eval(const std::string& code, EntityId entity);

    // Global names a script sees in the sandbox (Lua base libraries and the
    // engine API), for static checks. Computed once.
    static const std::set<std::string>& SandboxGlobals(Engine& engine);

    const std::vector<ScriptError>& Errors() const { return errors_; }
    void ClearErrors() { errors_.clear(); }
    Json Status() const;

    // ----- used by the Lua bindings --------------------------------------
    Engine& GetEngine() { return engine_; }
    void RecordError(const std::string& script, EntityId entity, const std::string& message);
    void AppendOutput(const std::string& line);
    // Loads a project Lua file in the sandbox and leaves its return value
    // on the Lua stack. Returns false (error message on the stack) on failure.
    bool LoadFile(const std::string& path);
    void ArmBudget() { budgetTicks = 0; }
    // Pushes the entity's live script instance; false if it has none.
    bool PushInstance(EntityId id);
    void FaultInstance(EntityId id, const std::string& message);
    std::vector<EntityId> InstanceIds() const;
    int budgetTicks = 0;  // incremented by the instruction-count hook
    float currentDt = 0.0f;

private:
    struct Module {
        int ref = -1;  // LUA_NOREF
        int64_t mtime = 0;
    };
    struct Instance {
        int ref = -1;
        std::string path;
        std::string params;  // serialized, to detect edits
        bool faulted = false;
    };

    lua_State* State();
    void Open();
    int GetModule(const std::string& path);  // ref or -1
    bool ReloadModule(const std::string& path, Module& module);
    void DestroyInstance(EntityId id, Instance& inst, bool callOnDestroy);
    bool CallMethod(EntityId id, Instance& inst, const char* name, float dt, bool withDt, EntityId other = kNullEntity);

    Engine& engine_;
    lua_State* L_ = nullptr;
    std::map<std::string, Module> modules_;
    std::map<EntityId, Instance> instances_;
    std::vector<ScriptError> errors_;
    std::vector<std::string>* output_ = nullptr;
};

}  // namespace oe
