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
#include <functional>

#include "imgui_internal.h"

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "platform/Platform.h"

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

std::string BaseName(const std::string& path) { return path.substr(path.find_last_of("/\\") + 1); }

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

// One message as a bubble or the Agent Chat panel shows it.
struct AskLine {
    std::string who, text, hint, kind, meta;  // meta: "4 tools · 18 s · $0.04" under a reply
    bool mine = false;
    Json attachments;  // the message's files (team.thread / team.messages form)
};

// The lines, then the running activity with moving dots; a change in what is drawn scrolls to it.
void DrawAskLines(const std::vector<AskLine>& lines, const std::string& activity, size_t& shown, double time,
                  const std::function<void(const std::string&)>& markdown, const std::function<void(const Json&)>& files) {
    for (size_t i = 0; i < lines.size(); ++i) {
        const AskLine& line = lines[i];
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
        } else if (!line.text.empty()) {
            markdown(line.text);
        }
        if (line.attachments.size() > 0) files(line.attachments);
        if (!line.meta.empty()) ImGui::TextDisabled("%s", line.meta.c_str());
        ImGui::PopID();
    }
    if (!activity.empty()) {
        static const char* const kDots[] = {".", "..", "..."};
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s%s", activity.c_str(), kDots[static_cast<int>(time * 3.0) % 3]);
        ImGui::PopTextWrapPos();
    }
    const size_t count = lines.size() * 2 + (activity.empty() ? 0 : 1);
    if (count != shown) ImGui::SetScrollHereY(1.0f);
    shown = count;
}

}  // namespace

std::string NativeEditor::Impl::SpotLabel(const AskBubble& bubble) const {
    const EntityRow* surface = Row(bubble.entity);
    std::string label = Format("(%.2f, %.2f, %.2f)", bubble.point.x, bubble.point.y, bubble.point.z);
    if (surface) label += " " + Format(Tr("on %s"), surface->name.c_str());
    return label;
}

std::string NativeEditor::Impl::AskContext(const AskBubble& bubble) const {
    const EntityRow* row = Row(bubble.entity);
    std::string components;
    if (row) {
        for (const std::string& c : row->components) components += (components.empty() ? "" : ", ") + c;
    }
    const std::string id = std::to_string(bubble.entity);
    std::string scene = scenePath;
    if (!scene.empty() && !engine.ProjectDir().empty()) scene = RelativePath(scenePath, engine.ProjectDir());
    const std::string where = " in scene \"" + sceneName + "\"" + (scene.empty() ? "" : " (" + scene + ")");
    if (!bubble.spot) {
        return "The person is pointing at this object in the editor's Scene view: entity #" + id + " \"" + (row ? row->name : "") +
               "\" (components: " + components + ")" + where + ". Read it with entity.get {id: " + id + "}; editor.state shows what else they see.";
    }
    const Vec3& p = bubble.point;
    const Vec3& n = bubble.normal;
    std::string text = Format("The person is pointing at a spot in the editor's Scene view: world position (%.3f, %.3f, %.3f)", p.x, p.y, p.z);
    if (row) {
        text += Format(", on the surface of entity #%s \"%s\" (components: %s; surface normal (%.2f, %.2f, %.2f))", id.c_str(), row->name.c_str(),
                       components.c_str(), n.x, n.y, n.z);
    } else {
        text += Format(", where the pointer's ray meets the ground plane (normal (%.0f, %.0f, %.0f)); nothing is drawn there", n.x, n.y, n.z);
    }
    return text + where + ". When they ask to put, build or move something \"here\", use that position (resting on that surface). "
                          "editor.state shows what else they see.";
}

// One conversation per object (or spot) and CLI: opening the bubble again shows it, with its model.
void NativeEditor::Impl::OpenAskBubble(EntityId entity, const std::string& backend) {
    for (AskBubble& bubble : askBubbles) {
        if (!bubble.spot && bubble.entity == entity && bubble.backend == backend) {
            bubble.focusInput = true;
            return;
        }
    }
    AskBubble bubble;
    bubble.serial = ++askSerial;
    bubble.entity = entity;
    bubble.backend = backend;
    if (!backend.empty()) bubble.thread = "e" + std::to_string(entity) + "-" + backend;
    askBubbles.push_back(std::move(bubble));
}

