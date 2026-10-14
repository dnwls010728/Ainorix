// 2D authoring tools of the Tiles panel: map layers and the tile rule of the brush (Tilemap),
// the sheet slicer and the collision polygon editor (Sprite Tools). Every change is a command.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "editor/EditorInternal.h"
#include "editor/EditorText.h"
#include "sokol_imgui.h"

#include "app/Engine.h"
#include "assets/Assets.h"
#include "render/GpuRenderer.h"
#include "render/RenderScene.h"
#include "scene/Components.h"
#include "scene/TileGrid.h"

namespace oe {

namespace {

// Pixel art previews sample the nearest texel (ImGui's default sampler is linear).
ImTextureID NearestTexture(sg_view view) {
    static sg_sampler sampler{};
    if (sg_query_sampler_state(sampler) != SG_RESOURCESTATE_VALID) {
        sg_sampler_desc desc{};
        desc.min_filter = desc.mag_filter = SG_FILTER_NEAREST;
        desc.wrap_u = desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        desc.label = "oe-editor-nearest";
        sampler = sg_make_sampler(&desc);
    }
    return static_cast<ImTextureID>(simgui_imtextureid_with_sampler(view, sampler));
}

// Tile rule data values typed as text: true/false, numbers, anything else a string.
Json ParseDataValue(const std::string& text) {
    if (text == "true") return Json(true);
    if (text == "false") return Json(false);
    char* end = nullptr;
    const double number = std::strtod(text.c_str(), &end);
    if (!text.empty() && end && *end == '\0' && std::isfinite(number)) return Json(number);
    return Json(text);
}

std::string DataValueText(const Json& value) {
    if (value.isString()) return value.asString();
    if (value.isNull()) return std::string();
    return value.dump();
}

}  // namespace

void NativeEditor::Impl::TwoDimensionalRect(const std::string& name) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    twoDimensionalRects[name] = {a.x, a.y, b.x - a.x, b.y - a.y};
}

// ----- Tilemap: layers -----------------------------------------------------------------

void NativeEditor::Impl::TileLayerTools() {
    const Tilemap* tm = engine.GetScene().Get<Tilemap>(Primary());
    if (!tm) return;
    if (tileLayer >= static_cast<int>(tm->layers.size())) tileLayer = -1;
    auto layerName = [&](int index) { return index < 0 ? std::string(Tr("Base Map")) : tm->layers[index]["name"].asString("Layer " + std::to_string(index + 1)); };
    auto choose = [&](int index) {
        tileLayer = index;
        tileLayerName = index < 0 ? std::string() : layerName(index);
        tileInfoId = 0;
    };
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(Tr("Map Layer")).x + ImGui::GetStyle().ItemInnerSpacing.x));
    if (ImGui::BeginCombo(Tr("Map Layer"), layerName(tileLayer).c_str())) {
        if (ImGui::Selectable(Tr("Base Map"), tileLayer < 0)) choose(-1);
        for (size_t i = 0; i < tm->layers.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(layerName(static_cast<int>(i)).c_str(), tileLayer == static_cast<int>(i))) choose(static_cast<int>(i));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    TwoDimensionalRect("layer");
    auto setLayer = [&](const Json& values, const std::string& merge = std::string()) {
        Json args = ObjectOf({{"id", Primary()}, {"layer", tileLayer}, {"values", values}});
        if (!merge.empty()) args["merge"] = merge;
        Call("tilemap.layer_set", args);
        tileInfoId = 0;
    };
    if (ImGui::SmallButton(Tr("Add Map Layer"))) {
        const int index = static_cast<int>(tm->layers.size());
        Call("tilemap.layer_set", ObjectOf({{"id", Primary()},
                                            {"layer", index},
                                            {"values", ObjectOf({{"name", "Layer " + std::to_string(index + 1)}, {"map", Json::MakeArray()}, {"layer", tm->layer + 1}})}}));
        choose(index);
        tm = engine.GetScene().Get<Tilemap>(Primary());
    }
    TwoDimensionalRect("layer-add");
    if (tileLayer < 0) return;
    const Json& layer = tm->layers[tileLayer];
    ImGui::SameLine();
    ImGui::BeginDisabled(tileLayer == 0);
    if (ImGui::SmallButton(Tr("Up"))) {
        Call("tilemap.layer_set", ObjectOf({{"id", Primary()}, {"layer", tileLayer}, {"moveTo", tileLayer - 1}}));
        choose(tileLayer - 1);
    }
    ImGui::EndDisabled();
    TwoDimensionalRect("layer-up");
    ImGui::SameLine();
    ImGui::BeginDisabled(tileLayer + 1 >= static_cast<int>(tm->layers.size()));
    if (ImGui::SmallButton(Tr("Down"))) {
        Call("tilemap.layer_set", ObjectOf({{"id", Primary()}, {"layer", tileLayer}, {"moveTo", tileLayer + 1}}));
        choose(tileLayer + 1);
    }
    ImGui::EndDisabled();
    TwoDimensionalRect("layer-down");
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Remove Layer"))) {
        Call("tilemap.layer_set", ObjectOf({{"id", Primary()}, {"layer", tileLayer}, {"remove", true}}));
        choose(-1);
        return;
    }
    TwoDimensionalRect("layer-remove");
    if (ImGui::BeginTable("##layer", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 90);
        auto field = [&](const char* label) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
        };
        field(Tr("Name"));
        if (ImGui::InputText("##layerName", &tileLayerName, ImGuiInputTextFlags_EnterReturnsTrue) && !tileLayerName.empty())
            setLayer(ObjectOf({{"name", tileLayerName}}));
        TwoDimensionalRect("layer-name");
        field(Tr("Sorting Layer"));
        int sorting = layer["layer"].asInt(tm->layer);
        if (ImGui::InputInt("##layerSorting", &sorting)) setLayer(ObjectOf({{"layer", sorting}}), "layer-sorting");
        field(Tr("Visible"));
        bool visible = layer["visible"].asBool(true);
        if (ImGui::Checkbox("##layerVisible", &visible)) setLayer(ObjectOf({{"visible", visible}}));
        TwoDimensionalRect("layer-visible");
        field(Tr("Collision"));
        bool collision = layer["collision"].asBool(true);
        if (ImGui::Checkbox("##layerCollision", &collision)) setLayer(ObjectOf({{"collision", collision}}));
        field(Tr("Y Sort"));
        bool ySort = layer["ySort"].asBool(tm->ySort);
        if (ImGui::Checkbox("##layerYSort", &ySort)) setLayer(ObjectOf({{"ySort", ySort}}));
        ImGui::EndTable();
    }
}

