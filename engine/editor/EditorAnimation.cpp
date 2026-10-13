#include "editor/EditorInternal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "app/Engine.h"
#include "editor/EditorText.h"
#include "scene/Components.h"

namespace oe {
namespace {
void AnimationSameLine(const char* label, bool checkbox = false) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float width = ImGui::CalcTextSize(label).x + (checkbox ? ImGui::GetFrameHeight() : ImGui::GetStyle().FramePadding.x * 2);
    if (right - ImGui::GetItemRectMax().x >= width + ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine();
}
}

void NativeEditor::Impl::AnimationRect(const std::string& name) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    animationRects[name] = {a.x, a.y, b.x - a.x, b.y - a.y};
}

void NativeEditor::Impl::AnimationWriteEvents(const Json& state, const Json& events, const std::string& merge) {
    Json markers = Json::MakeArray();
    for (const Json& event : events.items()) {
        Json item = Json::MakeObject(); item["name"] = event["name"];
        if (state["type"].asString() == "SpriteAnimation") item["frame"] = event["frame"];
        else item["time"] = event["time"];
        markers.push(item);
    }
    Json args = Json::MakeObject(); args["id"] = state["id"]; args["clip"] = state["clip"]; args["events"] = markers;
    if (!merge.empty()) args["merge"] = merge;
    Call("animation.set_events", args);
}

void NativeEditor::Impl::AnimationTick(double dt) {
    if (!showAnimation || InPlaySession()) { animationAccumulator = 0; return; }
    Json state = engine.AnimationPreviewCall("state", Json());
    if (!state["active"].asBool() || !state["playing"].asBool()) { animationAccumulator = 0; return; }
    animationAccumulator += std::max(0.0, std::min(dt, 0.25));
    const int frames = static_cast<int>(animationAccumulator / Engine::kFixedDt);
    if (frames > 0) {
        animationAccumulator -= frames * Engine::kFixedDt;
        Call("animation.preview", ObjectOf({{"frames", frames}}));
    }
}

