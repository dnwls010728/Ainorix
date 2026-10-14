// Native editor views: the Scene view (free camera, picking, gizmo, entity
// icons, asset drops) and the Game view (game camera, input forwarding).
// Both are GpuRenderer::RenderToTexture images drawn as ImGui images.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>

#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"
#include "ImGuizmo.h"
#include "imgui_internal.h"
#include "sokol_imgui.h"

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/Log.h"
#include "nav/Navigation.h"
#include "physics/PhysicsWorld.h"
#include "platform/GamepadInput.h"
#include "render/GpuRenderer.h"
#include "render/RenderScene.h"
#include "render/UI.h"
#include "scene/Components.h"

namespace oe {

namespace {

constexpr int kSceneSlot = 0;
constexpr int kGameSlot = 1;

bool EndsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i]) return false;
    }
    return true;
}

std::string Stem(const std::string& path) {
    std::string name = path.substr(path.find_last_of('/') + 1);
    return name.substr(0, name.find('.'));
}

// Projects a world point into a view rectangle; false when behind the camera.
bool Project(const Mat4& viewProj, const Vec3& p, ImVec2 pos, ImVec2 size, ImVec2& out) {
    Vec4 c = viewProj * Vec4(p, 1.0f);
    if (c.w <= 1e-4f) return false;
    float x = c.x / c.w, y = c.y / c.w;
    out = ImVec2(pos.x + (x * 0.5f + 0.5f) * size.x, pos.y + (0.5f - y * 0.5f) * size.y);
    return true;
}

// Small overlay button row drawn on top of a view image: flat buttons, the ones that are on
// lifted and bright, the others dimmed (OverlayBar draws the floating bar behind them).
bool OverlayToggle(const char* label, bool* value, const char* tip) {
    ImGui::PushStyleColor(ImGuiCol_Button, *value ? ImVec4(1.0f, 1.0f, 1.0f, 0.14f) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, *value ? 0.20f : 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.24f));
    ImGui::PushStyleColor(ImGuiCol_Text, *value ? ImVec4(0.96f, 0.97f, 1.0f, 1.0f) : ImVec4(0.70f, 0.72f, 0.76f, 1.0f));
    bool pressed = ImGui::SmallButton(label);
    ImGui::PopStyleColor(4);
    if (pressed) *value = !*value;
    HelpTooltip(tip);
    return pressed;
}

// A rounded, translucent bar behind the overlay buttons drawn between Begin and End.
struct OverlayBar {
    ImDrawList* draw;
    ImVec2 start;
    explicit OverlayBar(ImVec2 at) : draw(ImGui::GetWindowDrawList()), start(at) {
        ImGui::SetCursorScreenPos(at + ImVec2(4, 4));
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
    }
    ~OverlayBar() {
        const ImVec2 end = ImGui::GetItemRectMax() + ImVec2(4, 4);
        draw->ChannelsSetCurrent(0);
        draw->AddRectFilled(start, end, IM_COL32(20, 20, 23, 215), 7.0f);
        draw->AddRect(start, end, IM_COL32(255, 255, 255, 18), 7.0f);
        draw->ChannelsMerge();
    }
};

// Text over scene geometry needs its own background regardless of the game's colors.
void ViewportLabel(ImDrawList* draw, ImVec2 at, const char* text, ImU32 color) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    draw->AddRectFilled(at - ImVec2(4, 3), at + size + ImVec2(4, 3), IM_COL32(18, 21, 27, 240), 3.0f);
    draw->AddText(at, color, text);
}

// Picking and asset drops must use the same projection as the scene image.
Json SceneCameraRequest(const EditorCamera& camera) {
    Json request = ObjectOf({{"eye", Vec3Json(camera.Eye())}, {"target", Vec3Json(camera.target)}, {"fov", Json(camera.fov)}});
    if (camera.mode2D) {
        request["projection"] = "orthographic";
        request["orthoSize"] = camera.OrthoHalfHeight();
    }
    return request;
}

// The view compass uses the same basis as the free camera, independent of scene entities.
bool SceneCompass(EditorCamera& cam, ImVec2 origin, float size) {
    const ImVec2 saved = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##scenecompass", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 center(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float radius = size * 0.115f, arm = size * 0.32f;
    struct Endpoint { int axis; int sign; ImVec2 point; float depth; };
    std::array<Endpoint, 6> ends;
    const Vec3 right = cam.Right(), up = cam.Up(), forward = cam.Forward();
    for (int i = 0; i < 6; ++i) {
        const int axis = i / 2, sign = i % 2 == 0 ? 1 : -1;
        const Vec3 dir = (axis == 0 ? Vec3(1, 0, 0) : axis == 1 ? Vec3(0, 1, 0) : Vec3(0, 0, 1)) * static_cast<float>(sign);
        ends[static_cast<size_t>(i)] = {axis, sign,
            ImVec2(center.x + Dot(dir, right) * arm, center.y - Dot(dir, up) * arm), -Dot(dir, forward)};
    }
    std::stable_sort(ends.begin(), ends.end(), [](const Endpoint& a, const Endpoint& b) { return a.depth < b.depth; });
    int hot = -1;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (int i = 0; i < 6; ++i) {
        const ImVec2 d(mouse.x - ends[static_cast<size_t>(i)].point.x, mouse.y - ends[static_cast<size_t>(i)].point.y);
        if (hovered && d.x * d.x + d.y * d.y <= radius * radius) hot = i;
    }
    dl->AddCircleFilled(center, size * 0.48f, IM_COL32(18, 21, 27, hovered ? 190 : 130));
    const ImU32 colors[] = {IM_COL32(246, 74, 92, 255), IM_COL32(145, 213, 54, 255), IM_COL32(65, 145, 255, 255)};
    const char* labels[] = {"X", "Y", "Z"};
    for (int i = 0; i < 6; ++i) {
        const Endpoint& end = ends[static_cast<size_t>(i)];
        const ImU32 color = colors[end.axis];
        if (end.sign > 0) dl->AddLine(center, end.point, color, 2.5f);
        dl->AddCircleFilled(end.point, radius, end.sign > 0 ? color : IM_COL32(36, 40, 48, 255));
        dl->AddCircle(end.point, radius, hot == i ? IM_COL32(255, 255, 255, 255) : color, 0, hot == i ? 2.0f : 1.0f);
        if (end.sign > 0) {
            const ImVec2 text = ImGui::CalcTextSize(labels[end.axis]);
            dl->AddText(ImVec2(end.point.x - text.x * 0.5f, end.point.y - text.y * 0.5f), IM_COL32(15, 20, 26, 255), labels[end.axis]);
        }
    }
    if (hovered) HelpTooltip(Tr("Click an axis to view the scene from that direction"));
    if (clicked && hot >= 0) {
        const Endpoint& end = ends[static_cast<size_t>(hot)];
        cam.Set2D(false);
        cam.pitch = 0;
        if (end.axis == 0) cam.yaw = end.sign > 0 ? 90.0f : -90.0f;
        else if (end.axis == 2) cam.yaw = end.sign > 0 ? 0.0f : 180.0f;
        else {
            // Keep the existing camera's pole limit, so the world-up view basis stays well defined.
            cam.yaw = 0;
            cam.pitch = end.sign > 0 ? -89.0f : 89.0f;
        }
    }
    ImGui::SetCursorScreenPos(saved);
    return hovered;
}

}  // namespace

ImTextureID NativeEditor::Impl::ViewTexture(sg_view view) const { return static_cast<ImTextureID>(simgui_imtextureid(view)); }

// GL render targets are stored bottom row first; sample them upside down.
bool NativeEditor::Impl::FlipViews() const { return !sg_query_features().origin_top_left; }

// ----- Scene view ------------------------------------------------------------------

