// Native editor core: lifecycle, window input -> Dear ImGui, command helpers,
// menus, toolbar, status bar, dock layout, prompts and the main loop.
// Panels live in EditorPanels.cpp, the Scene/Game views in EditorViewports.cpp.
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ImGuizmo.h"
#include "imgui_internal.h"
#include "sokol_imgui.h"

#include "app/Engine.h"
#include "scene/TileGrid.h"
#include "app/Project.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "render/GpuDevice.h"
#include "render/GpuRenderer.h"

namespace oe {

extern const unsigned char kDefaultFontData[];
extern const size_t kDefaultFontSize;

namespace {

constexpr float kBaseFontSize = 15.0f;

ImGuiKey ToImGuiKey(WindowKey k) {
    int i = static_cast<int>(k);
    if (k >= WindowKey::A && k <= WindowKey::Z) return static_cast<ImGuiKey>(ImGuiKey_A + (i - static_cast<int>(WindowKey::A)));
    if (k >= WindowKey::Num0 && k <= WindowKey::Num9) return static_cast<ImGuiKey>(ImGuiKey_0 + (i - static_cast<int>(WindowKey::Num0)));
    if (k >= WindowKey::F1 && k <= WindowKey::F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (i - static_cast<int>(WindowKey::F1)));
    switch (k) {
        case WindowKey::Tab: return ImGuiKey_Tab;
        case WindowKey::Left: return ImGuiKey_LeftArrow;
        case WindowKey::Right: return ImGuiKey_RightArrow;
        case WindowKey::Up: return ImGuiKey_UpArrow;
        case WindowKey::Down: return ImGuiKey_DownArrow;
        case WindowKey::PageUp: return ImGuiKey_PageUp;
        case WindowKey::PageDown: return ImGuiKey_PageDown;
        case WindowKey::Home: return ImGuiKey_Home;
        case WindowKey::End: return ImGuiKey_End;
        case WindowKey::Insert: return ImGuiKey_Insert;
        case WindowKey::Delete: return ImGuiKey_Delete;
        case WindowKey::Backspace: return ImGuiKey_Backspace;
        case WindowKey::Space: return ImGuiKey_Space;
        case WindowKey::Enter: return ImGuiKey_Enter;
        case WindowKey::KeypadEnter: return ImGuiKey_KeypadEnter;
        case WindowKey::Escape: return ImGuiKey_Escape;
        case WindowKey::Shift: return ImGuiKey_LeftShift;
        case WindowKey::Control: return ImGuiKey_LeftCtrl;
        case WindowKey::Alt: return ImGuiKey_LeftAlt;
        case WindowKey::Super: return ImGuiKey_LeftSuper;
        case WindowKey::Minus: return ImGuiKey_Minus;
        case WindowKey::Equal: return ImGuiKey_Equal;
        case WindowKey::LeftBracket: return ImGuiKey_LeftBracket;
        case WindowKey::RightBracket: return ImGuiKey_RightBracket;
        case WindowKey::Backslash: return ImGuiKey_Backslash;
        case WindowKey::Semicolon: return ImGuiKey_Semicolon;
        case WindowKey::Apostrophe: return ImGuiKey_Apostrophe;
        case WindowKey::Comma: return ImGuiKey_Comma;
        case WindowKey::Period: return ImGuiKey_Period;
        case WindowKey::Slash: return ImGuiKey_Slash;
        case WindowKey::GraveAccent: return ImGuiKey_GraveAccent;
        default: return ImGuiKey_None;
    }
}

// Key names the game sees (InputState, input.key), as the Win32/web platforms report them.
std::string GameKeyName(WindowKey k) {
    int i = static_cast<int>(k);
    if (k >= WindowKey::A && k <= WindowKey::Z) return std::string(1, static_cast<char>('A' + (i - static_cast<int>(WindowKey::A))));
    if (k >= WindowKey::Num0 && k <= WindowKey::Num9) return std::string(1, static_cast<char>('0' + (i - static_cast<int>(WindowKey::Num0))));
    switch (k) {
        case WindowKey::Space: return "Space";
        case WindowKey::Left: return "Left";
        case WindowKey::Right: return "Right";
        case WindowKey::Up: return "Up";
        case WindowKey::Down: return "Down";
        case WindowKey::Shift: return "Shift";
        case WindowKey::Control: return "Control";
        case WindowKey::Escape: return "Escape";
        case WindowKey::Enter:
        case WindowKey::KeypadEnter: return "Enter";
        case WindowKey::Tab: return "Tab";
        default: return std::string();
    }
}

void SimguiLog(const char* tag, uint32_t level, uint32_t item, const char* message, uint32_t line, const char*, void*) {
    const char* msg = message ? message : "(no message in release builds)";
    if (level <= 1) OE_LOG_ERROR("editor", "%s item %u line %u: %s", tag, item, line, msg);
    else if (level == 2) OE_LOG_WARN("editor", "%s item %u line %u: %s", tag, item, line, msg);
}

// Entity presets of the Create menu.
struct Preset {
    const char* key;
    const char* group;
    const char* label;
    const char* entityName;
    const char* components;  // JSON
};
const Preset kPresets[] = {
    {"empty", "Basic", "Empty", "Empty", "{}"},
    {"cube", "Basic", "Cube", "Cube", R"({"Transform":{"position":[0,0.5,0]},"MeshRenderer":{"mesh":"cube"}})"},
    {"sphere", "Basic", "Sphere", "Sphere", R"({"Transform":{"position":[0,0.5,0]},"MeshRenderer":{"mesh":"sphere"}})"},
    {"plane", "Basic", "Plane", "Plane", R"({"Transform":{"scale":[4,1,4]},"MeshRenderer":{"mesh":"plane"}})"},
    {"pyramid", "Basic", "Pyramid", "Pyramid", R"({"Transform":{"position":[0,0.5,0]},"MeshRenderer":{"mesh":"pyramid"}})"},
    {"camera", "Rendering", "Camera", "Camera", R"({"Transform":{"position":[0,3,8],"rotation":[-15,0,0]},"Camera":{"active":false}})"},
    {"light", "Rendering", "Directional Light", "Directional Light", R"({"Transform":{"rotation":[-50,30,0]},"DirectionalLight":{}})"},
    {"pointlight", "Rendering", "Point Light", "Point Light",
     R"({"Transform":{"position":[0,2,0],"scale":[0.2,0.2,0.2]},"MeshRenderer":{"mesh":"sphere","color":[1,0.85,0.6],"unlit":true,"castShadows":false},"PointLight":{}})"},
    {"crate", "Physics", "Physics Crate", "Crate", R"({"Transform":{"position":[0,3,0]},"MeshRenderer":{"mesh":"cube","color":[0.7,0.5,0.3]},"Collider":{},"RigidBody":{}})"},
    {"ball", "Physics", "Physics Ball", "Ball",
     R"({"Transform":{"position":[0,3,0]},"MeshRenderer":{"mesh":"sphere","color":[0.9,0.9,0.95]},"Collider":{"shape":"sphere","bounciness":0.6},"RigidBody":{}})"},
    {"wall", "Physics", "Static Wall", "Wall", R"({"Transform":{"position":[0,1,-4],"scale":[6,2,0.5]},"MeshRenderer":{"mesh":"cube","color":[0.6,0.6,0.65]},"Collider":{}})"},
    {"sprite", "2D", "Sprite", "Sprite", R"({"Transform":{},"Sprite":{}})"},
    {"tilemap", "2D", "Tilemap", "Tilemap", R"({"Transform":{"position":[0,0,0]},"Tilemap":{"map":["","","####"],"legend":{"#":0},"solid":"#"}})"},
    {"box2d", "2D", "2D Physics Box", "Box", R"({"Transform":{"position":[0,3,0],"scale":[1,1,0.2]},"MeshRenderer":{"mesh":"cube","color":[0.7,0.5,0.3]},"Collider2D":{},"RigidBody2D":{}})"},
    {"ball2d", "2D", "2D Physics Ball", "Ball",
     R"({"Transform":{"position":[0,3,0]},"MeshRenderer":{"mesh":"sphere","color":[0.9,0.9,0.95]},"Collider2D":{"shape":"circle","bounciness":0.5},"RigidBody2D":{}})"},
    {"platform2d", "2D", "One-way Platform", "Platform",
     R"({"Transform":{"position":[0,1,0],"scale":[3,0.2,0.2]},"MeshRenderer":{"mesh":"cube","color":[0.55,0.45,0.35]},"Collider2D":{"oneWay":true}})"},
    {"character2d", "2D", "2D Character", "Character",
     R"({"Transform":{"position":[0,2,0]},"MeshRenderer":{"mesh":"cube","color":[0.3,0.6,1]},"CharacterBody2D":{"radius":0.5,"height":1}})"},
    {"camera2d", "2D", "2D Camera", "2D Camera", R"({"Transform":{"position":[0,0,20]},"Camera":{"projection":"orthographic","orthoSize":5.625,"active":false}})"},
    {"text", "UI", "UI Text", "Text", R"({"UIText":{"text":"Hello"}})"},
    {"button", "UI", "UI Button", "Button", R"({"UIButton":{}})"},
    {"panel", "UI", "UI Panel", "Panel", R"({"UIPanel":{"anchor":"center","x":0,"y":0,"width":400,"height":240,"color":[0.1,0.11,0.15],"opacity":0.9,"radius":12}})"},
    {"image", "UI", "UI Image", "Image", R"({"UIImage":{"width":128,"height":128}})"},
    {"slider", "UI", "UI Slider", "Slider", R"({"UISlider":{}})"},
    {"progress", "UI", "UI Progress Bar", "Progress",
     R"({"UISlider":{"interactable":false,"handle":false,"height":16,"radius":8,"fillColor":[0.24,0.77,0.49]}})"},
    {"menu", "UI", "UI Menu (vertical layout)", "Menu",
     R"({"UIPanel":{"anchor":"center","x":0,"y":0,"width":320,"height":0,"color":[0.1,0.11,0.15],"opacity":0.9,"radius":12},"UILayout":{"padding":20,"spacing":12,"crossAlign":"stretch","fit":true}})"},
};

// ----- Preferences stored in the ImGui .ini ([OwnEngine][Editor]) -----------

NativeEditor::Impl* g_settingsTarget = nullptr;

void* SettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    return std::strcmp(name, "Editor") == 0 ? g_settingsTarget : nullptr;
}

void SettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    auto* m = static_cast<NativeEditor::Impl*>(entry);
    float f = 0;
    int i = 0;
    float v[3];
    if (std::sscanf(line, "UiScale=%f", &f) == 1) m->uiScale = Clamp(f, 0.6f, 2.5f);
    else if (std::sscanf(line, "Grid=%d", &i) == 1) m->showGrid = i != 0;
    else if (std::sscanf(line, "Colliders=%d", &i) == 1) m->showColliders = i != 0;
    else if (std::sscanf(line, "Icons=%d", &i) == 1) m->showIcons = i != 0;
    else if (std::sscanf(line, "Snap=%d", &i) == 1) m->snap = i != 0;
    else if (std::sscanf(line, "SnapMove=%f", &f) == 1) m->snapMove = f;
    else if (std::sscanf(line, "SnapAngle=%f", &f) == 1) m->snapAngle = f;
    else if (std::sscanf(line, "SnapScale=%f", &f) == 1) m->snapScale = f;
    else if (std::sscanf(line, "GizmoLocal=%d", &i) == 1) m->gizmoLocal = i != 0;
    else if (std::sscanf(line, "GameAspect=%d", &i) == 1) m->gameAspect = std::max(0, std::min(3, i));
    else if (std::sscanf(line, "NetworkPlayers=%d", &i) == 1) m->networkPlayers = std::max(1, std::min(8, i));
    else if (std::sscanf(line, "NetworkLatency=%d", &i) == 1) m->networkLatency = std::max(0, std::min(30, i));
    else if (std::sscanf(line, "FlySpeed=%f", &f) == 1) m->flySpeed = Clamp(f, 0.5f, 200.0f);
    else if (std::strncmp(line, "Language=", 9) == 0 && !m->forceLanguage) SetEditorLanguage(ParseEditorLanguage(line + 9));
    else if (std::sscanf(line, "Panels=%d", &i) == 1) {
        m->showHierarchy = i & 1;
        m->showInspector = i & 2;
        m->showScene = i & 4;
        m->showGame = i & 8;
        m->showConsole = i & 16;
        m->showAssets = i & 32;
        m->showScripts = i & 64;
        m->showTiles = i & 128;
        m->showNetwork = i & 256;
    } else if (std::sscanf(line, "Camera=%f,%f,%f", &v[0], &v[1], &v[2]) == 3) {
        m->cam.target = Vec3(v[0], v[1], v[2]);
    } else if (std::sscanf(line, "CameraAngles=%f,%f,%f", &v[0], &v[1], &v[2]) == 3) {
        m->cam.yaw = v[0];
        m->cam.pitch = Clamp(v[1], -89.0f, 89.0f);
        m->cam.distance = Clamp(v[2], 0.05f, 5000.0f);
    } else if (std::sscanf(line, "Camera2D=%d", &i) == 1) {
        m->cam.Set2D(i != 0);
    }
}

void SettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
    NativeEditor::Impl* m = g_settingsTarget;
    if (!m) return;
    int panels = (m->showHierarchy ? 1 : 0) | (m->showInspector ? 2 : 0) | (m->showScene ? 4 : 0) | (m->showGame ? 8 : 0) |
                 (m->showConsole ? 16 : 0) | (m->showAssets ? 32 : 0) | (m->showScripts ? 64 : 0) | (m->showTiles ? 128 : 0) | (m->showNetwork ? 256 : 0);
    buf->appendf("[%s][Editor]\n", handler->TypeName);
    buf->appendf("UiScale=%.2f\nGrid=%d\nColliders=%d\nIcons=%d\nSnap=%d\n", m->uiScale, m->showGrid, m->showColliders, m->showIcons, m->snap);
    buf->appendf("SnapMove=%.3f\nSnapAngle=%.3f\nSnapScale=%.3f\nGizmoLocal=%d\n", m->snapMove, m->snapAngle, m->snapScale, m->gizmoLocal);
    buf->appendf("NetworkPlayers=%d\nNetworkLatency=%d\n", m->networkPlayers, m->networkLatency);
    buf->appendf("GameAspect=%d\nFlySpeed=%.2f\nPanels=%d\nLanguage=%s\n", m->gameAspect, m->flySpeed, panels, EditorLanguageCode(GetEditorLanguage()));
    buf->appendf("Camera=%.4f,%.4f,%.4f\nCameraAngles=%.3f,%.3f,%.4f\nCamera2D=%d\n\n", m->cam.target.x, m->cam.target.y, m->cam.target.z, m->cam.yaw,
                 m->cam.pitch, m->cam.distance, m->cam.mode2D);
}

