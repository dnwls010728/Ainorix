// 2D authoring tools of the Tiles panel: map layers and the tile rule of the brush (Tilemap). Sprite
// sheets and collision outlines have their own window (EditorSprite.cpp). Every change is a command.
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

}  // namespace oe