void NativeEditor::Impl::SceneCameraInput(ImGuiIO& io) {
    bool active = sceneButtonActive;
    float h = std::max(1.0f, sceneImageSize.y);
    sceneLooking = false;
    if (active && io.MouseDown[ImGuiMouseButton_Right] && !TilePainting()) {  // painting: right button erases
        if (cam.mode2D) {
            cam.Pan(io.MouseDelta.x, io.MouseDelta.y, h);
        } else {
            sceneLooking = true;
            cam.Look(-io.MouseDelta.x * 0.25f, -io.MouseDelta.y * 0.25f);
            Vec3 move(0, 0, 0);
            if (ImGui::IsKeyDown(ImGuiKey_W)) move.z += 1;
            if (ImGui::IsKeyDown(ImGuiKey_S)) move.z -= 1;
            if (ImGui::IsKeyDown(ImGuiKey_D)) move.x += 1;
            if (ImGui::IsKeyDown(ImGuiKey_A)) move.x -= 1;
            if (ImGui::IsKeyDown(ImGuiKey_E)) move.y += 1;
            if (ImGui::IsKeyDown(ImGuiKey_Q)) move.y -= 1;
            float speed = flySpeed * (io.KeyShift ? 3.0f : 1.0f) * io.DeltaTime;
            cam.Fly(move * speed);
            if (io.MouseWheel != 0) flySpeed = Clamp(flySpeed * std::pow(1.2f, io.MouseWheel), 0.5f, 200.0f);
        }
    } else if (active && io.MouseDown[ImGuiMouseButton_Middle]) {
        cam.Pan(io.MouseDelta.x, io.MouseDelta.y, h);
    } else if (active && io.MouseDown[ImGuiMouseButton_Left] && io.KeyAlt) {
        cam.Orbit(-io.MouseDelta.x * 0.3f, -io.MouseDelta.y * 0.3f);
    } else if (sceneHovered && io.MouseWheel != 0) {
        cam.Zoom(io.MouseWheel);
    }
}

void NativeEditor::Impl::SceneOverlays(ImDrawList* dl) {
    if (!showIcons) return;
    Scene& scene = engine.GetScene();
    Mat4 vp = sceneProjMat * sceneViewMat;
    ImVec2 mouse = ImGui::GetIO().MousePos;
    float r = ImGui::GetFontSize() * 0.55f;
    EntityId hovered = kNullEntity;
    // Entities without visible geometry get a clickable marker.
    for (const EntityRow& row : rows) {
        const char* letter = nullptr;
        ImU32 col = 0;
        if (row.Has("Camera")) letter = "C", col = IM_COL32(120, 180, 255, 230);
        else if (row.Has("DirectionalLight")) letter = "D", col = IM_COL32(255, 210, 100, 230);
        else if (row.Has("SpotLight") && !row.Has("MeshRenderer")) letter = "S", col = IM_COL32(255, 190, 90, 230);
        else if (row.Has("PointLight") && !row.Has("MeshRenderer")) letter = "P", col = IM_COL32(255, 190, 90, 230);
        else if (row.Has("ReflectionProbe") && !row.Has("MeshRenderer")) letter = "R", col = IM_COL32(140, 190, 255, 230);
        else if (row.Has("Decal") && !row.Has("MeshRenderer")) letter = "D", col = IM_COL32(220, 155, 110, 230);
        else if (row.Has("AudioSource") && !row.Has("MeshRenderer")) letter = "S", col = IM_COL32(140, 220, 255, 230);
        if (!letter || !scene.Exists(row.id)) continue;
        ImVec2 s;
        if (!Project(vp, scene.WorldMatrix(row.id).TransformPoint(Vec3(0, 0, 0)), sceneImagePos, sceneImageSize, s)) continue;
        if (s.x < sceneImagePos.x || s.y < sceneImagePos.y || s.x > sceneImagePos.x + sceneImageSize.x || s.y > sceneImagePos.y + sceneImageSize.y) continue;
        bool sel = IsSelected(row.id);
        bool hover = (mouse.x - s.x) * (mouse.x - s.x) + (mouse.y - s.y) * (mouse.y - s.y) <= r * r * 1.4f;
        if (hover) hovered = row.id;
        dl->AddCircleFilled(s, r, sel ? IM_COL32(255, 158, 26, 240) : IM_COL32(20, 22, 28, 200));
        dl->AddCircle(s, r, col, 0, hover ? 2.5f : 1.5f);
        ImVec2 ts = ImGui::CalcTextSize(letter);
        dl->AddText(ImVec2(s.x - ts.x * 0.5f, s.y - ts.y * 0.5f), sel ? IM_COL32(20, 20, 20, 255) : col, letter);
        if (hover) ViewportLabel(dl, ImVec2(s.x + r + 4, s.y - ts.y * 0.5f), row.name.c_str(), IM_COL32(238, 240, 245, 255));
    }
    markerHover = hovered;
}

void NativeEditor::Impl::SceneGizmo() {
    gizmoUsing = false;
    EntityId id = Primary();
    if (gizmoOp == GizmoOp::None || id == kNullEntity || !selected["components"].has("Transform") || !engine.GetScene().Exists(id)) {
        gizmoWasUsing = false;
        return;
    }
    Scene& scene = engine.GetScene();
    ImGuizmo::SetOrthographic(cam.mode2D);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(sceneImagePos.x, sceneImagePos.y, sceneImageSize.x, sceneImageSize.y);
    ImGuizmo::OPERATION op = gizmoOp == GizmoOp::Translate ? ImGuizmo::TRANSLATE : gizmoOp == GizmoOp::Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
    if (cam.mode2D) {  // 2D: move and scale in the XY plane, rotate around Z
        op = gizmoOp == GizmoOp::Translate ? ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y
             : gizmoOp == GizmoOp::Rotate ? ImGuizmo::ROTATE_Z
                                          : ImGuizmo::SCALE_X | ImGuizmo::SCALE_Y;
    }
    ImGuizmo::MODE mode = (gizmoLocal || gizmoOp == GizmoOp::Scale) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    bool useSnap = snap != ImGui::GetIO().KeyCtrl;
    float step = gizmoOp == GizmoOp::Translate ? snapMove : gizmoOp == GizmoOp::Rotate ? snapAngle : snapScale;
    float snapValues[3] = {step, step, step};
    Mat4 world = scene.WorldMatrix(id);
    float m[16];
    std::memcpy(m, world.m, sizeof(m));
    bool changed = ImGuizmo::Manipulate(sceneViewMat.m, sceneProjMat.m, op, mode, m, nullptr, useSnap ? snapValues : nullptr);
    bool using_ = ImGuizmo::IsUsing();
    if (using_ && !gizmoWasUsing) ++gizmoSerial;  // one undo step per drag
    gizmoWasUsing = using_;
    gizmoUsing = using_ || ImGuizmo::IsOver(op);
    if (!changed) return;

    Mat4 newWorld;
    std::memcpy(newWorld.m, m, sizeof(m));
    const EntityRow* row = Row(id);
    Mat4 parentWorld = (row && row->parent != kNullEntity && scene.Exists(row->parent)) ? scene.WorldMatrix(row->parent) : Mat4::Identity();
    Mat4 local = parentWorld.Inverse() * newWorld;
    Vec3 p, r, s;
    DecomposeTRS(local, p, r, s);
    const Json& t = selected["components"]["Transform"];
    Json values = Json::MakeObject();
    if (gizmoOp == GizmoOp::Translate) {
        values["position"] = Vec3Json(p);
    } else if (gizmoOp == GizmoOp::Rotate) {
        Vec3 cur = JsonVec3(t["rotation"]);
        values["rotation"] = Vec3Json(Vec3(NearestAngle(r.x, cur.x), NearestAngle(r.y, cur.y), NearestAngle(r.z, cur.z)));
    } else {
        Vec3 cur = JsonVec3(t["scale"], Vec3(1, 1, 1));
        // Keep mirrored axes mirrored (the decomposition returns positive scale).
        values["scale"] = Vec3Json(Vec3(cur.x < 0 ? -s.x : s.x, cur.y < 0 ? -s.y : s.y, cur.z < 0 ? -s.z : s.z));
    }
    Call("component.set", ObjectOf({{"id", Json(id)}, {"type", Json("Transform")}, {"values", values}, {"merge", Json("gizmo:" + std::to_string(gizmoSerial))}}));
    // Keep the reference rotation current within one drag.
    Refresh(true);
}

EntityId NativeEditor::Impl::ScenePickId(float x, float y, Vec3* point, Vec3* normal) {
    Json camera = SceneCameraRequest(cam);
    Json r = Call("render.pick", ObjectOf({{"x", Json(static_cast<int>(x))},
                                           {"y", Json(static_cast<int>(y))},
                                           {"width", Json(static_cast<int>(sceneImageSize.x))},
                                           {"height", Json(static_cast<int>(sceneImageSize.y))},
                                           {"camera", camera}}),
                  true);
    const Json& hit = r["result"];
    if (point && hit["point"].isArray()) *point = JsonVec3(hit["point"]);
    if (normal && hit["normal"].isArray()) *normal = JsonVec3(hit["normal"]);
    return hit["point"].isArray() || !point ? static_cast<EntityId>(hit["id"].asNumber(0)) : kNullEntity;
}