// Toolbar icons drawn as shapes, so they do not depend on font glyphs.
enum class Icon { Play, Pause, Stop, Step, Select, Move, Rotate, Scale };

bool IconButton(const char* id, Icon icon, bool active, const char* tooltip, bool enabled = true) {
    float h = ImGui::GetFrameHeight();
    ImVec2 size(h * 1.25f, h);
    if (!enabled) ImGui::BeginDisabled();
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool pressed = ImGui::Button(id, size);
    if (active) ImGui::PopStyleColor();
    if (!enabled) ImGui::EndDisabled();
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    float r = h * 0.26f;
    ImU32 col = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    switch (icon) {
        case Icon::Play: dl->AddTriangleFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.8f, c.y + r), ImVec2(c.x + r, c.y), IM_COL32(90, 200, 120, 255)); break;
        case Icon::Pause:
            dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.25f, c.y + r), col);
            dl->AddRectFilled(ImVec2(c.x + r * 0.25f, c.y - r), ImVec2(c.x + r * 0.8f, c.y + r), col);
            break;
        case Icon::Stop: dl->AddRectFilled(ImVec2(c.x - r * 0.85f, c.y - r * 0.85f), ImVec2(c.x + r * 0.85f, c.y + r * 0.85f), IM_COL32(230, 90, 80, 255)); break;
        case Icon::Step:
            dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r, c.y + r), ImVec2(c.x + r * 0.5f, c.y), col);
            dl->AddRectFilled(ImVec2(c.x + r * 0.55f, c.y - r), ImVec2(c.x + r, c.y + r), col);
            break;
        case Icon::Select:
            dl->AddTriangleFilled(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x - r * 0.7f, c.y + r * 0.7f), ImVec2(c.x + r * 0.7f, c.y + r * 0.2f), col);
            break;
        case Icon::Move:
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, 1.6f);
            dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, 1.6f);
            dl->AddTriangleFilled(ImVec2(c.x + r * 1.2f, c.y), ImVec2(c.x + r * 0.7f, c.y - r * 0.35f), ImVec2(c.x + r * 0.7f, c.y + r * 0.35f), col);
            dl->AddTriangleFilled(ImVec2(c.x - r * 1.2f, c.y), ImVec2(c.x - r * 0.7f, c.y + r * 0.35f), ImVec2(c.x - r * 0.7f, c.y - r * 0.35f), col);
            dl->AddTriangleFilled(ImVec2(c.x, c.y - r * 1.2f), ImVec2(c.x + r * 0.35f, c.y - r * 0.7f), ImVec2(c.x - r * 0.35f, c.y - r * 0.7f), col);
            dl->AddTriangleFilled(ImVec2(c.x, c.y + r * 1.2f), ImVec2(c.x - r * 0.35f, c.y + r * 0.7f), ImVec2(c.x + r * 0.35f, c.y + r * 0.7f), col);
            break;
        case Icon::Rotate:
            dl->PathArcTo(c, r, 0.4f, 5.6f, 20);
            dl->PathStroke(col, 1.8f);
            dl->AddTriangleFilled(ImVec2(c.x + r * 1.35f, c.y - r * 0.05f), ImVec2(c.x + r * 0.55f, c.y - r * 0.05f), ImVec2(c.x + r * 0.95f, c.y + r * 0.55f), col);
            break;
        case Icon::Scale:
            dl->AddRect(ImVec2(c.x - r, c.y - r * 0.2f), ImVec2(c.x + r * 0.2f, c.y + r), col, 0.0f, 1.6f);
            dl->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.1f), ImVec2(c.x + r, c.y - r), col, 1.6f);
            dl->AddTriangleFilled(ImVec2(c.x + r * 1.1f, c.y - r * 1.1f), ImVec2(c.x + r * 0.4f, c.y - r), ImVec2(c.x + r, c.y - r * 0.4f), col);
            break;
    }
    return pressed;
}

}  // namespace

