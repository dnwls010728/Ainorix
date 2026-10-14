#include "editor/EditorInternal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "app/Engine.h"
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
        Json marker=ObjectOf({{"name","event"}});
        if (sprite) { int index=0; for(size_t i=0;i<state["frameTimes"].size();++i) if(state["frameTimes"][i].asFloat()<=value+1e-7) index=static_cast<int>(i); marker["frame"]=index; }
        else marker["time"]=value;
        events.push(marker); AnimationWriteEvents(state,events);
    };
    auto mode = [&](const char* label, bool chosen, const char* id) {
        const ImVec2 dimensions = ImGui::CalcTextSize(label) + ImVec2(12, 0);
        const bool pressed = ImGui::InvisibleButton(id, dimensions); AnimationRect(id);
        const ImVec2 start = ImGui::GetItemRectMin();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(start, start + dimensions, ImGui::GetColorU32(chosen ? ImGuiCol_ButtonActive :
            ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button), 3);
        draw->AddText(start + ImVec2(6, 0), ImGui::GetColorU32(ImGuiCol_Text), label);
        return pressed;
    };
    if (mode(Tr("Timeline"), !animationFrames, "timeline-tab")) animationFrames = false;
    if (sprite) { ImGui::SameLine(); if (mode(Tr("Sprite Frames"), animationFrames, "frames-tab")) animationFrames = true; }
    ImGui::SameLine(); ImGui::TextUnformatted(row->name.c_str()); ImGui::SameLine(); ImGui::TextDisabled("%s", Tr("Clip"));
    ImGui::SameLine(); ImGui::SetNextItemWidth(std::min(220.0f, ImGui::GetContentRegionAvail().x * 0.35f));
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
    AnimationRect("clip"); ImGui::SameLine(); ImGui::TextDisabled("%.2f s", duration);
    ImGui::SameLine(); ImGui::TextDisabled("%s", kIconInfo);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(400);
        ImGui::TextUnformatted(Tr("Preview changes no scene fields and runs no scripts or events."));
        ImGui::TextUnformatted(Tr("IK is excluded from preview: gameplay scripts normally move its targets."));
        const bool linked = sprite && !state["asset"].asString().empty();
        ImGui::TextUnformatted(Tr(linked ? "Sprite events write the shared animation asset immediately; no undo." : sprite ? "Sprite events save with the scene." : "Model events write the shared sidecar immediately; no undo."));
        ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
    // Opens the separate controller workspace.
    ImGui::SameLine(); if (ImGui::SmallButton(kIconWorkflow)) Call("editor.animation", ObjectOf({{"controller", "animations/new.controller.json"}}));
    AnimationRect("controller-tab"); HelpTooltip(Tr("Animation Controller"));
    ImGui::SameLine(); if(ImGui::SmallButton(WithIcon(kIconPlus,Tr("Add Event")).c_str())) addEvent(current); AnimationRect("add-event");
    if (!(sprite && animationFrames)) {  // the timeline's lanes show the properties
        ImGui::SameLine(); if (ImGui::SmallButton(WithIcon(kIconPlus, Tr("Add Property")).c_str())) ImGui::OpenPopup("##addProperty");
        AnimationRect("add-property");
        HelpTooltip(Tr("Keyframe a field of this entity or one of its children (position, rotation, scale, color, opacity, ...)."));
    }
    auto icon = [&](const char* glyph, const char* id, const char* tooltip, bool active = false, bool mirror = false) {
        ImGui::PushID(id); if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImDrawList* list = ImGui::GetWindowDrawList(); const int first = list->VtxBuffer.Size;
        const bool pressed = ImGui::SmallButton(glyph); AnimationRect(id);
        if (mirror) { const float axis = ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x; for (int i = first; i < list->VtxBuffer.Size; ++i) list->VtxBuffer[i].pos.x = axis - list->VtxBuffer[i].pos.x; }
        if (active) ImGui::PopStyleColor(); HelpTooltip(tooltip); ImGui::PopID(); return pressed;
    };
    if (icon(kIconHistory, "restart", Tr("Restart (Home)"))) seek(0);
    ImGui::SameLine(); if (icon(kIconStepForward, "step-back", Tr("Previous frame (Left)"), false, true)) step(-1);
    ImGui::SameLine(); const bool playing = state["playing"].asBool();
    if (icon(playing ? kIconPause : kIconPlay, "play", Tr("Play / Pause (Space)"), playing)) Call("animation.preview", ObjectOf({{"playing", !playing}}));
    ImGui::SameLine(); if (icon(kIconStepForward, "step", Tr("Next frame (Right)"))) step(1);
    ImGui::SameLine(); if (icon(kIconRefreshCw, "loop", Tr("Loop"), state["loop"].asBool())) Call("animation.preview", ObjectOf({{"loop", !state["loop"].asBool()}}));
    ImGui::SameLine(); int frame = 0;
    if (sprite) for (size_t i = 0; i < state["frameTimes"].size(); ++i) if (state["frameTimes"][i].asNumber() <= current + 1e-7) frame = static_cast<int>(i);
    ImGui::Text("%.3f / %.3f s  |  %s %d", current, duration, Tr("Frame"), sprite ? frame : static_cast<int>(std::round(current * 60)));
    ImGui::SameLine(); ImGui::TextUnformatted(Tr("Speed")); ImGui::SameLine(); ImGui::SetNextItemWidth(65);
    float speed = state["speed"].asFloat();
    if (ImGui::DragFloat("##animationSpeed", &speed, 0.1f, -100, 100, "%.1fx")) Call("animation.preview", ObjectOf({{"speed", speed}}));
    if (!sprite) {
        ImGui::SameLine(); bool ik = state["ik"].asBool(); if (ImGui::Checkbox(Tr("Apply IK"), &ik)) Call("animation.preview", ObjectOf({{"ik", ik}})); AnimationRect("ik");
        if (!ik && state["ikGoals"].asInt() > 0) { ImGui::SameLine(); ImGui::TextDisabled("%s", Tr("IK off")); }
    }
    if (animationEditorFocused && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) Call("animation.preview", ObjectOf({{"playing", !playing}}));
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step(1);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) seek(0);
    }
    if (sprite && animationFrames) { AnimationSpriteFrames(state); ImGui::End(); return; }
    std::vector<int> eventLanes; std::vector<float> labelEnds;
    // A label column on the left names each lane (events, frames, property tracks), as in a dope sheet; the
    // lanes share the ruler's time scale. Narrow panels drop the column.
    const float fullWidth = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const float gutter = fullWidth > 360 ? std::min(ImGui::GetFontSize() * 10.0f, fullWidth * 0.28f) : 0.0f;
    const float available = fullWidth - gutter;
    for (const Json& event : events.items()) {
        const float x = duration > 0 ? 6 + event["time"].asFloat() * (available-12)/duration : 6;
        const float textWidth = ImGui::CalcTextSize(event["name"].asString().c_str()).x * 0.72f;
        const float labelX = std::min(x+8, available-textWidth-4);
        size_t lane=0; while(lane<labelEnds.size() && labelEnds[lane]>labelX-4) ++lane;
        if(lane==labelEnds.size()) labelEnds.push_back(-10000);
        labelEnds[lane]=labelX+textWidth; eventLanes.push_back(static_cast<int>(lane));
    }
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    const ImVec2 origin = rowStart + ImVec2(gutter, 0), size(available, 50 + std::max(0,static_cast<int>(labelEnds.size())-1)*15.0f);
    if (gutter > 0) {
        ImGui::GetWindowDrawList()->AddText(rowStart + ImVec2(4, 32), ImGui::GetColorU32(ImGuiCol_TextDisabled), Tr("Events"));
        ImGui::SetCursorScreenPos(origin);
    }
    ImGui::InvisibleButton("##animationTimeline", size); AnimationRect("timeline"); AnimationRect("time");
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, origin + size, ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
    const float scale = duration > 0 ? (size.x - 12) / duration : 0;
    auto xAt = [&](float time) { return origin.x + 6 + time * scale; };
    const float font = ImGui::GetFontSize() * 0.72f;
    double interval = 1;
    if (!sprite) for (double candidate : {0.01,0.02,0.05,0.1,0.25,0.5,1.0,2.0,5.0,10.0,30.0,60.0,300.0}) {
        interval = candidate; if (duration / candidate <= std::max(2.0f, size.x / 80)) break;
    }
    auto tick = [&](float time, const std::string& label, bool major) {
        const float x = xAt(time); draw->AddLine(ImVec2(x, origin.y + 16), ImVec2(x, origin.y + (major ? 28 : 22)), ImGui::GetColorU32(ImGuiCol_TextDisabled));
        if (major) draw->AddText(ImGui::GetFont(), font, ImVec2(Clamp(x - 6, origin.x + 2, origin.x + size.x - 45), origin.y + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
    };
    if (sprite) {
        const int stride = std::max(1, static_cast<int>(std::ceil(state["frameCount"].asInt() / std::max(2.0f, size.x / 55))));
        for (size_t i = 0; i < state["frameTimes"].size(); ++i) tick(state["frameTimes"][i].asFloat(), std::to_string(i), static_cast<int>(i) % stride == 0);
    } else {
        const double minor = interval / 5;
        for (int i = 0; i <= 2000 && i * minor <= duration + 1e-6; ++i) {
            char label[32]; std::snprintf(label, sizeof(label), "%.2g", i * minor);
            tick(static_cast<float>(i * minor), label, i % 5 == 0);
        }
    }
    const float head = xAt(current);
    draw->AddLine(ImVec2(head, origin.y + 17), ImVec2(head, origin.y + size.y), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2);
    char headLabel[32]; std::snprintf(headLabel, sizeof(headLabel), "%.3f s", current);
    const std::string headText = sprite ? std::to_string(frame) : headLabel;
    const float headWidth = ImGui::CalcTextSize(headText.c_str()).x * 0.72f + 8;
    const ImVec2 badge(Clamp(head + 4, origin.x + 4, origin.x + size.x - headWidth), origin.y + 16);
    draw->AddRectFilled(badge, badge + ImVec2(headWidth, font + 2), ImGui::GetColorU32(ImGuiCol_ButtonActive), 2);
    draw->AddText(ImGui::GetFont(), font, badge + ImVec2(3,0), ImGui::GetColorU32(ImGuiCol_Text), headText.c_str());
    int hit = -1;
    for (size_t i = 0; i < events.size(); ++i) {
        const float x = xAt(events[i]["time"].asFloat());
        const std::string name = events[i]["name"].asString();
        const float textWidth = ImGui::CalcTextSize(name.c_str()).x * 0.72f;
        const float labelX = std::min(x + 8, origin.x + size.x - textWidth - 4);
        const int lane = eventLanes[i];
        const float y = origin.y + 32 + lane * 15;
        const bool markerSelected = animationEvent == static_cast<int>(i);
        const ImU32 color = markerSelected ? IM_COL32(255,211,95,255) : IM_COL32(120,214,160,255);
        draw->AddTriangleFilled(ImVec2(x,y),ImVec2(x-5,y+12),ImVec2(x+5,y+12),color);
        if (markerSelected) draw->AddCircle(ImVec2(x,y+6),8,color,12,2);
        draw->AddText(ImGui::GetFont(),font,ImVec2(labelX,y),color,name.c_str());
        animationRects["marker:"+std::to_string(i)] = {x-6,y,12,14};
        if (hovered && ImGui::IsMouseHoveringRect(ImVec2(x-6,y),ImVec2(x+6,y+14))) hit=static_cast<int>(i);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ++dragSerial["animation-event"]; animationDragIndex=hit; animationDragEvents=events;
        if (hit>=0) { animationEvent=hit; seek(events[hit]["time"].asNumber()); }
        if (hit<0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) addEvent(Clamp((ImGui::GetIO().MousePos.x-origin.x-6)/std::max(1.0f,scale),0,duration));
    }
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float value=Clamp((ImGui::GetIO().MousePos.x-origin.x-6)/std::max(1.0f,scale),0,duration);
        if (animationDragIndex>=0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            Json& marker=animationDragEvents[animationDragIndex];
            if(sprite) { int index=0;double nearest=1e100;for(size_t i=0;i<state["frameTimes"].size();++i){double distance=std::fabs(value-state["frameTimes"][i].asNumber());if(distance<nearest){nearest=distance;index=static_cast<int>(i);}} marker["frame"]=index; }
            else marker["time"]=value;
            AnimationWriteEvents(state,animationDragEvents,"animation-marker:"+std::to_string(Primary())+":"+state["clip"].asString()+":"+std::to_string(dragSerial["animation-event"]));
        } else if(animationDragIndex<0) seek(value);
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) animationDragIndex=-1;
    if(sprite) {
        const ImVec2 strip=ImGui::GetCursorScreenPos(); ImGui::Dummy(ImVec2(size.x+gutter,18));
        if (gutter > 0) ImGui::GetWindowDrawList()->AddText(strip + ImVec2(4, 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), Tr("Frames"));
        AnimationFrameStrip(state,strip+ImVec2(gutter,0),ImVec2(size.x,22));
    }
    AnimationTracks(state, origin.x + 6, scale, gutter);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1));
    if(ImGui::BeginTable("##animationEvents",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,ImVec2(0,std::max(60.0f,ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupColumn(Tr("Event name")); ImGui::TableSetupColumn(Tr(sprite ? "Frame" : "Time (s)"),ImGuiTableColumnFlags_WidthFixed,110);
        ImGui::TableSetupColumn("##actions",ImGuiTableColumnFlags_WidthFixed,28); ImGui::TableHeadersRow();
        for(size_t i=0;i<events.size();++i){
            ImGui::PushID(static_cast<int>(i));ImGui::TableNextRow();
            if(animationEvent==static_cast<int>(i)) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,ImGui::GetColorU32(ImGuiCol_Header));
            ImGui::TableNextColumn();const std::string key=std::to_string(Primary())+":"+state["clip"].asString()+":"+std::to_string(i);
            std::string& name=animationNames.emplace(key,events[i]["name"].asString()).first->second;
            ImGui::SetNextItemWidth(-1);if(ImGui::InputText("##name",&name,ImGuiInputTextFlags_EnterReturnsTrue)){events[i]["name"]=name;AnimationWriteEvents(state,events);}
            if(ImGui::IsItemClicked()){animationEvent=static_cast<int>(i);seek(events[i]["time"].asNumber());}
            if(!ImGui::IsItemActive())name=events[i]["name"].asString(); AnimationRect("name:"+std::to_string(i)); ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if(sprite){int value=events[i]["frame"].asInt();if(ImGui::InputInt("##frame",&value,0,0,ImGuiInputTextFlags_EnterReturnsTrue)){events[i]["frame"]=value;AnimationWriteEvents(state,events);}}
            else {float value=events[i]["time"].asFloat();if(ImGui::InputFloat("##time",&value,0,0,"%.3f",ImGuiInputTextFlags_EnterReturnsTrue)){events[i]["time"]=value;AnimationWriteEvents(state,events);}}
            if(ImGui::IsItemClicked()){animationEvent=static_cast<int>(i);seek(events[i]["time"].asNumber());}
            AnimationRect("event-time:"+std::to_string(i)); ImGui::TableNextColumn();
            if(ImGui::SmallButton(kIconTrash2)){Json remaining=events;remaining.items().erase(remaining.items().begin()+static_cast<ptrdiff_t>(i));AnimationWriteEvents(state,remaining);animationEvent=-1;}
            HelpTooltip(Tr("Remove Event"));AnimationRect("remove-event:"+std::to_string(i));ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::End();
}
}  // namespace oe