void NativeEditor::Impl::OpenAskSpot(const Vec3& point, const Vec3& normal, EntityId surface, const std::string& backend) {
    // Spots a few centimetres apart are the same place (and the same conversation).
    const Vec3 rounded(std::round(point.x * 10.0f) / 10.0f, std::round(point.y * 10.0f) / 10.0f, std::round(point.z * 10.0f) / 10.0f);
    for (AskBubble& bubble : askBubbles) {
        if (bubble.spot && bubble.backend == backend && Length(bubble.point - point) < 0.05f) {
            bubble.focusInput = true;
            return;
        }
    }
    AskBubble bubble;
    bubble.serial = ++askSerial;
    bubble.entity = surface;
    bubble.spot = true;
    bubble.point = point;
    bubble.normal = normal;
    bubble.backend = backend;
    if (!backend.empty()) {
        uint32_t hash = 2166136261u;
        for (char c : Format("%.1f,%.1f,%.1f", rounded.x, rounded.y, rounded.z)) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
        bubble.thread = Format("p%08x-", hash) + backend;
    }
    askBubbles.push_back(std::move(bubble));
}

void NativeEditor::Impl::SceneAskMenu() {
    if (!ImGui::BeginPopup("##sceneAsk")) return;
    // One submenu per target (the object, this spot): the team, then the installed CLIs.
    // Returns "team", a CLI id, or "" while nothing was chosen.
    const bool teamReady = teamAvailable && team["agents"].size() > 0;
    auto targetMenu = [&](const std::string& label) {
        std::string chosen;
        if (!ImGui::BeginMenu(label.c_str())) return chosen;
        if (ImGui::MenuItem(WithIcon(kIconUsers, Tr("Ask the Team")).c_str(), nullptr, false, teamReady)) chosen = "team";
        if (!teamReady && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (teamAvailable) ImGui::SetTooltip("%s", Tr("The team has no agents yet (Team panel)."));
            else ImGui::SetTooltip("%s", Tr("Agent teams need `oe editor`."));
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", Tr("Ask an Agent"));
        int installed = 0;
        bool detecting = teamBackends.size() == 0;
        for (const Json& backend : teamBackends.items()) {
            const std::string status = backend["status"].asString("");
            detecting = detecting || status == "detecting";
            if (status != "installed") continue;
            ++installed;
            const std::string id = backend["id"].asString("");
            if (ImGui::MenuItem((WithIcon(kIconBot, backend["name"].asString(id)) + "##" + id).c_str())) chosen = id;
        }
        if (installed == 0) {
            if (detecting) ImGui::TextDisabled("%s", Tr("Looking for agent CLIs..."));
            else ImGui::TextDisabled("%s", Tr("No agent CLI is installed."));
        }
        ImGui::EndMenu();
        return chosen;
    };

    // The object under the pointer (choosing selects it) ...
    if (const EntityRow* row = Row(askMenuEntity)) {
        const std::string chosen = targetMenu(WithIcon(kIconBox, row->name) + "##object");
        if (!chosen.empty()) {
            SelectOnly(askMenuEntity);
            scrollToRow = askMenuEntity;
            Refresh(true);
            OpenAskBubble(askMenuEntity, chosen == "team" ? std::string() : chosen);
        }
    }
    // ... and the spot itself: "put something here".
    AskBubble probe;
    probe.spot = true;
    probe.point = askMenuPoint;
    probe.entity = askMenuEntity;
    const std::string chosen = targetMenu(WithIcon(kIconMapPin, std::string(Tr("This spot")) + "  " + SpotLabel(probe)) + "##spot");
    if (!chosen.empty()) OpenAskSpot(askMenuPoint, askMenuNormal, askMenuEntity, chosen == "team" ? std::string() : chosen);
    ImGui::EndPopup();
}

void NativeEditor::Impl::AskSend(AskBubble& bubble) {
    const std::string text = Trimmed(bubble.input);
    if (text.empty() && bubble.attachments.empty()) return;
    Json files = Json::MakeArray();
    for (const std::string& file : bubble.attachments) files.push(file);
    if (bubble.backend.empty()) {
        const EntityRow* row = Row(bubble.entity);
        const std::string prefix = bubble.spot ? "[" + std::string(Tr("This spot")) + " " + SpotLabel(bubble) + "] "
                                               : "[" + (row ? row->name : std::string()) + " #" + std::to_string(bubble.entity) + "] ";
        const Json result = Call("team.send", ObjectOf({{"text", Json(prefix + text)}, {"attachments", files}}), true);
        if (!Ok(result)) {
            Notify(result["error"]["message"].asString(""), true);
            return;
        }
        bubble.sent.push_back(static_cast<uint64_t>(result["result"]["message"]["id"].asNumber(0)));
    } else {
        const Json result = Call("team.ask", ObjectOf({{"thread", Json(bubble.thread)}, {"backend", Json(bubble.backend)},
                                                       {"model", Json(bubble.model)}, {"text", Json(text)},
                                                       {"context", Json(AskContext(bubble))}, {"attachments", files}}), true);
        if (!Ok(result)) {
            const std::string hint = result["error"]["hint"].asString("");
            Notify(result["error"]["message"].asString("") + (hint.empty() ? "" : " " + hint), true);
            return;
        }
        bubble.state = result["result"];
        bubble.polled = time;
    }
    bubble.input.clear();
    bubble.attachments.clear();
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
    const std::string about = bubble.spot ? std::string(kIconMapPin) + " " + SpotLabel(bubble) : row ? row->name : std::string();
    ImGui::TextDisabled("\xc2\xb7 %s", about.c_str());
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
    std::vector<AskLine> lines;
    std::string activity;
    if (!agent && !bubble.sent.empty()) {
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
            AskLine line;
            line.who = mine ? Tr("You") : who.empty() ? target : who;
            line.text = mine ? WithoutObjectPrefix(message["text"].asString("")) : message["text"].asString("");
            line.hint = message["hint"].asString("");
            line.kind = kind;
            line.mine = mine;
            line.attachments = message["attachments"];
            lines.push_back(std::move(line));
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
    const bool empty = agent ? bubble.state["messages"].size() == 0 && !bubble.state["running"].asBool() : lines.empty() && activity.empty();
    if (empty) {
        ImGui::PushTextWrapPos(0.0f);
        if (agent) ImGui::TextDisabled("%s", Format(Tr("%s works on this object directly; the conversation stays in this bubble."), target.c_str()).c_str());
        else ImGui::TextDisabled("%s", Tr("Goes to the team chat: the lead answers unless you @mention someone."));
        ImGui::PopTextWrapPos();
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFontSize() * kLogHeightEm));
        if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY)) {
            if (agent) DrawDirectConversation(bubble.state, target, bubble.shown, false);
            else DrawAskLines(lines, activity, bubble.shown, time, [&](const std::string& text) { DrawChatMarkdown(text, {}, false, ImGui::GetColorU32(ImGuiCol_Text)); },
                              [&](const Json& files) { DrawAttachments(files, 8.0f); });
        }
        ImGui::EndChild();
    }

    // Input: Enter sends, Shift+Enter adds a line; # completes a file, @ an agent of the team
    // (team bubbles) or a scene object (agent bubbles).
    const bool running = agent && bubble.state["running"].asBool();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    AttachmentChips(bubble.attachments);
    const ImVec2 inputPos = ImGui::GetCursorScreenPos();
    const float inputHeight = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    std::array<float, 4> inputRect{};
    bool typing = false;
    const bool entered = ChatInput("##input", bubble.input, ImVec2(ImGui::GetContentRegionAvail().x - (side + spacing) * 2.0f, inputHeight), bubble.complete,
                                   agent ? ChatMentions::Entities : ChatMentions::Agents, bubble.focusInput, &inputRect, &typing);
    askRects[std::to_string(bubble.serial) + ":input"] = inputRect;
    if (bubble.input.empty() && !typing) {
        const std::string hint = Format(Tr("Ask %s about %s"), target.c_str(), bubble.spot ? Tr("this spot") : row ? row->name.c_str() : "");
        ImGui::GetWindowDrawList()->AddText(ImVec2(inputPos.x + ImGui::GetStyle().FramePadding.x, inputPos.y + ImGui::GetStyle().FramePadding.y),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), hint.c_str());
    }
    ImGui::SameLine(0, spacing);
    if (IconButton("##attach", kIconPaperclip, side)) ChooseAttachment(bubble.attachments);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Attach a file (or drop files on the bubble)"));
    ImGui::SameLine(0, spacing);
    const ImVec2 buttonPos = ImGui::GetCursorScreenPos();
    askRects[std::to_string(bubble.serial) + ":send"] = {buttonPos.x, buttonPos.y, side, side};
    if (running) {
        if (IconButton("##stop", kIconSquare, side)) Call("team.thread_cancel", ObjectOf({{"thread", Json(bubble.thread)}}), true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Stop"));
    } else {
        if (IconButton("##send", kIconSend, side, !Trimmed(bubble.input).empty() || !bubble.attachments.empty()) || entered) AskSend(bubble);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Send (Enter)"));
    }
    return false;
}