// ----- Shared helpers -----------------------------------------------------------

Json Vec3Json(const Vec3& v) { return Json(Json::Array{Json(v.x), Json(v.y), Json(v.z)}); }

Vec3 JsonVec3(const Json& j, Vec3 def) {
    if (!j.isArray() || j.size() < 3) return def;
    return Vec3(j[0].asFloat(def.x), j[1].asFloat(def.y), j[2].asFloat(def.z));
}

Json ObjectOf(std::initializer_list<std::pair<const char*, Json>> members) {
    Json j = Json::MakeObject();
    for (const auto& m : members) j[m.first] = m.second;
    return j;
}

void HelpTooltip(const std::string& text) {
    if (text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

std::string AssetKindForField(const std::string& type, const std::string& field) {
    if (type == "MeshRenderer" && field == "mesh") return "model";
    if (field == "material") return "material";
    if (field == "texture" || field == "tileset") return "texture";
    if (type == "Script" && field == "path") return "script";
    if (type == "Prefab" && field == "path") return "prefab";
    if (type == "AudioSource" && field == "clip") return "audio";
    if (field == "font") return "font";
    return "";
}

Json NativeEditor::Impl::Call(const std::string& name, const Json& args, bool quiet) {
    Json r;
    if (gamePeer > 0 && name.compare(0, 6, "input.") == 0) {
        Json route = Json::MakeObject(); route["peer"] = gamePeer; route["command"] = name;
        route["args"] = args.isNull() ? Json::MakeObject() : args;
        r = engine.Call("net.peer_call", route); if (r["ok"].asBool()) r = r["result"];
    } else r = engine.Call(name, args.isNull() ? Json::MakeObject() : args);
    if (!r["ok"].asBool() && !quiet) {
        const Json& e = r["error"];
        std::string text = name + ": " + e["message"].asString("failed");
        if (!e["hint"].asString("").empty()) text += "\n" + e["hint"].asString();
        Notify(text, true);
    }
    return r;
}

void NativeEditor::Impl::Notify(const std::string& text, bool error) {
    lastNotice = text;
    toasts.push_back({text, error, time + (error ? 6.0 : 2.5)});
    if (toasts.size() > 5) toasts.erase(toasts.begin());
    if (error) OE_LOG_WARN("editor", "%s", text.c_str());
}

const EntityRow* NativeEditor::Impl::Row(EntityId id) const {
    auto it = rowIndex.find(id);
    return it == rowIndex.end() ? nullptr : &rows[it->second];
}

std::string NativeEditor::Impl::UniqueName(const std::string& base) const {
    auto taken = [&](const std::string& n) {
        for (const EntityRow& r : rows) {
            if (r.name == n) return true;
        }
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        std::string n = base + " " + std::to_string(i);
        if (!taken(n)) return n;
    }
}

bool NativeEditor::Impl::IsSelected(EntityId id) const { return std::find(selection.begin(), selection.end(), id) != selection.end(); }

void NativeEditor::Impl::SelectOnly(EntityId id) {
    selection.clear();
    if (id != kNullEntity) selection.push_back(id);
    anchorRow = id;
    selectedId = kNullEntity;  // reload the inspector
}

void NativeEditor::Impl::ToggleSelect(EntityId id) {
    auto it = std::find(selection.begin(), selection.end(), id);
    if (it != selection.end()) selection.erase(it);
    else selection.insert(selection.begin(), id);  // newest becomes primary
    anchorRow = id;
    selectedId = kNullEntity;
}

bool NativeEditor::Impl::Playing() const { return sim["playing"].asBool(false); }
bool NativeEditor::Impl::InPlaySession() const { return sim["inPlaySession"].asBool(false); }
bool NativeEditor::Impl::Dirty() const { return sim["dirty"].asBool(false); }

void NativeEditor::Impl::Refresh(bool force) {
    uint64_t rev = engine.Revision();
    if (!force && rev == seenRevision) return;
    // While the game runs every frame is a new revision: 10 refreshes a second are plenty.
    if (!force && Playing() && time - lastRefresh < 0.1) return;
    seenRevision = rev;
    lastRefresh = time;
    sim = Call("sim.state", Json(), true)["result"];
    Json summary = Call("scene.summary", Json(), true)["result"];
    sceneName = summary["name"].asString("");
    scenePath = summary["path"].asString("");
    rows.clear();
    rowIndex.clear();
    for (const Json& e : summary["entities"].items()) {
        EntityRow r;
        r.id = static_cast<EntityId>(e["id"].asNumber(0));
        r.name = e["name"].asString("");
        r.parent = static_cast<EntityId>(e["parent"].asNumber(0));
        for (const Json& c : e["components"].items()) r.components.push_back(c.asString(""));
        rowIndex[r.id] = rows.size();
        rows.push_back(std::move(r));
    }
    selection.erase(std::remove_if(selection.begin(), selection.end(), [&](EntityId id) { return rowIndex.count(id) == 0; }), selection.end());
    EntityId primary = Primary();
    if (primary == kNullEntity) {
        selected = Json();
        selectedId = kNullEntity;
    } else {
        Json r = Call("entity.get", ObjectOf({{"id", Json(primary)}}), true);
        selected = r["ok"].asBool() ? r["result"] : Json();
        selectedId = primary;
        selectedRevision = rev;
    }
}

void NativeEditor::Impl::RefreshAssets(bool force) {
    if (!force && time - assetsTime < 2.0) return;  // the project folder can change behind our back
    assetsTime = time;
    Json r = Call("asset.list", Json(), true);
    if (r["ok"].asBool()) assets = r["result"];
    sceneFiles.clear();
    for (const Json& a : assets.items()) {
        if (a["kind"].asString("") == "scene") sceneFiles.push_back(a["path"].asString(""));
    }
}

void NativeEditor::Impl::PollLog() {
    if (time - lastLogPoll < 0.2 && lastLogPoll >= 0) return;
    lastLogPoll = time;
    Json r = Call("log.get", ObjectOf({{"since", Json(logSeq)}, {"limit", Json(500)}}), true);
    if (!r["ok"].asBool()) return;
    for (const Json& e : r["result"]["entries"].items()) {
        LogLine l;
        l.seq = static_cast<uint64_t>(e["seq"].asNumber(0));
        l.level = e["level"].asString("info");
        l.category = e["category"].asString("");
        l.message = e["message"].asString("");
        if (l.level == "warn") ++warnCount;
        if (l.level == "error") ++errorCount;
        logSeq = std::max(logSeq, l.seq);
        log.push_back(std::move(l));
    }
    while (log.size() > 2000) log.pop_front();
}

// Commands from HTTP / MCP (agents): show edits as they land.
void NativeEditor::Impl::OnRemoteCall(const std::string& name, const Json& args, const Json& result) {
    const Command* cmd = engine.Commands().Find(name);
    if (!cmd || !cmd->mutates || !result["ok"].asBool()) return;
    EntityId id = static_cast<EntityId>(result["result"]["id"].asNumber(0));
    const Json& a = args["id"];
    if (id == kNullEntity && a.isNumber()) id = static_cast<EntityId>(a.asNumber());
    if (id == kNullEntity && a.isString()) {
        for (const EntityRow& r : rows) {
            if (r.name == a.asString()) id = r.id;
        }
    }
    std::string what = name;
    if (id != kNullEntity) {
        const EntityRow* r = Row(id);
        what += " " + (r ? r->name : "#" + std::to_string(id));
        remoteEdits[id] = time;
    }
    if (args["type"].isString()) what += " " + args["type"].asString();
    OE_LOG_INFO("api", "%s", what.c_str());
    Notify("API: " + what);
    selectedId = kNullEntity;  // reload the inspector
}

// ----- Actions ------------------------------------------------------------------

void NativeEditor::Impl::Save() {
    if (InPlaySession()) {
        Notify(Tr("Stop the game before saving (the scene is restored on stop)."), true);
        return;
    }
    if (scenePath.empty()) {
        openSaveAs = true;
        return;
    }
    if (Ok(Call("scene.save", Json()))) Notify(Format(Tr("Saved %s"), sceneName.c_str()));
    Refresh(true);
}

void NativeEditor::Impl::TogglePlay() {
    if (InPlaySession()) {
        ReleaseGameInput();
        Call("sim.stop", Json());
        gamePeer = 0;
        gameFocused = false;
    } else {
        if (networkPlayers > 1) {
            Json result = Call("net.spawn_local_peers", ObjectOf({{"count", Json(networkPlayers - 1)}}));
            if (!Ok(result)) return;
            if (networkLatency) Call("net.simulate", ObjectOf({{"latencyFrames", Json(networkLatency)}}));
        } else Call("sim.play", Json());
        requestGameFocus = true;
    }
    Refresh(true);
}

void NativeEditor::Impl::Undo() {
    Call("history.undo", Json());
    selectedId = kNullEntity;
    Refresh(true);
}

void NativeEditor::Impl::Redo() {
    Call("history.redo", Json());
    selectedId = kNullEntity;
    Refresh(true);
}

void NativeEditor::Impl::DeleteSelection() {
    // Children go with their parents: skip entities whose ancestor is also selected.
    std::vector<EntityId> roots;
    for (EntityId id : selection) {
        bool covered = false;
        for (const EntityRow* r = Row(id); r && r->parent != kNullEntity; r = Row(r->parent)) covered = covered || IsSelected(r->parent);
        if (!covered) roots.push_back(id);
    }
    if (roots.empty()) return;
    if (roots.size() == 1) {
        Call("entity.delete", ObjectOf({{"id", Json(roots[0])}}));
    } else {
        Json cmds = Json::MakeArray();
        for (EntityId id : roots) cmds.push(ObjectOf({{"command", Json("entity.delete")}, {"args", ObjectOf({{"id", Json(id)}})}}));
        Call("api.batch", ObjectOf({{"calls", cmds}}));
    }
    selection.clear();
    Refresh(true);
}

void NativeEditor::Impl::DuplicateSelection() {
    std::vector<EntityId> copies;
    for (EntityId id : selection) {
        Json r = Call("entity.duplicate", ObjectOf({{"id", Json(id)}}));
        if (Ok(r)) copies.push_back(static_cast<EntityId>(r["result"]["id"].asNumber(0)));
    }
    if (!copies.empty()) {
        selection = copies;
        selectedId = kNullEntity;
    }
    Refresh(true);
}

void NativeEditor::Impl::FrameSelection() {
    EntityId id = Primary();
    if (id == kNullEntity || !engine.GetScene().Exists(id)) return;
    Vec3 p = engine.GetScene().WorldMatrix(id).TransformPoint(Vec3(0, 0, 0));
    Vec3 s = JsonVec3(selected["components"]["Transform"]["scale"], Vec3(1, 1, 1));
    float radius = std::max(0.5f, std::max(std::fabs(s.x), std::max(std::fabs(s.y), std::fabs(s.z))));
    if (const Tilemap* tm = engine.GetScene().Get<Tilemap>(id)) {
        // The whole map (the entity origin is its top-left corner).
        int w = 0, h = 0;
        MapSize(*tm, w, h);
        float ts = std::max(0.001f, tm->tileSize);
        p = engine.GetScene().WorldMatrix(id).TransformPoint(Vec3(0.5f * static_cast<float>(w) * ts, -0.5f * static_cast<float>(h) * ts, 0));
        radius = std::max(radius, 0.5f * static_cast<float>(std::max(w, h)) * ts * std::max(std::fabs(s.x), std::fabs(s.y)));
    }
    cam.Frame(p, radius);
}

void NativeEditor::Impl::CreatePreset(const char* key) {
    const Preset* p = nullptr;
    for (const Preset& x : kPresets) {
        if (std::strcmp(x.key, key) == 0) p = &x;
    }
    if (!p) return;
    Json components = Json::parse(p->components);
    // New objects appear where the Scene view looks (lights keep their position).
    if (components.has("Transform") && std::strcmp(key, "light") != 0) {
        Json& t = components["Transform"];
        Vec3 pos = JsonVec3(t["position"], Vec3(0, 0, 0));
        auto round = [](float v) { return std::round(v * 100.0f) / 100.0f; };
        if (cam.mode2D) pos = Vec3(round(pos.x + cam.target.x), round(pos.y + cam.target.y), pos.z);
        else pos = Vec3(round(pos.x + cam.target.x), pos.y, round(pos.z + cam.target.z));
        t["position"] = Vec3Json(pos);
    }
    Json args = ObjectOf({{"name", Json(UniqueName(p->entityName))}, {"components", components}});
    // UI elements created while a UI element is selected go inside it.
    auto isUI = [](const std::string& c) { return c.rfind("UI", 0) == 0 && c != "UILayout" && c != "UICanvas"; };
    bool newIsUI = false;
    for (const auto& kv : components.members()) newIsUI = newIsUI || isUI(kv.first);
    const EntityRow* sel = Row(Primary());
    if (newIsUI && sel && std::any_of(sel->components.begin(), sel->components.end(), isUI)) args["parent"] = Json(sel->id);
    Json r = Call("entity.create", args);
    if (Ok(r)) {
        Refresh(true);
        SelectOnly(static_cast<EntityId>(r["result"]["id"].asNumber(0)));
        scrollToRow = Primary();
        Notify(Format(Tr("Created %s"), Tr(p->label)));
    }
}

void NativeEditor::Impl::RequestAction(PendingAction action) {
    if (InPlaySession()) {  // back to the edit-time scene first: that is what gets saved or dropped
        Call("sim.stop", Json());
        ReleaseGameInput();
        gameFocused = false;
        Refresh(true);
    }
    if (Dirty()) {
        pending = std::move(action);
        openSavePrompt = true;
        return;
    }
    RunAction(action);
}

void NativeEditor::Impl::RunAction(const PendingAction& action) {
    switch (action.kind) {
        case PendingAction::Kind::LoadScene:
            if (Ok(Call("scene.load", ObjectOf({{"path", Json(action.path)}})))) {
                selection.clear();
                Notify(Format(Tr("Opened %s"), action.path.c_str()));
            }
            break;
        case PendingAction::Kind::NewScene:
            Call("scene.new", ObjectOf({{"name", Json("Untitled")}}));
            selection.clear();
            break;
        case PendingAction::Kind::Quit: quit = true; break;
        case PendingAction::Kind::None: break;
    }
    Refresh(true);
}

void NativeEditor::Impl::ImportDroppedFiles() {
    std::vector<std::string> files;
    files.swap(droppedFiles);
    int imported = 0;
    std::string last;
    for (const std::string& f : files) {
        try {
            last = ImportAssetFile(engine, f);
            ++imported;
        } catch (const ApiError& e) {
            Notify(Format(Tr("Import failed: %s"), e.what()), true);
        }
    }
    if (imported > 0) {
        RefreshAssets(true);
        selectedAsset = last;
        showAssets = true;
        Notify(imported == 1 ? Format(Tr("Imported %s"), last.c_str()) : Format(Tr("Imported %d files"), imported));
    }
}

void NativeEditor::Impl::RunConsoleCommand(const std::string& input) {
    std::string line = input;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\n')) line.pop_back();
    if (line.empty()) return;
    consoleHistory.push_back(line);
    historyPos = -1;
    size_t space = line.find(' ');
    std::string name = line.substr(0, space);
    Json args = Json::MakeObject();
    if (space != std::string::npos) {
        std::string err;
        args = Json::parse(line.substr(space + 1), &err);
        if (!err.empty()) {
            OE_LOG_ERROR("console", "arguments must be one JSON object: %s", err.c_str());
            return;
        }
    }
    OE_LOG_INFO("console", "> %s", line.c_str());
    Json r = engine.Call(name, args);
    std::string out = r.dump(2);
    if (out.size() > 20000) out = out.substr(0, 20000) + "\n... (truncated)";
    if (r["ok"].asBool()) OE_LOG_INFO("console", "%s", out.c_str());
    else OE_LOG_ERROR("console", "%s", out.c_str());
    selectedId = kNullEntity;
    Refresh(true);
    PollLog();
}

// ----- Style and input -------------------------------------------------------------

void NativeEditor::Impl::ApplyStyle() {
    float scale = dpiScale * uiScale;
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 4.0f;
    st.ChildRounding = 4.0f;
    st.FrameRounding = 3.0f;
    st.PopupRounding = 4.0f;
    st.GrabRounding = 3.0f;
    st.TabRounding = 4.0f;
    st.ScrollbarRounding = 6.0f;
    st.WindowPadding = ImVec2(8, 8);
    st.FramePadding = ImVec2(6, 4);
    st.ItemSpacing = ImVec2(8, 5);
    st.IndentSpacing = 16.0f;
    st.WindowBorderSize = 1.0f;
    st.TabBarBorderSize = 1.0f;
    st.DockingSeparatorSize = 3.0f;
    ImVec4* c = st.Colors;
    const ImVec4 bg(0.105f, 0.115f, 0.135f, 1.0f), panel(0.135f, 0.145f, 0.17f, 1.0f), frame(0.18f, 0.195f, 0.225f, 1.0f);
    const ImVec4 accent(0.26f, 0.52f, 0.96f, 1.0f), accentDim(0.22f, 0.38f, 0.66f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.93f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.53f, 0.58f, 1.0f);
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.155f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.06f, 0.065f, 0.08f, 1.0f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.25f, 0.29f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.28f, 0.33f, 1.0f);
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_MenuBarBg] = bg;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accentDim;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.27f, 0.32f, 1.0f);
    c[ImGuiCol_ButtonActive] = accentDim;
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.25f, 0.33f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.29f, 0.38f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.36f, 0.55f, 1.0f);
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_Tab] = bg;
    c[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.26f, 0.33f, 1.0f);
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = bg;
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 0.62f, 0.1f, 1.0f);
    st.ScaleAllSizes(scale);
    st.FontSizeBase = kBaseFontSize;
    st.FontScaleDpi = scale;
    appliedScale = scale;
}