void NativeEditor::Impl::ScenePick(float x, float y, bool additive) {
    EntityId id = ScenePickId(x, y);
    if (additive) {
        if (id != kNullEntity) ToggleSelect(id);
    } else {
        SelectOnly(id);
    }
    if (id != kNullEntity) scrollToRow = id;
    Refresh(true);
}

void NativeEditor::Impl::SceneDrop(const std::string& path, float x, float y) {
    Vec3 origin, dir, hit = cam.target;
    ScreenRay(sceneViewMat, sceneProjMat, x, y, sceneImageSize.x, sceneImageSize.y, origin, dir);
    Vec3 planeNormal = cam.mode2D ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    RayPlane(origin, dir, Vec3(0, 0, 0), planeNormal, hit);
    auto round = [](float v) { return std::round(v * 100.0f) / 100.0f; };
    hit = Vec3(round(hit.x), round(hit.y), round(hit.z));
    // Entity under the cursor, for assets that modify one.
    auto target = [&]() -> EntityId {
        Json camera = SceneCameraRequest(cam);
        Json r = Call("render.pick", ObjectOf({{"x", Json(static_cast<int>(x))}, {"y", Json(static_cast<int>(y))},
                                               {"width", Json(static_cast<int>(sceneImageSize.x))}, {"height", Json(static_cast<int>(sceneImageSize.y))},
                                               {"camera", camera}}),
                      true);
        return static_cast<EntityId>(r["result"]["id"].asNumber(0));
    };
    Json r;
    if (AssetManager::KindOf(path) == "model") {
        r = Call("entity.create", ObjectOf({{"name", Json(UniqueName(Stem(path)))},
                                            {"components", ObjectOf({{"Transform", ObjectOf({{"position", Vec3Json(hit)}})},
                                                                     {"MeshRenderer", ObjectOf({{"mesh", Json(path)}})}})}}));
    } else if (EndsWith(path, ".prefab.json")) {
        r = Call("prefab.instantiate", ObjectOf({{"path", Json(path)}, {"position", Vec3Json(hit)}}));
    } else if (EndsWith(path, ".vfx.json")) {
        r = Call("vfx.spawn", ObjectOf({{"path", path}, {"position", Vec3Json(hit)}, {"autoDestroy", false}}));
    } else if (EndsWith(path, ".scene.json")) {
        RequestAction({PendingAction::Kind::LoadScene, path});
        return;
    } else if (EndsWith(path, ".png") || EndsWith(path, ".jpg") || EndsWith(path, ".jpeg") || EndsWith(path, ".bmp") || EndsWith(path, ".tga")) {
        EntityId id = target();
        const EntityRow* row = Row(id);
        if (row && (row->Has("MeshRenderer") || row->Has("Sprite"))) {
            Call("component.set", ObjectOf({{"id", Json(id)}, {"type", Json(row->Has("Sprite") ? "Sprite" : "MeshRenderer")}, {"values", ObjectOf({{"texture", Json(path)}})}}));
            SelectOnly(id);
        } else {
            r = Call("entity.create", ObjectOf({{"name", Json(UniqueName(Stem(path)))},
                                                {"components", ObjectOf({{"Transform", ObjectOf({{"position", Vec3Json(hit)}})},
                                                                         {"Sprite", ObjectOf({{"texture", Json(path)}})}})}}));
        }
    } else if (EndsWith(path, ".mat.json")) {
        EntityId id = target();
        if (Row(id) && Row(id)->Has("MeshRenderer")) {
            Call("component.set", ObjectOf({{"id", Json(id)}, {"type", Json("MeshRenderer")}, {"values", ObjectOf({{"material", Json(path)}})}}));
            SelectOnly(id);
        } else {
            Notify(Tr("Drop a material on an entity with a MeshRenderer"), true);
        }
    } else if (EndsWith(path, ".lua")) {
        EntityId id = target();
        if (id == kNullEntity) id = Primary();
        if (id != kNullEntity) {
            Call("component.add", ObjectOf({{"id", Json(id)}, {"type", Json("Script")}, {"values", ObjectOf({{"path", Json(path)}})}}));
            SelectOnly(id);
        } else {
            Notify(Tr("Drop a script on an entity"), true);
        }
    } else if (EndsWith(path, ".wav")) {
        r = Call("entity.create", ObjectOf({{"name", Json(UniqueName(Stem(path)))},
                                            {"components", ObjectOf({{"Transform", ObjectOf({{"position", Vec3Json(hit)}})},
                                                                     {"AudioSource", ObjectOf({{"clip", Json(path)}})}})}}));
    } else {
        Notify(Format(Tr("Nothing to place for %s"), path.c_str()), true);
        return;
    }
    if (r.isObject() && Ok(r)) {
        Refresh(true);
        SelectOnly(static_cast<EntityId>(r["result"]["id"].asNumber(0)));
    }
    Refresh(true);
}

