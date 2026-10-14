// Property tracks in the Animation panel: one lane per keyframed component field of the animated entity or its
// children, under the timeline ruler and on its time scale. Keys are diamonds: click selects (and seeks),
// drag moves, double-click on a lane adds one, right-click removes one. The selected key's time and value are
// edited below the lanes. Every change is animation.set_tracks.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"

#include "app/Engine.h"
#include "scene/PropertyTracks.h"

namespace oe {

namespace {

std::string TrackLabel(const Json& track) {
    const std::string target = track["target"].asString();
    return (target.empty() ? std::string() : target + " / ") + track["component"].asString() + "." + track["field"].asString();
}

// Key values of these field types blend between keys; the others hold until the next key.
bool Blends(const std::string& oeType) { return oeType == "float" || oeType == "vec3" || oeType == "color"; }

constexpr ImU32 kKey = IM_COL32(150, 190, 255, 255);
constexpr ImU32 kKeySelected = IM_COL32(255, 211, 95, 255);

void Diamond(ImDrawList* draw, ImVec2 c, float r, ImU32 color) {
    draw->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), color);
    draw->AddQuad(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), IM_COL32(0, 0, 0, 170));
}

}  // namespace

bool NativeEditor::Impl::AnimationWriteTracks(const Json& state, const Json& tracks, const std::string& merge) {
    Json args = ObjectOf({{"id", state["id"]}, {"clip", state["clip"]}, {"tracks", tracks}});
    if (!merge.empty()) args["merge"] = merge;
    return Ok(Call("animation.set_tracks", args));
}