void NativeEditor::Impl::ApplyEvents(const std::vector<WindowEvent>& events) {
    ImGuiIO& io = ImGui::GetIO();
    bool gameInput = gameFocused && InPlaySession();
    bool locked = gameInput && windowInput.mouseLocked;
    for (const WindowEvent& e : events) {
        if (e.type == WindowEvent::Type::Key || e.type == WindowEvent::Type::MouseButton) {
            io.AddKeyEvent(ImGuiMod_Ctrl, e.ctrl);
            io.AddKeyEvent(ImGuiMod_Shift, e.shift);
            io.AddKeyEvent(ImGuiMod_Alt, e.alt);
            io.AddKeyEvent(ImGuiMod_Super, e.super);
        }
        switch (e.type) {
            case WindowEvent::Type::MouseMove:
                if (!locked) io.AddMousePosEvent(e.x, e.y);
                break;
            case WindowEvent::Type::MouseButton:
                if (locked) {
                    // Mouse look: the hidden cursor is not over any panel; clicks belong to the game.
                    if (e.button < 2) {
                        const char* name = e.button == 0 ? "MouseLeft" : "MouseRight";
                        Call("input.mouse", ObjectOf({{"button", Json(name)}, {"down", Json(e.down)}}), true);
                        gameMouseDown[e.button] = e.down;
                    }
                } else if (e.button < ImGuiMouseButton_COUNT) {
                    io.AddMouseButtonEvent(e.button, e.down);
                }
                break;
            case WindowEvent::Type::MouseWheel:
                if (!locked) io.AddMouseWheelEvent(e.x, e.y);
                break;
            case WindowEvent::Type::Key: {
                ImGuiKey k = ToImGuiKey(e.key);
                if (k != ImGuiKey_None) io.AddKeyEvent(k, e.down);
                if (gameInput) {
                    std::string name = GameKeyName(e.key);
                    // Ctrl+P (stop) stays with the editor.
                    bool editorChord = e.ctrl && e.key == WindowKey::P;
                    if (!name.empty() && !editorChord) {
                        if (e.down && !gameKeysDown.count(name)) {
                            gameKeysDown.insert(name);
                            Call("input.key", ObjectOf({{"key", Json(name)}, {"down", Json(true)}}), true);
                        } else if (!e.down && gameKeysDown.count(name)) {
                            gameKeysDown.erase(name);
                            Call("input.key", ObjectOf({{"key", Json(name)}, {"down", Json(false)}}), true);
                        }
                    }
                }
                break;
            }
            case WindowEvent::Type::Text:
                if (!gameInput) io.AddInputCharacter(e.codepoint);
                break;
            case WindowEvent::Type::Focus:
                io.AddFocusEvent(e.down);
                if (!e.down) { ReleaseGameInput(); gameFocused = false; }
                break;
            case WindowEvent::Type::Close: RequestAction({PendingAction::Kind::Quit, ""}); break;
            case WindowEvent::Type::DropFile: droppedFiles.push_back(e.path); break;
        }
    }
}