void NativeEditor::Impl::ScenePanel() {
    if (focusSceneTab > 0 && --focusSceneTab == 0) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin(WithIcon(kIconBox, TrId("Scene")).c_str(), &showScene, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    sceneFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    sceneHovered = false;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!open || avail.x < 8 || avail.y < 8 || !engine.Gpu()) {
        sceneLooking = false;
        ImGui::End();
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    Scene& scene = engine.GetScene();
    int w = static_cast<int>(avail.x), h = static_cast<int>(avail.y);
    float aspect = static_cast<float>(w) / static_cast<float>(h);
    // First time a project's Scene view is shown without a saved camera: if the default viewpoint
    // shows no geometry at all (a level built away from the origin), fit the scene so it never opens
    // onto an empty void. Scenes whose content already sits in the default view keep it (a dark
    // night scene is not empty, just unlit); a saved camera clears the flag before this runs.
    if (autoFramePending) {
        autoFramePending = false;
        // A pixel-perfect (2D) game camera: open in the 2D view, fitted to the scene.
        bool pixelGame = false;
        for (const auto& kv : scene.Pool<Camera>()) pixelGame = pixelGame || (kv.second.active && kv.second.pixelPerfect && kv.second.projection == "orthographic");
        if (pixelGame) {
            cam.Set2D(true);
            FrameScene();
        } else if (!scene.Entities().empty() && !DefaultViewShowsGeometry(aspect)) {
            FrameScene();
        }
    }
    RenderView sceneCam;
    MakeSceneView(scene, aspect, sceneCam);
    RenderView view = MakeLookAtView(cam.Eye(), cam.target, cam.fov, aspect);
    view.proj = cam.Projection(aspect);
    view.clearColor = sceneCam.clearColor;
    view.shaderTime = static_cast<float>(engine.SimTime());
    view.playing = engine.InPlaySession();
    view.drawGrid = showGrid && !cam.mode2D;
    view.editorFill = (showEditorLight && !InPlaySession()) ? 0.32f : 0.0f;  // editing only; the Game view and play keep the real lighting
    view.highlight = Primary();
    // The game's UI over the world, laid out for this view's size, to be edited where it is seen.
    // While the game plays its scripts own the UI: the Game view shows it.
    const bool sceneUI = showSceneUI && !InPlaySession() && !TilePainting();
    view.drawUI = sceneUI;
    sceneUIShown = sceneUI;
    if (showGrid && cam.mode2D && cam.Eye().z > 1e-3f) {
        // 2D: a grid in the XY plane that follows the camera and coarsens with zoom.
        // Orthographic scale does not change with the grid's distance from the camera.
        const float halfH = cam.OrthoHalfHeight(), halfW = halfH * aspect;
        // Like Unity's scene grid: decade levels (.., 0.1, 1, 10, ..) that fade in as their
        // cells grow on screen, so zooming never pops from one spacing to the next.
        const float unitsPerPixel = 2.0f * halfH / static_cast<float>(h);
        const float step = Grid2DStep(unitsPerPixel, kGrid2DMinPixels);
        // Lines carry no alpha: blend towards a tone that contrasts with the background.
        const Color base = view.clearColor;
        const bool bright = base.r * 0.3f + base.g * 0.59f + base.b * 0.11f > 0.45f;
        const Color gridColor = bright ? Color(0.08f, 0.1f, 0.14f) : Color(0.75f, 0.78f, 0.82f);
        auto mix = [&](const Color& c, float a) { return base * (1.0f - a) + c * a; };
        const float x0 = cam.target.x - halfW, x1 = cam.target.x + halfW, y0 = cam.target.y - halfH, y1 = cam.target.y + halfH;
        // A line belongs to the coarsest level it lies on; weaker levels are drawn first.
        auto lines = [&](bool vertical, int level) {
            const float lo = vertical ? x0 : y0, hi = vertical ? x1 : y1;
            const float pixels = step * (level == 0 ? 1.0f : level == 1 ? 10.0f : 100.0f) / unitsPerPixel;
            const Color c = mix(gridColor, 0.42f * Grid2DAlpha(pixels));
            for (long long i = static_cast<long long>(std::ceil(lo / step)); static_cast<float>(i) * step <= hi; ++i) {
                if (i == 0 || (i % 100 == 0 ? 2 : i % 10 == 0 ? 1 : 0) != level) continue;
                const float f = static_cast<float>(i) * step;
                view.lines.push_back(vertical ? DebugLine{Vec3(f, y0, 0), Vec3(f, y1, 0), c} : DebugLine{Vec3(x0, f, 0), Vec3(x1, f, 0), c});
            }
        };
        for (int level = 0; level < 3; ++level) lines(true, level), lines(false, level);
        if (y0 <= 0 && y1 >= 0) view.lines.push_back({Vec3(x0, 0, 0), Vec3(x1, 0, 0), mix(Color(0.9f, 0.25f, 0.25f), 0.8f)});  // X axis
        if (x0 <= 0 && x1 >= 0) view.lines.push_back({Vec3(0, y0, 0), Vec3(0, y1, 0), mix(Color(0.35f, 0.8f, 0.3f), 0.8f)});   // Y axis
    }
    if (showColliders) {
        TilesetLookup tilesets = engine.Assets().Tilesets();
        AppendColliderLines(scene, view.lines, &tilesets);
    }
    if (showNavMesh) engine.Nav().AppendLines(view.lines);  // baked again when an edit changed the ground
    // Light ranges are in world units, independent of Transform.scale, as in GatherLights.
    auto ring = [&](const Vec3& center, const Vec3& right, const Vec3& up, float radius, Color color) {
        if (!std::isfinite(radius) || radius <= 0) return;
        Vec3 previous = center + right * radius;
        for (int i = 1; i <= 48; ++i) {
            const float angle = static_cast<float>(i) / 48.0f * 2 * kPi;
            const Vec3 point = center + (right * std::cos(angle) + up * std::sin(angle)) * radius;
            view.lines.push_back({previous, point, color});
            previous = point;
        }
    };
    const EntityId lightId = Primary();
    const Mat4 lightWorld = scene.Exists(lightId) ? scene.WorldMatrix(lightId) : Mat4::Identity();
    const Vec3 lightCenter = lightWorld.TransformPoint(Vec3());
    if (const PointLight* light = scene.Get<PointLight>(lightId)) {
        const Color color = light->intensity > 0 ? Color(1, 0.7f, 0.2f) : Color(0.45f, 0.45f, 0.45f);
        const float radius = std::max(0.01f, light->range);
        ring(lightCenter, Vec3(1, 0, 0), Vec3(0, 1, 0), radius, color);
        ring(lightCenter, Vec3(1, 0, 0), Vec3(0, 0, 1), radius, color);
        ring(lightCenter, Vec3(0, 1, 0), Vec3(0, 0, 1), radius, color);
    }
    if (scene.Get<SpotLight>(lightId)) {
        const RenderSpotLight light = MakeRenderSpotLight(scene, lightId);
        const Vec3 right = Normalize(Cross(light.dir, std::fabs(light.dir.y) > 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0)));
        const Vec3 up = Cross(right, light.dir);
        const Color color = light.color.r + light.color.g + light.color.b > 0 ? Color(1, 0.7f, 0.2f) : Color(0.45f, 0.45f, 0.45f);
        // A radial range ends on a sphere: the cone rim is at range*cos(angle), radius range*sin(angle).
        auto cone = [&](float cosine, Color tint) {
            const Vec3 center = light.pos + light.dir * (light.range * cosine);
            const float radius = light.range * std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
            ring(center, right, up, radius, tint);
            for (const Vec3& axis : {right, -right, up, -up})
                view.lines.push_back({light.pos, center + axis * radius, tint});
        };
        cone(light.outerCos, color);
        cone(light.innerCos, color * 0.65f);
        view.lines.push_back({light.pos, light.pos + light.dir * light.range, color});
    }
    if (const ReflectionProbe* probe = scene.Get<ReflectionProbe>(lightId)) {
        // The box the probe covers (world axes, whatever the entity's rotation) and where its picture is taken.
        const Vec3 center = lightCenter + probe->offset, half = probe->size * 0.5f;
        const Color color = probe->intensity > 0 ? Color(0.45f, 0.72f, 1.0f) : Color(0.45f, 0.45f, 0.45f);
        auto corner = [&](int c) {
            return center + Vec3(c & 1 ? half.x : -half.x, c & 2 ? half.y : -half.y, c & 4 ? half.z : -half.z);
        };
        for (int c = 0; c < 8; ++c)
            for (int bit : {1, 2, 4})
                if (!(c & bit)) view.lines.push_back({corner(c), corner(c | bit), color});
        for (const Vec3& axis : {Vec3(0.25f, 0, 0), Vec3(0, 0.25f, 0), Vec3(0, 0, 0.25f)})
            view.lines.push_back({lightCenter - axis, lightCenter + axis, color});
    }
    if (const Decal* decal = scene.Get<Decal>(lightId)) {
        const Vec3 size = decal->size;
        auto corner = [&](int c) { return lightWorld.TransformPoint(Vec3(c&1 ? size.x*0.5f : -size.x*0.5f,
                                                                       c&2 ? size.y*0.5f : -size.y*0.5f,
                                                                       c&4 ? size.z*0.5f : -size.z*0.5f)); };
        const Color color = decal->enabled ? Color(0.9f,0.65f,0.4f) : Color(0.45f,0.45f,0.45f);
        for (int c = 0; c < 8; ++c) for (int bit : {1,2,4})
            if (!(c&bit)) view.lines.push_back({corner(c),corner(c|bit),color});
        view.lines.push_back({lightCenter,lightWorld.TransformPoint(Vec3(0,-size.y*0.5f,0)),color});
    }
    if (const PlanarReflection* mirror = scene.Get<PlanarReflection>(lightId)) {
        const Vec3 origin = lightWorld.TransformPoint(mirror->offset);
        const Vec3 normal = Normalize(lightWorld.Inverse().Transposed().TransformDir(mirror->normal));
        const Vec3 right = Normalize(Cross(normal, std::fabs(normal.y) > 0.95f ? Vec3(0,0,1) : Vec3(0,1,0)));
        const Vec3 up = Cross(normal,right);
        const Color color = mirror->enabled ? Color(0.45f,0.72f,1) : Color(0.45f,0.45f,0.45f);
        const Vec3 corners[] = {origin-right-up,origin+right-up,origin+right+up,origin-right+up};
        for (int i = 0; i < 4; ++i) view.lines.push_back({corners[i],corners[(i+1)%4],color});
        view.lines.push_back({origin,origin+normal,color});
    }
    if (const DirectionalLight* light = scene.Get<DirectionalLight>(lightId)) {
        // Infinite reach: draw parallel rays, never a misleading finite range box.
        const Vec3 direction = Normalize(lightWorld.TransformDir(Vec3(0, 0, -1)));
        const Vec3 right = Normalize(Cross(direction, std::fabs(direction.y) > 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0)));
        const Color color = light->intensity > 0 ? Color(1, 0.85f, 0.35f) : Color(0.45f, 0.45f, 0.45f);
        for (int i = -1; i <= 1; ++i) {
            const Vec3 start = lightCenter + right * (static_cast<float>(i) * 0.65f);
            const Vec3 tip = start + direction * 2.5f;
            view.lines.push_back({start, tip, color});
            view.lines.push_back({tip, tip - direction * 0.4f + right * 0.2f, color});
            view.lines.push_back({tip, tip - direction * 0.4f - right * 0.2f, color});
        }
    }
    // Darkness2D is rendered in the game camera plane, not the light entity's rotated/scaled plane.
    const Mat4 gameCameraWorld = sceneCam.view.Inverse();
    const Vec3 lightRight = Normalize(gameCameraWorld.TransformDir(Vec3(1, 0, 0)));
    const Vec3 lightUp = Normalize(gameCameraWorld.TransformDir(Vec3(0, 1, 0)));
    for (const auto& kv : scene.Pool<Light2D>()) {
        const bool lightSelected = IsSelected(kv.first);
        if (!showIcons && !lightSelected) continue;
        const Light2D& light = kv.second;
        const Color color = !light.enabled || light.strength <= 0 ? Color(0.4f, 0.4f, 0.4f)
            : lightSelected ? Color(1, 0.7f, 0.2f) : Color(0.8f, 0.7f, 0.4f);
        const Vec3 center = scene.WorldMatrix(kv.first).TransformPoint(Vec3());
        ring(center, lightRight, lightUp, light.radius, color);
        if (lightSelected) ring(center, lightRight, lightUp, light.radius * Clamp(light.inner, 0.0f, 0.99f), color * 0.7f);
    }
    // The selected camera's clip volume uses its renderer matrix, not an approximate FOV shape.
    // Match the Game view aspect (previous frame while resizing); hidden Game uses the logical project view.
    std::vector<Vec3> cameraFootprint;
    if (scene.Get<Camera>(Primary())) {
        const InputState& input = engine.Input();
        const float cameraAspect = showGame && gameImageSize.x > 0 && gameImageSize.y > 0
            ? gameImageSize.x / gameImageSize.y
            : static_cast<float>(std::max(1, input.viewWidth)) / static_cast<float>(std::max(1, input.viewHeight));
        RenderView cameraView;
        std::array<Vec3, 8> corners;
        const bool cameraValid = MakeCameraView(scene, Primary(), cameraAspect, cameraView);
        if (cameraValid && cam.mode2D) {
            cameraFootprint = CameraPlaneFootprint(cameraView.view, cameraView.proj);
        } else if (cameraValid && !cam.mode2D && CameraFrustumCorners(cameraView.view, cameraView.proj, corners)) {
            const Color color(0.35f, 0.85f, 1.0f);
            for (size_t i = 0; i < 4; ++i) {
                const size_t next = (i + 1) % 4;
                view.lines.push_back({corners[i], corners[next], color});
                view.lines.push_back({corners[i + 4], corners[next + 4], color});
                view.lines.push_back({corners[i], corners[i + 4], color});
            }
        }
    }
    engine.AppendDebugLines(view.lines);
    sg_view tex = engine.Gpu()->RenderToTexture(engine.AnimationRenderScene(), view, w, h, kSceneSlot);
    sceneViewMat = view.view;
    sceneProjMat = view.proj;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    sceneImagePos = pos;
    sceneImageSize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    // Give ImGuizmo the current pointer before the view's button can claim it.
    // IsOver() without an operation reports the previous frame's hover state.
    const float compassSize = ImGui::GetFontSize() * 5.5f;
    const ImVec2 compassPos(pos.x + sceneImageSize.x - compassSize - 8, pos.y + 8);
    const bool showCompass = !cam.mode2D && sceneImageSize.x > compassSize + ImGui::GetFontSize() * 20 && sceneImageSize.y > compassSize + 16;
    const bool compassHot = showCompass && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(compassPos, compassPos + ImVec2(compassSize, compassSize));
    const bool painting = TilePainting();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool flip = FlipViews();
    dl->AddImage(ViewTexture(tex), pos, pos + sceneImageSize, ImVec2(0, flip ? 1.0f : 0.0f), ImVec2(1, flip ? 0.0f : 1.0f));
    // In 2D the range is an editing guide, not geometry at the near/far planes.
    // Draw it above sprites with a dark outline that also reads on a bright sky.
    if (!cameraFootprint.empty()) {
        std::vector<ImVec2> outline;
        for (const Vec3& point : cameraFootprint) {
            ImVec2 screen;
            if (Project(sceneProjMat * sceneViewMat, point, pos, sceneImageSize, screen)) outline.push_back(screen);
        }
        if (outline.size() == cameraFootprint.size()) {
            dl->PushClipRect(pos, pos + sceneImageSize, true);
            dl->AddPolyline(outline.data(), static_cast<int>(outline.size()), IM_COL32(18, 21, 27, 240), 4.0f, ImDrawFlags_Closed);
            dl->AddPolyline(outline.data(), static_cast<int>(outline.size()), IM_COL32(110, 225, 255, 255), 2.0f, ImDrawFlags_Closed);
            dl->PopClipRect();
        }
    }
    ImGui::Dummy(sceneImageSize);
    gizmoUsing = false;
    // A UI element is placed by its rectangle, not by a Transform: it gets handles instead of the gizmo.
    bool selectedUI = false;
    if (const EntityRow* row = Row(Primary())) {
        for (const char* type : {"UIText", "UIPanel", "UIButton", "UIImage", "UISlider", "UIInput", "UIDropdown", "UIJoystick"}) selectedUI = selectedUI || row->Has(type);
    }
    if (sceneUI && selectedUI) {
        gizmoWasUsing = false;
    } else if (!painting && (!compassHot || gizmoWasUsing)) {
        // ImGuizmo also checks last frame's hovered item. The scene background
        // must not block a handle entered and pressed in the same event batch.
        ImGuiContext& context = *ImGui::GetCurrentContext();
        const ImGuiID previousHover = context.HoveredIdPreviousFrame;
        if (previousHover == ImGui::GetID("##sceneview")) context.HoveredIdPreviousFrame = 0;
        SceneGizmo();
        context.HoveredIdPreviousFrame = previousHover;
    }
    bool clickedLeft = false, clickedRight = false;
    if (gizmoUsing || compassHot) {
        sceneHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(pos, pos + sceneImageSize);
        sceneButtonActive = false;
        // The gizmo only takes the left button: a right click on its handles still opens the object menu.
        clickedRight = gizmoUsing && !compassHot && sceneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    } else {
        ImGui::SetCursorScreenPos(pos);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##sceneview", sceneImageSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        sceneHovered = ImGui::IsItemHovered();
        clickedLeft = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        clickedRight = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        sceneButtonActive = ImGui::IsItemActive();
    }
    if (!compassHot) SceneCameraInput(io);
    else { sceneLooking = false; scenePressPending = false; }
    // UI first: it is in front of the world. Alt+drag stays the camera's.
    if (sceneUI && UIEdit(dl, pos, w, h, sceneHovered && !compassHot && !gizmoUsing && !io.KeyAlt, true)) clickedLeft = false;

    // Assets dropped into the view.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            SceneDrop(std::string(static_cast<const char*>(p->Data), static_cast<size_t>(p->DataSize)), io.MousePos.x - pos.x, io.MousePos.y - pos.y);
        }
        ImGui::EndDragDropTarget();
    }

    if (showCompass) SceneCompass(cam, compassPos, compassSize);

    SceneOverlays(dl);
    BonesOverlay(dl);
    if (painting) {
        // Tile brush: clicks paint the selected tilemap instead of selecting.
        gizmoUsing = false;
        if (!compassHot) TileSceneInput(dl, clickedLeft && !io.KeyAlt, clickedRight);
    } else {
        tileStroke = TileStroke::None;
    }

    // Right click (not a fly/pan drag) on an object: select it and offer to ask an agent about it.
    if (clickedRight && !painting) sceneRightPressed = true;
    if (sceneRightPressed && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        sceneRightPressed = false;
        if (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16.0f) {
            // The object under the pointer and the spot on its surface; over empty space the spot
            // is where the pointer's ray meets the ground plane (y = 0; z = 0 in 2D).
            const float x = io.MousePos.x - pos.x, y = io.MousePos.y - pos.y;
            Vec3 point, normal(0, 1, 0);
            EntityId id = ScenePickId(x, y, &point, &normal);
            if (id == kNullEntity) {
                Vec3 origin, dir;
                ScreenRay(sceneViewMat, sceneProjMat, x, y, sceneImageSize.x, sceneImageSize.y, origin, dir);
                normal = cam.mode2D ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
                point = cam.target;
                RayPlane(origin, dir, Vec3(0, 0, 0), normal, point);
            }
            if (markerHover != kNullEntity) id = markerHover;
            askMenuEntity = id;
            askMenuPoint = point;
            askMenuNormal = normal;
            askMenuUI = false;
            // The UI is in front of the world: over an element the menu is about that element, and
            // "this spot" is a place on the canvas (reference pixels), not what lies behind it.
            if (sceneUI) {
                const EntityId element = UIElementAt(LayoutUI(scene, w, h, &engine.Assets()), x, y, true);
                if (element != kNullEntity) {
                    const float scale = std::max(1e-3f, UIScale(scene, w, h));
                    askMenuEntity = element;
                    askMenuPoint = Vec3(std::round(x / scale), std::round(y / scale), 0);
                    askMenuNormal = Vec3(0, 0, 1);
                    askMenuUI = true;
                }
            }
            ImGui::OpenPopup("##sceneAsk");
        }
    }
    SceneAskMenu();

    // Click (not drag) selects: markers first, then the pixel's entity.
    if (clickedLeft && !io.KeyAlt && !gizmoUsing && !painting) {
        scenePressPending = true;
        scenePressMarker = markerHover;
    }
    if (scenePressPending && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        scenePressPending = false;
        if (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16.0f && !gizmoWasUsing) {
            if (scenePressMarker != kNullEntity) {
                if (io.KeyCtrl) ToggleSelect(scenePressMarker);
                else SelectOnly(scenePressMarker);
                scrollToRow = scenePressMarker;
                Refresh(true);
            } else {
                ScenePick(io.MousePos.x - pos.x, io.MousePos.y - pos.y, io.KeyCtrl);
            }
        }
    }

    // Overlay controls (top-left).
    {
        OverlayBar bar(ImVec2(pos.x + 8, pos.y + 8));
        OverlayToggle(Tr("Grid"), &showGrid, Tr("Ground grid"));
        ImGui::SameLine(0, 3);
        OverlayToggle(Tr("Colliders"), &showColliders, Tr("Collider wireframes: green solid, yellow trigger, cyan character"));
        ImGui::SameLine(0, 3);
        OverlayToggle(Tr("Icons"), &showIcons, Tr("Markers for cameras, lights and sounds"));
        ImGui::SameLine(0, 3);
        OverlayToggle(Tr("Fill Light"), &showEditorLight, Tr("Fill light for editing: brightens a scene lit by its gameplay scripts (a night level) so you can see it while the game is not running"));
        ImGui::SameLine(0, 3);
        bool mode2D = cam.mode2D;
        if (OverlayToggle("2D", &mode2D, Tr("Look along -Z (2D games); drag with right/middle mouse to pan"))) cam.Set2D(mode2D);
        ImGui::SameLine(0, 3);
        OverlayToggle("UI", &showSceneUI, Tr("The game's UI over the world: click an element to select it, drag to move, handles to resize"));
        ImGui::SameLine(0, 3);
        OverlayToggle(Tr("Bones"), &showBones, Tr("Skeleton of the selected model: hover a joint for its name; the bones a selected goal or attachment uses are lit"));
        ImGui::SameLine(0, 3);
        OverlayToggle(Tr("NavMesh"), &showNavMesh, Tr("Navigation mesh: where agents can walk, baked from the static colliders or meshes (NavMesh component)"));
        if (Row(Primary()) && Row(Primary())->Has("Tilemap")) {
            ImGui::SameLine(0, 3);
            if (OverlayToggle(Tr("Paint Tiles"), &tilePaint, Tr("Paint the selected Tilemap (brush from the Tiles panel): left paint, right erase, Shift rectangle, Ctrl pick"))) {
                showTiles = true;
                if (tilePaint && !cam.mode2D) cam.Set2D(true);
            }
        }
    }
    // Help line (bottom-left) while flying.
    if (sceneLooking) {
        std::string text = Format(Tr("Fly: WASD / QE   Shift: faster   Wheel: speed %.1f m/s"), flySpeed);
        ViewportLabel(dl, ImVec2(pos.x + 8, pos.y + sceneImageSize.y - ImGui::GetFontSize() - 8), text.c_str(), IM_COL32(238, 240, 245, 255));
    }
    if (InPlaySession()) {
        const char* t = Tr(Playing() ? "Playing - edits are undone on Stop" : "Paused - edits are undone on Stop");
        ImVec2 ts = ImGui::CalcTextSize(t);
        ViewportLabel(dl, ImVec2(pos.x + sceneImageSize.x - ts.x - 10, pos.y + (showCompass ? compassSize + 16 : 8)), t, IM_COL32(255, 213, 128, 255));
    }
    AskBubbles(pos, sceneImageSize);
    ImGui::End();
}

