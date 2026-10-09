// Ask bubbles (docs/EDITOR.md "Asking about an object"): right-click an object in the Scene view,
// then ask the agent team (the message goes to the team chat, naming the object) or one installed
// agent CLI directly (team.ask: one conversation per object and CLI, outside the team chat, with a
// model picked in the bubble). The bubble stays next to the object while the camera moves and
// shows the conversation. Reads team.thread / the chat cache, writes through team.ask / team.send.
#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "imgui_internal.h"

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Log.h"

namespace oe {
namespace {

constexpr float kBubbleWidthEm = 24.0f;  // bubble width in font sizes
constexpr float kLogHeightEm = 15.0f;    // the conversation scrolls above this height

const ImVec4 kAccent(0.42f, 0.62f, 1.0f, 1.0f);

std::string Trimmed(const std::string& text) {
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

// The "[Girl #605] " a team bubble puts before its messages, hidden again when it shows them.
std::string WithoutObjectPrefix(const std::string& text) {
    if (text.empty() || text[0] != '[') return text;
    const size_t end = text.find("] ");
    return end == std::string::npos || end > 80 ? text : text.substr(end + 2);
}

// A square icon button drawn by hand, the icon centered (Button() puts it after its padding).
bool IconButton(const char* id, const char* icon, float side, bool enabled = true) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered && enabled) draw->AddRectFilled(p, ImVec2(p.x + side, p.y + side), IM_COL32(255, 255, 255, 24), ImGui::GetStyle().FrameRounding);
    const ImVec2 size = ImGui::CalcTextSize(icon);
    draw->AddText(ImVec2(p.x + (side - size.x) * 0.5f, p.y + (side - size.y) * 0.5f),
                  ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), icon);
    return clicked && enabled;
}

}  // namespace

std::string NativeEditor::Impl::AskContext(EntityId entity) const {
    const EntityRow* row = Row(entity);
    std::string components;
    if (row) {
        for (const std::string& c : row->components) components += (components.empty() ? "" : ", ") + c;
    }
    const std::string id = std::to_string(entity);
    std::string scene = scenePath;
    if (!scene.empty() && !engine.ProjectDir().empty()) scene = RelativePath(scenePath, engine.ProjectDir());
    return "The person is pointing at this object in the editor's Scene view: entity #" + id + " \"" + (row ? row->name : "") +
           "\" (components: " + components + ") in scene \"" + sceneName + "\"" + (scene.empty() ? "" : " (" + scene + ")") +
           ". Read it with entity.get {id: " + id + "}; editor.state shows what else they see.";
}

void NativeEditor::Impl::OpenAskBubble(EntityId entity, const std::string& backend) {
    for (AskBubble& bubble : askBubbles) {
        if (bubble.entity == entity && bubble.backend == backend) {
            bubble.focusInput = true;
            return;
        }
    }
    AskBubble bubble;
    bubble.serial = ++askSerial;
    bubble.entity = entity;
    bubble.backend = backend;
    if (!backend.empty()) {
        // One conversation per object and CLI: opening the bubble again shows it, with its model.
        bubble.thread = "e" + std::to_string(entity) + "-" + backend;
        const Json state = Call("team.thread", ObjectOf({{"thread", Json(bubble.thread)}}), true);
        if (Ok(state)) {
            bubble.state = state["result"];
            if (bubble.state["backend"].asString("") == backend) bubble.model = bubble.state["model"].asString("");
        }
        bubble.polled = time;
    }
    askBubbles.push_back(std::move(bubble));
}