void NativeEditor::Impl::UpdateCursor() {
    if (!window) return;
    if (gameFocused && windowInput.mouseLocked) return;  // the platform hides it
    WindowCursor c = WindowCursor::Arrow;
    switch (ImGui::GetMouseCursor()) {
        case ImGuiMouseCursor_None: c = WindowCursor::Hidden; break;
        case ImGuiMouseCursor_TextInput: c = WindowCursor::TextInput; break;
        case ImGuiMouseCursor_ResizeAll: c = WindowCursor::ResizeAll; break;
        case ImGuiMouseCursor_ResizeNS: c = WindowCursor::ResizeNS; break;
        case ImGuiMouseCursor_ResizeEW: c = WindowCursor::ResizeEW; break;
        case ImGuiMouseCursor_ResizeNESW: c = WindowCursor::ResizeNESW; break;
        case ImGuiMouseCursor_ResizeNWSE: c = WindowCursor::ResizeNWSE; break;
        case ImGuiMouseCursor_Hand: c = WindowCursor::Hand; break;
        case ImGuiMouseCursor_NotAllowed: c = WindowCursor::NotAllowed; break;
        default: break;
    }
    window->SetCursor(c);
}

void NativeEditor::Impl::Shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global)) TogglePlay();
    if (gameFocused && InPlaySession()) return;  // keys belong to the game
    if (scriptEditorFocused) return;              // and to the code editor (it saves with Ctrl+S itself)
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) Save();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global)) Undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, global) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global)) Redo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) RequestAction({PendingAction::Kind::NewScene, ""});
    if (io.WantTextInput) return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, global)) DuplicateSelection();
    // Entity keys work while the pointer or focus is in the hierarchy or Scene view.
    bool entityContext = sceneHovered || sceneFocused || ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);
    if (!entityContext || sceneLooking) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !selection.empty()) DeleteSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.KeyCtrl) FrameSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && Primary() != kNullEntity) {
        renaming = Primary();
        const EntityRow* r = Row(renaming);
        renameBuffer = r ? r->name : "";
    }
    if (!io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) gizmoOp = GizmoOp::None;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmoOp = GizmoOp::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmoOp = GizmoOp::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmoOp = GizmoOp::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) gizmoLocal = !gizmoLocal;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) selection.clear();
}

// ----- Menus, toolbar, status bar ------------------------------------------------------

void NativeEditor::Impl::CreateMenuItems() {
    const char* group = "";
    bool open = false;
    for (const Preset& p : kPresets) {
        if (std::strcmp(group, p.group) != 0) {
            if (open) ImGui::EndMenu();
            group = p.group;
            open = ImGui::BeginMenu(Tr(group));
        }
        if (open && ImGui::MenuItem(Tr(p.label))) CreatePreset(p.key);
    }
    if (open) ImGui::EndMenu();
}