void NativeEditor::Impl::AnimationPanel() {
    animationRects.clear();
    if (focusAnimation) { ImGui::SetNextWindowFocus(); focusAnimation = false; }
    if (!ImGui::Begin(TrId("Animation").c_str(), &showAnimation)) {
        animationEditorFocused = false;
        ImGui::End();
        if (!showAnimation) Call("animation.preview_close", Json());
        return;
    }
    animationEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (InPlaySession()) {
        ImGui::TextWrapped("%s", Tr("Stop the game to preview animations.")); ImGui::End(); return;
    }
    const EntityRow* row = Row(Primary());
    if (!row || (!row->Has("SpriteAnimation") && !row->Has("MeshRenderer"))) {
        if (engine.AnimationPreviewCall("state", Json())["active"].asBool()) Call("animation.preview_close", Json());
        ImGui::TextWrapped("%s", Tr("Select a sprite or a model with animation clips.")); ImGui::End(); return;
    }
    Json state = engine.AnimationPreviewCall("state", Json());
    if (!state["active"].asBool() || state["id"].asInt() != static_cast<int>(Primary())) {
        const Json response = Call("animation.preview", ObjectOf({{"id", Primary()}}), true);
        if (!Ok(response)) { ImGui::TextWrapped("%s", response["error"]["message"].asString().c_str()); ImGui::End(); return; }
        state = response["result"]; animationEvent = animationDragIndex = -1; animationAccumulator = 0; animationNames.clear();
    }
    ImGui::TextUnformatted(row->name.c_str()); ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x * 0.4f));
    if (ImGui::BeginCombo("##animationClip", state["clip"].asString().c_str())) {
        for (const Json& clip : state["clips"].items()) {
            const std::string name = clip["name"].asString();
            if (ImGui::Selectable(name.c_str(), name == state["clip"].asString())) {
                const Json response = Call("animation.preview", ObjectOf({{"clip", name}, {"time", 0}, {"playing", false}}));
                if (Ok(response)) state = response["result"];
                animationEvent = animationDragIndex = -1; animationAccumulator = 0; animationNames.clear();
            }
        }
        ImGui::EndCombo();
    }
    const bool spriteClip = state["type"].asString() == "SpriteAnimation";
    AnimationRect("clip");
    const bool playing = state["playing"].asBool();
    AnimationSameLine(Tr(playing ? "Pause Preview" : "Play Preview"));
    if (ImGui::Button(Tr(playing ? "Pause Preview" : "Play Preview"))) {
        Call("animation.preview", ObjectOf({{"playing", !playing}})); animationAccumulator = 0;
    }
    AnimationRect("play"); AnimationSameLine(Tr("Step Preview"));
    if (ImGui::Button(Tr("Step Preview"))) Call("animation.preview", ObjectOf({{"frames", 1}, {"playing", false}}));
    AnimationRect("step"); AnimationSameLine(Tr("Restart Preview"));
    if (ImGui::Button(Tr("Restart Preview"))) Call("animation.preview", ObjectOf({{"time", state["speed"].asFloat() < 0 ? state["duration"].asFloat() : 0}}));
    AnimationRect("restart"); AnimationSameLine(Tr("Loop Preview"), true);
    bool loop = state["loop"].asBool();
    if (ImGui::Checkbox(Tr("Loop Preview"), &loop)) Call("animation.preview", ObjectOf({{"loop", loop}}));
    AnimationSameLine("                "); ImGui::SetNextItemWidth(75);
    float speed = state["speed"].asFloat();
    if (ImGui::DragFloat("##animationSpeed", &speed, 0.1f, -100, 100, "%.1fx")) Call("animation.preview", ObjectOf({{"speed", speed}}));
    HelpTooltip(Tr("Preview speed; negative plays backwards."));
    // An explicit seek pauses preview; authored component fields and simulation never change.
    const float duration = state["duration"].asFloat(), current = state["time"].asFloat();
    float timeValue = current;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##animationTime", &timeValue, 0, duration, "%.3f s"))
        Call("animation.preview", ObjectOf({{"time", timeValue}, {"playing", false}}));
    AnimationRect("time");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(80.0f, ImGui::GetContentRegionAvail().x), 64);
    ImGui::InvisibleButton("##animationTimeline", size, ImGuiButtonFlags_MouseButtonLeft); AnimationRect("timeline");
    const bool hovered = ImGui::IsItemHovered();
    const bool timelineActive = ImGui::IsItemActive();  // tooltip widgets below replace ImGui's last item
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, origin + size, ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
    const float scale = duration > 0 ? size.x / duration : 0;
    for (int i = 0; i <= 10; ++i) {
        const float x = origin.x + size.x * static_cast<float>(i) / 10;
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + 10), ImGui::GetColorU32(ImGuiCol_TextDisabled));
        if (i == 0 || i == 10 || (size.x > 240 && i % 2 == 0)) {
            char label[32]; std::snprintf(label, sizeof(label), "%.2f", duration * static_cast<float>(i) / 10);
            const float labelWidth = ImGui::CalcTextSize(label).x;
            draw->AddText(ImVec2(Clamp(x - labelWidth * 0.5f, origin.x, origin.x + size.x - labelWidth), origin.y + 43),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
        }
    }
    Json events = state["events"];
    int hit = -1;
    for (size_t i = 0; i < events.size(); ++i) {
        const float x = origin.x + events[i]["time"].asFloat() * scale;
        const ImVec2 a(x - 6, origin.y + 18), b(x + 6, origin.y + 37);
        const bool markerSelected = animationEvent == static_cast<int>(i);
        draw->AddTriangleFilled(ImVec2(x, a.y), ImVec2(a.x, b.y), b, ImGui::GetColorU32(markerSelected ? ImGuiCol_PlotHistogramHovered : ImGuiCol_PlotHistogram));
        animationRects["marker:" + std::to_string(i)] = {a.x, a.y, b.x - a.x, b.y - a.y};
        if (hovered && ImGui::IsMouseHoveringRect(a, b)) { hit = static_cast<int>(i); ImGui::SetTooltip("%s", events[i]["name"].asString().c_str()); }
    }
    const float playhead = origin.x + current * scale;
    draw->AddLine(ImVec2(playhead, origin.y), ImVec2(playhead, origin.y + size.y), ImGui::GetColorU32(ImGuiCol_Text), 2);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ++dragSerial["animation-event"];
        animationDragIndex = hit; animationDragEvents = events;
        if (hit >= 0) { animationEvent = hit; animationEventName = events[static_cast<size_t>(hit)]["name"].asString(); }
    }
    if (timelineActive && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float value = duration > 0 ? Clamp((ImGui::GetIO().MousePos.x - origin.x) / size.x, 0, 1) * duration : 0;
        if (animationDragIndex >= 0 && static_cast<size_t>(animationDragIndex) < animationDragEvents.size()) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                Json& marker = animationDragEvents[static_cast<size_t>(animationDragIndex)];
                if (spriteClip) {
                    const int frame = std::max(0, static_cast<int>(std::llround(value * state["fps"].asNumber())));
                    const int maxFrame = std::max(0, state["frameCount"].asInt() - 1);
                    marker["frame"] = std::min(frame, maxFrame); marker["time"] = marker["frame"].asNumber() / state["fps"].asNumber();
                } else marker["time"] = value;
                AnimationWriteEvents(state, animationDragEvents, "animation-marker:" + std::to_string(Primary()) + ":" +
                    state["clip"].asString() + ":" + std::to_string(dragSerial["animation-event"]));
            }
        } else Call("animation.preview", ObjectOf({{"time", value}, {"playing", false}}));
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) animationDragIndex = -1;
    ImGui::TextDisabled("%s", Tr("Preview changes no scene fields and runs no scripts or events."));
    if (ImGui::Button(Tr("Add Event"))) {
        Json marker = Json::MakeObject(); marker["name"] = "event";
        if (state["type"].asString() == "SpriteAnimation") {
            const int maxFrame = std::max(0, state["frameCount"].asInt() - 1);
            marker["frame"] = std::min(maxFrame, static_cast<int>(std::floor(current * state["fps"].asNumber() + 1e-4)));
            marker["time"] = marker["frame"].asNumber() / state["fps"].asNumber();
        } else marker["time"] = current;
        events.push(marker); AnimationWriteEvents(state, events); animationEvent = -1;
    }
    AnimationRect("add-event"); ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr(spriteClip ? "Sprite events save with the scene." : "Model events write the shared sidecar immediately; no undo."));
    if (ImGui::BeginTable("##animationEvents", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::max(70.0f, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupColumn(Tr("Event name")); ImGui::TableSetupColumn(Tr("Event time")); ImGui::TableSetupColumn(Tr("Actions"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < events.size(); ++i) {
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
            const std::string key = std::to_string(Primary()) + ":" + state["clip"].asString() + ":" + std::to_string(i);
            const std::string sourceName = events[i]["name"].asString();
            auto entry = animationNames.emplace(key, sourceName).first;
            std::string& name = entry->second;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue)) {
                events[i]["name"] = name; AnimationWriteEvents(state, events);
            }
            if (!ImGui::IsItemActive()) name = sourceName;
            AnimationRect("name:" + std::to_string(i)); ImGui::TableNextColumn();
            if (state["type"].asString() == "SpriteAnimation") {
                int frame = events[i]["frame"].asInt(); ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt("##eventFrame", &frame, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    events[i]["frame"] = frame; AnimationWriteEvents(state, events);
                }
            } else {
                float eventTime = events[i]["time"].asFloat(); ImGui::SetNextItemWidth(-1);
                if (ImGui::InputFloat("##eventTime", &eventTime, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                    events[i]["time"] = eventTime; AnimationWriteEvents(state, events);
                }
            }
            AnimationRect("event-time:" + std::to_string(i)); ImGui::TableNextColumn();
            if (ImGui::SmallButton(Tr("Remove Event"))) {
                Json remaining = Json::MakeArray(); for (size_t j = 0; j < events.size(); ++j) if (j != i) remaining.push(events[j]);
                AnimationWriteEvents(state, remaining); animationEvent = -1;
            }
            AnimationRect("remove-event:" + std::to_string(i)); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if (!showAnimation) Call("animation.preview_close", Json());
}

}  // namespace oe