void NativeEditor::Impl::AnimationTracks(const Json& state, float x0, float scale, float gutter) {
    const Json tracks = state["tracks"].isArray() ? state["tracks"] : Json::MakeArray();
    const double duration = state["duration"].asNumber(), current = state["time"].asNumber();
    const bool sprite = state["type"].asString() == "SpriteAnimation";
    const float laneHeight = ImGui::GetFrameHeight();
    const float laneWidth = std::max(40.0f, ImGui::GetContentRegionAvail().x - gutter);
    if (animationTrack >= static_cast<int>(tracks.size())) animationTrack = animationKey = -1;
    if (animationTrack >= 0 && animationKey >= static_cast<int>(tracks[animationTrack]["keys"].size())) animationKey = -1;

    // The Add Property button sits in the panel's toolbar beside Add Event; without tracks nothing takes room here.
    AnimationAddPropertyPopup(state, tracks);
    if (tracks.size() == 0) return;
    ImGui::Dummy(ImVec2(0, 2));
    auto timeAt = [&](float x) {
        double t = Clamp((x - x0) / std::max(1e-3f, scale), 0, static_cast<float>(duration));
        if (sprite) {  // sprite keys sit on frame starts
            double nearest = 1e100, best = t;
            for (const Json& start : state["frameTimes"].items())
                if (std::fabs(start.asNumber() - t) < nearest) nearest = std::fabs(start.asNumber() - t), best = start.asNumber();
            return best;
        }
        return std::round(t * 60.0) / 60.0;  // model keys on fixed steps
    };
    auto seek = [&](double t) { Call("animation.preview", ObjectOf({{"time", t}, {"playing", false}})); };
    auto oeTypeOf = [&](const Json& track) {
        auto it = typeByName.find(track["component"].asString());
        return it == typeByName.end() ? std::string() : (*it->second)["schema"]["properties"][track["field"].asString()]["x-oe-type"].asString("");
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const Json& track = tracks[i];
        const Json& keys = track["keys"];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const bool trackSelected = animationTrack == static_cast<int>(i);
        // Label column: the property, its interpolation and the add-key button; right-click for more.
        if (gutter > 0) {
            const float keyButton = laneHeight;
            ImGui::InvisibleButton("##label", ImVec2(gutter - keyButton - 6, laneHeight));
            if (ImGui::IsItemClicked()) animationTrack = static_cast<int>(i), animationKey = -1;
            AnimationRect("track-label:" + std::to_string(i));
            const std::string label = TrackLabel(track);
            HelpTooltip(label + "\n" + track["interpolation"].asString("linear"));
            draw->PushClipRect(row, row + ImVec2(gutter - keyButton - 6, laneHeight), true);
            draw->AddText(row + ImVec2(4, (laneHeight - ImGui::GetFontSize()) * 0.5f),
                          ImGui::GetColorU32(trackSelected ? ImGuiCol_Text : ImGuiCol_TextDisabled), label.c_str());
            draw->PopClipRect();
            if (ImGui::BeginPopupContextItem("##trackMenu")) {
                const char* const modes[3][2] = {{"linear", "Linear"}, {"smooth", "Smooth"}, {"step", "Step"}};  // value, menu label
                for (const auto& entry : modes) {
                    const char* mode = entry[0];
                    if (ImGui::MenuItem(Tr(entry[1]), nullptr, track["interpolation"].asString("linear") == mode)) {
                        Json next = tracks;
                        if (std::string(mode) == "linear") next[i].erase("interpolation");
                        else next[i]["interpolation"] = mode;
                        AnimationWriteTracks(state, next);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem(Tr("Remove Property"))) {
                    Json next = tracks;
                    next.items().erase(next.items().begin() + static_cast<std::ptrdiff_t>(i));
                    AnimationWriteTracks(state, next);
                    animationTrack = animationKey = -1;
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine(0, 2);
            if (ImGui::Button(kIconPlus, ImVec2(keyButton, laneHeight))) {
                // A key at the playhead holding the value the property has there.
                Json next = tracks;
                const double t = timeAt(x0 + static_cast<float>(current) * scale);
                const Json value = SamplePropertyTrack(track, t, !Blends(oeTypeOf(track)));
                Json& list = next[i]["keys"];
                size_t at = 0;
                while (at < list.size() && list[at]["time"].asNumber() < t - 1e-9) ++at;
                if (at < list.size() && std::fabs(list[at]["time"].asNumber() - t) < 1e-9) list[at]["value"] = value;
                else list.items().insert(list.items().begin() + static_cast<std::ptrdiff_t>(at), ObjectOf({{"time", t}, {"value", value}}));
                if (AnimationWriteTracks(state, next))
                    animationTrack = static_cast<int>(i), animationKey = static_cast<int>(at);
            }
            AnimationRect("track-key:" + std::to_string(i));
            HelpTooltip(Tr("Add a key at the playhead"));
            ImGui::SameLine(0, 4);
        }
        // The lane.
        const ImVec2 lane = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##lane", ImVec2(laneWidth, laneHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        AnimationRect("track:" + std::to_string(i));
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        draw->AddRectFilled(lane, lane + ImVec2(laneWidth, laneHeight), trackSelected ? IM_COL32(92, 148, 255, 34) : (i % 2 ? IM_COL32(255, 255, 255, 8) : IM_COL32(255, 255, 255, 14)), 2);
        const float mid = lane.y + laneHeight * 0.5f;
        // Segments between keys show where the value changes (flat for step).
        for (size_t k = 0; k + 1 < keys.size(); ++k) {
            const float a = x0 + keys[k]["time"].asFloat() * scale, b = x0 + keys[k + 1]["time"].asFloat() * scale;
            const bool same = keys[k]["value"] == keys[k + 1]["value"];
            draw->AddLine(ImVec2(a, mid), ImVec2(b, mid), same ? IM_COL32(255, 255, 255, 40) : IM_COL32(150, 190, 255, 110), same ? 1.0f : 2.0f);
        }
        int hit = -1;
        for (size_t k = 0; k < keys.size(); ++k) {
            const float x = x0 + keys[k]["time"].asFloat() * scale;
            const bool chosen = trackSelected && animationKey == static_cast<int>(k);
            Diamond(draw, ImVec2(x, mid), chosen ? 6.5f : 5.5f, chosen ? kKeySelected : kKey);
            animationRects["key:" + std::to_string(i) + ":" + std::to_string(k)] = {x - 7, mid - 7, 14, 14};
            if (hovered && std::fabs(mouse.x - x) < 7 && std::fabs(mouse.y - mid) < 9) hit = static_cast<int>(k);
        }
        const float head = x0 + static_cast<float>(current) * scale;
        draw->AddLine(ImVec2(head, lane.y), ImVec2(head, lane.y + laneHeight), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2);
        if (hit >= 0) {
            const Json& key = keys[static_cast<size_t>(hit)];
            ImGui::SetTooltip("%.3f s\n%s", key["time"].asNumber(), key["value"].dump().c_str());
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            animationTrack = static_cast<int>(i);
            animationKey = hit;
            if (hit >= 0) {
                animationKeyDragging = true;
                animationKeyDragTracks = tracks;
                ++dragSerial["animation-key"];
                seek(keys[static_cast<size_t>(hit)]["time"].asNumber());
            } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                const double t = timeAt(mouse.x);
                Json next = tracks;
                Json& list = next[i]["keys"];
                size_t at = 0;
                while (at < list.size() && list[at]["time"].asNumber() < t) ++at;
                list.items().insert(list.items().begin() + static_cast<std::ptrdiff_t>(at),
                                    ObjectOf({{"time", t}, {"value", SamplePropertyTrack(track, t, !Blends(oeTypeOf(track)))}}));
                if (AnimationWriteTracks(state, next)) animationKey = static_cast<int>(at);
            } else {
                seek(timeAt(mouse.x));
            }
        }
        if (hovered && hit >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && keys.size() > 1) {
            Json next = tracks;
            next[i]["keys"].items().erase(next[i]["keys"].items().begin() + hit);
            AnimationWriteTracks(state, next);
            animationKey = -1;
        }
        if (active && animationKeyDragging && trackSelected && animationKey >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            // The dragged key keeps its place among the others: it stops at its neighbours.
            Json next = animationKeyDragTracks;
            Json& list = next[i]["keys"];
            if (static_cast<size_t>(animationKey) < list.size()) {
                double t = timeAt(mouse.x);
                if (animationKey > 0) t = std::max(t, list[animationKey - 1]["time"].asNumber());
                if (animationKey + 1 < static_cast<int>(list.size())) t = std::min(t, list[animationKey + 1]["time"].asNumber());
                if (std::fabs(list[animationKey]["time"].asNumber() - t) > 1e-9 || next != tracks) {
                    list[animationKey]["time"] = t;
                    if (next != tracks)
                        AnimationWriteTracks(state, next, "animation-key:" + std::to_string(Primary()) + ":" + state["clip"].asString() + ":" +
                                                              std::to_string(dragSerial["animation-key"]));
                }
            }
        }
        ImGui::PopID();
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) animationKeyDragging = false;
    if (animationTrack >= 0 && animationKey >= 0) {
        // Adding a property/key can select an index absent from this frame's original snapshot.
        // Read the accepted edit, including inserted values, before drawing the selected key.
        const Json latest = engine.AnimationPreviewCall("state", Json());
        AnimationKeyEditor(latest, latest["tracks"]);
    }
}

void NativeEditor::Impl::AnimationKeyEditor(const Json& state, Json tracks) {
    if (animationTrack < 0 || animationKey < 0) return;
    const size_t i = static_cast<size_t>(animationTrack), k = static_cast<size_t>(animationKey);
    if (!tracks.isArray() || i >= tracks.size()) {
        animationTrack = animationKey = -1;
        return;
    }
    if (!tracks[i]["keys"].isArray() || k >= tracks[i]["keys"].size()) {
        animationKey = -1;
        return;
    }
    Json& key = tracks[i]["keys"][k];
    const Json& track = tracks[i];
    auto it = typeByName.find(track["component"].asString());
    const Json schema = it == typeByName.end() ? Json() : (*it->second)["schema"]["properties"][track["field"].asString()];
    const std::string oeType = schema["x-oe-type"].asString("");
    const std::string merge = "animation-key-value:" + std::to_string(Primary()) + ":" + std::to_string(i) + ":" + std::to_string(k) + ":" +
                              std::to_string(dragSerial["animation-key-value"]);
    ImGui::PushID("##keyEditor");
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kKeySelected), "%s", TrackLabel(track).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("Time"));
    ImGui::SameLine();
    float keyTime = key["time"].asFloat();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (ImGui::InputFloat("##time", &keyTime, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
        const double lo = k > 0 ? tracks[i]["keys"][k - 1]["time"].asNumber() : 0.0;
        const double hi = k + 1 < tracks[i]["keys"].size() ? tracks[i]["keys"][k + 1]["time"].asNumber() : state["duration"].asNumber();
        key["time"] = std::clamp(static_cast<double>(keyTime), lo, std::max(lo, hi));
        AnimationWriteTracks(state, tracks);
    }
    AnimationRect("key-time");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("Value"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 8, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 8));
    Json value = key["value"];
    bool changed = false, drag = false;
    if (oeType == "float") {
        float v = value.asFloat();
        if (schema.has("minimum") && schema.has("maximum")) changed = ImGui::SliderFloat("##value", &v, schema["minimum"].asFloat(), schema["maximum"].asFloat(), "%.3g");
        else changed = ImGui::DragFloat("##value", &v, 0.01f, 0, 0, "%.3g");
        drag = true;
        if (changed) value = Json(v);
    } else if (oeType == "int") {
        int v = value.asInt();
        changed = ImGui::DragInt("##value", &v, 0.2f);
        drag = true;
        if (changed) value = Json(v);
    } else if (oeType == "bool") {
        bool v = value.asBool();
        changed = ImGui::Checkbox("##value", &v);
        if (changed) value = Json(v);
    } else if (oeType == "vec3" || oeType == "color") {
        // Reflection also accepts {x,y,z}/{r,g,b} and hex colors; convert those to widget triples.
        const ComponentType* type = TypeRegistry::Find(track["component"].asString());
        const FieldInfo* field = type ? type->FindField(track["field"].asString()) : nullptr;
        if (field) {
            auto scratch = type->makePool();
            void* component = scratch->Add(1);
            if (FieldFromJson(*field, component, value, nullptr)) value = FieldToJson(*field, component);
        }
        if (!value.isArray() || value.size() != 3) {
            ImGui::PopID();
            return;
        }
        float v[3] = {value[0].asFloat(), value[1].asFloat(), value[2].asFloat()};
        changed = oeType == "color" ? ImGui::ColorEdit3("##value", v, ImGuiColorEditFlags_Float) : ImGui::DragFloat3("##value", v, 0.02f, 0, 0, "%.3g");
        drag = true;
        if (changed) value = Json(Json::Array{v[0], v[1], v[2]});
    } else if (schema.has("enum")) {
        if (ImGui::BeginCombo("##value", value.asString().c_str())) {
            for (const Json& option : schema["enum"].items())
                if (ImGui::Selectable(option.asString().c_str(), option == value)) value = option, changed = true;
            ImGui::EndCombo();
        }
    } else {
        std::string text = value.isString() ? value.asString() : value.dump();
        if (ImGui::InputText("##value", &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
            const bool number = oeType == "frame" && !text.empty() && text.find_first_not_of("0123456789") == std::string::npos;
            value = number ? Json(std::atoi(text.c_str())) : Json(text);
            changed = true;
        }
    }
    if (drag && ImGui::IsItemActivated()) ++dragSerial["animation-key-value"];
    AnimationRect("key-value");
    if (changed) {
        key["value"] = value;
        AnimationWriteTracks(state, tracks, drag ? merge : std::string());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(tracks[i]["keys"].size() <= 1);
    if (ImGui::SmallButton(kIconTrash2)) {
        tracks[i]["keys"].items().erase(tracks[i]["keys"].items().begin() + static_cast<std::ptrdiff_t>(k));
        AnimationWriteTracks(state, tracks);
        animationKey = -1;
    }
    ImGui::EndDisabled();
    AnimationRect("key-remove");
    HelpTooltip(Tr("Remove Key"));
    ImGui::PopID();
}

void NativeEditor::Impl::AnimationAddPropertyPopup(const Json& state, const Json& tracks) {
    if (!ImGui::BeginPopup("##addProperty")) return;
    if (ImGui::IsWindowAppearing()) {
        animationPropertyFilter.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    ImGui::InputTextWithHint("##find", Tr("Search properties"), &animationPropertyFilter);
    ImGui::Separator();
    // The animated entity and its children (breadth first, at most 64), each with its path of names.
    const EntityId owner = static_cast<EntityId>(state["id"].asNumber());
    std::vector<std::pair<EntityId, std::string>> entities{{owner, std::string()}};
    for (size_t at = 0; at < entities.size() && entities.size() < 64; ++at)
        for (const EntityRow& row : rows)
            if (row.parent == entities[at].first && entities.size() < 64)
                entities.push_back({row.id, entities[at].second.empty() ? row.name : entities[at].second + "/" + row.name});
    ImGui::BeginChild("##properties", ImVec2(ImGui::GetFontSize() * 18, ImGui::GetFontSize() * 20));
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string filter = lower(animationPropertyFilter);
    for (const auto& entity : entities) {
        const EntityRow* row = Row(entity.first);
        if (!row) continue;
        ImGui::PushID(static_cast<int>(entity.first));
        const std::string who = entity.second.empty() ? row->name + " " + Tr("(this entity)") : entity.second;
        bool header = false;
        for (const std::string& component : row->components) {
            if (component == "SpriteAnimation" || component == "Animator") continue;
            auto it = typeByName.find(component);
            if (it == typeByName.end()) continue;
            for (const auto& field : (*it->second)["schema"]["properties"].members()) {
                const std::string oeType = field.second["x-oe-type"].asString("");
                if (oeType == "json" || oeType == "entity") continue;
                const std::string label = component + "." + field.first;
                if (!filter.empty() && lower(who + " " + label).find(filter) == std::string::npos) continue;
                if (!header) {
                    ImGui::SeparatorText(who.c_str());
                    header = true;
                }
                bool taken = false;
                for (const Json& t : tracks.items())
                    taken = taken || (t["target"].asString() == entity.second && t["component"].asString() == component && t["field"].asString() == field.first);
                if (ImGui::Selectable(label.c_str(), false, taken ? ImGuiSelectableFlags_Disabled : 0)) {
                    // The first key holds the value the property has now, at the playhead.
                    const Json got = Call("entity.get", ObjectOf({{"id", entity.first}}), true);
                    Json value = got["result"]["components"][component][field.first];
                    if (value.isNull()) value = (*it->second)["defaults"][field.first];
                    Json track = ObjectOf({{"component", component}, {"field", field.first}});
                    if (!entity.second.empty()) track["target"] = entity.second;
                    track["keys"] = Json::Array{ObjectOf({{"time", state["time"]}, {"value", value}})};
                    Json next = tracks;
                    next.push(track);
                    if (AnimationWriteTracks(state, next)) {
                        animationTrack = static_cast<int>(next.size()) - 1, animationKey = 0;
                        ImGui::CloseCurrentPopup();
                    }
                }
                AnimationRect("property:" + std::to_string(entity.first) + ":" + label);
                if (field.second.has("description")) HelpTooltip(field.second["description"].asString());
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

}  // namespace oe