void NativeEditor::Impl::MainMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    bool session = InPlaySession();
    if (ImGui::BeginMenu(Tr("File"))) {
        if (ImGui::MenuItem(Tr("New Scene"), "Ctrl+N")) RequestAction({PendingAction::Kind::NewScene, ""});
        if (ImGui::BeginMenu(Tr("Open Scene"), !sceneFiles.empty())) {
            for (const std::string& s : sceneFiles) {
                bool current = !scenePath.empty() && scenePath.size() >= s.size() && scenePath.compare(scenePath.size() - s.size(), s.size(), s) == 0;
                if (ImGui::MenuItem(s.c_str(), nullptr, current)) RequestAction({PendingAction::Kind::LoadScene, s});
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Save Scene"), "Ctrl+S", false, !session)) Save();
        if (ImGui::MenuItem(Tr("Save Scene As..."), nullptr, false, !session)) openSaveAs = true;
        ImGui::Separator();
        ImGui::TextDisabled("%s", Tr("Import: drop files on the window"));
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Exit"), "Alt+F4")) RequestAction({PendingAction::Kind::Quit, ""});
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr("Edit"))) {
        if (ImGui::MenuItem(Tr("Undo"), "Ctrl+Z", false, sim["undo"].asInt(0) > 0)) Undo();
        if (ImGui::MenuItem(Tr("Redo"), "Ctrl+Y", false, sim["redo"].asInt(0) > 0)) Redo();
        ImGui::Separator();
        bool any = !selection.empty();
        if (ImGui::MenuItem(Tr("Duplicate"), "Ctrl+D", false, any)) DuplicateSelection();
        if (ImGui::MenuItem(Tr("Delete"), "Del", false, any)) DeleteSelection();
        if (ImGui::MenuItem(Tr("Rename"), "F2", false, any)) {
            renaming = Primary();
            renameBuffer = Row(renaming) ? Row(renaming)->name : "";
        }
        if (ImGui::MenuItem(Tr("Save as Prefab..."), nullptr, false, any)) {
            prefabPath = "prefabs/" + (Row(Primary()) ? Row(Primary())->name : std::string("prefab")) + ".prefab.json";
            openPrefabPrompt = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Frame Selected"), "F", false, any)) FrameSelection();
        if (ImGui::MenuItem(Tr("Select None"), "Esc", false, any)) selection.clear();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr("Create"))) {
        CreateMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr("View"))) {
        ImGui::MenuItem(Tr("Hierarchy"), nullptr, &showHierarchy);
        ImGui::MenuItem(Tr("Inspector"), nullptr, &showInspector);
        ImGui::MenuItem(Tr("Scene"), nullptr, &showScene);
        ImGui::MenuItem(Tr("Game"), nullptr, &showGame);
        ImGui::MenuItem(Tr("Assets"), nullptr, &showAssets);
        ImGui::MenuItem(Tr("Console"), nullptr, &showConsole);
        ImGui::MenuItem(Tr("Scripts"), nullptr, &showScripts);
        ImGui::MenuItem(Tr("Tiles"), nullptr, &showTiles);
        ImGui::Separator();
        ImGui::MenuItem(Tr("Grid"), "", &showGrid);
        ImGui::MenuItem(Tr("Colliders"), "", &showColliders);
        ImGui::MenuItem(Tr("Entity Icons"), "", &showIcons);
        bool mode2D = cam.mode2D;
        if (ImGui::MenuItem(Tr("2D Scene View"), "", &mode2D)) cam.Set2D(mode2D);
        ImGui::Separator();
        if (ImGui::BeginMenu(Tr("Interface Size"))) {
            for (int pct : {80, 90, 100, 110, 125, 150, 175, 200}) {
                char label[16];
                std::snprintf(label, sizeof(label), "%d%%", pct);
                if (ImGui::MenuItem(label, nullptr, std::fabs(uiScale * 100.0f - static_cast<float>(pct)) < 1.0f)) uiScale = static_cast<float>(pct) / 100.0f;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr("Language"))) {
            for (EditorLanguage l : {EditorLanguage::English, EditorLanguage::Korean, EditorLanguage::Japanese}) {
                if (ImGui::MenuItem(EditorLanguageName(l), nullptr, GetEditorLanguage() == l)) SetEditorLanguage(l);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(Tr("Reset Layout"))) resetLayout = true;
        ImGui::MenuItem(Tr("Network"), nullptr, &showNetwork);
        ImGui::MenuItem(Tr("ImGui Metrics"), nullptr, &showMetrics);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr("Play"))) {
        if (ImGui::MenuItem(Tr(session ? "Stop" : "Play"), "Ctrl+P")) TogglePlay();
        if (ImGui::MenuItem(Tr(Playing() ? "Pause" : "Resume"), nullptr, false, session)) Call(Playing() ? "sim.pause" : "sim.play", Json());
        if (ImGui::MenuItem(Tr("Step One Frame"), nullptr, false, session && !Playing())) Call("sim.step", ObjectOf({{"frames", Json(1)}}));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr("Help"))) {
        ImGui::TextDisabled("%s", Tr("Scene view"));
        ImGui::BulletText("%s", Tr("Right mouse: look around, WASD/QE fly (wheel = speed)"));
        ImGui::BulletText("%s", Tr("Middle mouse: pan    Alt + left mouse: orbit    Wheel: zoom"));
        ImGui::BulletText("%s", Tr("Q select, W move, E rotate, R scale, X local/world, F frame"));
        ImGui::BulletText("%s", Tr("Ctrl while dragging a gizmo: snap"));
        ImGui::TextDisabled("%s", Tr("Game view"));
        ImGui::BulletText("%s", Tr("Click it to give the game keyboard and mouse; Esc frees a locked mouse"));
        ImGui::TextDisabled("%s", Tr("Everywhere"));
        ImGui::BulletText("%s", Tr("Ctrl+S save, Ctrl+Z/Y undo/redo, Ctrl+D duplicate, Del delete, Ctrl+P play/stop"));
        ImGui::BulletText("%s", Tr("Drop files on the window to import them into the project"));
        ImGui::Separator();
        ImGui::TextDisabled(Tr("OwnEngine %s - agents can attach with: oe mcp --connect <port>"), OE_VERSION);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void NativeEditor::Impl::Toolbar(float barHeight) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    bool open = ImGui::BeginViewportSideBar("##Toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, barHeight, flags);
    ImGui::PopStyleVar();
    if (open && ImGui::BeginMenuBar()) {
        bool session = InPlaySession();
        if (IconButton("##select", Icon::Select, gizmoOp == GizmoOp::None, Tr("Select (Q)"))) gizmoOp = GizmoOp::None;
        ImGui::SameLine(0, 2);
        if (IconButton("##move", Icon::Move, gizmoOp == GizmoOp::Translate, Tr("Move (W)"))) gizmoOp = GizmoOp::Translate;
        ImGui::SameLine(0, 2);
        if (IconButton("##rotate", Icon::Rotate, gizmoOp == GizmoOp::Rotate, Tr("Rotate (E)"))) gizmoOp = GizmoOp::Rotate;
        ImGui::SameLine(0, 2);
        if (IconButton("##scale", Icon::Scale, gizmoOp == GizmoOp::Scale, Tr("Scale (R)"))) gizmoOp = GizmoOp::Scale;
        ImGui::SameLine();
        if (ImGui::Button(Tr(gizmoLocal ? "Local" : "World"))) gizmoLocal = !gizmoLocal;
        HelpTooltip(Tr("Gizmo orientation (X)"));
        ImGui::SameLine();
        ImGui::Checkbox(Tr("Snap"), &snap);
        HelpTooltip(Tr("Snap gizmo moves (hold Ctrl to toggle while dragging)"));
        if (snap) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
            ImGui::DragFloat("##snapMove", &snapMove, 0.05f, 0.01f, 100.0f, "%.2f m");
            HelpTooltip(Tr("Move snap"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
            ImGui::DragFloat("##snapAngle", &snapAngle, 0.5f, 1.0f, 180.0f, "%.0f deg");
            HelpTooltip(Tr("Rotate snap"));
        }

        // Play controls in the middle.
        float group = ImGui::GetFrameHeight() * 1.25f * 3 + 4;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8, (ImGui::GetWindowWidth() - group) * 0.5f));
        if (session) {
            if (IconButton("##stop", Icon::Stop, true, Tr("Stop (Ctrl+P) - restores the scene"))) TogglePlay();
        } else if (IconButton("##play", Icon::Play, false, Tr("Play (Ctrl+P)"))) {
            TogglePlay();
        }
        ImGui::SameLine(0, 2);
        if (IconButton("##pause", Icon::Pause, session && !Playing(), Tr(Playing() ? "Pause" : "Resume"), session)) Call(Playing() ? "sim.pause" : "sim.play", Json());
        ImGui::SameLine(0, 2);
        if (IconButton("##step", Icon::Step, false, Tr("Step one frame (1/60 s)"), session && !Playing())) Call("sim.step", ObjectOf({{"frames", Json(1)}}));

        if (engine.NetworkEnabled()) {
            Json network = engine.NetworkCall("state", Json::MakeObject());
            ImGui::SameLine(); ImGui::BeginDisabled(session);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            int maximum = std::min(8, network["maxPlayers"].asInt(4));
            networkPlayers = std::max(1, std::min(maximum, networkPlayers));
            std::string preview = std::string(Tr("Players")) + ": " + std::to_string(networkPlayers);
            if (ImGui::BeginCombo("##networkPlayers", preview.c_str())) {
                for (int n = 1; n <= maximum; ++n)
                    if (ImGui::Selectable(std::to_string(n).c_str(), n == networkPlayers)) networkPlayers = n;
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        }

        // Scene name + save on the right.
        std::string title = (sceneName.empty() ? std::string(Tr("(unsaved scene)")) : sceneName) + (Dirty() && !session ? " *" : "");
        float w = ImGui::CalcTextSize(title.c_str()).x + ImGui::CalcTextSize(Tr("Save")).x + ImGui::GetStyle().FramePadding.x * 2 + 24;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8, ImGui::GetWindowWidth() - w));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(title.c_str());
        HelpTooltip(scenePath);
        ImGui::SameLine();
        ImGui::BeginDisabled(session);
        if (ImGui::Button(Tr("Save"))) Save();
        ImGui::EndDisabled();
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

void NativeEditor::Impl::StatusBar(float barHeight) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    bool open = ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, barHeight, flags);
    if (open && ImGui::BeginMenuBar()) {
        bool session = InPlaySession();
        if (session) {
            ImGui::TextColored(Playing() ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", Tr(Playing() ? "PLAYING" : "PAUSED"));
            ImGui::SameLine();
        }
        ImGui::Text(Tr("frame %llu"), static_cast<unsigned long long>(sim["frame"].asNumber(0)));
        ImGui::SameLine();
        ImGui::TextDisabled(Tr("|  %d entities  |  %d fps  |  %s"), static_cast<int>(rows.size()), fps,
                            engine.Gpu() ? engine.Gpu()->Name() : "software");
        if (!options.serverInfo.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|  %s", options.serverInfo.c_str());
        }
        int scriptErrors = sim["scriptErrors"].asInt(0);
        std::string counts = (errorCount + scriptErrors > 0 ? Format(Tr("%d errors"), errorCount + scriptErrors) + "  " : std::string()) +
                             (warnCount > 0 ? Format(Tr("%d warnings"), warnCount) : std::string());
        if (!counts.empty()) {
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8, ImGui::GetWindowWidth() - ImGui::CalcTextSize(counts.c_str()).x - 16));
            ImGui::PushStyleColor(ImGuiCol_Text, errorCount + scriptErrors > 0 ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(0.95f, 0.75f, 0.3f, 1));
            if (ImGui::Selectable(counts.c_str(), false, 0, ImGui::CalcTextSize(counts.c_str()))) {
                showConsole = true;
                ImGui::SetWindowFocus("###Console");
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

void NativeEditor::Impl::DockLayout(ImGuiID dockspace) {
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
    bool empty = !node || (node->IsLeafNode() && node->Windows.Size == 0);
    if (layoutBuilt && !resetLayout) return;
    layoutBuilt = true;
    if (!empty && !resetLayout) return;  // restored from the .ini
    resetLayout = false;
    showHierarchy = showInspector = showScene = showGame = showConsole = showAssets = showScripts = showTiles = true;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, vp->WorkSize);
    ImGuiID center = dockspace;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGui::DockBuilderDockWindow("###Hierarchy", left);
    ImGui::DockBuilderDockWindow("###Tiles", right);
    ImGui::DockBuilderDockWindow("###Inspector", right);
    ImGui::DockBuilderDockWindow("###Network", right);
    ImGui::DockBuilderDockWindow("###Scene", center);
    ImGui::DockBuilderDockWindow("###Game", center);
    ImGui::DockBuilderDockWindow("###Scripts", center);
    ImGui::DockBuilderDockWindow("###Assets", bottom);
    ImGui::DockBuilderDockWindow("###Console", bottom);
    ImGui::DockBuilderFinish(dockspace);
    if (ImGuiDockNode* r = ImGui::DockBuilderGetNode(right)) r->SelectedTabId = ImHashStr("###Inspector");  // Tiles waits behind it
    focusSceneTab = 2;  // once the windows are docked (they appear this frame)
}

void NativeEditor::Impl::Modals() {
    if (openSavePrompt) {
        ImGui::OpenPopup(TrId("Save changes?").c_str());
        openSavePrompt = false;
    }
    if (openSaveAs) {
        std::string base = sceneName.empty() ? "untitled" : sceneName;
        for (char& ch : base) ch = (ch == ' ') ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        saveAsPath = "scenes/" + base + ".scene.json";
        ImGui::OpenPopup(TrId("Save Scene As").c_str());
        openSaveAs = false;
    }
    if (openPrefabPrompt) {
        ImGui::OpenPopup(TrId("Save as Prefab").c_str());
        openPrefabPrompt = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(TrId("Save changes?").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(Tr("Save the changes to \"%s\" first?"), sceneName.c_str());
        ImGui::Spacing();
        if (ImGui::Button(Tr("Save"), ImVec2(ImGui::GetFontSize() * 6, 0))) {
            Save();
            if (!Dirty()) RunAction(pending);
            pending = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Don't Save"), ImVec2(ImGui::GetFontSize() * 6, 0))) {
            RunAction(pending);
            pending = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Cancel"), ImVec2(ImGui::GetFontSize() * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pending = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(TrId("Save Scene As").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(Tr("Path in the project (*.scene.json):"));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##path", &saveAsPath, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(Tr("Save"), ImVec2(ImGui::GetFontSize() * 6, 0)) || enter) {
            if (Ok(Call("scene.save", ObjectOf({{"path", Json(saveAsPath)}})))) {
                Notify(Format(Tr("Saved %s"), saveAsPath.c_str()));
                RefreshAssets(true);
                Refresh(true);
                if (pending.kind != PendingAction::Kind::None) RunAction(pending);
                pending = {};
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Cancel"), ImVec2(ImGui::GetFontSize() * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pending = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(TrId("Save as Prefab").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(Tr("Prefab file (*.prefab.json), with the selected entity and its children:"));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##prefab", &prefabPath, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(Tr("Create"), ImVec2(ImGui::GetFontSize() * 6, 0)) || enter) {
            if (Ok(Call("prefab.create", ObjectOf({{"id", Json(Primary())}, {"path", Json(prefabPath)}})))) {
                Notify(Format(Tr("Created %s"), prefabPath.c_str()));
                RefreshAssets(true);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Cancel"), ImVec2(ImGui::GetFontSize() * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void NativeEditor::Impl::Toasts() {
    toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& t) { return t.until < time; }), toasts.end());
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - 12;
    int i = 0;
    for (auto it = toasts.rbegin(); it != toasts.rend(); ++it, ++i) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12, y), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.95f);
        char name[32];
        std::snprintf(name, sizeof(name), "##toast%d", i);
        ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoInputs;
        ImGui::PushStyleColor(ImGuiCol_Border, it->error ? ImVec4(0.9f, 0.35f, 0.3f, 1) : ImVec4(0.3f, 0.6f, 0.95f, 1));
        if (ImGui::Begin(name, nullptr, f)) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
            ImGui::TextUnformatted(it->text.c_str());
            ImGui::PopTextWrapPos();
            y -= ImGui::GetWindowHeight() + 6;
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }
}

