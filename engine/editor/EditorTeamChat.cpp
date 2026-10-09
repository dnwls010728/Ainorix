// Team Chat panel (docs/TEAM.md §7.3): the team's messages with their attachments, the live line
// of every running turn with its Stop button, and the input: files dropped on the panel are
// attached, `@` completes an agent and `#` a project file, each with a preview. Reads
// team.messages / team.state and writes through team.send / team.cancel only.
#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorMarkdown.h"
#include "editor/EditorText.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "imgui_internal.h"

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Log.h"
#include "render/GpuRenderer.h"
#include "render/Mesh.h"

namespace oe {
namespace {

constexpr size_t kMaxChatEntries = 1000;  // kept in the panel; older ones are dropped from the top
constexpr size_t kMaxCandidates = 300;    // entries of the completion list (eight rows show, the rest scroll)
constexpr size_t kMaxPendingAttachments = 8;

std::string Lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string BaseName(const std::string& path) { return path.substr(path.find_last_of("/\\") + 1); }

bool EndsWith(const std::string& text, const char* suffix) {
    const size_t length = std::strlen(suffix);
    return text.size() > length && text.compare(text.size() - length, length, suffix) == 0;
}

// "2026-10-03T14:02:11Z" (UTC) -> "23:02" on this PC's clock; the text itself when it is not a time.
std::string LocalClock(const std::string& utc) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(utc.c_str(), "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &minute, &second) != 6) return utc;
    // Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil).
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const long long yearOfEra = year - era * 400;
    const long long dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const long long days = era * 146097 + yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear - 719468;
    const std::time_t stamp = static_cast<std::time_t>(days * 86400 + hour * 3600 + minute * 60 + second);
    const std::tm* local = std::localtime(&stamp);
    return local ? Format("%02d:%02d", local->tm_hour, local->tm_min) : utc;
}

// The reference being typed before `cursor`: a word that starts with '@' (an agent) or '#' (a
// project file). Gives the marker, where it is and the text after it; false otherwise.
bool TokenAt(const char* text, int cursor, char& marker, int& at, std::string& prefix) {
    for (at = cursor - 1; at >= 0; --at) {
        const char c = text[at];
        if (c == ' ' || c == '\n' || c == '\t') break;
    }
    ++at;  // first character of the word
    if (at >= cursor || (text[at] != '@' && text[at] != '#')) return false;
    marker = text[at];
    prefix = Lower(std::string(text + at + 1, static_cast<size_t>(cursor - at - 1)));
    return true;
}

// Size that fits `width` x `height` into the box, keeping the aspect (never larger than `zoom` x).
ImVec2 FitSize(int width, int height, ImVec2 box, float zoom) {
    const float scale = std::min(zoom, std::min(box.x / static_cast<float>(std::max(1, width)), box.y / static_cast<float>(std::max(1, height))));
    return ImVec2(std::max(1.0f, static_cast<float>(width) * scale), std::max(1.0f, static_cast<float>(height) * scale));
}

}  // namespace

ImU32 AgentColor(const std::string& id) {
    const uint64_t hash = Fnv1a64(reinterpret_cast<const uint8_t*>(id.data()), id.size());
    return ImU32(ImColor::HSV(static_cast<float>(hash % 360) / 360.0f, 0.55f, 0.85f));
}

// Agents (and "all") a typed mention prefix could mean: (id, can take a turn now).
std::vector<std::pair<std::string, bool>> NativeEditor::Impl::MentionCandidates(const std::string& prefix) const {
    std::vector<std::pair<std::string, bool>> out;
    const Json& roster = team;
    for (const Json& agent : roster["agents"].items()) {
        const std::string id = agent["id"].asString("");
        if (id.compare(0, prefix.size(), prefix) != 0 && Lower(agent["name"].asString("")).compare(0, prefix.size(), prefix) != 0) continue;
        bool offline = false;
        const Json& live = teamState;
        for (const Json& entry : live["agents"].items()) offline = offline || (entry["id"].asString("") == id && entry["state"].asString("") == "offline");
        out.emplace_back(id, !offline);
    }
    if (std::string("all").compare(0, prefix.size(), prefix) == 0 && !out.empty()) out.emplace_back("all", true);
    return out;
}

// Project files a typed `#` prefix could mean: names that start with it first, then paths that
// contain it.
std::vector<std::string> NativeEditor::Impl::FileCandidates(const std::string& prefix) const {
    std::vector<std::string> starts, contains;
    const Json& list = assets;
    for (const Json& asset : list.items()) {
        const std::string path = asset["path"].asString("");
        const std::string lowered = Lower(path);
        if (Lower(BaseName(path)).compare(0, prefix.size(), prefix) == 0) starts.push_back(path);
        else if (lowered.find(prefix) != std::string::npos) contains.push_back(path);
    }
    starts.insert(starts.end(), contains.begin(), contains.end());
    if (starts.size() > kMaxCandidates) starts.resize(kMaxCandidates);
    return starts;
}

void NativeEditor::Impl::PollChat() {
    const double revision = teamState["revision"].asNumber(0.0);
    if (revision == chatRevision) return;
    Json args = Json::MakeObject();
    args["limit"] = chatLoaded ? 500 : 200;
    if (chatLoaded) args["after"] = chatLast;
    const Json response = Call("team.messages", args, true);
    if (!Ok(response)) return;
    const Json& result = response["result"];
    // The log was cleared (team.clear) or trimmed: forget what no longer exists.
    const double first = result["first"].asNumber(0.0);
    chat.erase(std::remove_if(chat.begin(), chat.end(), [&](const ChatEntry& entry) { return first == 0.0 || entry.message["id"].asNumber(0.0) < first; }), chat.end());
    for (const Json& message : result["messages"].items()) {
        ChatEntry entry;
        entry.message = message;
        // Project files named in the text become buttons that open them.
        const std::string text = message["text"].asString("");
        for (size_t start = 0; start < text.size() && entry.links.size() < 8;) {
            const size_t end = std::min(text.find_first_of(" \t\r\n\"'`()[]<>,", start), text.size());
            std::string token = text.substr(start, end - start);
            while (!token.empty() && (token.back() == '.' || token.back() == ':' || token.back() == ';')) token.pop_back();
            if ((EndsWith(token, ".lua") || EndsWith(token, ".scene.json")) && token.find("..") == std::string::npos && token.find(':') == std::string::npos && token[0] != '/' &&
                std::find(entry.links.begin(), entry.links.end(), token) == entry.links.end() && FileExists(JoinPath(engine.ProjectDir(), token))) {
                entry.links.push_back(token);
            }
            // Any existing project file a word names is tinted in the text (not only scripts and scenes).
            if (token.size() > 2 && token.find('.') != std::string::npos && token.find("..") == std::string::npos && token.find(':') == std::string::npos && token[0] != '/' &&
                token[0] != '@' && entry.files.size() < 32 && std::find(entry.files.begin(), entry.files.end(), token) == entry.files.end() &&
                FileExists(JoinPath(engine.ProjectDir(), token))) {
                entry.files.push_back(token);
            }
            size_t covered = 0;
            if (!token.empty() && token[0] == '@' && ChatTokenColor(token, nullptr, covered)) entry.rich = true;
            start = end + 1;
        }
        entry.rich = entry.rich || !entry.files.empty();
        const std::string kind = message["kind"].asString("");
        if (chatLoaded && !chatVisible && message["from"].asString("") != "user" && kind != "notice") ++chatUnread;
        chat.push_back(std::move(entry));
        chatScrollToBottom = true;
    }
    if (chat.size() > kMaxChatEntries) chat.erase(chat.begin(), chat.begin() + static_cast<std::ptrdiff_t>(chat.size() - kMaxChatEntries));
    const bool more = result["messages"].size() >= static_cast<size_t>(args["limit"].asInt());
    if (result["messages"].size() > 0) chatLast = result["messages"][result["messages"].size() - 1]["id"].asNumber(0.0);
    else if (!more) chatLast = std::max(chatLast, result["last"].asNumber(0.0));
    chatLoaded = true;
    if (!more) chatRevision = revision;  // a full page: the rest comes next frame
}

void NativeEditor::Impl::SendChat() {
    chatFocusInput = true;  // Enter took the keyboard away from the input: give it back
    if (chatInput.find_first_not_of(" \t\r\n") == std::string::npos && chatAttachments.empty()) return;
    Json args = ObjectOf({{"text", Json(chatInput)}});
    if (!chatAttachments.empty()) {
        Json files = Json::MakeArray();
        for (const std::string& file : chatAttachments) files.push(file);
        args["attachments"] = std::move(files);
    }
    if (Ok(Call("team.send", args))) {
        chatInput.clear();
        chatAutoMention.clear();
        chatAttachments.clear();
        chatScrollToBottom = true;
    }
}

// Files dropped on the Team Chat panel are attached to the next message; the others are left
// for the import (ImportDroppedFiles), as before.
void NativeEditor::Impl::RouteDroppedFiles() {
    std::vector<std::string> other;
    for (size_t i = 0; i < droppedFiles.size(); ++i) {
        const ImVec2 at = i < droppedAt.size() ? droppedAt[i] : ImVec2(-1, -1);
        const bool onChat = chatVisible && at.x >= chatRect[0] && at.y >= chatRect[1] && at.x < chatRect[0] + chatRect[2] && at.y < chatRect[1] + chatRect[3];
        if (!onChat) {
            other.push_back(droppedFiles[i]);
        } else if (chatAttachments.size() < kMaxPendingAttachments) {
            if (std::find(chatAttachments.begin(), chatAttachments.end(), droppedFiles[i]) == chatAttachments.end()) chatAttachments.push_back(droppedFiles[i]);
            chatFocusInput = true;
        } else {
            Notify(Tr("A message carries at most 8 attachments."), true);
        }
    }
    droppedFiles.swap(other);
    droppedAt.clear();
}

// A picture from a file, fitted into `box`; false (nothing drawn) when it cannot be shown.
bool NativeEditor::Impl::DrawImageFile(const std::string& absolutePath, ImVec2 box, float zoom) {
    const std::shared_ptr<const Texture> image = AvatarTexture("file:" + absolutePath);
    if (!image || image->width <= 0 || image->height <= 0 || !engine.Gpu()) return false;
    ImGui::Image(ViewTexture(engine.Gpu()->ImageView(image)), FitSize(image->width, image->height, box, zoom));
    return true;
}

void NativeEditor::Impl::TeamChatPanel() {
    if (requestTeamChatFocus) {
        ImGui::SetNextWindowFocus();
        requestTeamChatFocus = false;
    }
    // The tab counts what arrived while it was hidden.
    const std::string title = WithIcon(kIconMessagesSquare, chatUnread > 0 ? Format("%s (%d)###Team Chat", Tr("Team Chat"), chatUnread) : TrId("Team Chat"));
    chatVisible = ImGui::Begin(title.c_str(), &showTeamChat);
    if (!chatVisible) {
        ImGui::End();
        return;
    }
    chatUnread = 0;
    chatRect = {ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowSize().x, ImGui::GetWindowSize().y};
    const ImGuiStyle& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize(), line = ImGui::GetTextLineHeight(), avatarSize = line * 1.6f;
    const std::string project = engine.ProjectDir();
    const Json& roster = team;
    const Json& live = teamState;
    auto agentOf = [&](const std::string& id) -> const Json* {
        for (const Json& agent : roster["agents"].items()) {
            if (agent["id"].asString("") == id) return &agent;
        }
        return nullptr;
    };
    auto avatarKey = [&](const Json* agent) {
        if (!agent) return std::string();
        return (*agent)["avatar"].asString("") == "file" ? "file:" + JoinPath(project, (*agent)["avatarPath"].asString("")) : (*agent)["avatar"].asString("");
    };
    auto openFile = [&](const std::string& path) {
        if (EndsWith(path, ".lua")) OpenScript(path);
        else if (EndsWith(path, ".scene.json")) RequestAction({PendingAction::Kind::LoadScene, path});
    };
    std::vector<const Json*> busy;  // agents with a turn running or waiting: one live line each
    for (const Json& entry : live["agents"].items()) {
        const std::string state = entry["state"].asString("");
        if (state == "thinking" || state == "working" || state == "queued") busy.push_back(&entry);
    }
    const float inputHeight = line * 2.0f + style.FramePadding.y * 2.0f;
    const float liveHeight = static_cast<float>(busy.size()) * (avatarSize + style.ItemSpacing.y);
    const float chipsHeight = chatAttachments.empty() ? 0.0f : ImGui::GetFrameHeight() + style.ItemSpacing.y;

    // ----- Messages
    ImGui::BeginChild("##messages", ImVec2(0, -(inputHeight + liveHeight + chipsHeight + line + style.ItemSpacing.y * 3.0f)));
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - line;
    if (chat.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", Tr("Mention an agent with @name to give it work. A message without a mention goes to the lead."));
        ImGui::TextDisabled("%s", Tr("Drop files on this panel to attach them; type # to refer to a project file."));
        ImGui::PopTextWrapPos();
    }
    for (ChatEntry& entry : chat) {
        const Json& message = entry.message;
        const std::string from = message["from"].asString(""), kind = message["kind"].asString("message"), text = message["text"].asString("");
        ImGui::PushID(static_cast<int>(message["id"].asNumber(0.0)));
        if (kind == "notice") {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopID();
            continue;
        }
        const Json* agent = agentOf(from);
        const std::string name = from == "user" ? std::string(Tr("You")) : agent ? (*agent)["name"].asString(from) : from;
        const ImVec2 avatarPos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avatarSize, avatarSize));
        DrawAvatar(ImGui::GetWindowDrawList(), avatarPos, avatarSize, avatarKey(agent), from, name);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(from == "user" ? ImGui::GetColorU32(ImGuiCol_Text) : AgentColor(from)), "%s", name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", LocalClock(message["time"].asString("")).c_str());
        const Json& turn = message["turn"];
        if (turn.isObject()) {
            // Collapsed turn summary; opens to the tool steps.
            ImGui::SameLine();
            const std::string summary = Format(Tr("%d steps, %d s"), turn["tools"].asInt(0), static_cast<int>(turn["seconds"].asNumber(0.0) + 0.5));
            if (turn["steps"].size() > 0) {
                if (ImGui::SmallButton((std::string(entry.open ? kIconChevronDown : kIconChevronRight) + " " + summary + "##steps").c_str())) entry.open = !entry.open;
            } else {
                ImGui::TextDisabled("%s", summary.c_str());
            }
            const Json& usage = turn["usage"];
            if (usage["known"].asBool()) {
                ImGui::TextDisabled(Tr("%.0f in, %.0f out, %.0f cached"), usage["inputTokens"].asNumber(), usage["outputTokens"].asNumber(), usage["cacheReadTokens"].asNumber());
            } else ImGui::TextDisabled("%s", Tr("Usage unavailable"));
            if (turn["costKnown"].asBool()) ImGui::TextDisabled("$%.4f", turn["costUsd"].asNumber());
            else ImGui::TextDisabled("%s", Tr("Cost unavailable"));
            if (entry.open) {
                for (const Json& step : turn["steps"].items()) ImGui::TextDisabled("   %s", step.asString("").c_str());
            }
        }
        // Text as Markdown; errors in red with their hint below.
        DrawChatMarkdown(text, entry.files, entry.rich, kind == "error" ? IM_COL32(240, 110, 110, 255) : ImGui::GetColorU32(ImGuiCol_Text));
        if (kind == "error" && !message["hint"].asString("").empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", message["hint"].asString("").c_str());
            ImGui::PopTextWrapPos();
        }
        // Attachments: pictures as thumbnails (click = the viewer), other files as buttons.
        int attachmentIndex = 0;
        for (const Json& attachment : message["attachments"].items()) {
            const std::string path = attachment["path"].asString("");
            ImGui::PushID(attachmentIndex++);
            if (attachment["type"].asString("") == "image" && DrawImageFile(JoinPath(project, path), ImVec2(font * 16.0f, font * 11.0f), 1.0f)) {
                if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsItemClicked()) chatViewerPath = path;
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextDisabled("%s  %dx%d", BaseName(path).c_str(), attachment["width"].asInt(0), attachment["height"].asInt(0));
                if (path.rfind("assets/", 0) != 0 && ImGui::SmallButton(Tr("Import to assets"))) ImportChatImage(path);
                ImGui::EndGroup();
            } else if (ImGui::SmallButton(BaseName(path).c_str())) {
                openFile(path);
            }
            HelpTooltip(path);
            ImGui::PopID();
        }
        for (const std::string& link : entry.links) {
            if (ImGui::SmallButton(link.c_str())) openFile(link);
            ImGui::SameLine();
        }
        if (!entry.links.empty()) ImGui::NewLine();
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::PopID();
    }
    // Stick to the bottom unless the person scrolled up to read.
    if (chatScrollToBottom && (atBottom || chatJustSent)) ImGui::SetScrollHereY(1.0f);
    chatScrollToBottom = chatJustSent = false;
    ImGui::EndChild();

    // ----- Live lines: who is working on what, with Stop
    chatStopRect = {0, 0, 0, 0};
    for (const Json* entry : busy) {
        const std::string id = (*entry)["id"].asString(""), state = (*entry)["state"].asString("");
        const Json* agent = agentOf(id);
        const std::string name = agent ? (*agent)["name"].asString(id) : id;
        ImGui::PushID(id.c_str());
        const ImVec2 avatarPos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avatarSize, avatarSize));
        DrawAvatar(ImGui::GetWindowDrawList(), avatarPos, avatarSize, avatarKey(agent), id, name);
        ImGui::SameLine();
        // Stop comes before the text: the activity changes all the time and must not move the button.
        if (ImGui::SmallButton(Tr("Stop"))) Call("team.cancel", ObjectOf({{"id", Json(id)}}));
        if (chatStopRect[2] == 0.0f) {
            const ImVec2 min = ImGui::GetItemRectMin(), size = ImGui::GetItemRectSize();
            chatStopRect = {min.x, min.y, size.x, size.y};
        }
        ImGui::SameLine();
        const std::string activity = state == "queued" ? std::string(Tr("Queued")) : state == "thinking" ? std::string(Tr("Thinking")) : (*entry)["activity"].asString("");
        ImGui::TextDisabled("%s: %s...", name.c_str(), activity.c_str());
        ImGui::PopID();
    }

    // ----- Files waiting to go with the next message
    for (size_t i = 0; i < chatAttachments.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (i > 0) ImGui::SameLine();
        const std::string file = chatAttachments[i];
        const bool remove = ImGui::Button((BaseName(file) + "  " + kIconX).c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            DrawImageFile(file, ImVec2(font * 14.0f, font * 14.0f), 1.0f);
            ImGui::TextDisabled("%s", file.c_str());
            ImGui::EndTooltip();
        }
        if (remove) chatAttachments.erase(chatAttachments.begin() + static_cast<std::ptrdiff_t>(i));
        ImGui::PopID();
    }

    // ----- Input: Enter sends, Shift+Enter is a new line; @agent and #file complete from a list
    ImGuiIO& io = ImGui::GetIO();
    const float sendWidth = ImGui::CalcTextSize(Tr("Send")).x + style.FramePadding.x * 2.0f;
    const float hintTop = ImGui::GetCursorScreenPos().y;
    ImGui::TextDisabled("%s", Tr("Enter sends, Shift+Enter starts a new line, @ mentions an agent, # refers to a project file."));

    // What the word being typed could become. Decided before the input runs, so that Up / Down
    // choose in the list instead of moving the caret.
    char marker = 0;
    int at = 0;
    std::string prefix;
    std::vector<ChatCompletion> items;
    if (TokenAt(chatInput.c_str(), static_cast<int>(chatInput.size()), marker, at, prefix)) items = Completions(marker, prefix);
    const std::string completeKey = items.empty() ? std::string() : std::string(1, marker) + prefix;
    if (completeKey != chatCompleteKey) {  // another word: start at the top again
        chatCompleteKey = completeKey;
        chatCompleteIndex = 0;
        chatCompleteScroll = true;
    }
    const int count = static_cast<int>(items.size());
    chatCompleteIndex = count > 0 ? std::min(chatCompleteIndex, count - 1) : 0;
    const ImGuiID inputId = ImGui::GetID("##chatInput");
    ImGuiKeyData* upKey = ImGui::GetKeyData(ImGuiKey_UpArrow);
    ImGuiKeyData* downKey = ImGui::GetKeyData(ImGuiKey_DownArrow);
    const bool upHeld = upKey->Down, downHeld = downKey->Down;
    const bool choosing = count > 0 && ImGui::GetActiveID() == inputId;
    if (choosing) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) chatCompleteIndex = (chatCompleteIndex + 1) % count, chatCompleteScroll = true;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) chatCompleteIndex = (chatCompleteIndex + count - 1) % count, chatCompleteScroll = true;
        upKey->Down = downKey->Down = false;  // hidden from the text field for this call only
    }
    if (chatFocusInput) {
        ImGui::SetKeyboardFocusHere();
        chatFocusInput = false;
    }
    // ImGui submits on Enter when Ctrl is not needed for a line break; with Shift held the
    // flag is left out, so Enter inserts the line break instead.
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | (io.KeyShift ? 0 : ImGuiInputTextFlags_CtrlEnterForNewLine);
    const bool enter = ImGui::InputTextMultiline(
        "##chatInput", &chatInput, ImVec2(-(sendWidth + style.ItemSpacing.x), inputHeight), flags,
        [](ImGuiInputTextCallbackData* data) {  // Tab: the chosen entry replaces the word
            const auto* self = static_cast<const NativeEditor::Impl*>(data->UserData);
            char typedMarker = 0;
            int wordAt = 0;
            std::string typed;
            if (!TokenAt(data->Buf, data->CursorPos, typedMarker, wordAt, typed)) return 0;
            const std::vector<ChatCompletion> options = self->Completions(typedMarker, typed);
            const ChatCompletion* chosen = nullptr;
            if (self->chatCompleteIndex < static_cast<int>(options.size()) && options[static_cast<size_t>(self->chatCompleteIndex)].enabled) {
                chosen = &options[static_cast<size_t>(self->chatCompleteIndex)];
            }
            for (const ChatCompletion& option : options) {
                if (!chosen && option.enabled) chosen = &option;  // the chosen one cannot be called: the first that can
            }
            if (!chosen) return 0;
            data->DeleteChars(wordAt, data->CursorPos - wordAt);
            data->InsertChars(wordAt, chosen->insert.c_str());
            return 0;
        },
        this);
    upKey->Down = upHeld;
    downKey->Down = downHeld;
    const ImVec2 inputMin = ImGui::GetItemRectMin();

    // Mentions and project files already in the text are tinted, so it shows what will be
    // understood as a reference. Drawn into the field's own window, over its text.
    for (ImGuiWindow* child : ImGui::GetCurrentWindow()->DC.ChildWindows) {
        if (!std::strstr(child->Name, "##chatInput")) continue;
        const ImVec2 origin(inputMin.x + style.FramePadding.x - child->Scroll.x, inputMin.y + style.FramePadding.y - child->Scroll.y);  // where the field draws its text
        const float rowHeight = ImGui::GetFontSize();
        child->DrawList->PushClipRect(child->InnerClipRect.Min, child->InnerClipRect.Max, true);
        int row = 0;
        for (size_t lineStart = 0; lineStart <= chatInput.size(); ++row) {
            const size_t lineEnd = std::min(chatInput.find('\n', lineStart), chatInput.size());
            for (size_t wordStart = lineStart; wordStart < lineEnd;) {
                const size_t wordEnd = std::min(chatInput.find_first_of(" \t", wordStart), lineEnd);
                size_t length = 0;
                const ImU32 color = wordEnd > wordStart ? ChatTokenColor(chatInput.substr(wordStart, wordEnd - wordStart), nullptr, length) : 0;
                if (color) {
                    const char* text = chatInput.c_str();
                    const float x = ImGui::CalcTextSize(text + lineStart, text + wordStart).x, tokenWidth = ImGui::CalcTextSize(text + wordStart, text + wordStart + length).x;
                    const ImVec2 min(origin.x + x, origin.y + static_cast<float>(row) * rowHeight);
                    child->DrawList->AddRectFilled(ImVec2(min.x - 2.0f, min.y), ImVec2(min.x + tokenWidth + 2.0f, min.y + rowHeight), (color & 0x00FFFFFFu) | 0x38000000u, 3.0f);
                    child->DrawList->AddText(min, color, text + wordStart, text + wordStart + length);
                }
                wordStart = wordEnd + 1;
            }
            lineStart = lineEnd + 1;
        }
        child->DrawList->PopClipRect();
    }

    ImGui::SameLine();
    if (ImGui::Button(Tr("Send"), ImVec2(sendWidth, inputHeight)) || enter) {
        chatJustSent = true;
        SendChat();
    }

    // ----- Completion list, floating above the input: agents with picture, role and state; files
    // with a thumbnail. At most eight rows show, the rest scroll. Up / Down move the choice, Tab
    // (or a click) takes it, and the chosen file is previewed beside the list.
    if (count > 0) {
        const float rowHeight = avatarSize + style.ItemSpacing.y;
        const float listHeight = static_cast<float>(std::min(count, 8)) * rowHeight + style.WindowPadding.y * 2.0f - style.ItemSpacing.y;
        // Its own window in the always-on-top layer, so it may reach above a short panel.
        ImGui::SetNextWindowPos(ImVec2(inputMin.x, hintTop - 2.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(std::min(chatRect[2] - style.WindowPadding.x * 2.0f, font * 34.0f), listHeight));
        ImGui::Begin("##chatComplete", nullptr,
                     ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        const ImVec2 listPos = ImGui::GetWindowPos(), listSize = ImGui::GetWindowSize();
        if (chatCompleteScroll) {  // keep the choice in view
            const float top = static_cast<float>(chatCompleteIndex) * rowHeight, bottom = top + rowHeight;
            if (top < ImGui::GetScrollY()) ImGui::SetScrollY(top);
            else if (bottom > ImGui::GetScrollY() + listHeight - style.WindowPadding.y * 2.0f) ImGui::SetScrollY(bottom - (listHeight - style.WindowPadding.y * 2.0f));
            chatCompleteScroll = false;
        }
        ImGuiListClipper clipper;  // only the rows in view cost anything (thumbnails are made on demand)
        clipper.Begin(count, rowHeight);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const ChatCompletion& row = items[static_cast<size_t>(i)];
                ImGui::PushID(i);
                ImGui::BeginDisabled(!row.enabled);  // offline agents are listed, but cannot be called
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", i == chatCompleteIndex, 0, ImVec2(0, avatarSize))) {
                    chatInput = chatInput.substr(0, static_cast<size_t>(at)) + row.insert;
                    chatFocusInput = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) chatCompleteIndex = i;  // the pointer chooses too
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (row.file.empty()) {
                    DrawAvatar(dl, pos, avatarSize, row.avatar, row.id, row.label);
                } else if (const std::shared_ptr<const Texture> thumbnail = AssetThumbnail(row.file)) {
                    if (engine.Gpu()) dl->AddImage(ViewTexture(engine.Gpu()->ImageView(thumbnail)), pos, ImVec2(pos.x + avatarSize, pos.y + avatarSize));
                }
                const float textX = pos.x + avatarSize + style.ItemSpacing.x, textY = pos.y + (avatarSize - line) * 0.5f;
                dl->AddText(ImVec2(textX, textY), ImGui::GetColorU32(row.enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), row.label.c_str());
                dl->AddText(ImVec2(textX + ImGui::CalcTextSize(row.label.c_str()).x + style.ItemSpacing.x * 1.5f, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), row.detail.c_str());
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::End();

        // Quick look at the chosen file, beside the list: the picture, the thumbnail of a model,
        // material or prefab, or the first lines of a script or text file.
        const ChatCompletion& chosen = items[static_cast<size_t>(chatCompleteIndex)];
        if (!chosen.file.empty()) {
            ImGui::SetNextWindowPos(ImVec2(listPos.x + listSize.x + 6.0f, listPos.y + listSize.y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::BeginTooltip();
            const std::string full = JoinPath(project, chosen.file);
            if (!DrawImageFile(full, ImVec2(font * 16.0f, font * 16.0f), 1.0f)) {
                const std::shared_ptr<const Texture> thumbnail = AssetThumbnail(chosen.file);
                std::string text;
                if (thumbnail && engine.Gpu()) {
                    ImGui::Image(ViewTexture(engine.Gpu()->ImageView(thumbnail)), ImVec2(font * 10.0f, font * 10.0f));
                } else if ((EndsWith(chosen.file, ".lua") || EndsWith(chosen.file, ".json") || EndsWith(chosen.file, ".md") || EndsWith(chosen.file, ".txt")) && ReadTextFile(full, text)) {
                    size_t cut = 0;
                    for (int lines = 0; lines < 14 && cut < text.size() && cut < 1500; ++lines) cut = std::min(text.find('\n', cut), text.size() - 1) + 1;
                    ImGui::PushFont(monoFont, 0.0f);
                    ImGui::TextUnformatted(text.substr(0, cut).c_str());
                    ImGui::PopFont();
                }
            }
            ImGui::TextDisabled("%s", chosen.file.c_str());
            ImGui::EndTooltip();
        }
    }
    ImGui::End();
}

// The entries a typed `@` or `#` word could complete to, in the order the list shows them.
std::vector<ChatCompletion> NativeEditor::Impl::Completions(char marker, const std::string& prefix) const {
    std::vector<ChatCompletion> out;
    if (marker == '#') {
        for (const std::string& file : FileCandidates(prefix)) {
            ChatCompletion row;
            row.file = file;
            row.insert = file + " ";
            row.label = BaseName(file);
            row.detail = file;
            out.push_back(std::move(row));
        }
        return out;
    }
    const Json& roster = team;
    const Json& live = teamState;
    for (const auto& candidate : MentionCandidates(prefix)) {
        ChatCompletion row;
        row.id = candidate.first;
        row.insert = "@" + candidate.first + " ";
        row.enabled = candidate.second;
        row.label = "@all";
        row.detail = Tr("Everyone who can take a turn");
        for (const Json& agent : roster["agents"].items()) {
            if (agent["id"].asString("") != candidate.first) continue;
            row.label = agent["name"].asString(candidate.first) + "  @" + candidate.first;
            row.avatar = agent["avatar"].asString("") == "file" ? "file:" + JoinPath(engine.ProjectDir(), agent["avatarPath"].asString("")) : agent["avatar"].asString("");
            row.detail = agent["description"].asString("");
            std::string state;
            for (const Json& entry : live["agents"].items()) {
                if (entry["id"].asString("") == candidate.first) state = entry["state"].asString("");
            }
            if (state == "offline") row.detail = std::string(Tr("Offline")) + (row.detail.empty() ? "" : "  ") + row.detail;
            else if (state == "thinking" || state == "working" || state == "queued") row.detail = std::string(Tr("Thinking")) + (row.detail.empty() ? "" : "  ") + row.detail;
        }
        out.push_back(std::move(row));
    }
    return out;
}

// Color of a word that refers to something: `@agent` (the agent's color; @all in the accent
// color) or a project file (`files`: the ones a message names; null = look the word up in the
// asset list). `length` is how much of the word the reference covers (trailing punctuation is
// left plain). 0 = an ordinary word.
ImU32 NativeEditor::Impl::ChatTokenColor(const std::string& word, const std::vector<std::string>* files, size_t& length) const {
    length = word.size();
    while (length > 0 && std::strchr(".,:;!?)", word[length - 1])) --length;
    if (length < 2) return 0;
    const std::string core = word.substr(0, length);
    if (core[0] == '@') {
        const std::string name = Lower(core.substr(1));
        if (name == "all") return IM_COL32(110, 170, 255, 255);
        const Json& roster = team;
        for (const Json& agent : roster["agents"].items()) {
            if (agent["id"].asString("") == name || Lower(agent["name"].asString("")) == name) return AgentColor(agent["id"].asString(""));
        }
        return 0;
    }
    if (core.find('.') == std::string::npos) return 0;  // files have an extension
    const ImU32 fileColor = IM_COL32(120, 210, 175, 255);
    if (files) return std::find(files->begin(), files->end(), core) != files->end() ? fileColor : 0;
    const Json& list = assets;
    for (const Json& asset : list.items()) {
        if (asset["path"].asString("") == core) return fileColor;
    }
    return 0;
}

namespace {

// ----- Markdown in chat messages

bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

std::string Trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

}  // namespace

std::vector<MarkdownSpan> ParseMarkdownInline(const std::string& s) {
    std::vector<MarkdownSpan> out;
    std::string current;
    unsigned style = 0;
    char italicMarker = 0;
    auto flush = [&] {
        if (!current.empty()) out.push_back({current, style, {}});
        current.clear();
    };
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        const char next = i + 1 < s.size() ? s[i + 1] : '\0';
        if (c == '\\' && next && std::strchr("\\`*_~[]()#>|-", next)) {
            current += next;
            i += 2;
            continue;
        }
        if (c == '`') {
            size_t ticks = 1;
            while (i + ticks < s.size() && s[i + ticks] == '`') ++ticks;
            const size_t close = s.find(std::string(ticks, '`'), i + ticks);
            if (close != std::string::npos) {
                flush();
                std::string code = s.substr(i + ticks, close - i - ticks);
                if (code.size() > 2 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
                out.push_back({code, style | kMdCode, {}});
                i = close + ticks;
                continue;
            }
        }
        if (c == '[') {
            const size_t middle = s.find("](", i + 1);
            const size_t end = middle == std::string::npos ? std::string::npos : s.find(')', middle + 2);
            if (end != std::string::npos && middle > i + 1) {
                flush();
                out.push_back({s.substr(i + 1, middle - i - 1), style | kMdLink, s.substr(middle + 2, end - middle - 2)});
                i = end + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_' || c == '~') && next == c) {
            const unsigned flag = c == '~' ? kMdStrike : kMdBold;
            const std::string pair(2, c);
            if (style & flag) {
                flush();
                style &= ~flag;
                i += 2;
                continue;
            }
            const size_t close = s.find(pair, i + 2);
            if (close != std::string::npos && close > i + 2 && s[i + 2] != ' ' && (c != '_' || i == 0 || !IsWordChar(s[i - 1]))) {
                flush();
                style |= flag;
                i += 2;
                continue;
            }
        }
        if (c == '*' || c == '_') {
            if ((style & kMdItalic) && c == italicMarker && (c != '_' || !IsWordChar(next))) {
                flush();
                style &= ~kMdItalic;
                ++i;
                continue;
            }
            if (!(style & kMdItalic) && next && next != ' ' && next != c && (c != '_' || i == 0 || !IsWordChar(s[i - 1])) &&
                s.find(c, i + 1) != std::string::npos) {
                flush();
                style |= kMdItalic;
                italicMarker = c;
                ++i;
                continue;
            }
        }
        current += c;
        ++i;
    }
    flush();
    return out;
}

bool IsMarkdownRule(const std::string& line) {
    char mark = 0;
    int count = 0;
    for (char c : line) {
        if (c == ' ') continue;
        if (c != '-' && c != '*' && c != '_') return false;
        if (mark && c != mark) return false;
        mark = c;
        ++count;
    }
    return count >= 3;
}

bool IsMarkdownTableSeparator(const std::string& line) {
    const std::string t = Trim(line);
    return !t.empty() && t.find('-') != std::string::npos && t.find_first_not_of("|-: ") == std::string::npos;
}

std::vector<std::string> MarkdownTableCells(const std::string& line) {
    std::string t = Trim(line);
    if (!t.empty() && t.front() == '|') t.erase(0, 1);
    if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\')) t.pop_back();
    std::vector<std::string> cells;
    std::string cell;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cell += '|';
            ++i;
        } else if (t[i] == '|') {
            cells.push_back(Trim(cell));
            cell.clear();
        } else {
            cell += t[i];
        }
    }
    cells.push_back(Trim(cell));
    return cells;
}

float NativeEditor::Impl::DrawChatInline(const std::string& line, ImVec2 origin, float lineWidth, float size, ImU32 color, bool bold,
                                         const std::vector<std::string>* files) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* uiFont = ImGui::GetFont();
    const float row = size + 4.0f;  // a little leading between wrapped lines
    const float space = uiFont->CalcTextSizeA(size, FLT_MAX, 0.0f, " ").x;
    const ImU32 linkColor = IM_COL32(110, 170, 255, 255), codeColor = IM_COL32(236, 190, 140, 255);
    float x = 0.0f, y = 0.0f;
    for (const MarkdownSpan& span : ParseMarkdownInline(line)) {
        const bool code = (span.style & kMdCode) != 0, link = (span.style & kMdLink) != 0;
        ImFont* font = code && monoFont ? monoFont : uiFont;
        const float fontSize = code ? size * 0.92f : size;
        const bool heavy = bold || (span.style & kMdBold);
        ImU32 col = link ? linkColor : code ? codeColor : color;
        if ((span.style & kMdItalic) && !link && !code) col = (col & 0x00FFFFFFu) | 0xC8000000u;  // no italic face: a softer tone
        const char* data = span.text.c_str();
        auto measure = [&](size_t b, size_t e) { return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, data + b, data + e).x; };
        auto draw = [&](size_t b, size_t e, float at, float w) {
            const ImVec2 pos(origin.x + at, origin.y + y + (row - fontSize) * 0.5f);
            ImU32 wordColor = col;
            size_t tinted = 0;
            if (files && !code && !link) {
                // @mentions and project files keep their colored chips inside Markdown text.
                if (const ImU32 token = ChatTokenColor(std::string(data + b, data + e), files, tinted)) {
                    const float tw = measure(b, b + tinted);
                    dl->AddRectFilled(ImVec2(pos.x - 2.0f, origin.y + y), ImVec2(pos.x + tw + 2.0f, origin.y + y + row), (token & 0x00FFFFFFu) | 0x30000000u, 3.0f);
                    wordColor = token;
                }
            }
            if (code) dl->AddRectFilled(ImVec2(pos.x - 3.0f, origin.y + y + 1.0f), ImVec2(pos.x + w + 3.0f, origin.y + y + row - 1.0f), IM_COL32(255, 255, 255, 20), 3.0f);
            auto text = [&](ImVec2 p) {
                if (tinted) {
                    dl->AddText(font, fontSize, p, wordColor, data + b, data + b + tinted);
                    dl->AddText(font, fontSize, ImVec2(p.x + measure(b, b + tinted), p.y), col, data + b + tinted, data + e);
                } else {
                    dl->AddText(font, fontSize, p, wordColor, data + b, data + e);
                }
            };
            text(pos);
            if (heavy) text(ImVec2(pos.x + std::max(0.6f, fontSize * 0.045f), pos.y));  // no bold face: a doubled stroke
            if (span.style & kMdStrike) dl->AddLine(ImVec2(pos.x, pos.y + fontSize * 0.55f), ImVec2(pos.x + w, pos.y + fontSize * 0.55f), col, 1.0f);
            if (link) {
                dl->AddLine(ImVec2(pos.x, pos.y + fontSize + 1.0f), ImVec2(pos.x + w, pos.y + fontSize + 1.0f), (col & 0x00FFFFFFu) | 0x80000000u, 1.0f);
                if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(pos.x, origin.y + y), ImVec2(pos.x + w, origin.y + y + row))) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s", span.target.c_str());
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) OpenChatLink(span.target);
                }
            }
        };
        for (size_t i = 0; i < span.text.size();) {
            if (data[i] == ' ' && !code) {
                x += space;
                ++i;
                continue;
            }
            // Inline code stays one piece with its spaces (cut only where a line ends).
            size_t end = code ? span.text.size() : i;
            while (end < span.text.size() && data[end] != ' ') ++end;
            float w = measure(i, end);
            if (x > 0.0f && x + w > lineWidth) {
                x = 0.0f;
                y += row;
            }
            // A word longer than the line (a path, a URL) is cut where the line ends, at whole UTF-8 characters.
            size_t piece = i;
            while (w > lineWidth && piece < end) {
                size_t cut = piece;
                float cutWidth = 0.0f;
                while (cut < end) {
                    size_t step = cut + 1;
                    while (step < end && (static_cast<unsigned char>(data[step]) & 0xC0) == 0x80) ++step;
                    const float grown = measure(piece, step);
                    if (grown > lineWidth && cut > piece) break;
                    cut = step;
                    cutWidth = grown;
                }
                draw(piece, cut, x, cutWidth);
                piece = cut;
                w = measure(piece, end);
                if (piece < end) {
                    x = 0.0f;
                    y += row;
                } else {
                    x += cutWidth;
                }
            }
            if (piece < end) {
                draw(piece, end, x, w);
                x += w;
            }
            i = end;
        }
    }
    return y + row;
}