// ----- Game view -----------------------------------------------------------------

void NativeEditor::Impl::ReleaseGameInput() {
    for (const std::string& k : gameKeysDown) Call("input.key", ObjectOf({{"key", Json(k)}, {"down", Json(false)}}), true);
    gameKeysDown.clear();
    if (gameComposing) Call("input.text", ObjectOf({{"composing", Json("")}}), true);
    gameComposing = false;
    for (const std::string& name : gameAxesForwarded)
        Call("input.axis", ObjectOf({{"name", Json(name)}, {"value", Json(0)}}), true);
    gameAxesForwarded.clear();
    for (int b = 0; b < 2; ++b) {
        if (!gameMouseDown[b]) continue;
        Call("input.mouse", ObjectOf({{"button", Json(b == 0 ? "MouseLeft" : "MouseRight")}, {"down", Json(false)}}), true);
        gameMouseDown[b] = false;
    }
    if (gameWantsLock || windowInput.mouseLocked) Call("input.mouse", ObjectOf({{"locked", Json(false)}}), true);
    windowInput.mouseLocked = false;
    windowInput.mouseDX = windowInput.mouseDY = 0;
    gameWantsLock = false;
}

void NativeEditor::Impl::GameInput() {
    bool session = engine.InPlaySession();
    if (!session && (gameFocused || gameWantsLock)) {
        ReleaseGameInput();
        gameFocused = false;
    }
    // The platform let go of a locked mouse (Escape, window lost focus): tell the game.
    if (gameWantsLock && !windowInput.mouseLocked) {
        Call("input.mouse", ObjectOf({{"locked", Json(false)}}), true);
        gameWantsLock = false;
    }
    bool want = gameFocused && session && GameEngine().Input().mouseLocked;
    if (gameFocused && session) {
        for (const char* name : InputState::kAxisNames) {
            auto axis = windowInput.axes.find(name);
            if (axis == windowInput.axes.end()) continue;  // no device: retain agent-injected input
            auto previous = GameEngine().Input().axes.find(name);
            if (previous == GameEngine().Input().axes.end() || previous->second != axis->second)
                Call("input.axis", ObjectOf({{"name", Json(name)}, {"value", Json(axis->second)}}), true);
            gameAxesForwarded.insert(name);
        }
        for (const char* name : GamepadInput::kButtonNames) {
            bool down = windowInput.IsDown(name);
            if (down == (gameKeysDown.count(name) != 0)) continue;
            Call("input.key", ObjectOf({{"key", Json(name)}, {"down", Json(down)}}), true);
            if (down) gameKeysDown.insert(name);
            else gameKeysDown.erase(name);
        }
    }
    // The wheel while the game holds the mouse (ImGui does not see it then).
    if (want && capturedWheel != 0) Call("input.mouse", ObjectOf({{"wheel", Json(capturedWheel)}}), true);
    capturedWheel = 0;
    if (want && (windowInput.mouseDX != 0 || windowInput.mouseDY != 0)) {
        Call("input.mouse", ObjectOf({{"dx", Json(windowInput.mouseDX)}, {"dy", Json(windowInput.mouseDY)}}), true);
    }
    windowInput.mouseDX = windowInput.mouseDY = 0;
    windowInput.mouseLocked = want;
    gameWantsLock = want;
}