// ----- Tilemap: the brush's tile rule ---------------------------------------------------

void NativeEditor::Impl::TileRuleTools() {
    if (!ImGui::CollapsingHeader(Tr("Tile Rule"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    const Tilemap* tm = engine.GetScene().Get<Tilemap>(Primary());
    if (!tm || tileBrush == ' ') return;
    const std::string key(1, tileBrush);
    TilesetLookup lookup = engine.Assets().Tilesets();
    const TileRules rules = BuildTileRules(*tm, &lookup);
    auto found = rules.tiles.find(tileBrush);
    if (found == rules.tiles.end()) return;
    const TileDef& def = found->second;
    // The legend entry, or the rule as resolved from the tileset (the first edit copies it into the legend).
    Json rule = tm->legend[key];
    if (!rule.isObject()) {
        rule = Json::MakeObject();
        rule["frame"] = def.frame;
        if (!def.variants.empty()) {
            rule["variants"] = Json::MakeArray();
            for (int f : def.variants) rule["variants"].push(f);
        }
        if (def.autotile != AutoTile::None) {
            rule["autotile"] = def.autotile == AutoTile::Sides ? "sides" : "blob";
            rule["frames"] = Json::MakeArray();
            for (int f : def.autoFrames) rule["frames"].push(f);
        }
        if (!def.connects.empty()) rule["connects"] = def.connects;
        if (!def.edges) rule["edges"] = false;
        const char* modes[] = {"none", "solid", "oneway"};
        if (def.collision == TileCollision::Shape) {
            rule["collision"] = Json::MakeArray();
            for (const auto& p : def.shape) rule["collision"].push(Json::Array{p.x, p.y});
        } else if (static_cast<int>(def.collision) < 3) {
            rule["collision"] = modes[static_cast<int>(def.collision)];
        }
        if (def.data.size() > 0) rule["data"] = def.data;
        if (!def.shadow) rule["shadow"] = false;
        if (!def.animation.empty()) {
            rule["animation"]["frames"] = Json::MakeArray();
            for (int f : def.animation) rule["animation"]["frames"].push(f);
            rule["animation"]["fps"] = def.fps;
        }
    }
    auto write = [&]() {
        Call("tilemap.rule_set", ObjectOf({{"id", Primary()}, {"char", key}, {"values", rule}, {"merge", "tile-rule:" + key}}));
        tileInfoId = 0;
    };
    if (!ImGui::BeginTable("##rule", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 90);
    auto field = [&](const char* label) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
    };
    field(Tr("Frame"));
    int frame = rule["frame"].asInt();
    if (ImGui::InputInt("##ruleFrame", &frame, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
        rule["frame"] = std::max(0, frame);
        write();
    }
    TwoDimensionalRect("rule-frame");
    field(Tr("Collision"));
    const char* collisions[] = {"none", "solid", "oneway", "slope-up", "slope-down", "half-bottom", "half-top"};
    if (ImGui::BeginCombo("##ruleCollision", rule["collision"].isArray() ? "polygon" : rule["collision"].asString("none").c_str())) {
        for (const char* value : collisions)
            if (ImGui::Selectable(value)) {
                rule["collision"] = value;
                write();
            }
        ImGui::EndCombo();
    }
    TwoDimensionalRect("rule-collision");
    field(Tr("Casts Shadows"));
    bool shadow = rule["shadow"].asBool(true);
    if (ImGui::Checkbox("##ruleShadow", &shadow)) {
        if (shadow) rule.erase("shadow");
        else rule["shadow"] = false;
        write();
    }
    HelpTooltip(Tr("Off: Light2D shines through this tile even when it collides (water, glass)."));
    TwoDimensionalRect("rule-shadow");
    field(Tr("Animated Tile"));
    bool animated = rule.has("animation");
    if (ImGui::Checkbox("##animated", &animated)) {
        if (animated) {
            rule["animation"]["frames"] = Json::Array{frame, frame + 1};
            rule["animation"]["fps"] = 8;
        } else {
            rule.erase("animation");
        }
        write();
    }
    TwoDimensionalRect("rule-animated");
    if (animated) {
        field(Tr("FPS"));
        float fps = rule["animation"]["fps"].asFloat(8);
        if (ImGui::InputFloat("##tileFps", &fps, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            rule["animation"]["fps"] = std::clamp(fps, 0.01f, 240.0f);
            write();
        }
        field(Tr("Frame Count"));
        int count = static_cast<int>(rule["animation"]["frames"].size());
        if (ImGui::InputInt("##tileFrames", &count, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
            count = std::clamp(count, 1, 256);
            rule["animation"]["frames"] = Json::MakeArray();
            for (int i = 0; i < count; ++i) rule["animation"]["frames"].push(frame + i);
            write();
        }
    }
    // Data: one row per key (value typed as text: numbers, true/false, else a string), then a new key.
    const Json data = rule["data"].isObject() ? rule["data"] : Json::MakeObject();
    for (const auto& entry : data.members()) {
        ImGui::PushID(entry.first.c_str());
        field(entry.first.c_str());
        std::string text = DataValueText(entry.second);
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputText("##value", &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
            rule["data"][entry.first] = ParseDataValue(text);
            write();
        }
        TwoDimensionalRect("rule-data:" + entry.first);
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) {
            rule["data"].erase(entry.first);
            if (rule["data"].size() == 0) rule.erase("data");
            write();
        }
        ImGui::PopID();
    }
    field(Tr("New Data"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##tileDataKey", Tr("key, then Enter"), &tileDataKey, ImGuiInputTextFlags_EnterReturnsTrue) && !tileDataKey.empty() &&
        !data.has(tileDataKey)) {
        rule["data"][tileDataKey] = 0;
        write();
        tileDataKey.clear();
    }
    TwoDimensionalRect("rule-data-key");
    ImGui::EndTable();
}

// ----- Sprite Tools -------------------------------------------------------------------

void NativeEditor::Impl::SpriteToolsPanel() {
    const Sprite* sp = engine.GetScene().Get<Sprite>(Primary());
    if (!sp) return;
    ImGui::TextUnformatted(Tr("Sprite Tools"));
    if (ImGui::CollapsingHeader(Tr("Sheet Slicer"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##slicePath", &slicePath);
        TwoDimensionalRect("atlas-path");
        HelpTooltip(Tr("Output .atlas.json path. Existing files are not overwritten."));
        int size[2] = {sliceWidth, sliceHeight};
        ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(Tr("Cell Pixels")).x + ImGui::GetStyle().ItemInnerSpacing.x));
        if (ImGui::InputInt2(Tr("Cell Pixels"), size)) {
            sliceWidth = std::clamp(size[0], 1, 4096);
            sliceHeight = std::clamp(size[1], 1, 4096);
        }
        TwoDimensionalRect("slice-size");
        ImGui::Checkbox(Tr("Trim"), &sliceTrim);
        HelpTooltip(Tr("Cut each cell to its opaque pixels; frames keep the cell as their size, so they do not jump."));
        TwoDimensionalRect("slice-trim");
        // Slicing points the sprite at its new atlas, frame 0; the pivot is the bottom center (feet).
        // A sprite drawn from an atlas slices that atlas's image again.
        std::string sheet = sp->texture;
        if (sheet.empty() && !sp->atlas.empty())
            if (auto atlas = engine.Assets().GetAtlas(sp->atlas)) sheet = (*atlas)["texture"].asString();
        auto slice = [&](Json args) {
            args["path"] = slicePath;
            args["texture"] = sheet;
            args["pivot"] = Json::Array{0.5, 0.0};
            if (Ok(Call("atlas.create", args)))
                Call("component.set", ObjectOf({{"id", Primary()}, {"type", "Sprite"}, {"values", ObjectOf({{"atlas", slicePath}, {"frame", 0}})}}));
        };
        ImGui::BeginDisabled(sheet.empty());
        if (ImGui::Button(Tr("Slice Grid"))) slice(ObjectOf({{"cellWidth", sliceWidth}, {"cellHeight", sliceHeight}, {"trim", sliceTrim}}));
        TwoDimensionalRect("slice");
        ImGui::SameLine();
        if (ImGui::Button(Tr("Find Sprites"))) slice(ObjectOf({{"auto", true}}));
        HelpTooltip(Tr("One frame per group of opaque pixels, found by the transparent space around it (top to bottom, left to right)."));
        TwoDimensionalRect("slice-auto");
        ImGui::EndDisabled();
        if (!sp->atlas.empty()) ImGui::TextDisabled("%s", sp->atlas.c_str());
    }
    if (!ImGui::CollapsingHeader(Tr("Collision Polygon"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    const Collider2D* collider = engine.GetScene().Get<Collider2D>(Primary());
    Json points = collider && collider->shape == "polygon" ? collider->points : Json::MakeArray();
    auto write = [&](const Json& value, const std::string& merge = std::string()) {
        Json args = ObjectOf({{"id", Primary()}, {"points", value}});
        if (!merge.empty()) args["merge"] = merge;
        return Call("sprite.collision", args);
    };
    if (ImGui::Button(Tr("Fit to Image"))) Call("sprite.outline", ObjectOf({{"id", Primary()}, {"alphaCutoff", outlineCutoff}}));
    HelpTooltip(Tr("A convex outline (8 corners at most) around the opaque pixels of the current frame."));
    TwoDimensionalRect("collision-fit");
    ImGui::SameLine();
    if (ImGui::Button(Tr("Create Rectangle"))) {
        points = Json::Array{Json::Array{-0.5, -0.5}, Json::Array{0.5, -0.5}, Json::Array{0.5, 0.5}, Json::Array{-0.5, 0.5}};
        write(points);
    }
    TwoDimensionalRect("collision-create");
    ImGui::SameLine();
    ImGui::BeginDisabled(collisionPoint < 0 || static_cast<size_t>(collisionPoint) >= points.size() || points.size() <= 3);
    if (ImGui::Button(Tr("Remove Point"))) {
        points.items().erase(points.items().begin() + collisionPoint);
        write(points);
        collisionPoint = -1;
    }
    TwoDimensionalRect("collision-remove");
    ImGui::EndDisabled();
    ImGui::Checkbox(Tr("Snap to Pixels"), &collisionSnap);
    TwoDimensionalRect("collision-snap");

    // Canvas: the sprite's picture in its local space, the polygon over it.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float side = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 canvasSize(side, std::min(260.0f, side));
    ImGui::InvisibleButton("##collisionCanvas", canvasSize);
    TwoDimensionalRect("collision-canvas");
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, origin + canvasSize, ImGui::GetColorU32(ImGuiCol_FrameBg));
    const Mat4 toLocal = engine.GetScene().WorldMatrix(Primary()).Inverse();
    const RenderItem* picture = nullptr;
    const std::vector<RenderItem> items = GatherRenderItems(engine.GetScene(), &engine.Assets());
    for (const RenderItem& item : items)
        if (item.id == Primary()) picture = &item;
    float extent = 0.5f;
    if (picture)
        for (const Vec3& p : picture->mesh->positions) {
            const Vec3 v = (toLocal * picture->world).TransformPoint(p);
            extent = std::max(extent, std::max(std::fabs(v.x), std::fabs(v.y)) * 1.15f);
        }
    for (const Json& p : points.items())
        if (p.isArray() && p.size() == 2) extent = std::max(extent, std::max(std::fabs(p[0].asFloat()), std::fabs(p[1].asFloat())) * 1.15f);
    const float zoom = std::min(canvasSize.x, canvasSize.y) / (2 * extent);
    const ImVec2 center = origin + canvasSize * 0.5f;
    auto screen = [&](float x, float y) { return ImVec2(center.x + x * zoom, center.y - y * zoom); };
    // One sprite pixel in local units: snapping grid.
    const float pixel = 1.0f / std::max(0.001f, sp->pixelsPerUnit);
    auto local = [&](const ImVec2& pos) {
        float x = (pos.x - center.x) / zoom, y = (center.y - pos.y) / zoom;
        if (collisionSnap) {
            x = std::round(x / pixel) * pixel;
            y = std::round(y / pixel) * pixel;
        }
        return Json::Array{std::round(x * 1000) / 1000, std::round(y * 1000) / 1000};
    };
    draw->PushClipRect(origin, origin + canvasSize, true);
    if (picture && picture->textureOverride && engine.Gpu() && picture->mesh->positions.size() >= 4) {
        const Mat4 m = toLocal * picture->world;
        const ImTextureID texture = sp->pixelArt ? NearestTexture(engine.Gpu()->ImageView(picture->textureOverride))
                                                 : ViewTexture(engine.Gpu()->ImageView(picture->textureOverride));
        const auto& mesh = *picture->mesh;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            ImVec2 p[3], uv[3];
            for (int k = 0; k < 3; ++k) {
                const uint32_t index = mesh.indices[i + static_cast<size_t>(k)];
                const Vec3 v = m.TransformPoint(mesh.positions[index]);
                p[k] = screen(v.x, v.y);
                uv[k] = ImVec2(mesh.uvs[index * 2] * picture->uvScale[0] + picture->uvOffset[0], mesh.uvs[index * 2 + 1] * picture->uvScale[1] + picture->uvOffset[1]);
            }
            draw->AddImageQuad(texture, p[0], p[1], p[2], p[2], uv[0], uv[1], uv[2], uv[2], IM_COL32(255, 255, 255, 170));
        }
    }
    const ImU32 line = ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (size_t i = 0; i < points.size(); ++i) {
        const Json& a = points[i];
        const Json& b = points[(i + 1) % points.size()];
        if (a.size() != 2 || b.size() != 2) continue;
        const ImVec2 pa = screen(a[0].asFloat(), a[1].asFloat()), pb = screen(b[0].asFloat(), b[1].asFloat());
        draw->AddLine(pa, pb, line, 2);
        draw->AddCircleFilled(pa, 5, ImGui::GetColorU32(collisionPoint == static_cast<int>(i) ? ImGuiCol_ButtonActive : ImGuiCol_Text));
        twoDimensionalRects["collision-point:" + std::to_string(i)] = {pa.x - 7, pa.y - 7, 14, 14};
        if (hovered && ImGui::IsMouseClicked(0) && (mouse.x - pa.x) * (mouse.x - pa.x) + (mouse.y - pa.y) * (mouse.y - pa.y) < 100) {
            collisionPoint = static_cast<int>(i);
            ++collisionSerial;
        }
    }
    draw->PopClipRect();
    if (hovered && ImGui::IsMouseDoubleClicked(0) && points.size() >= 3 && points.size() < 64) {
        // The new corner goes into the edge nearest to the click.
        const Json at = local(mouse);
        size_t edge = points.size() - 1;
        float best = INFINITY;
        for (size_t i = 0; i < points.size(); ++i) {
            const ImVec2 pa = screen(points[i][0].asFloat(), points[i][1].asFloat());
            const ImVec2 pb = screen(points[(i + 1) % points.size()][0].asFloat(), points[(i + 1) % points.size()][1].asFloat());
            const ImVec2 ab = pb - pa;
            const float t = std::clamp(((mouse.x - pa.x) * ab.x + (mouse.y - pa.y) * ab.y) / std::max(1e-6f, ab.x * ab.x + ab.y * ab.y), 0.0f, 1.0f);
            const ImVec2 q = pa + ab * t;
            const float d = (mouse.x - q.x) * (mouse.x - q.x) + (mouse.y - q.y) * (mouse.y - q.y);
            if (d < best) {
                best = d;
                edge = i;
            }
        }
        points.items().insert(points.items().begin() + static_cast<std::ptrdiff_t>(edge + 1), at);
        write(points);
    }
    if (collisionPoint >= 0 && static_cast<size_t>(collisionPoint) < points.size() && ImGui::IsMouseDragging(0) && ImGui::IsItemActive()) {
        const Json at = local(mouse);
        if (points[collisionPoint] != at) {
            points[collisionPoint] = at;
            write(points, "polygon:" + std::to_string(collisionSerial));
        }
    }
    HelpTooltip(Tr("Drag vertices. Double-click adds a point. Scene undo restores edits."));
}

}  // namespace oe