// A fenced code block: monospace, unwrapped, on a dark rounded well with its language in the corner.
// Right-click copies it.
void NativeEditor::Impl::DrawChatCode(const std::string& code, const std::string& language, float lineWidth) {
    const float pad = 8.0f;
    ImGui::PushFont(monoFont, 0.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos(), extent = ImGui::CalcTextSize(code.c_str());
    ImGui::PopFont();
    const float blockHeight = extent.y + pad * 2.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + lineWidth, at.y + blockHeight), IM_COL32(0, 0, 0, 80), 6.0f);
    if (!language.empty()) {
        const float small = ImGui::GetFontSize() * 0.82f;
        const float w = ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0.0f, language.c_str()).x;
        dl->AddText(ImGui::GetFont(), small, ImVec2(at.x + lineWidth - pad - w, at.y + 4.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), language.c_str());
    }
    dl->PushClipRect(at, ImVec2(at.x + lineWidth, at.y + blockHeight), true);
    dl->AddText(monoFont, ImGui::GetFontSize(), ImVec2(at.x + pad, at.y + pad), IM_COL32(222, 226, 234, 255), code.c_str());
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(lineWidth, blockHeight));
    if (ImGui::BeginPopupContextItem("##code")) {
        if (ImGui::MenuItem(Tr("Copy"))) ImGui::SetClipboardText(code.c_str());
        ImGui::EndPopup();
    }
}