// ----- NativeEditor ------------------------------------------------------------------

NativeEditor::NativeEditor(Engine& engine, Window* window, Options options) : impl_(std::make_unique<Impl>(engine, window, std::move(options))) {}

NativeEditor::~NativeEditor() {
    if (!impl_->ready) return;
    impl_->engine.SetRemoteCallObserver(nullptr);
    if (!impl_->options.layoutFile.empty()) ImGui::SaveIniSettingsToDisk(impl_->options.layoutFile.c_str());
    impl_->ReleaseGameInput();
    if (impl_->offAtt.id) sg_destroy_view(impl_->offAtt);
    if (impl_->offImage.id) sg_destroy_image(impl_->offImage);
    simgui_shutdown();
    if (g_settingsTarget == impl_.get()) g_settingsTarget = nullptr;
}

bool NativeEditor::Init(std::string* error) {
    Impl& m = *impl_;
    if (!m.engine.Gpu()) {
        if (error) *error = "the editor needs the GPU renderer (none is available here)";
        return false;
    }
    if (!m.options.layoutFile.empty()) CreateDirectories(ParentPath(m.options.layoutFile));
    m.forceLanguage = !m.options.language.empty();
    SetEditorLanguage(ParseEditorLanguage(m.forceLanguage ? m.options.language : PlatformUserLanguage()));
    simgui_desc_t d{};
    d.max_vertices = 1 << 19;
    d.color_format = SG_PIXELFORMAT_RGBA8;
    d.depth_format = SG_PIXELFORMAT_NONE;
    d.sample_count = 1;
    d.ini_filename = m.options.layoutFile.empty() ? nullptr : m.options.layoutFile.c_str();
    d.no_default_font = true;
    d.logger.func = SimguiLog;
    simgui_setup(&d);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDragClickToInputText = true;
    if (m.window) ImGui::GetMainViewport()->PlatformHandleRaw = m.window->NativeHandle();  // IME window placement (Win32)

    g_settingsTarget = &m;
    ImGuiSettingsHandler h;
    h.TypeName = "OwnEngine";
    h.TypeHash = ImHashStr("OwnEngine");
    h.ReadOpenFn = SettingsReadOpen;
    h.ReadLineFn = SettingsReadLine;
    h.WriteAllFn = SettingsWriteAll;
    ImGui::AddSettingsHandler(&h);

    // UI font: the engine's embedded Roboto, with system fonts merged in for
    // Korean and Japanese (the interface language's first, so shared CJK
    // characters take its shapes). Code uses Dear ImGui's own font.
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    std::snprintf(cfg.Name, sizeof(cfg.Name), "Roboto");
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kDefaultFontData), static_cast<int>(kDefaultFontSize), kBaseFontSize, &cfg);
    std::vector<const char*> korean = {"C:/Windows/Fonts/malgun.ttf", "/System/Library/Fonts/AppleSDGothicNeo.ttc",
                                       "/System/Library/Fonts/Supplemental/AppleGothic.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                       "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/nanum/NanumGothic.ttf"};
    std::vector<const char*> japanese = {"C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msgothic.ttc",
                                         "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc",
                                         "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                                         "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf", "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf"};
    std::vector<std::vector<const char*>> families = {korean, japanese};
    if (GetEditorLanguage() == EditorLanguage::Japanese) std::swap(families[0], families[1]);
    // Merges the CJK fonts into the font added just before (UI and code fonts alike).
    auto mergeCjk = [&] {
        std::string merged;
        for (const auto& family : families) {
            for (const char* path : family) {
                if (!FileExists(path)) continue;
                if (path != merged) {  // Noto CJK covers both: merge it once
                    ImFontConfig merge;
                    merge.MergeMode = true;
                    io.Fonts->AddFontFromFileTTF(path, kBaseFontSize, &merge);
                    merged = path;
                }
                break;
            }
        }
    };
    mergeCjk();
    m.monoFont = io.Fonts->AddFontDefaultVector();
    mergeCjk();  // Korean/Japanese in scripts and the code editor

    m.engine.SetRemoteCallObserver([&m](const std::string& name, const Json& args, const Json& result) { m.OnRemoteCall(name, args, result); });
    m.componentTypes = m.Call("component.types", Json(), true)["result"];
    for (const Json& t : m.componentTypes.items()) m.typeByName[t["name"].asString("")] = &t;
    for (const Json& c : m.Call("api.list", Json(), true)["result"].items()) m.commandNames.push_back(c["name"].asString(""));
    m.ready = true;
    m.Refresh(true);
    m.RefreshAssets(true);
    return true;
}

void NativeEditor::Update(const std::vector<WindowEvent>& events, int width, int height, float dpiScale, double dt) {
    Impl& m = *impl_;
    m.width = std::max(1, width);
    m.height = std::max(1, height);
    m.dpiScale = dpiScale > 0 ? dpiScale : 1.0f;
    m.time += dt;
    if (std::fabs(m.appliedScale - m.dpiScale * m.uiScale) > 1e-3f) m.ApplyStyle();
    m.ApplyEvents(events);

    simgui_frame_desc_t fd{};
    fd.width = m.width;
    fd.height = m.height;
    fd.delta_time = dt > 0 ? dt : 1.0 / 60.0;
    fd.dpi_scale = 1.0f;
    simgui_new_frame(&fd);
    ImGuizmo::BeginFrame();

    m.GameInput();
    m.engine.Tick(dt);
    m.Refresh(false);
    m.PollLog();
    m.RefreshAssets(false);

    float bar = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 0.5f;
    m.MainMenu();
    m.Toolbar(bar);
    m.StatusBar(ImGui::GetFrameHeight());
    ImGuiID dockspace = ImGui::GetID("OwnEngineDockSpace2");  // 2: windows are named "Label###Id" since translations
    m.DockLayout(dockspace);
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());

    m.Shortcuts();
    if (m.showScene) m.ScenePanel();
    if (m.showGame) m.GamePanel();
    if (m.showHierarchy) m.HierarchyPanel();
    if (m.showInspector) m.InspectorPanel();
    if (m.showAssets) m.AssetsPanel();
    if (m.showConsole) m.ConsolePanel();
    if (m.showScripts) m.ScriptsPanel();
    if (m.showTiles) m.TilesPanel();
    if (m.showNetwork) m.NetworkPanel();
    if (m.showMetrics) ImGui::ShowMetricsWindow(&m.showMetrics);
    m.Modals();
    m.Toasts();
    if (!m.droppedFiles.empty()) m.ImportDroppedFiles();
    m.UpdateCursor();

    ++m.fpsFrames;
    if (m.time - m.fpsTimer >= 1.0) {
        m.fps = m.fpsFrames;
        m.fpsFrames = 0;
        m.fpsTimer = m.time;
    }
}