void NativeEditor::Impl::GamePanel() {
    if (requestGameFocus) {
        ImGui::SetNextWindowFocus();
        requestGameFocus = false;
        gameFocused = engine.InPlaySession();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin(WithIcon(kIconGamepad2, TrId("Game")).c_str(), &showGame, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    gameHovered = false;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!open || avail.x < 8 || avail.y < 8 || !engine.Gpu()) {
        if (gameFocused) {
            ReleaseGameInput();
            gameFocused = false;
        }
        ImGui::End();
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    bool session = InPlaySession();

    // Bar above the image (the game's own UI owns the image corners).
    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(6, 3));
    const char* aspectNames[] = {Tr("Free aspect"), "16:9", "16:10", "4:3"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    ImGui::Combo("##aspect", &gameAspect, aspectNames, 4);
    HelpTooltip(Tr("Game view aspect ratio"));
    Json peers; if (engine.NetworkEnabled()) peers = engine.NetworkCall("local_peers", Json::MakeObject());
    if (peers.size() > 1) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        std::string label = std::string(Tr("Player")) + " " + std::to_string(gamePeer + 1);
        if (ImGui::BeginCombo("##gamePeer", label.c_str())) {
            for (size_t i = 0; i < peers.size(); ++i) {
                std::string peerLabel = std::string(Tr("Player")) + " " + std::to_string(i + 1);
                if (ImGui::Selectable(peerLabel.c_str(), static_cast<size_t>(gamePeer) == i)) {
                    ReleaseGameInput(); gamePeer = static_cast<int>(i); gameFocused = false;
                }
            }
            ImGui::EndCombo();
        }
    } else gamePeer = 0;
    ImVec2 barEnd = ImGui::GetCursorScreenPos();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Tr(!session ? "Click UI to select it: drag moves, handles resize, arrows nudge. Play (Ctrl+P) runs the game here"
                                 : gameFocused ? (windowInput.mouseLocked ? "Game has the mouse - Esc releases it" : "Game has keyboard, mouse and gamepad - click outside to release")
                                               : "Click the view to play"));
    ImGui::SetCursorScreenPos(ImVec2(barEnd.x - 6, barEnd.y + 3));
    avail = ImGui::GetContentRegionAvail();
    if (avail.x < 8 || avail.y < 8) {
        ImGui::End();
        return;
    }

    // Letterbox to the chosen aspect ratio.
    static const float ratios[] = {0.0f, 16.0f / 9.0f, 16.0f / 10.0f, 4.0f / 3.0f};
    ImVec2 size = avail;
    float ratio = ratios[std::max(0, std::min(3, gameAspect))];
    if (ratio > 0) {
        if (size.x / size.y > ratio) size.x = std::floor(size.y * ratio);
        else size.y = std::floor(size.x / ratio);
    }
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 pos(std::floor(origin.x + (avail.x - size.x) * 0.5f), std::floor(origin.y + (avail.y - size.y) * 0.5f));
    int w = std::max(1, static_cast<int>(size.x)), h = std::max(1, static_cast<int>(size.y));
    // Scripts project with the view they are drawn into (name plates, UI that follows the world),
    // not the project's window size or wherever the mouse last was.
    GameEngine().SetViewSize(w, h);
    Scene& scene = session ? GameEngine().GetScene() : engine.AnimationRenderScene();
    GameEngine().BeginInterpolatedRender();  // smooth motion between fixed steps, like the game window
    RenderView view;
    bool hasCamera = MakeSceneView(scene, static_cast<float>(w) / static_cast<float>(h), view);
    view.shaderTime = static_cast<float>(GameEngine().SimTime());
    view.playing = GameEngine().InPlaySession();
    view.drawUI = true;
    GameEngine().AppendDebugLines(view.lines);
    sg_view tex = engine.Gpu()->RenderToTexture(scene, view, w, h, kGameSlot);
    GameEngine().EndInterpolatedRender();
    gameImagePos = pos;
    gameImageSize = ImVec2(static_cast<float>(w), static_cast<float>(h));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, origin + avail, IM_COL32(8, 9, 11, 255));
    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##gameview", gameImageSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    gameHovered = ImGui::IsItemHovered();
    bool flip = FlipViews();
    dl->AddImage(ViewTexture(tex), pos, pos + gameImageSize, ImVec2(0, flip ? 1.0f : 0.0f), ImVec2(1, flip ? 0.0f : 1.0f));
    if (!hasCamera) {
        const char* t = Tr("No active camera: add a Camera component (Create > Rendering > Camera)");
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(pos.x + (gameImageSize.x - ts.x) * 0.5f, pos.y + (gameImageSize.y - ts.y) * 0.5f), IM_COL32(230, 232, 238, 230), t);
    }

    // Focus: clicking the view hands keyboard and mouse to the running game.
    for (int b = 0; b < 2; ++b) {
        if (!ImGui::IsItemClicked(b)) continue;
        if (session) {
            gameFocused = true;
            float x = io.MousePos.x - pos.x, y = io.MousePos.y - pos.y;
            Call("input.mouse", ObjectOf({{"x", Json(x)}, {"y", Json(y)}, {"width", Json(w)}, {"height", Json(h)},
                                          {"button", Json(b == 0 ? "MouseLeft" : "MouseRight")}, {"down", Json(true)}}),
                 true);
            gameMouseDown[b] = true;
        }
    }
    for (int b = 0; b < 2; ++b) {
        if (gameMouseDown[b] && !windowInput.mouseLocked && ImGui::IsMouseReleased(b)) {
            Call("input.mouse", ObjectOf({{"button", Json(b == 0 ? "MouseLeft" : "MouseRight")}, {"down", Json(false)}}), true);
            gameMouseDown[b] = false;
        }
    }
    if (gameFocused && !windowInput.mouseLocked && !gameHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        ReleaseGameInput();
        gameFocused = false;
    }
    if (gameFocused && session && gameHovered && !windowInput.mouseLocked) {
        float x = io.MousePos.x - pos.x, y = io.MousePos.y - pos.y;
        if (x != lastGameMouse[0] || y != lastGameMouse[1]) {
            Call("input.mouse", ObjectOf({{"x", Json(x)}, {"y", Json(y)}, {"width", Json(w)}, {"height", Json(h)}}), true);
            lastGameMouse[0] = x;
            lastGameMouse[1] = y;
        }
    }

    if (gameFocused && session && gameHovered && io.MouseWheel != 0)
        Call("input.mouse", ObjectOf({{"wheel", Json(io.MouseWheel)}}), true);
    if (!session) UIEdit(dl, pos, w, h, gameHovered, false);

    if (session) dl->AddRect(pos, pos + gameImageSize, gameFocused ? IM_COL32(90, 200, 120, 255) : IM_COL32(255, 180, 60, 160), 0.0f, 2.0f);
    ImGui::End();
}