void NativeEditor::Impl::DrawChatMarkdown(const std::string& text, const std::vector<std::string>& files, bool tint, ImU32 color) {
    const float font = ImGui::GetFontSize();
    const float lineWidth = std::max(font * 4.0f, ImGui::GetContentRegionAvail().x);
    const std::vector<std::string>* refs = tint ? &files : nullptr;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 rule = ImGui::GetColorU32(ImGuiCol_Separator), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    std::vector<std::string> lines;
    for (size_t start = 0; start <= text.size();) {
        const size_t end = std::min(text.find('\n', start), text.size());
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    while (!lines.empty() && Trim(lines.back()).empty()) lines.pop_back();
    size_t first = 0;
    while (first < lines.size() && Trim(lines[first]).empty()) ++first;

    bool blank = false;
    for (size_t k = first; k < lines.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        const std::string& raw = lines[k];
        const std::string body = Trim(raw);
        const size_t indent = raw.find_first_not_of(" \t");
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (body.empty()) {
            if (!blank) ImGui::Dummy(ImVec2(0.0f, font * 0.3f));  // paragraph gap
            blank = true;
            ImGui::PopID();
            continue;
        }
        blank = false;
        size_t hashes = 0;
        while (hashes < body.size() && body[hashes] == '#') ++hashes;
        size_t digits = 0;
        while (digits < body.size() && std::isdigit(static_cast<unsigned char>(body[digits]))) ++digits;
        const bool bullet = body.size() > 2 && std::strchr("-*+", body[0]) && body[1] == ' ';
        const bool numbered = digits > 0 && digits < body.size() - 1 && (body[digits] == '.' || body[digits] == ')') && body[digits + 1] == ' ';

        if (body.rfind("```", 0) == 0) {
            // Fenced code: everything up to the closing fence (or the end of the message).
            const std::string language = Trim(body.substr(3));
            std::string code;
            for (++k; k < lines.size() && Trim(lines[k]).rfind("```", 0) != 0; ++k) code += lines[k] + "\n";
            while (!code.empty() && code.back() == '\n') code.pop_back();
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            DrawChatCode(code, language, lineWidth);
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        } else if (hashes >= 1 && hashes <= 6 && hashes < body.size() && body[hashes] == ' ') {
            const float scale = hashes == 1 ? 1.3f : hashes == 2 ? 1.18f : hashes == 3 ? 1.08f : 1.0f;
            ImGui::Dummy(ImVec2(0.0f, font * 0.25f));
            const ImVec2 head = ImGui::GetCursorScreenPos();
            const float blockHeight = DrawChatInline(body.substr(hashes + 1), head, lineWidth, font * scale, color, true, refs);
            ImGui::Dummy(ImVec2(lineWidth, blockHeight + (hashes <= 2 ? 4.0f : 0.0f)));
            if (hashes <= 2) dl->AddLine(ImVec2(head.x, head.y + blockHeight + 2.0f), ImVec2(head.x + lineWidth, head.y + blockHeight + 2.0f), rule, 1.0f);
        } else if (IsMarkdownRule(body)) {
            ImGui::Dummy(ImVec2(lineWidth, font * 0.8f));
            dl->AddLine(ImVec2(at.x, at.y + font * 0.4f), ImVec2(at.x + lineWidth, at.y + font * 0.4f), rule, 1.0f);
        } else if (body[0] == '|' && k + 1 < lines.size() && IsMarkdownTableSeparator(lines[k + 1])) {
            std::vector<std::vector<std::string>> tableRows{MarkdownTableCells(body)};
            for (k += 2; k < lines.size() && !Trim(lines[k]).empty() && Trim(lines[k])[0] == '|'; ++k) tableRows.push_back(MarkdownTableCells(lines[k]));
            --k;
            const int columns = static_cast<int>(std::min<size_t>(16, tableRows[0].size()));
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            if (columns > 0 && ImGui::BeginTable("##table", columns, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
                for (size_t r = 0; r < tableRows.size(); ++r) {
                    ImGui::TableNextRow();
                    if (r == 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 255, 255, 14));
                    for (int c = 0; c < columns; ++c) {
                        ImGui::TableSetColumnIndex(c);
                        const std::string cell = c < static_cast<int>(tableRows[r].size()) ? tableRows[r][static_cast<size_t>(c)] : std::string();
                        const ImVec2 cellAt = ImGui::GetCursorScreenPos();
                        const float cellWidth = std::max(font, ImGui::GetContentRegionAvail().x);
                        ImGui::Dummy(ImVec2(cellWidth, DrawChatInline(cell, cellAt, cellWidth, font, color, r == 0, refs)));
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        } else if (body[0] == '>') {
            const std::string quoted = Trim(body.substr(1));
            const float inset = font * 0.9f;
            const float blockHeight = DrawChatInline(quoted, ImVec2(at.x + inset, at.y), lineWidth - inset, font, (color & 0x00FFFFFFu) | 0xB0000000u, false, refs);
            dl->AddRectFilled(ImVec2(at.x + 2.0f, at.y), ImVec2(at.x + 5.0f, at.y + blockHeight), IM_COL32(255, 255, 255, 50), 1.5f);
            ImGui::Dummy(ImVec2(lineWidth, blockHeight));
        } else if (bullet || numbered) {
            // Nesting from the indent (two spaces a level); bullets alternate dot / ring by level.
            const int level = std::min(4, static_cast<int>(indent == std::string::npos ? 0 : indent) / 2);
            const float markerWidth = font * 1.3f;
            const float left = font * 1.1f * static_cast<float>(level) + markerWidth;
            std::string item = Trim(body.substr(bullet ? 2 : digits + 2));
            const float row = font + 4.0f, middle = at.y + row * 0.5f;
            int task = 0;  // 1 = open box, 2 = checked box
            if (bullet && item.size() >= 3 && item[0] == '[' && item[2] == ']' && (item[1] == ' ' || item[1] == 'x' || item[1] == 'X')) {
                task = item[1] == ' ' ? 1 : 2;
                item = Trim(item.substr(3));
            }
            if (task) {
                const float box = font * 0.7f, bx = at.x + left - markerWidth * 0.5f - box * 0.5f;
                if (task == 2) {
                    dl->AddRectFilled(ImVec2(bx, middle - box * 0.5f), ImVec2(bx + box, middle + box * 0.5f), IM_COL32(92, 148, 255, 255), 3.0f);
                    dl->AddPolyline(std::array<ImVec2, 3>{ImVec2(bx + box * 0.22f, middle), ImVec2(bx + box * 0.43f, middle + box * 0.2f), ImVec2(bx + box * 0.78f, middle - box * 0.22f)}.data(),
                                    3, IM_COL32(255, 255, 255, 255), 1.6f);
                } else {
                    dl->AddRect(ImVec2(bx, middle - box * 0.5f), ImVec2(bx + box, middle + box * 0.5f), dim, 3.0f, 1.2f);
                }
            } else if (bullet) {
                const ImVec2 dot(at.x + left - markerWidth * 0.55f, middle);
                if (level % 2 == 0) dl->AddCircleFilled(dot, font * 0.13f, dim);
                else dl->AddCircle(dot, font * 0.13f, dim, 0, 1.2f);
            } else {
                const std::string number = body.substr(0, digits + 1);
                const float w = ImGui::CalcTextSize(number.c_str()).x;
                dl->AddText(ImVec2(at.x + left - w - font * 0.35f, at.y + 2.0f), dim, number.c_str());
            }
            const ImU32 itemColor = task == 2 ? (color & 0x00FFFFFFu) | 0x90000000u : color;
            ImGui::Dummy(ImVec2(lineWidth, DrawChatInline(item, ImVec2(at.x + left, at.y), lineWidth - left, font, itemColor, false, refs)));
        } else {
            ImGui::Dummy(ImVec2(lineWidth, DrawChatInline(body, at, lineWidth, font, color, false, refs)));
        }
        ImGui::PopID();
    }
}

void NativeEditor::Impl::OpenChatLink(const std::string& target) {
    if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0) {
        // Only web pages leave the editor; other schemes (file:, custom handlers) are ignored.
        ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        if (platform.Platform_OpenInShellFn) platform.Platform_OpenInShellFn(ImGui::GetCurrentContext(), target.c_str());
        return;
    }
    const std::string path = target.substr(0, target.find('#'));
    if (EndsWith(path, ".lua")) OpenScript(path);
    else if (EndsWith(path, ".scene.json")) RequestAction({PendingAction::Kind::LoadScene, path});
    else if (EndsWith(path, ".vfx.json")) OpenEffect(path);
    else if (EndsWith(path, ".shader.json") || EndsWith(path, ".mat.json")) OpenShader(path);
}

void NativeEditor::Impl::ImportChatImage(const std::string& path) {
    const Json imported = Call("asset.import", ObjectOf({{"source", Json(JoinPath(engine.ProjectDir(), path))}}));
    if (!Ok(imported)) return;
    RefreshAssets(true);
    selectedAsset = imported["result"]["path"].asString("");
    Notify(Format(Tr("Imported %s"), selectedAsset.c_str()));
}

// Picture viewer: fit or actual size on a checkerboard, with the file's size and path.
void NativeEditor::Impl::ImageViewer() {
    if (chatViewerPath.empty()) return;
    const float font = ImGui::GetFontSize();
    const std::string full = JoinPath(engine.ProjectDir(), chatViewerPath);
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(font * 44.0f, font * 34.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin(TrId("Image").c_str(), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        const std::shared_ptr<const Texture> image = AvatarTexture("file:" + full);
        ImGui::Checkbox(Tr("Actual size"), &chatViewerActual);
        ImGui::SameLine();
        if (chatViewerPath.rfind("assets/", 0) != 0) {
            if (ImGui::Button(Tr("Import to assets"))) ImportChatImage(chatViewerPath);
            ImGui::SameLine();
        }
        if (ImGui::Button(Tr("Show in folder"))) PlatformOpenUrl(ParentPath(full));
        ImGui::SameLine();
        if (ImGui::Button(Tr("Copy path"))) ImGui::SetClipboardText(full.c_str());
        if (image) ImGui::TextDisabled("%s  %dx%d", chatViewerPath.c_str(), image->width, image->height);
        else ImGui::TextDisabled("%s  %s", chatViewerPath.c_str(), Tr("(cannot be shown: missing, larger than 16 MiB or 8192 px)"));
        ImGui::BeginChild("##canvas", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        if (image && engine.Gpu()) {
            const ImVec2 size = FitSize(image->width, image->height, chatViewerActual ? ImVec2(1e9f, 1e9f) : ImGui::GetContentRegionAvail(), chatViewerActual ? 1.0f : 8.0f);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            // Checkerboard behind transparent pixels (bounded: large pictures get larger squares).
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float cell = std::max(10.0f, std::max(size.x, size.y) / 64.0f);
            for (float y = 0; y < size.y; y += cell) {
                for (float x = 0; x < size.x; x += cell) {
                    const bool dark = (static_cast<int>(x / cell) + static_cast<int>(y / cell)) % 2 == 0;
                    dl->AddRectFilled(ImVec2(at.x + x, at.y + y), ImVec2(at.x + std::min(x + cell, size.x), at.y + std::min(y + cell, size.y)), dark ? IM_COL32(52, 55, 62, 255) : IM_COL32(72, 76, 84, 255));
                }
            }
            ImGui::Image(ViewTexture(engine.Gpu()->ImageView(image)), size);
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) chatViewerPath.clear();
}

void NativeEditor::FocusTeamChat() { impl_->showTeamChat = impl_->requestTeamChatFocus = impl_->chatFocusInput = true; }
std::string NativeEditor::TeamChatInput() const { return impl_->chatInput; }
std::array<float, 4> NativeEditor::TeamChatStopRect() const { return impl_->chatStopRect; }
std::array<float, 4> NativeEditor::TeamChatRect() const { return impl_->chatRect; }
std::vector<std::string> NativeEditor::TeamChatAttachments() const { return impl_->chatAttachments; }
void NativeEditor::ViewChatImage(const std::string& path) { impl_->chatViewerPath = path; }
std::string NativeEditor::ViewedChatImage() const { return impl_->chatViewerPath; }

}  // namespace oe