bool NativeEditor::DrawToWindow() {
    Impl& m = *impl_;
    GpuDevice& device = m.engine.Gpu()->Device();
    sg_swapchain sc = device.Swapchain();
    if (sc.invalid) {
        ImGui::Render();  // minimized: finish the frame without drawing it
        return false;
    }
    sg_pass pass{};
    pass.swapchain = sc;
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = {0.08f, 0.085f, 0.1f, 1.0f};
    pass.label = "oe-editor";
    sg_begin_pass(&pass);
    simgui_render();
    sg_end_pass();
    sg_commit();
    device.Present();
    return true;
}

bool NativeEditor::DrawToImage(RenderTarget& out) {
    Impl& m = *impl_;
    if (m.offW != m.width || m.offH != m.height) {
        if (m.offAtt.id) sg_destroy_view(m.offAtt);
        if (m.offImage.id) sg_destroy_image(m.offImage);
        sg_image_desc d{};
        d.width = m.width;
        d.height = m.height;
        d.pixel_format = SG_PIXELFORMAT_RGBA8;
        d.sample_count = 1;
        d.usage.color_attachment = true;
        d.label = "oe-editor-offscreen";
        m.offImage = sg_make_image(&d);
        sg_view_desc vd{};
        vd.color_attachment.image = m.offImage;
        m.offAtt = sg_make_view(&vd);
        m.offW = m.width;
        m.offH = m.height;
    }
    sg_pass pass{};
    pass.attachments.colors[0] = m.offAtt;
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = {0.08f, 0.085f, 0.1f, 1.0f};
    pass.label = "oe-editor-offscreen";
    sg_begin_pass(&pass);
    simgui_render();
    sg_end_pass();
    sg_commit();
    out.Resize(m.width, m.height);
    return m.engine.Gpu()->Device().ReadPixels(m.offImage, m.width, m.height, out.color.data());
}

bool NativeEditor::QuitRequested() const { return impl_->quit; }
InputState& NativeEditor::WindowInput() { return impl_->windowInput; }

void NativeEditor::Select(EntityId id) {
    impl_->SelectOnly(id);
    impl_->Refresh(true);
}

EntityId NativeEditor::Selected() const { return impl_->Primary(); }

void NativeEditor::SetTileBrush(bool paint, char brush) {
    impl_->tilePaint = paint;
    impl_->tileBrush = brush;
    if (paint) {
        impl_->showTiles = true;
        impl_->cam.Set2D(true);
        impl_->FrameSelection();
    }
}

std::array<float, 4> NativeEditor::SceneViewRect() const {
    return {impl_->sceneImagePos.x, impl_->sceneImagePos.y, impl_->sceneImageSize.x, impl_->sceneImageSize.y};
}

void NativeEditor::FocusGameView(bool focus) {
    Impl& m = *impl_;
    if (focus) {
        m.gameFocused = true;
        m.requestGameFocus = true;
    } else {
        m.ReleaseGameInput();
        m.gameFocused = false;
    }
}

void NativeEditor::FocusNetworkPanel() { impl_->showNetwork = impl_->requestNetworkFocus = true; }
bool NativeEditor::SetGamePeer(size_t peer) {
    if (peer > 7 || !impl_->engine.LocalPeer(peer)) return false;
    if (impl_->gamePeer == static_cast<int>(peer)) return true;
    impl_->ReleaseGameInput(); impl_->gamePeer = static_cast<int>(peer); impl_->gameFocused = false; return true;
}
bool NativeEditor::SetNetworkPlayers(int players) {
    Json state = impl_->engine.NetworkCall("state", Json::MakeObject());
    if (impl_->InPlaySession() || players < 1 || players > 8 ||
        (players > 1 && (state["mode"].asString() == "none" || players > state["maxPlayers"].asInt()))) return false;
    impl_->networkPlayers = players; return true;
}
bool NativeEditor::GameViewFocused() const { return impl_->gameFocused; }

void NativeEditor::OpenScript(const std::string& path) { impl_->OpenScript(path); }

std::vector<std::string> NativeEditor::ScriptProblems(const std::string& path) const {
    std::vector<std::string> out;
    for (const ScriptTab& t : impl_->scripts) {
        if (t.path != path) continue;
        for (const ScriptDiagnostic& d : t.diagnostics) out.push_back(Format("%s %d: %s", d.error ? "error" : "warning", d.line, d.message.c_str()));
        for (const ScriptDiagnostic& d : t.runtime) out.push_back(Format("runtime %d: %s", d.line, d.message.c_str()));
    }
    return out;
}
std::string NativeEditor::LastNotice() const { return impl_->lastNotice; }

int RunNativeEditor(Engine& engine, Window& window, const NativeEditor::Options& options) {
    NativeEditor editor(engine, &window, options);
    std::string err;
    if (!editor.Init(&err)) {
        OE_LOG_ERROR("editor", "%s", err.c_str());
        return 1;
    }
    window.SetEventMode(true);
    double last = PlatformTimeSeconds();
    while (!editor.QuitRequested()) {
        engine.RunPostedJobs();  // HTTP / MCP calls from agents
        window.PumpEvents(editor.WindowInput());
        double now = PlatformTimeSeconds();
        double dt = std::min(0.25, now - last);
        last = now;
        editor.Update(window.TakeEvents(), window.Width(), window.Height(), window.DpiScale(), dt);
        if (!editor.DrawToWindow()) PlatformSleep(0.016);  // minimized
    }
    window.SetEventMode(false);
    return 0;
}

}  // namespace oe