namespace {
const char* UITypeName(UIRect::Kind kind) {
    switch (kind) {
        case UIRect::Kind::Text: return "UIText";
        case UIRect::Kind::Panel: return "UIPanel";
        case UIRect::Kind::Button: return "UIButton";
        case UIRect::Kind::Image: return "UIImage";
        case UIRect::Kind::Slider: return "UISlider";
        case UIRect::Kind::Input: return "UIInput";
        case UIRect::Kind::Dropdown: return "UIDropdown";
        case UIRect::Kind::Joystick: return "UIJoystick";
    }
    return "UIPanel";
}
// Pivot of an anchor preset along one axis: 0 = left/top, 0.5 = centre (and stretch), 1 = right/bottom.
float AnchorPivot(const std::string& anchor, const char* low, const char* high) {
    if (anchor.rfind("stretch", 0) == 0) return 0.5f;
    if (anchor.find(low) != std::string::npos) return 0.0f;
    if (anchor.find(high) != std::string::npos) return 1.0f;
    return 0.5f;
}
}  // namespace

EntityId NativeEditor::Impl::UIElementAt(const std::vector<UIRect>& rects, float x, float y, bool inScene) const {
    const Scene& scene = engine.GetScene();
    const EntityId primary = Primary();
    EntityId hit = kNullEntity;
    for (const UIRect& r : rects) {  // last = topmost
        if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) continue;
        if (r.clipped && !(x >= r.clip[0] && x < r.clip[2] && y >= r.clip[1] && y < r.clip[3])) continue;
        // Over the world a panel that draws nothing (a screen-wide container) must not be in the way
        // of what is behind it; the Hierarchy still selects it.
        if (inScene && r.kind == UIRect::Kind::Panel) {
            const UIPanel* panel = scene.Get<UIPanel>(r.entity);
            if (panel && panel->opacity <= 0.0f && panel->texture.empty() && r.entity != primary) continue;
        }
        hit = r.entity;
    }
    return hit;
}