void NativeEditor::Impl::SceneAskMenu() {
    if (!ImGui::BeginPopup("##sceneAsk")) return;
    const EntityRow* row = Row(askMenuEntity);
    if (!row) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextDisabled("%s", row->name.c_str());
    ImGui::Separator();
    const bool teamReady = teamAvailable && team["agents"].size() > 0;
    if (ImGui::MenuItem(WithIcon(kIconUsers, Tr("Ask the Team")).c_str(), nullptr, false, teamReady)) OpenAskBubble(askMenuEntity, "");
    if (!teamReady && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", Tr(teamAvailable ? "The team has no agents yet (Team panel)." : "Agent teams need `oe editor`."));
    }
    if (ImGui::BeginMenu(WithIcon(kIconBot, Tr("Ask an Agent")).c_str(), teamAvailable)) {
        int installed = 0;
        bool detecting = teamBackends.size() == 0;
        for (const Json& backend : teamBackends.items()) {
            const std::string status = backend["status"].asString("");
            detecting = detecting || status == "detecting";
            if (status != "installed") continue;
            ++installed;
            const std::string label = backend["name"].asString("") + "##" + backend["id"].asString("");
            if (ImGui::MenuItem(label.c_str())) OpenAskBubble(askMenuEntity, backend["id"].asString(""));
        }
        if (installed == 0) ImGui::TextDisabled("%s", Tr(detecting ? "Looking for agent CLIs..." : "No agent CLI is installed."));
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

void NativeEditor::Impl::AskSend(AskBubble& bubble) {
    const std::string text = Trimmed(bubble.input);
    if (text.empty()) return;
    if (bubble.backend.empty()) {
        const EntityRow* row = Row(bubble.entity);
        const std::string prefix = "[" + (row ? row->name : std::string()) + " #" + std::to_string(bubble.entity) + "] ";
        const Json result = Call("team.send", ObjectOf({{"text", Json(prefix + text)}}), true);
        if (!Ok(result)) {
            Notify(result["error"]["message"].asString(""), true);
            return;
        }
        bubble.sent.push_back(static_cast<uint64_t>(result["result"]["message"]["id"].asNumber(0)));
    } else {
        const Json result = Call("team.ask", ObjectOf({{"thread", Json(bubble.thread)}, {"backend", Json(bubble.backend)},
                                                       {"model", Json(bubble.model)}, {"text", Json(text)},
                                                       {"context", Json(AskContext(bubble.entity))}}), true);
        if (!Ok(result)) {
            const std::string hint = result["error"]["hint"].asString("");
            Notify(result["error"]["message"].asString("") + (hint.empty() ? "" : " " + hint), true);
            return;
        }
        bubble.state = result["result"];
        bubble.polled = time;
    }
    bubble.input.clear();
    bubble.focusInput = true;
}

bool NativeEditor::Impl::AskBubbleBody(AskBubble& bubble) {
    const EntityRow* row = Row(bubble.entity);
    const Json* backend = bubble.backend.empty() ? nullptr : TeamBackend(bubble.backend);
    const std::string target = bubble.backend.empty() ? std::string(Tr("Team")) : backend ? (*backend)["name"].asString(bubble.backend) : bubble.backend;
    const bool agent = !bubble.backend.empty();
    const float side = ImGui::GetFrameHeight();

    // Header: who is asked about what, and close.
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kAccent, "%s", agent ? kIconBot : kIconUsers);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(target.c_str());
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextDisabled("\xc2\xb7 %s", row ? row->name.c_str() : "");
    ImGui::SameLine(right - side);
    if (IconButton("##close", kIconX, side)) return true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Close (the conversation is kept)"));

    // Agent bubbles: the model of the next message.
    if (agent) {
        std::string label = bubble.model.empty() ? std::string(Tr("CLI default")) : bubble.model;
        if (backend) {
            for (const Json& model : (*backend)["models"].items()) {
                if (model["id"].asString("") == bubble.model) label = model["label"].asString(bubble.model);
            }
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##model", (std::string(Tr("Model")) + ": " + label).c_str())) {
            if (ImGui::Selectable(Tr("CLI default"), bubble.model.empty())) bubble.model.clear();
            if (backend) {
                for (const Json& model : (*backend)["models"].items()) {
                    const std::string id = model["id"].asString("");
                    if (ImGui::Selectable((model["label"].asString(id) + "##" + id).c_str(), bubble.model == id)) bubble.model = id;
                }
            }
            ImGui::EndCombo();
        }
    }

    // The conversation: this CLI's thread, or what this bubble sent to the team and the answers since.
    struct Line {
        std::string who, text, hint, kind;
        bool mine = false;
    };
    std::vector<Line> lines;
    std::string activity;
    if (agent) {
        for (const Json& message : bubble.state["messages"].items()) {
            const bool mine = message["from"].asString("") == "user";
            lines.push_back({mine ? Tr("You") : target, message["text"].asString(""), message["hint"].asString(""), message["kind"].asString("message"), mine});
        }
        if (bubble.state["running"].asBool()) {
            activity = Format("%s  %.0fs", bubble.state["activity"].asString("Starting").c_str(), bubble.state["elapsed"].asNumber(0));
        }
    } else if (!bubble.sent.empty()) {
        for (const ChatEntry& entry : chat) {
            const Json& message = entry.message;
            const uint64_t id = static_cast<uint64_t>(message["id"].asNumber(0));
            if (id < bubble.sent.front()) continue;
            const bool mine = std::find(bubble.sent.begin(), bubble.sent.end(), id) != bubble.sent.end();
            bool toUser = false;
            for (const Json& to : message["to"].items()) toUser = toUser || to.asString("") == "user";
            const std::string from = message["from"].asString("");
            const std::string kind = message["kind"].asString("message");
            if (!mine && (from == "user" || (kind == "message" && !toUser))) continue;
            std::string who = from;
            for (const Json& member : team["agents"].items()) {
                if (member["id"].asString("") == from) who = member["name"].asString(from);
            }
            lines.push_back({mine ? Tr("You") : who.empty() ? target : who, mine ? WithoutObjectPrefix(message["text"].asString("")) : message["text"].asString(""),
                             message["hint"].asString(""), kind, mine});
        }
        for (const Json& member : teamState["agents"].items()) {
            const std::string state = member["state"].asString("idle");
            if (state == "idle" || state == "offline") continue;
            std::string name = member["id"].asString("");
            for (const Json& profile : team["agents"].items()) {
                if (profile["id"].asString("") == name) name = profile["name"].asString(name);
            }
            activity += (activity.empty() ? "" : "   ") + name + ": " + member["activity"].asString(state);
        }
    }
    if (lines.empty() && activity.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", agent ? Format(Tr("%s works on this object directly; the conversation stays in this bubble."), target.c_str()).c_str()
                                        : Tr("Goes to the team chat: the lead answers unless you @mention someone."));
        ImGui::PopTextWrapPos();
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFontSize() * kLogHeightEm));
        if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY)) {
            for (size_t i = 0; i < lines.size(); ++i) {
                const Line& line = lines[i];
                ImGui::PushID(static_cast<int>(i));
                if (i > 0) ImGui::Dummy(ImVec2(0, 2));
                ImGui::TextColored(line.mine ? ImVec4(0.62f, 0.64f, 0.68f, 1) : kAccent, "%s", line.who.c_str());
                if (line.kind == "error") {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ImVec4(0.94f, 0.43f, 0.43f, 1), "%s", line.text.c_str());
                    if (!line.hint.empty()) ImGui::TextDisabled("%s", line.hint.c_str());
                    ImGui::PopTextWrapPos();
                } else if (line.kind == "notice") {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", line.text.c_str());
                    ImGui::PopTextWrapPos();
                } else {
                    DrawChatMarkdown(line.text, {}, false, ImGui::GetColorU32(ImGuiCol_Text));
                }
                ImGui::PopID();
            }
            if (!activity.empty()) {
                static const char* const kDots[] = {".", "..", "..."};
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s%s", activity.c_str(), kDots[static_cast<int>(time * 3.0) % 3]);
                ImGui::PopTextWrapPos();
            }
            const size_t count = lines.size() + (activity.empty() ? 0 : 1);
            if (count != bubble.shown) ImGui::SetScrollHereY(1.0f);
            bubble.shown = count;
        }
        ImGui::EndChild();
    }

    // Input: Enter sends, Ctrl+Enter adds a line.
    const bool running = agent && bubble.state["running"].asBool();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    if (bubble.focusInput) {
        ImGui::SetKeyboardFocusHere();
        bubble.focusInput = false;
    }
    const ImVec2 inputPos = ImGui::GetCursorScreenPos();
    const float inputHeight = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    const bool entered = ImGui::InputTextMultiline("##input", &bubble.input, ImVec2(ImGui::GetContentRegionAvail().x - side - spacing, inputHeight),
                                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
    {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        askRects[std::to_string(bubble.serial) + ":input"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    if (bubble.input.empty() && !ImGui::IsItemActive()) {
        const std::string hint = Format(Tr("Ask %s about %s"), target.c_str(), row ? row->name.c_str() : "");
        ImGui::GetWindowDrawList()->AddText(ImVec2(inputPos.x + ImGui::GetStyle().FramePadding.x, inputPos.y + ImGui::GetStyle().FramePadding.y),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), hint.c_str());
    }
    ImGui::SameLine(0, spacing);
    const ImVec2 buttonPos = ImGui::GetCursorScreenPos();
    askRects[std::to_string(bubble.serial) + ":send"] = {buttonPos.x, buttonPos.y, side, side};
    if (running) {
        if (IconButton("##stop", kIconSquare, side)) Call("team.thread_cancel", ObjectOf({{"thread", Json(bubble.thread)}}), true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Stop"));
    } else {
        if (IconButton("##send", kIconSend, side, !Trimmed(bubble.input).empty()) || entered) AskSend(bubble);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Send (Enter)"));
    }
    return false;
}

void NativeEditor::Impl::AskBubbles(ImVec2 viewPos, ImVec2 viewSize) {
    askRects.clear();
    Scene& scene = engine.GetScene();
    askBubbles.erase(std::remove_if(askBubbles.begin(), askBubbles.end(), [&](const AskBubble& b) { return !scene.Exists(b.entity); }),
                     askBubbles.end());
    if (askBubbles.empty() || viewSize.x < 64 || viewSize.y < 64) return;
    const Mat4 viewProj = sceneProjMat * sceneViewMat;
    const float bubbleWidth = ImGui::GetFontSize() * kBubbleWidthEm;
    const float margin = 8.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    std::map<EntityId, int> slots;
    int closed = -1;
    for (size_t i = 0; i < askBubbles.size(); ++i) {
        AskBubble& bubble = askBubbles[i];
        // Agent conversations refresh often while a turn runs.
        if (!bubble.backend.empty() && time - bubble.polled > (bubble.state["running"].asBool() ? 0.25 : 2.0)) {
            bubble.polled = time;
            const Json state = Call("team.thread", ObjectOf({{"thread", Json(bubble.thread)}}), true);
            if (Ok(state)) bubble.state = state["result"];
        }
        // Next to the object's origin; bubbles of the same object stand side by side.
        const Vec3 world = scene.WorldMatrix(bubble.entity).TransformPoint(Vec3(0, 0, 0));
        const Vec4 clip = viewProj * Vec4(world, 1.0f);
        ImVec2 anchor(viewPos.x + viewSize.x * 0.5f, viewPos.y + viewSize.y * 0.5f);
        bool visible = clip.w > 1e-4f;
        if (visible) {
            anchor = ImVec2(viewPos.x + (clip.x / clip.w * 0.5f + 0.5f) * viewSize.x, viewPos.y + (0.5f - clip.y / clip.w * 0.5f) * viewSize.y);
            visible = anchor.x >= viewPos.x && anchor.x <= viewPos.x + viewSize.x && anchor.y >= viewPos.y && anchor.y <= viewPos.y + viewSize.y;
        }
        const int slot = slots[bubble.entity]++;
        const float bubbleHeight = std::max(bubble.size.y, ImGui::GetFrameHeight() * 4.0f);
        ImVec2 at(anchor.x + 56.0f + static_cast<float>(slot) * (bubbleWidth + margin), anchor.y - bubbleHeight - 48.0f);  // up and to the right, clear of the gizmo
        at.x = std::clamp(at.x, viewPos.x + margin, std::max(viewPos.x + margin, viewPos.x + viewSize.x - bubbleWidth - margin));
        at.y = std::clamp(at.y, viewPos.y + margin + ImGui::GetFrameHeight() * 1.6f, std::max(viewPos.y + margin, viewPos.y + viewSize.y - bubbleHeight - margin));
        if (visible) {
            // The nearest point of the bubble's outline.
            const float x = std::clamp(anchor.x, at.x + 14.0f, at.x + bubbleWidth - 14.0f);
            const float y = std::clamp(anchor.y, at.y + 14.0f, at.y + bubbleHeight - 14.0f);
            const ImVec2 edge = anchor.y > at.y + bubbleHeight ? ImVec2(x, at.y + bubbleHeight)
                              : anchor.y < at.y                ? ImVec2(x, at.y)
                                                               : ImVec2(anchor.x < at.x ? at.x : at.x + bubbleWidth, y);
            draw->AddLine(anchor, edge, IM_COL32(108, 158, 255, 150), 1.5f);
            draw->AddCircleFilled(anchor, 4.5f, IM_COL32(108, 158, 255, 255));
            draw->AddCircle(anchor, 4.5f, IM_COL32(16, 18, 22, 255), 0, 1.5f);
        }
        ImGui::SetCursorScreenPos(at);
        ImGui::PushID(bubble.serial);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.12f, 0.14f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.42f, 0.62f, 1.0f, 0.45f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
        const bool shown = ImGui::BeginChild("##bubble", ImVec2(bubbleWidth, 0),
                                             ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
        if (shown && AskBubbleBody(bubble)) closed = static_cast<int>(i);
        ImGui::EndChild();
        bubble.size = ImGui::GetItemRectSize();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }
    if (closed >= 0) askBubbles.erase(askBubbles.begin() + closed);
}

}  // namespace oe
