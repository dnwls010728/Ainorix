// Native editor views: the Scene view (free camera, picking, gizmo, entity
// icons, asset drops) and the Game view (game camera, input forwarding).
// Both are GpuRenderer::RenderToTexture images drawn as ImGui images.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "editor/EditorInternal.h"
#include "editor/EditorText.h"
#include "ImGuizmo.h"
#include "sokol_imgui.h"

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/Log.h"
#include "physics/PhysicsWorld.h"
#include "platform/GamepadInput.h"
#include "render/GpuRenderer.h"
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

// Small overlay button row drawn on top of a view image.
bool OverlayToggle(const char* label, bool* value, const char* tip) {
    ImGui::PushStyleColor(ImGuiCol_Button, *value ? ImVec4(0.22f, 0.38f, 0.66f, 0.9f) : ImVec4(0.1f, 0.11f, 0.13f, 0.75f));
    bool pressed = ImGui::SmallButton(label);
    ImGui::PopStyleColor();
    if (pressed) *value = !*value;
    HelpTooltip(tip);
    return pressed;
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
        else if (row.Has("PointLight") && !row.Has("MeshRenderer")) letter = "P", col = IM_COL32(255, 190, 90, 230);
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
        if (hover) dl->AddText(ImVec2(s.x + r + 4, s.y - ts.y * 0.5f), IM_COL32(230, 232, 238, 255), row.name.c_str());
    }
    // Light2D: the reach of each 2D light as a ring in its entity's XY plane.
    for (const auto& kv : scene.Pool<Light2D>()) {
        const Light2D& light = kv.second;
        const Mat4 world = scene.WorldMatrix(kv.first);
        const Vec3 center = world.TransformPoint(Vec3(0, 0, 0));
        const ImU32 color = IsSelected(kv.first) ? IM_COL32(255, 158, 26, 255) : IM_COL32(255, 220, 120, light.enabled ? 170 : 70);
        ImVec2 previous;
        bool havePrevious = false;
        for (int i = 0; i <= 48; ++i) {
            const float a = static_cast<float>(i) / 48.0f * 6.2831853f;
            ImVec2 point;
            const bool ok = Project(vp, center + Vec3(std::cos(a) * light.radius, std::sin(a) * light.radius, 0), sceneImagePos, sceneImageSize, point);
            if (ok && havePrevious) dl->AddLine(previous, point, color, 1.5f);
            previous = point;
            havePrevious = ok;
        }
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
    ImGuizmo::SetOrthographic(false);
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
    gizmoUsing = using_ || ImGuizmo::IsOver();
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

void NativeEditor::Impl::ScenePick(float x, float y, bool additive) {
    Json camera = ObjectOf({{"eye", Vec3Json(cam.Eye())}, {"target", Vec3Json(cam.target)}, {"fov", Json(cam.fov)}});
    Json r = Call("render.pick", ObjectOf({{"x", Json(static_cast<int>(x))},
                                           {"y", Json(static_cast<int>(y))},
                                           {"width", Json(static_cast<int>(sceneImageSize.x))},
                                           {"height", Json(static_cast<int>(sceneImageSize.y))},
                                           {"camera", camera}}),
                  true);
    EntityId id = static_cast<EntityId>(r["result"]["id"].asNumber(0));
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
        Json camera = ObjectOf({{"eye", Vec3Json(cam.Eye())}, {"target", Vec3Json(cam.target)}, {"fov", Json(cam.fov)}});
        Json r = Call("render.pick", ObjectOf({{"x", Json(static_cast<int>(x))}, {"y", Json(static_cast<int>(y))},
                                               {"width", Json(static_cast<int>(sceneImageSize.x))}, {"height", Json(static_cast<int>(sceneImageSize.y))},
                                               {"camera", camera}}),
                      true);
        return static_cast<EntityId>(r["result"]["id"].asNumber(0));
    };
    Json r;
    if (EndsWith(path, ".glb") || EndsWith(path, ".gltf")) {
        r = Call("entity.create", ObjectOf({{"name", Json(UniqueName(Stem(path)))},
                                            {"components", ObjectOf({{"Transform", ObjectOf({{"position", Vec3Json(hit)}})},
                                                                     {"MeshRenderer", ObjectOf({{"mesh", Json(path)}})}})}}));
    } else if (EndsWith(path, ".prefab.json")) {
        r = Call("prefab.instantiate", ObjectOf({{"path", Json(path)}, {"position", Vec3Json(hit)}}));
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
    bool open = ImGui::Begin(TrId("Scene").c_str(), &showScene, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
    RenderView sceneCam;
    MakeSceneView(scene, aspect, sceneCam);
    RenderView view = MakeLookAtView(cam.Eye(), cam.target, cam.fov, aspect);
    view.clearColor = sceneCam.clearColor;
    view.shaderTime = static_cast<float>(engine.SimTime());
    view.drawGrid = showGrid && !cam.mode2D;
    view.highlight = Primary();
    if (showGrid && cam.mode2D && cam.Eye().z > 1e-3f) {
        // 2D: a grid in the XY plane that follows the camera and coarsens with zoom.
        // Sized by the eye's distance to that plane (the target may sit at another depth).
        const float halfH = cam.Eye().z * std::tan(Radians(cam.fov) * 0.5f), halfW = halfH * aspect;
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
    engine.AppendDebugLines(view.lines);
    sg_view tex = engine.Gpu()->RenderToTexture(scene, view, w, h, kSceneSlot);
    sceneViewMat = view.view;
    sceneProjMat = view.proj;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    sceneImagePos = pos;
    sceneImageSize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    // ImGuizmo only grabs the mouse when no ImGui item is hovered, so while the
    // pointer is on the gizmo the view is a plain (non-interactive) item.
    const bool painting = TilePainting();
    bool gizmoHot = !painting && gizmoOp != GizmoOp::None && Primary() != kNullEntity && !sceneButtonActive && (ImGuizmo::IsUsing() || ImGuizmo::IsOver());
    bool clickedLeft = false, clickedRight = false;
    if (gizmoHot) {
        ImGui::Dummy(sceneImageSize);
        sceneHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(pos, pos + sceneImageSize);
        sceneButtonActive = false;
    } else {
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##sceneview", sceneImageSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        sceneHovered = ImGui::IsItemHovered();
        clickedLeft = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        clickedRight = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        sceneButtonActive = ImGui::IsItemActive();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool flip = FlipViews();
    dl->AddImage(ViewTexture(tex), pos, pos + sceneImageSize, ImVec2(0, flip ? 1.0f : 0.0f), ImVec2(1, flip ? 0.0f : 1.0f));
    SceneCameraInput(io);

    // Assets dropped into the view.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            SceneDrop(std::string(static_cast<const char*>(p->Data), static_cast<size_t>(p->DataSize)), io.MousePos.x - pos.x, io.MousePos.y - pos.y);
        }
        ImGui::EndDragDropTarget();
    }

    SceneOverlays(dl);
    if (painting) {
        // Tile brush: clicks paint the selected tilemap instead of selecting.
        gizmoUsing = false;
        TileSceneInput(dl, clickedLeft && !io.KeyAlt, clickedRight);
    } else {
        tileStroke = TileStroke::None;
        SceneGizmo();
    }

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
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 6, pos.y + 6));
    OverlayToggle(Tr("Grid"), &showGrid, Tr("Ground grid"));
    ImGui::SameLine(0, 3);
    OverlayToggle(Tr("Colliders"), &showColliders, Tr("Collider wireframes: green solid, yellow trigger, cyan character"));
    ImGui::SameLine(0, 3);
    OverlayToggle(Tr("Icons"), &showIcons, Tr("Markers for cameras, lights and sounds"));
    ImGui::SameLine(0, 3);
    bool mode2D = cam.mode2D;
    if (OverlayToggle("2D", &mode2D, Tr("Look along -Z (2D games); drag with right/middle mouse to pan"))) cam.Set2D(mode2D);
    if (Row(Primary()) && Row(Primary())->Has("Tilemap")) {
        ImGui::SameLine(0, 3);
        if (OverlayToggle(Tr("Paint Tiles"), &tilePaint, Tr("Paint the selected Tilemap (brush from the Tiles panel): left paint, right erase, Shift rectangle, Ctrl pick"))) {
            showTiles = true;
            if (tilePaint && !cam.mode2D) cam.Set2D(true);
        }
    }
    // Help line (bottom-left) while flying.
    if (sceneLooking) {
        std::string text = Format(Tr("Fly: WASD / QE   Shift: faster   Wheel: speed %.1f m/s"), flySpeed);
        dl->AddText(ImVec2(pos.x + 8, pos.y + sceneImageSize.y - ImGui::GetFontSize() - 8), IM_COL32(230, 232, 238, 220), text.c_str());
    }
    if (InPlaySession()) {
        const char* t = Tr(Playing() ? "Playing - edits are undone on Stop" : "Paused - edits are undone on Stop");
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(pos.x + sceneImageSize.x - ts.x - 10, pos.y + 8), IM_COL32(255, 200, 90, 230), t);
    }
    ImGui::End();
}