// Editing UI where it is drawn (the Scene view and the stopped Game view): click selects the topmost
// element under the pointer, dragging it moves it (x, y), the eight handles resize it (width, height,
// keeping the opposite edge in place), arrow keys nudge. Every drag is one undo step
// (component.set {merge}).
bool NativeEditor::Impl::UIEdit(ImDrawList* dl, ImVec2 pos, int w, int h, bool hovered, bool inScene) {
    Scene& scene = engine.GetScene();
    ImGuiIO& io = ImGui::GetIO();
    const std::vector<UIRect> rects = LayoutUI(scene, w, h, &engine.Assets());
    const float scale = std::max(1e-3f, UIScale(scene, w, h));
    const ImVec2 mouse(io.MousePos.x - pos.x, io.MousePos.y - pos.y);
    auto rectOf = [&](EntityId id) -> const UIRect* {
        const UIRect* found = nullptr;
        for (const UIRect& r : rects)
            if (r.entity == id && (!found || r.kind == UIRect::Kind::Panel)) found = &r;
        return found;
    };
    const EntityId primary = Primary();
    const UIRect* sel = primary != kNullEntity ? rectOf(primary) : nullptr;

    // Handles of the primary selection.
    const float hs = std::max(4.0f, ImGui::GetFontSize() * 0.3f);
    ImVec2 handles[8];
    int hoverHandle = -1;
    if (sel) {
        const float x0 = pos.x + sel->x, y0 = pos.y + sel->y, x1 = x0 + sel->w, y1 = y0 + sel->h, xm = (x0 + x1) * 0.5f, ym = (y0 + y1) * 0.5f;
        const ImVec2 points[8] = {{x0, y0}, {xm, y0}, {x1, y0}, {x1, ym}, {x1, y1}, {xm, y1}, {x0, y1}, {x0, ym}};
        for (int i = 0; i < 8; ++i) {
            handles[i] = points[i];
            if (std::fabs(io.MousePos.x - points[i].x) <= hs + 2 && std::fabs(io.MousePos.y - points[i].y) <= hs + 2) hoverHandle = i;
        }
    }

    bool pressed = false;
    if (hovered && uiDragHandle < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pressed = true;
        const UIRect* hit = nullptr;
        if (hoverHandle >= 0) {
            hit = sel;
        } else {
            hit = rectOf(UIElementAt(rects, mouse.x, mouse.y, inScene));
        }
        if (!hit) {
            if (inScene) pressed = false;  // the world's click (ScenePanel picks)
            else if (!io.KeyCtrl) SelectOnly(kNullEntity);
        } else if (io.KeyCtrl && hoverHandle < 0) {
            ToggleSelect(hit->entity);
        } else {
            if (!IsSelected(hit->entity)) {
                SelectOnly(hit->entity);
                scrollToRow = hit->entity;
            }
            const UIRect* r = rectOf(hit->entity);
            uiDragEntity = hit->entity;
            uiDragType = UITypeName(r->kind);
            uiDragHandle = hoverHandle >= 0 ? hoverHandle + 1 : 0;
            uiDragMouse = io.MousePos;
            Json comp = Call("entity.get", ObjectOf({{"id", Json(uiDragEntity)}}), true)["result"]["components"][uiDragType];
            uiDragStart[0] = comp["x"].asFloat(0);
            uiDragStart[1] = comp["y"].asFloat(0);
            // Text and images sized by their content get explicit sizes once a handle moves them.
            const bool stretched = comp["anchor"].asString().rfind("stretch", 0) == 0;
            uiDragStart[2] = comp["width"].asFloat(0) != 0 || stretched ? comp["width"].asFloat(0) : r->w / scale;
            uiDragStart[3] = comp["height"].asFloat(0) != 0 || stretched ? comp["height"].asFloat(0) : r->h / scale;
            ++uiDragSerial;
        }
    }
    if (uiDragHandle >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !scene.Exists(uiDragEntity)) {
            uiDragHandle = -1;
        } else {
            const float dx = (io.MousePos.x - uiDragMouse.x) / scale, dy = (io.MousePos.y - uiDragMouse.y) / scale;
            if (dx != 0 || dy != 0) {
                Json comp = Call("entity.get", ObjectOf({{"id", Json(uiDragEntity)}}), true)["result"]["components"][uiDragType];
                const std::string anchor = comp["anchor"].asString();
                const float px = AnchorPivot(anchor, "left", "right"), py = AnchorPivot(anchor, "top", "bottom");
                float x = uiDragStart[0], y = uiDragStart[1], wide = uiDragStart[2], tall = uiDragStart[3];
                const int handle = uiDragHandle - 1;  // 0 TL, 1 T, 2 TR, 3 R, 4 BR, 5 B, 6 BL, 7 L
                Json values = Json::MakeObject();
                if (uiDragHandle == 0) {
                    x += dx;
                    y += dy;
                } else {
                    const bool left = handle == 0 || handle == 6 || handle == 7, right = handle >= 2 && handle <= 4;
                    const bool top = handle <= 2, bottom = handle >= 4 && handle <= 6;
                    if (right) { wide += dx; x += px * dx; }
                    if (left) { wide -= dx; x += (1.0f - px) * dx; }
                    if (bottom) { tall += dy; y += py * dy; }
                    if (top) { tall -= dy; y += (1.0f - py) * dy; }
                    const bool stretched = anchor.rfind("stretch", 0) == 0;
                    if (!stretched) { wide = std::max(1.0f, wide); tall = std::max(1.0f, tall); }
                    values["width"] = std::round(wide);
                    values["height"] = std::round(tall);
                }
                values["x"] = std::round(x);
                values["y"] = std::round(y);
                Call("component.set", ObjectOf({{"id", Json(uiDragEntity)}, {"type", Json(uiDragType)}, {"values", values},
                                                {"merge", Json("uiedit:" + std::to_string(uiDragSerial))}}));
            }
        }
    }
    // Arrow keys nudge the selected elements by one reference pixel (ten with Shift).
    if (hovered && sel && uiDragHandle < 0 && !io.WantTextInput) {
        float nx = 0, ny = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nx = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nx = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) ny = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ny = 1;
        if (nx != 0 || ny != 0) {
            const float step = io.KeyShift ? 10.0f : 1.0f;
            for (EntityId id : selection) {
                const UIRect* r = rectOf(id);
                if (!r) continue;
                const char* type = UITypeName(r->kind);
                Json comp = Call("entity.get", ObjectOf({{"id", Json(id)}}), true)["result"]["components"][type];
                Call("component.set", ObjectOf({{"id", Json(id)}, {"type", Json(type)},
                                                {"values", ObjectOf({{"x", Json(comp["x"].asFloat(0) + nx * step)}, {"y", Json(comp["y"].asFloat(0) + ny * step)}})}}));
            }
        }
    }

    // Outlines: every selected element, handles and size readout on the primary one.
    dl->PushClipRect(pos, pos + ImVec2(static_cast<float>(w), static_cast<float>(h)), true);
    for (EntityId id : selection) {
        const UIRect* r = rectOf(id);
        if (!r) continue;
        const ImVec2 a(pos.x + r->x, pos.y + r->y), b(a.x + r->w, a.y + r->h);
        dl->AddRect(a, b, IM_COL32(20, 22, 28, 200), 0.0f, 3.0f);
        dl->AddRect(a, b, IM_COL32(255, 158, 26, 255), 0.0f, 1.5f);
    }
    if (sel) {
        for (int i = 0; i < 8; ++i) {
            const ImVec2 a(handles[i].x - hs, handles[i].y - hs), b(handles[i].x + hs, handles[i].y + hs);
            dl->AddRectFilled(a, b, i == hoverHandle || i == uiDragHandle - 1 ? IM_COL32(255, 220, 150, 255) : IM_COL32(255, 255, 255, 255));
            dl->AddRect(a, b, IM_COL32(255, 158, 26, 255));
        }
        const EntityRecord* rec = scene.Record(primary);
        const std::string label = Format("%s   %.0f x %.0f", rec ? rec->name.c_str() : "", sel->w / scale, sel->h / scale);
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        ImVec2 at(pos.x + sel->x, pos.y + sel->y - ts.y - hs - 4);
        if (at.y < pos.y) at.y = pos.y + sel->y + sel->h + hs + 4;
        dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x + ts.x + 3, at.y + ts.y + 1), IM_COL32(20, 22, 28, 220), 3.0f);
        dl->AddText(at, IM_COL32(255, 220, 150, 255), label.c_str());
    }
    dl->PopClipRect();
    if (hovered && uiDragHandle < 0 && hoverHandle >= 0) {
        static const ImGuiMouseCursor cursors[8] = {ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW,
                                                    ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW};
        ImGui::SetMouseCursor(cursors[hoverHandle]);
    }
    return uiDragHandle >= 0 || pressed;
}

}  // namespace oe
