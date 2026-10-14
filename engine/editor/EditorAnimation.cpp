#include "editor/EditorInternal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "app/Engine.h"
#include "core/Log.h"
#include "editor/EditorText.h"
#include "editor/EditorIcons.h"
#include "scene/Components.h"

namespace oe {
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
        animationEditorFocused = false; ImGui::End();
        if (!showAnimation) Call("animation.preview_close", Json());
        return;
    }
    animationEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (InPlaySession()) { ImGui::TextWrapped("%s", Tr("Stop the game to preview animations.")); ImGui::End(); return; }
    const EntityRow* row = Row(Primary());
    if (!row || (!row->Has("SpriteAnimation") && !row->Has("MeshRenderer"))) {
        Call("animation.preview_close", Json());
        ImGui::TextWrapped("%s", Tr("Select a sprite or a model with animation clips.")); ImGui::End(); return;
    }
    Json state = engine.AnimationPreviewCall("state", Json());
    if (!state["active"].asBool() || state["id"].asInt() != static_cast<int>(Primary())) {
        const Json response = Call("animation.preview", ObjectOf({{"id", Primary()}}), true);
        if (!Ok(response) && row->Has("SpriteAnimation")) {
            AnimationSpriteFrames(ObjectOf({{"id", Primary()}, {"clip", "new_clip"}})); ImGui::End(); return;
        }
        if (!Ok(response)) { ImGui::TextWrapped("%s", response["error"]["message"].asString().c_str()); ImGui::End(); return; }
        state = response["result"]; animationEvent = animationDragIndex = -1; animationNames.clear();
        animationTrack = animationKey = -1; animationKeyDragging = false;
    }
    const bool sprite = state["type"].asString() == "SpriteAnimation";
    const float duration = state["duration"].asFloat(), current = state["time"].asFloat();
    auto seek = [&](double time) { Call("animation.preview", ObjectOf({{"time", std::clamp(time, 0.0, static_cast<double>(duration))}, {"playing", false}})); };
    auto step = [&](int direction) {
        double next = current + direction * Engine::kFixedDt;
        if (sprite) {
            const Json& starts = state["frameTimes"];
            if (direction > 0) { next = duration; for (const Json& value : starts.items()) if (value.asNumber() > current + 1e-6) { next = value.asNumber(); break; } }
            else { next = 0; for (const Json& value : starts.items()) if (value.asNumber() < current - 1e-6) next = value.asNumber(); }
        }
        seek(next);
    };
    Json events = state["events"];
    auto addEvent = [&](float value) {
        Json marker = ObjectOf({{"name", "event"}});
        if (sprite) {
            int index = 0;
            for (size_t i = 0; i < state["frameTimes"].size(); ++i) if (state["frameTimes"][i].asFloat() <= value + 1e-7) index = static_cast<int>(i);
            marker["frame"] = index;
        } else marker["time"] = value;
        events.push(marker);
        AnimationWriteEvents(state, events);
    };
    const float fontSize = ImGui::GetFontSize();
    const ImU32 accent = IM_COL32(92, 148, 255, 255);
    const ImU32 eventColor = IM_COL32(120, 214, 160, 255), eventSelected = IM_COL32(255, 211, 95, 255);
    int frame = 0;  // the sequence frame under the playhead (sprites)
    if (sprite) for (size_t i = 0; i < state["frameTimes"].size(); ++i) if (state["frameTimes"][i].asNumber() <= current + 1e-7) frame = static_cast<int>(i);
    const int frameCount = sprite ? static_cast<int>(state["frameTimes"].size()) : static_cast<int>(std::round(duration * 60));

    // ----- Row 1: view tabs, what is animated, the clip; actions on the right.
    auto tab = [&](const char* label, bool chosen, const char* id) {
        ImGui::PushStyleColor(ImGuiCol_Button, chosen ? ImVec4(0.36f, 0.58f, 1.0f, 0.55f) : ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, chosen ? ImVec4(0.36f, 0.58f, 1.0f, 0.65f) : ImVec4(1, 1, 1, 0.12f));
        const bool pressed = ImGui::Button(label);
        ImGui::PopStyleColor(2);
        AnimationRect(id);
        return pressed;
    };
    if (tab(WithIcon(kIconFilm, Tr("Timeline")).c_str(), !animationFrames, "timeline-tab")) animationFrames = false;
    if (sprite) {
        ImGui::SameLine(0, 1);
        if (tab(WithIcon(kIconImage, Tr("Sprite Frames")).c_str(), animationFrames, "frames-tab")) animationFrames = true;
    }
    ImGui::SameLine(0, fontSize);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(row->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kIconChevronRight);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x * 0.3f, fontSize * 8, fontSize * 16));
    if (ImGui::BeginCombo("##animationClip", state["displayName"].asString(state["clip"].asString()).c_str())) {
        for (const Json& clip : state["clips"].items()) {
            const std::string name = clip["name"].asString();
            if (ImGui::Selectable((clip["displayName"].asString(name) + "##" + name).c_str(), name == state["clip"].asString())) {
                Call("animation.preview", ObjectOf({{"clip", name}, {"time", 0}, {"playing", false}}));
                animationEvent = animationDragIndex = -1; animationNames.clear();
                animationTrack = animationKey = -1; animationKeyDragging = false;
            }
        }
        ImGui::EndCombo();
    }
    AnimationRect("clip");
    HelpTooltip(Tr("Clip"));
    ImGui::SameLine();
    {
        // What the clip is, at a glance: its length, frame count and whether it loops.
        const std::string facts = sprite ? Format("%.2f s  |  %d %s", duration, frameCount, Tr("frames")) : Format("%.2f s", duration);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", facts.c_str());
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", kIconInfo);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(400);
        ImGui::TextUnformatted(Tr("Preview changes no scene fields and runs no scripts or events."));
        ImGui::TextUnformatted(Tr("IK is excluded from preview: gameplay scripts normally move its targets."));
        const bool linked = sprite && !state["asset"].asString().empty();
        ImGui::TextUnformatted(Tr(linked ? "Sprite events write the shared animation asset immediately; no undo." : sprite ? "Sprite events save with the scene." : "Model events write the shared sidecar immediately; no undo."));
        ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
    {
        // Actions, right-aligned when they fit.
        const bool properties = !(sprite && animationFrames);
        const ImGuiStyle& style = ImGui::GetStyle();
        const float width = ImGui::CalcTextSize(WithIcon(kIconPlus, Tr("Add Event")).c_str()).x + style.FramePadding.x * 2 +
                            (properties ? ImGui::CalcTextSize(WithIcon(kIconPlus, Tr("Add Property")).c_str()).x + style.FramePadding.x * 2 + style.ItemSpacing.x : 0) +
                            ImGui::GetFrameHeight() + style.ItemSpacing.x;
        ImGui::SameLine();
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width;
        if (ImGui::GetCursorPosX() < right) ImGui::SetCursorPosX(right);
        if (ImGui::Button(WithIcon(kIconPlus, Tr("Add Event")).c_str())) addEvent(current);
        AnimationRect("add-event");
        HelpTooltip(Tr("A named marker at the playhead; scripts get onAnimationEvent(name, clip) when playback crosses it."));
        if (properties) {  // the timeline's lanes show the properties
            ImGui::SameLine();
            if (ImGui::Button(WithIcon(kIconPlus, Tr("Add Property")).c_str())) ImGui::OpenPopup("##addProperty");
            AnimationRect("add-property");
            HelpTooltip(Tr("Keyframe a field of this entity or one of its children (position, rotation, scale, color, opacity, ...)."));
        }
        ImGui::SameLine();
        // Opens the separate controller workspace.
        if (ImGui::Button(kIconWorkflow, ImVec2(ImGui::GetFrameHeight(), 0))) Call("editor.animation", ObjectOf({{"controller", "animations/new.controller.json"}}));
        AnimationRect("controller-tab");
        HelpTooltip(Tr("Animation Controller"));
    }

    // ----- Row 2: transport and where the playhead is.
    const bool playing = state["playing"].asBool();
    const float button = ImGui::GetFrameHeight() * 1.25f;
    auto icon = [&](const char* glyph, const char* id, const char* tooltip, bool active = false, bool mirror = false) {
        ImGui::PushID(id);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.58f, 1.0f, 0.55f));
        ImDrawList* list = ImGui::GetWindowDrawList(); const int first = list->VtxBuffer.Size;
        const bool pressed = ImGui::Button(glyph, ImVec2(button, 0)); AnimationRect(id);
        if (mirror) { const float axis = ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x; for (int i = first; i < list->VtxBuffer.Size; ++i) list->VtxBuffer[i].pos.x = axis - list->VtxBuffer[i].pos.x; }
        if (active) ImGui::PopStyleColor();
        HelpTooltip(tooltip); ImGui::PopID(); return pressed;
    };
    if (icon(kIconHistory, "restart", Tr("Restart (Home)"))) seek(0);
    ImGui::SameLine(0, 2); if (icon(kIconStepForward, "step-back", Tr("Previous frame (Left)"), false, true)) step(-1);
    ImGui::SameLine(0, 2);
    if (icon(playing ? kIconPause : kIconPlay, "play", Tr("Play / Pause (Space)"), playing)) Call("animation.preview", ObjectOf({{"playing", !playing}}));
    ImGui::SameLine(0, 2); if (icon(kIconStepForward, "step", Tr("Next frame (Right)"))) step(1);
    ImGui::SameLine(0, 2); if (icon(kIconRefreshCw, "loop", Tr("Loop"), state["loop"].asBool())) Call("animation.preview", ObjectOf({{"loop", !state["loop"].asBool()}}));
    {
        // The playhead as a readout chip: time, and the frame out of all frames.
        ImGui::SameLine(0, fontSize);
        const std::string time = Format("%.3f / %.3f s", current, duration);
        const std::string frames = Format("%s %d / %d", Tr("Frame"), sprite ? frame : static_cast<int>(std::round(current * 60)), std::max(0, sprite ? frameCount - 1 : frameCount));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float pad = 8, gap = fontSize;
        const ImVec2 chip(ImGui::CalcTextSize(time.c_str()).x + ImGui::CalcTextSize(frames.c_str()).x + pad * 2 + gap, ImGui::GetFrameHeight());
        ImGui::Dummy(chip);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, at + chip, IM_COL32(0, 0, 0, 70), 4);
        const float textY = at.y + (chip.y - fontSize) * 0.5f;
        draw->AddText(ImVec2(at.x + pad, textY), ImGui::GetColorU32(ImGuiCol_Text), time.c_str());
        draw->AddText(ImVec2(at.x + pad + ImGui::CalcTextSize(time.c_str()).x + gap, textY), IM_COL32(150, 190, 255, 255), frames.c_str());
        AnimationRect("readout");
    }
    ImGui::SameLine(0, fontSize);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Tr("Speed"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fontSize * 4.5f);
    float speed = state["speed"].asFloat();
    if (ImGui::DragFloat("##animationSpeed", &speed, 0.1f, -100, 100, "%.1fx")) Call("animation.preview", ObjectOf({{"speed", speed}}));
    if (!sprite) {
        ImGui::SameLine(0, fontSize); bool ik = state["ik"].asBool(); if (ImGui::Checkbox(Tr("Apply IK"), &ik)) Call("animation.preview", ObjectOf({{"ik", ik}})); AnimationRect("ik");
        if (!ik && state["ikGoals"].asInt() > 0) { ImGui::SameLine(); ImGui::TextDisabled("%s", Tr("IK off")); }
    }
    if (animationEditorFocused && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) Call("animation.preview", ObjectOf({{"playing", !playing}}));
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step(1);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) seek(0);
    }
    if (sprite && animationFrames) { AnimationSpriteFrames(state); ImGui::End(); return; }
    ImGui::Dummy(ImVec2(0, 2));
    // The dope sheet scrolls in its own area so the event list stays in view: beside it in a wide panel (the usual
    // low, wide bottom dock), below it in a narrow one.
    const bool sideBySide = ImGui::GetContentRegionAvail().x > fontSize * 48;
    const float listWidth = sideBySide ? std::clamp(ImGui::GetContentRegionAvail().x * 0.3f, fontSize * 16, fontSize * 26) : 0.0f;
    {
        const float listHeight = sideBySide ? 0.0f : events.size() == 0 ? ImGui::GetFrameHeightWithSpacing() + 6 : ImGui::GetFrameHeightWithSpacing() * 4.8f;
        const float room = std::max(ImGui::GetFrameHeight() * 3, ImGui::GetContentRegionAvail().y - listHeight);
        const float wanted = sideBySide ? room : animationSheetHeight > 0 ? animationSheetHeight : room;
        ImGui::BeginChild("##dopeSheet", ImVec2(sideBySide ? ImGui::GetContentRegionAvail().x - listWidth - ImGui::GetStyle().ItemSpacing.x : 0.0f, std::min(wanted, room)),
                          ImGuiChildFlags_None, animationSheetHeight > std::min(wanted, room) ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollbar);
    }

    // ----- Dope sheet: a label column on the left names each lane (events, frames, property tracks); the ruler
    // and the lanes share one time scale. Narrow panels drop the column.
    const float fullWidth = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const float gutter = fullWidth > 360 ? std::min(fontSize * 10.0f, fullWidth * 0.28f) : 0.0f;
    const float available = fullWidth - gutter;
    const float scale = duration > 0 ? (available - 12) / duration : 0;
    // Event labels that would overlap stack into rows.
    std::vector<int> eventLanes; std::vector<float> labelEnds;
    for (const Json& event : events.items()) {
        const float x = duration > 0 ? 6 + event["time"].asFloat() * scale : 6;
        const float textWidth = ImGui::CalcTextSize(event["name"].asString().c_str()).x;
        const float labelX = std::min(x + 10, available - textWidth - 4);
        size_t lane = 0; while (lane < labelEnds.size() && labelEnds[lane] > labelX - 6) ++lane;
        if (lane == labelEnds.size()) labelEnds.push_back(-10000);
        labelEnds[lane] = labelX + textWidth; eventLanes.push_back(static_cast<int>(lane));
    }
    const float rulerHeight = std::round(fontSize * 1.6f), eventRow = fontSize + 6;
    const float eventsHeight = std::max(fontSize * 2.0f, 8 + std::max<size_t>(1, labelEnds.size()) * eventRow);
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    const ImVec2 origin = rowStart + ImVec2(gutter, 0), size(available, rulerHeight + eventsHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    auto laneLabel = [&](ImVec2 at, float height, const char* glyph, const char* text) {
        if (gutter <= 0) return;
        draw->AddRectFilled(at, at + ImVec2(gutter - 6, height), IM_COL32(255, 255, 255, 8), 3);
        const std::string label = WithIcon(glyph, text);
        draw->AddText(at + ImVec2(6, (height - fontSize) * 0.5f), IM_COL32(200, 204, 214, 255), label.c_str());
    };
    if (gutter > 0) {
        draw->AddText(rowStart + ImVec2(6, (rulerHeight - fontSize) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), Tr(sprite ? "Frame" : "Time (s)"));
        laneLabel(rowStart + ImVec2(0, rulerHeight), eventsHeight, kIconMapPin, Tr("Events"));
        ImGui::SetCursorScreenPos(origin);
    }
    ImGui::InvisibleButton("##animationTimeline", size); AnimationRect("timeline"); AnimationRect("time");
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    auto xAt = [&](float time) { return origin.x + 6 + time * scale; };
    // Ruler: darker band with ticks; the events lane below it.
    draw->AddRectFilled(origin, origin + ImVec2(size.x, rulerHeight), IM_COL32(0, 0, 0, 80), 3);
    draw->AddRectFilled(origin + ImVec2(0, rulerHeight), origin + size, ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
    if (sprite) {  // frame cells as faint bands so events line up with frames
        const Json& starts = state["frameTimes"];
        for (size_t i = 0; i < starts.size(); i += 2) {
            const float a = xAt(starts[i].asFloat()), b = xAt(i + 1 < starts.size() ? starts[i + 1].asFloat() : duration);
            draw->AddRectFilled(ImVec2(a, origin.y + rulerHeight), ImVec2(b, origin.y + size.y), IM_COL32(255, 255, 255, 7));
        }
    }
    auto tick = [&](float time, const std::string& label, bool major) {
        const float x = xAt(time);
        draw->AddLine(ImVec2(x, origin.y + rulerHeight - (major ? 9 : 5)), ImVec2(x, origin.y + rulerHeight), IM_COL32(255, 255, 255, major ? 120 : 60));
        if (major) draw->AddText(ImVec2(Clamp(x + 3, origin.x + 2, origin.x + size.x - ImGui::CalcTextSize(label.c_str()).x - 2), origin.y + 2),
                                 IM_COL32(190, 194, 204, 255), label.c_str());
    };
    if (sprite) {
        const int stride = std::max(1, static_cast<int>(std::ceil(state["frameCount"].asInt() / std::max(2.0f, size.x / (fontSize * 3.0f)))));
        for (size_t i = 0; i < state["frameTimes"].size(); ++i) tick(state["frameTimes"][i].asFloat(), std::to_string(i), static_cast<int>(i) % stride == 0);
    } else {
        double interval = 1;
        for (double candidate : {0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0}) {
            interval = candidate; if (duration / candidate <= std::max(2.0f, size.x / (fontSize * 5.0f))) break;
        }
        const double minor = interval / 5;
        for (int i = 0; i <= 2000 && i * minor <= duration + 1e-6; ++i) {
            char label[32]; std::snprintf(label, sizeof(label), "%.3g", i * minor);
            tick(static_cast<float>(i * minor), label, i % 5 == 0);
        }
    }
    int hit = -1;
    const float eventsTop = origin.y + rulerHeight;
    for (size_t i = 0; i < events.size(); ++i) {
        const float x = xAt(events[i]["time"].asFloat());
        const std::string name = events[i]["name"].asString();
        const float textWidth = ImGui::CalcTextSize(name.c_str()).x;
        const float labelX = std::min(x + 10, origin.x + size.x - textWidth - 4);
        const float y = eventsTop + 4 + eventLanes[i] * eventRow;
        const bool markerSelected = animationEvent == static_cast<int>(i);
        const ImU32 color = markerSelected ? eventSelected : eventColor;
        // A pin: a stem from the ruler to a flag with the name beside it.
        draw->AddLine(ImVec2(x, eventsTop), ImVec2(x, y + eventRow - 4), color, markerSelected ? 2.0f : 1.0f);
        draw->AddTriangleFilled(ImVec2(x, y), ImVec2(x + 8, y + 4), ImVec2(x, y + 9), color);
        if (markerSelected) draw->AddRectFilled(ImVec2(labelX - 3, y - 1), ImVec2(labelX + textWidth + 3, y + fontSize + 1), IM_COL32(255, 211, 95, 40), 3);
        draw->AddText(ImVec2(labelX, y - 1), color, name.c_str());
        animationRects["marker:" + std::to_string(i)] = {x - 6, y, 14, 14};
        if (hovered && ImGui::IsMouseHoveringRect(ImVec2(x - 6, y - 2), ImVec2(x + 10, y + 14))) hit = static_cast<int>(i);
    }
    if (events.size() == 0) {
        const char* hint = Tr("Double-click here or press Add Event to mark a moment of the clip.");
        const ImVec2 textSize = ImGui::CalcTextSize(hint);
        draw->AddText(ImVec2(origin.x + std::max(8.0f, (size.x - textSize.x) * 0.5f), eventsTop + (eventsHeight - fontSize) * 0.5f), IM_COL32(255, 255, 255, 70), hint);
    }
    if (hit >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ++dragSerial["animation-event"]; animationDragIndex = hit; animationDragEvents = events;
        if (hit >= 0) { animationEvent = hit; seek(events[hit]["time"].asNumber()); }
        if (hit < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) addEvent(Clamp((ImGui::GetIO().MousePos.x - origin.x - 6) / std::max(1.0f, scale), 0, duration));
    }
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float value = Clamp((ImGui::GetIO().MousePos.x - origin.x - 6) / std::max(1.0f, scale), 0, duration);
        if (animationDragIndex >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            Json& marker = animationDragEvents[animationDragIndex];
            if (sprite) {
                int index = 0; double nearest = 1e100;
                for (size_t i = 0; i < state["frameTimes"].size(); ++i) {
                    const double distance = std::fabs(value - state["frameTimes"][i].asNumber());
                    if (distance < nearest) { nearest = distance; index = static_cast<int>(i); }
                }
                marker["frame"] = index;
            } else marker["time"] = value;
            AnimationWriteEvents(state, animationDragEvents, "animation-marker:" + std::to_string(Primary()) + ":" + state["clip"].asString() + ":" + std::to_string(dragSerial["animation-event"]));
        } else if (animationDragIndex < 0) seek(value);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) animationDragIndex = -1;
    float lanesBottom = origin.y + size.y;
    if (sprite) {
        const float stripHeight = std::round(fontSize * 2.6f);
        const ImVec2 strip = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size.x + gutter, stripHeight));
        laneLabel(strip, stripHeight, kIconImage, Tr("Frames"));
        AnimationFrameStrip(state, strip + ImVec2(gutter, 0), ImVec2(size.x, stripHeight), origin.x + 6, scale, frame);
        lanesBottom = strip.y + stripHeight;
    }
    const float tracksBottom = AnimationTracks(state, origin.x + 6, scale, gutter);
    if (tracksBottom > 0) lanesBottom = tracksBottom;
    {
        // The playhead through the ruler and every lane, with its time (or frame) on a handle in the ruler.
        const float head = xAt(current);
        ImDrawList* top = ImGui::GetWindowDrawList();
        top->AddLine(ImVec2(head, origin.y), ImVec2(head, lanesBottom), accent, 2);
        char headLabel[32]; std::snprintf(headLabel, sizeof(headLabel), "%.3f s", current);
        const std::string headText = sprite ? std::to_string(frame) : headLabel;
        const float headWidth = ImGui::CalcTextSize(headText.c_str()).x + 10;
        const ImVec2 badge(Clamp(head - headWidth * 0.5f, origin.x, origin.x + size.x - headWidth), origin.y + 1);
        top->AddRectFilled(badge, badge + ImVec2(headWidth, rulerHeight - 6), accent, 4);
        top->AddTriangleFilled(ImVec2(head - 5, origin.y + rulerHeight - 6), ImVec2(head + 5, origin.y + rulerHeight - 6), ImVec2(head, origin.y + rulerHeight), accent);
        top->AddText(badge + ImVec2(5, (rulerHeight - 6 - fontSize) * 0.5f), IM_COL32(255, 255, 255, 255), headText.c_str());
    }
    animationSheetHeight = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
    ImGui::EndChild();
    // After the lanes: a property it adds is selected without the lanes (drawn from the older state) dropping it.
    if (!(sprite && animationFrames)) AnimationAddPropertyPopup(state, state["tracks"].isArray() ? state["tracks"] : Json::MakeArray());

    // ----- Event list: names and times typed in.
    if (sideBySide) {
        ImGui::SameLine();
        ImGui::BeginChild("##eventList", ImVec2(0, 0), ImGuiChildFlags_Borders);
    } else {
        ImGui::Dummy(ImVec2(0, 4));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Format("%s  %s (%d)", kIconMapPin, Tr("Events"), static_cast<int>(events.size())).c_str());
    if (events.size() == 0) {
        if (!sideBySide) ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", Tr("None yet: scripts receive onAnimationEvent(name, clip) when playback crosses one."));
        ImGui::PopTextWrapPos();
        if (sideBySide) ImGui::EndChild();
        ImGui::End();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 2));
    if (ImGui::BeginTable("##animationEvents", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                          ImVec2(0, std::max(60.0f, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupColumn(Tr("Event name")); ImGui::TableSetupColumn(Tr(sprite ? "Frame" : "Time (s)"), ImGuiTableColumnFlags_WidthFixed, fontSize * 7);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + 4); ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        for (size_t i = 0; i < events.size(); ++i) {
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow();
            if (animationEvent == static_cast<int>(i)) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 211, 95, 36));
            ImGui::TableNextColumn(); const std::string key = std::to_string(Primary()) + ":" + state["clip"].asString() + ":" + std::to_string(i);
            std::string& name = animationNames.emplace(key, events[i]["name"].asString()).first->second;
            ImGui::SetNextItemWidth(-1); if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue)) { events[i]["name"] = name; AnimationWriteEvents(state, events); }
            if (ImGui::IsItemClicked()) { animationEvent = static_cast<int>(i); seek(events[i]["time"].asNumber()); }
            if (!ImGui::IsItemActive()) name = events[i]["name"].asString(); AnimationRect("name:" + std::to_string(i)); ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (sprite) { int value = events[i]["frame"].asInt(); if (ImGui::InputInt("##frame", &value, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) { events[i]["frame"] = value; AnimationWriteEvents(state, events); } }
            else { float value = events[i]["time"].asFloat(); if (ImGui::InputFloat("##time", &value, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) { events[i]["time"] = value; AnimationWriteEvents(state, events); } }
            if (ImGui::IsItemClicked()) { animationEvent = static_cast<int>(i); seek(events[i]["time"].asNumber()); }
            AnimationRect("event-time:" + std::to_string(i)); ImGui::TableNextColumn();
            if (ImGui::Button(kIconTrash2, ImVec2(ImGui::GetFrameHeight(), 0))) { Json remaining = events; remaining.items().erase(remaining.items().begin() + static_cast<ptrdiff_t>(i)); AnimationWriteEvents(state, remaining); animationEvent = -1; }
            HelpTooltip(Tr("Remove Event")); AnimationRect("remove-event:" + std::to_string(i)); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    if (sideBySide) ImGui::EndChild();
    ImGui::End();
}
}  // namespace oe