// ----- Game view -----------------------------------------------------------------

void NativeEditor::Impl::ReleaseGameInput() {
    for (const std::string& k : gameKeysDown) Call("input.key", ObjectOf({{"key", Json(k)}, {"down", Json(false)}}), true);
    gameKeysDown.clear();
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
    bool open = ImGui::Begin(TrId("Game").c_str(), &showGame, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
    Scene& scene = GameEngine().GetScene();
    RenderView view;
    bool hasCamera = MakeSceneView(scene, static_cast<float>(w) / static_cast<float>(h), view);
    view.shaderTime = static_cast<float>(GameEngine().SimTime());
    view.drawUI = true;
    GameEngine().AppendDebugLines(view.lines);
    sg_view tex = engine.Gpu()->RenderToTexture(scene, view, w, h, kGameSlot);
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
    if (!session) GameUIEdit(dl, pos, w, h);

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

// Editing UI where it is drawn: click selects the topmost element under the pointer, dragging it
// moves it (x, y), the eight handles resize it (width, height, keeping the opposite edge in place),
// arrow keys nudge. Every drag is one undo step (component.set {merge}).
void NativeEditor::Impl::GameUIEdit(ImDrawList* dl, ImVec2 pos, int w, int h) {
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

    if (gameHovered && uiDragHandle < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const UIRect* hit = nullptr;
        if (hoverHandle >= 0) {
            hit = sel;
        } else {
            for (const UIRect& r : rects) {  // last = topmost
                if (mouse.x < r.x || mouse.x >= r.x + r.w || mouse.y < r.y || mouse.y >= r.y + r.h) continue;
                if (r.clipped && !(mouse.x >= r.clip[0] && mouse.x < r.clip[2] && mouse.y >= r.clip[1] && mouse.y < r.clip[3])) continue;
                hit = &r;
            }
        }
        if (!hit) {
            if (!io.KeyCtrl) SelectOnly(kNullEntity);
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
    if (gameHovered && sel && uiDragHandle < 0 && !io.WantTextInput) {
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
    dl->PushClipRect(pos, pos + gameImageSize, true);
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
    if (gameHovered && uiDragHandle < 0 && hoverHandle >= 0) {
        static const ImGuiMouseCursor cursors[8] = {ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW,
                                                    ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW};
        ImGui::SetMouseCursor(cursors[hoverHandle]);
    }
}

}  // namespace oe