void NativeEditor::Impl::AskBubbles(ImVec2 viewPos, ImVec2 viewSize) {
    askRects.clear();
    Scene& scene = engine.GetScene();
    askBubbles.erase(std::remove_if(askBubbles.begin(), askBubbles.end(), [&](const AskBubble& b) { return !b.spot && !scene.Exists(b.entity); }),
                     askBubbles.end());
    if (askBubbles.empty() || viewSize.x < 64 || viewSize.y < 64) return;
    const Mat4 viewProj = sceneProjMat * sceneViewMat;
    const float bubbleWidth = ImGui::GetFontSize() * kBubbleWidthEm;
    const float margin = 8.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    std::vector<ImRect> placed;  // bubbles already laid out this frame: later ones move out of their way
    int closed = -1;
    for (size_t i = 0; i < askBubbles.size(); ++i) {
        AskBubble& bubble = askBubbles[i];
        // Agent conversations refresh often while a turn runs.
        if (!bubble.backend.empty() && time - bubble.polled > (bubble.state["running"].asBool() ? 0.25 : 2.0)) {
            bubble.polled = time;
            const bool first = !bubble.state.has("thread");  // not read yet (indexing a null Json makes it an object)
            const Json state = Call("team.thread", ObjectOf({{"thread", Json(bubble.thread)}}), true);
            if (Ok(state)) bubble.state = state["result"];
            // Reopened: the conversation continues with the model it last used.
            if (first && bubble.state["backend"].asString("") == bubble.backend) bubble.model = bubble.state["model"].asString("");
        }
        // Next to the object's origin; bubbles of the same object stand side by side.
        const Vec3 world = bubble.spot ? bubble.point : scene.WorldMatrix(bubble.entity).TransformPoint(Vec3(0, 0, 0));
        const Vec4 clip = viewProj * Vec4(world, 1.0f);
        ImVec2 anchor(viewPos.x + viewSize.x * 0.5f, viewPos.y + viewSize.y * 0.5f);
        bool visible = clip.w > 1e-4f;
        if (visible) {
            anchor = ImVec2(viewPos.x + (clip.x / clip.w * 0.5f + 0.5f) * viewSize.x, viewPos.y + (0.5f - clip.y / clip.w * 0.5f) * viewSize.y);
            visible = anchor.x >= viewPos.x && anchor.x <= viewPos.x + viewSize.x && anchor.y >= viewPos.y && anchor.y <= viewPos.y + viewSize.y;
        }
        const float bubbleHeight = std::max(bubble.size.y, ImGui::GetFrameHeight() * 4.0f);
        const float top = viewPos.y + margin + ImGui::GetFrameHeight() * 1.6f, bottom = viewPos.y + viewSize.y - margin;
        auto clampInView = [&](ImVec2 p) {
            p.x = std::clamp(p.x, viewPos.x + margin, std::max(viewPos.x + margin, viewPos.x + viewSize.x - bubbleWidth - margin));
            p.y = std::clamp(p.y, top, std::max(top, bottom - bubbleHeight));
            return p;
        };
        ImVec2 at = clampInView(ImVec2(anchor.x + 56.0f, anchor.y - bubbleHeight - 48.0f));  // up and to the right, clear of the gizmo
        // Not on top of another bubble: below it, or (no room below) in a column to its left.
        for (int attempt = 0; attempt < 16; ++attempt) {
            const ImRect self(at, ImVec2(at.x + bubbleWidth, at.y + bubbleHeight));
            const auto hit = std::find_if(placed.begin(), placed.end(), [&](const ImRect& other) { return other.Overlaps(self); });
            if (hit == placed.end()) break;
            at.y = hit->Max.y + margin;
            if (at.y + bubbleHeight > bottom) at = ImVec2(hit->Min.x - bubbleWidth - margin, top);
            at = clampInView(at);
        }
        placed.emplace_back(at, ImVec2(at.x + bubbleWidth, at.y + bubbleHeight));
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
        bubble.pos = at;
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

void NativeEditor::Impl::DrawDirectConversation(const Json& state, const std::string& target, size_t& shown, bool details) {
    std::vector<AskLine> lines;
    for (const Json& message : state["messages"].items()) {
        AskLine line;
        line.mine = message["from"].asString("") == "user";
        line.who = line.mine ? Tr("You") : target;
        line.text = message["text"].asString("");
        line.hint = message["hint"].asString("");
        line.kind = message["kind"].asString("message");
        if (line.kind == "notice") line.who = Tr("Editor");
        line.attachments = message["attachments"];
        if (details && message.has("turn")) {
            const Json& turn = message["turn"];
            line.meta = Format(Tr("%d tools"), turn["tools"].asInt(0)) + Format("  \xc2\xb7  %.0f s", turn["seconds"].asNumber(0));
            if (turn["costKnown"].asBool(false)) line.meta += Format("  \xc2\xb7  $%.2f", turn["costUsd"].asNumber(0));
        }
        lines.push_back(std::move(line));
    }
    std::string activity;
    if (state["running"].asBool()) activity = Format("%s  %.0fs", state["activity"].asString("Starting").c_str(), state["elapsed"].asNumber(0));
    DrawAskLines(lines, activity, shown, time, [&](const std::string& text) { DrawChatMarkdown(text, {}, false, ImGui::GetColorU32(ImGuiCol_Text)); },
                 [&](const Json& files) { DrawAttachments(files, details ? 12.0f : 8.0f); });
}

void NativeEditor::Impl::DrawAttachments(const Json& attachments, float thumbnailEm) {
    const float font = ImGui::GetFontSize();
    int index = 0;
    for (const Json& attachment : attachments.items()) {
        const std::string path = attachment["path"].asString("");
        ImGui::PushID(index++);
        if (attachment["type"].asString("") == "image" &&
            DrawImageFile(JoinPath(engine.ProjectDir(), path), ImVec2(font * thumbnailEm, font * thumbnailEm * 0.7f), 1.0f)) {
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsItemClicked()) chatViewerPath = path;
        } else if (ImGui::SmallButton(WithIcon(kIconFile, BaseName(path)).c_str())) {
            OpenChatLink(path);
        }
        HelpTooltip(path);
        ImGui::PopID();
    }
}

void NativeEditor::Impl::AttachmentChips(std::vector<std::string>& files) {
    if (files.empty()) return;
    const float font = ImGui::GetFontSize();
    float x = 0.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    for (size_t i = 0; i < files.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const std::string label = BaseName(files[i]) + "  " + kIconX;
        const float w = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (i > 0 && x + w < avail) ImGui::SameLine();
        else x = 0.0f;
        x += w + ImGui::GetStyle().ItemSpacing.x;
        const bool remove = ImGui::SmallButton(label.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            DrawImageFile(files[i], ImVec2(font * 14.0f, font * 14.0f), 1.0f);
            ImGui::TextDisabled("%s", files[i].c_str());
            ImGui::TextDisabled("%s", Tr("Click to remove"));
            ImGui::EndTooltip();
        }
        ImGui::PopID();
        if (remove) {
            files.erase(files.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

void NativeEditor::Impl::ChooseAttachment(std::vector<std::string>& files) {
    if (files.size() >= 8) {
        Notify(Tr("A message carries at most 8 attachments."), true);
        return;
    }
    FileDialogOptions request;
    request.title = Tr("Attach a File");
    request.directory = engine.ProjectDir();
    const FileDialogResult result = window ? window->ChooseFile(request) : FileDialogResult{};
    if (result.status == FileDialogResult::Status::Cancelled) return;
    if (result.status == FileDialogResult::Status::Unavailable) {
        Notify(Tr("Native file dialogs unavailable; drop the file on the input instead."), true);
        return;
    }
    if (result.status == FileDialogResult::Status::Error) {
        Notify(result.error, true);
        return;
    }
    if (std::find(files.begin(), files.end(), result.path) == files.end()) files.push_back(result.path);
}

void NativeEditor::Impl::AgentChatSelect(const std::string& backend, const std::string& thread) {
    agentChatBackend = backend;
    agentChatThread = thread.empty() ? "chat-" + backend : thread;
    // The newest panel conversation of that CLI, when one exists and none was named.
    if (thread.empty()) {
        std::string newest;
        for (const Json& item : agentChatThreads.items()) {
            const std::string key = item["thread"].asString("");
            if (item["backend"].asString("") != backend || key.rfind("chat-", 0) != 0) continue;
            if (newest.empty() || item["updated"].asString("") > newest) {
                newest = item["updated"].asString("");
                agentChatThread = key;
            }
        }
    }
    agentChatState = Json();
    agentChatModel.clear();
    agentChatShown = 0;
    agentChatPolled = -100;
    agentChatFocusInput = true;
}

void NativeEditor::Impl::AgentChatSend() {
    const std::string text = Trimmed(agentChatInput);
    if ((text.empty() && agentChatAttachments.empty()) || agentChatBackend.empty()) return;
    Json files = Json::MakeArray();
    for (const std::string& file : agentChatAttachments) files.push(file);
    std::string context = "The person writes from the editor's Agent Chat panel, outside the agent team.";
    if (const EntityRow* row = Row(Primary())) {
        std::string components;
        for (const std::string& c : row->components) components += (components.empty() ? "" : ", ") + c;
        context += " Selected in the editor: entity #" + std::to_string(Primary()) + " \"" + row->name + "\" (components: " + components + ").";
    }
    context += " editor.state shows what they see.";
    const Json result = Call("team.ask", ObjectOf({{"thread", Json(agentChatThread)}, {"backend", Json(agentChatBackend)},
                                                   {"model", Json(agentChatModel)}, {"text", Json(text)}, {"context", Json(context)},
                                                   {"attachments", files}}), true);
    if (!Ok(result)) {
        const std::string hint = result["error"]["hint"].asString("");
        Notify(result["error"]["message"].asString("") + (hint.empty() ? "" : " " + hint), true);
        return;
    }
    agentChatState = result["result"];
    agentChatPolled = time;
    agentChatThreadsPolled = -100;
    agentChatInput.clear();
    agentChatAttachments.clear();
    agentChatFocusInput = true;
}

void NativeEditor::Impl::AgentChatPanel() {
    if (requestAgentChatFocus) {
        ImGui::SetNextWindowFocus();
        requestAgentChatFocus = false;
    }
    if (!ImGui::Begin(WithIcon(kIconBot, TrId("Agent Chat")).c_str(), &showAgentChat)) {
        agentChatRect = {0, 0, 0, 0};
        ImGui::End();
        return;
    }
    agentChatRect = {ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowSize().x, ImGui::GetWindowSize().y};
    // The installed CLIs; the first one until the person picks another.
    std::vector<const Json*> installed;
    bool detecting = teamBackends.size() == 0;
    for (const Json& backend : teamBackends.items()) {
        detecting = detecting || backend["status"].asString("") == "detecting";
        if (backend["status"].asString("") == "installed") installed.push_back(&backend);
    }
    if (installed.empty()) {
        ImGui::TextDisabled("%s", Tr(detecting ? "Looking for agent CLIs..." : "No agent CLI is installed."));
        if (!detecting) ImGui::TextDisabled("%s", Tr("Agents work through the coding-agent CLIs installed on this PC (Claude Code, Codex)."));
        ImGui::End();
        return;
    }
    if (time - agentChatThreadsPolled > 2.0) {
        agentChatThreadsPolled = time;
        const Json threads = Call("team.threads", Json(), true);
        if (Ok(threads)) agentChatThreads = threads["result"];
    }
    const Json* backend = TeamBackend(agentChatBackend);
    if (!backend || (*backend)["status"].asString("") != "installed") {
        AgentChatSelect((*installed.front())["id"].asString(""), "");
        backend = installed.front();
    }
    const bool running = agentChatState["running"].asBool();
    if (time - agentChatPolled > (running ? 0.25 : 1.5)) {
        agentChatPolled = time;
        const bool first = !agentChatState.has("thread");  // not read yet (indexing a null Json makes it an object)
        const Json state = Call("team.thread", ObjectOf({{"thread", Json(agentChatThread)}}), true);
        if (Ok(state)) agentChatState = state["result"];
        if (first && agentChatState["backend"].asString("") == agentChatBackend) agentChatModel = agentChatState["model"].asString("");
    }
    const std::string target = (*backend)["name"].asString(agentChatBackend);
    const ImGuiStyle& style = ImGui::GetStyle();

    // Toolbar: CLI, model, conversation, new conversation.
    const float field = ImGui::GetFontSize() * 9.0f;
    ImGui::SetNextItemWidth(field);
    if (ImGui::BeginCombo("##cli", target.c_str())) {
        for (const Json* item : installed) {
            const std::string id = (*item)["id"].asString("");
            if (ImGui::Selectable(((*item)["name"].asString(id) + "##" + id).c_str(), id == agentChatBackend) && id != agentChatBackend) AgentChatSelect(id, "");
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    std::string modelLabel = agentChatModel.empty() ? std::string(Tr("CLI default")) : agentChatModel;
    for (const Json& model : (*backend)["models"].items()) {
        if (model["id"].asString("") == agentChatModel) modelLabel = model["label"].asString(agentChatModel);
    }
    ImGui::SetNextItemWidth(field);
    if (ImGui::BeginCombo("##model", modelLabel.c_str())) {
        if (ImGui::Selectable(Tr("CLI default"), agentChatModel.empty())) agentChatModel.clear();
        for (const Json& model : (*backend)["models"].items()) {
            const std::string id = model["id"].asString("");
            if (ImGui::Selectable((model["label"].asString(id) + "##" + id).c_str(), agentChatModel == id)) agentChatModel = id;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Model of the next message"));
    // Conversations of this CLI: the panel's own and those of Scene bubbles (object or spot).
    auto threadLabel = [&](const Json& item) {
        const std::string key = item["thread"].asString("");
        std::string title = item["title"].asString("");
        if (title.empty()) title = Tr("New conversation");
        if (key.size() > 1 && key[0] == 'e' && key[1] >= '0' && key[1] <= '9') {
            const EntityRow* row = Row(static_cast<EntityId>(std::strtoul(key.c_str() + 1, nullptr, 10)));
            return std::string(kIconBox) + " " + (row ? row->name : std::string("#") + key.substr(1, key.find('-') - 1)) + ": " + title;
        }
        if (key.rfind("p", 0) == 0 && key.rfind("chat-", 0) != 0) return std::string(kIconMapPin) + " " + title;
        return title;
    };
    std::string current = Tr("New conversation");
    for (const Json& item : agentChatThreads.items()) {
        if (item["thread"].asString("") == agentChatThread) current = threadLabel(item);
    }
    ImGui::SameLine();
    const float newWidth = ImGui::CalcTextSize(Tr("New")).x + ImGui::CalcTextSize(kIconPlus).x + style.FramePadding.x * 2 + style.ItemInnerSpacing.x * 2;
    ImGui::SetNextItemWidth(std::max(field, ImGui::GetContentRegionAvail().x - newWidth - ImGui::GetFrameHeight() - style.ItemSpacing.x * 2.0f));
    if (ImGui::BeginCombo("##thread", current.c_str())) {
        for (const Json& item : agentChatThreads.items()) {
            if (item["backend"].asString("") != agentChatBackend) continue;
            const std::string key = item["thread"].asString("");
            if (ImGui::Selectable((threadLabel(item) + "##" + key).c_str(), key == agentChatThread) && key != agentChatThread) AgentChatSelect(agentChatBackend, key);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(WithIcon(kIconPlus, Tr("New")).c_str())) {
        int n = 1;
        for (const Json& item : agentChatThreads.items()) {
            if (item["thread"].asString("").rfind("chat-" + agentChatBackend, 0) == 0) ++n;
        }
        AgentChatSelect(agentChatBackend, "chat-" + agentChatBackend + "-" + std::to_string(n));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Start a new conversation (the CLI forgets the earlier one)"));
    ImGui::SameLine();
    if (IconButton("##settings", kIconSettings, ImGui::GetFrameHeight())) {
        showSettings = true;
        settingsTab = "agents";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Agent settings: installed CLIs, login, engine connection, permissions"));

    // The conversation, then the files waiting to go and the input.
    const float inputHeight = ImGui::GetTextLineHeight() * 3.0f + style.FramePadding.y * 2.0f;
    const float chipsHeight = agentChatAttachments.empty() ? 0.0f : ImGui::GetFrameHeight() + style.ItemSpacing.y;
    const float logHeight = ImGui::GetContentRegionAvail().y - inputHeight - chipsHeight - style.ItemSpacing.y;
    if (ImGui::BeginChild("##log", ImVec2(0, std::max(ImGui::GetFrameHeight(), logHeight)), ImGuiChildFlags_None)) {
        if (agentChatState["messages"].size() == 0 && !agentChatState["running"].asBool()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", Format(Tr("Talk to %s directly, outside the team. It works in this project through the open editor."), target.c_str()).c_str());
            ImGui::PopTextWrapPos();
        } else {
            DrawDirectConversation(agentChatState, target, agentChatShown, true);
        }
    }
    ImGui::EndChild();
    AttachmentChips(agentChatAttachments);
    const float side = ImGui::GetFrameHeight();
    const ImVec2 inputPos = ImGui::GetCursorScreenPos();
    bool typing = false;
    const bool entered = ChatInput("##input", agentChatInput, ImVec2(ImGui::GetContentRegionAvail().x - (side + style.ItemInnerSpacing.x) * 2.0f, inputHeight),
                                   agentChatComplete, ChatMentions::Entities, agentChatFocusInput, &agentChatInputRect, &typing);
    if (agentChatInput.empty() && !typing) {
        ImGui::GetWindowDrawList()->AddText(ImVec2(inputPos.x + style.FramePadding.x, inputPos.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            Format(Tr("Message %s (Enter sends; @ an object, # a file)"), target.c_str()).c_str());
    }
    ImGui::SameLine(0, style.ItemInnerSpacing.x);
    if (IconButton("##attach", kIconPaperclip, side)) ChooseAttachment(agentChatAttachments);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Attach a file (or drop files on this panel)"));
    ImGui::SameLine(0, style.ItemInnerSpacing.x);
    if (agentChatState["running"].asBool()) {
        if (IconButton("##stop", kIconSquare, side)) Call("team.thread_cancel", ObjectOf({{"thread", Json(agentChatThread)}}), true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Stop"));
    } else {
        if (IconButton("##send", kIconSend, side, !Trimmed(agentChatInput).empty() || !agentChatAttachments.empty()) || entered) AgentChatSend();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Send (Enter)"));
    }
    ImGui::End();
}

}  // namespace oe
