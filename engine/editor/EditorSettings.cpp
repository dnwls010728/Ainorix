// Settings window (docs/EDITOR.md "Settings"): Edit > Settings. The Agents tab shows, for each
// agent CLI, whether it is installed and signed in and how its last turn reached the editor (the
// ownengine MCP server, or the `oe exec --connect` shell fallback), and sets the agent that is asked
// directly (Agent Chat, Ask bubbles): which CLI, its model, standing instructions and what it may
// do. Reads team.backends / team.connection, writes through team.settings.
#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"

#include <cfloat>

#include "imgui_internal.h"

#include "app/Engine.h"
#include "core/Log.h"

namespace oe {
namespace {

const ImVec4 kGood(0.45f, 0.82f, 0.52f, 1.0f);
const ImVec4 kBad(0.94f, 0.43f, 0.43f, 1.0f);
const ImVec4 kWarn(0.95f, 0.75f, 0.35f, 1.0f);

void Status(const ImVec4& color, const char* icon, const std::string& text) {
    ImGui::TextColored(color, "%s", icon);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(text.c_str());
}

// A command the person runs in a terminal, with a button that copies it.
void CopyableCommand(const std::string& command) {
    ImGui::PushID(command.c_str());
    ImGui::TextDisabled("%s", command.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Copy"))) ImGui::SetClipboardText(command.c_str());
    ImGui::PopID();
}

const ImVec4 kTitle(0.93f, 0.94f, 0.97f, 1.0f);
const ImVec4 kHelp(0.60f, 0.63f, 0.69f, 1.0f);

// A section of the window: a rounded box a little lighter than the window, with a title, an
// optional line under it saying what the section is for, and room around its content. Sections
// read as separate blocks instead of one long page. EndCard closes it (always).
void BeginCard(const char* id, const char* icon, const char* title, const char* about = nullptr) {
    const float font = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.035f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.09f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(font * 1.1f, font * 0.9f));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(font * 0.6f, font * 0.55f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.42f, 0.62f, 1.0f, 1.0f), "%s", icon);
    ImGui::SameLine(0, font * 0.5f);
    ImGui::TextColored(kTitle, "%s", title);
    if (about) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kHelp, "%s", about);
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0, font * 0.1f));
}

void EndCard() {
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.35f));
}

// A setting's name in the left column; the control follows on the same line at `column`.
void FieldLabel(const char* label, float column) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(column);
}

// What a setting does, under its control, in the control's column.
void FieldHelp(const std::string& text, float column) {
    ImGui::SetCursorPosX(column);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kHelp, "%s", text.c_str());
    ImGui::PopTextWrapPos();
}

// A command line shown as it is typed: in a dark box, with Copy.
void CodeLine(const std::string& text) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float copy = ImGui::CalcTextSize(Tr("Copy")).x + style.FramePadding.x * 2.0f;
    ImGui::PushID(text.c_str());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0.35f));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - copy - style.ItemSpacing.x);
    std::string shown = text;
    ImGui::InputText("##code", &shown, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button(Tr("Copy"))) ImGui::SetClipboardText(text.c_str());
    ImGui::PopID();
}

}  // namespace

void NativeEditor::Impl::SettingsWindow() {
    if (!showSettings) return;
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(font * 50.0f, font * 42.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(font * 36.0f, font * 20.0f), ImVec2(FLT_MAX, FLT_MAX));
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(WithIcon(kIconSettings, TrId("Settings")).c_str(), &showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##settingsTabs")) {
        const ImGuiTabItemFlags agentsFlags = settingsTab == "agents" ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(WithIcon(kIconBot, Tr("Agents")).c_str(), nullptr, agentsFlags)) {
            // The page scrolls under the tabs, with room between its edge and the sections.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(font * 0.9f, font * 0.8f));
            if (ImGui::BeginChild("##agentsPage", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding)) {
                if (!teamAvailable) ImGui::TextDisabled("%s", Tr("Agent teams need `oe editor`."));
                else SettingsAgentsTab();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    settingsTab.clear();
    ImGui::End();
}

void NativeEditor::Impl::SettingsAgentsTab() {
    // Detection and the engine connection of the latest turns change while the window is open.
    if (time - settingsPolled > 1.0) {
        settingsPolled = time;
        const Json backends = Call("team.backends", ObjectOf({{"async", Json(true)}}), true);
        if (Ok(backends)) teamBackends = backends["result"];
        const Json connection = Call("team.connection", Json(), true);
        if (Ok(connection)) settingsConnection = connection["result"];
    }
    const float font = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();

    // ----- The agent that is asked directly: chosen here once, not where the question is asked.
    BeginCard("##agentCard", kIconBot, Tr("Agent Chat and Ask bubbles"), Tr("The agent that answers in Agent Chat and in the Scene view's Ask bubbles."));
    {
        const float column = font * 8.0f;
        const float field = std::min(font * 20.0f, ImGui::GetContentRegionAvail().x - column);
        const std::string chosen = settingsConnection["askBackend"].asString("");
        const Json* answering = TeamBackend(settingsConnection["askAgent"].asString(""));
        const std::string automatic = Format(Tr("Automatic (%s)"), answering ? (*answering)["name"].asString("").c_str() : Tr("none installed"));
        const Json* chosenBackend = TeamBackend(chosen);
        FieldLabel(Tr("Agent"), column);
        ImGui::SetNextItemWidth(field);
        if (ImGui::BeginCombo("##askBackend", chosen.empty() ? automatic.c_str() : chosenBackend ? (*chosenBackend)["name"].asString(chosen).c_str() : chosen.c_str())) {
            // Another CLI has other models: the model goes back to its default.
            auto choose = [&](const std::string& id) {
                if (id != chosen && Ok(Call("team.settings", ObjectOf({{"askBackend", Json(id)}, {"askModel", Json("")}})))) settingsPolled = -100;
            };
            if (ImGui::Selectable(automatic.c_str(), chosen.empty())) choose(std::string());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("The first installed CLI that can run turns"));
            for (const Json& backend : teamBackends.items()) {
                if (!backend["runsTurns"].asBool(true)) continue;
                const std::string id = backend["id"].asString("");
                const bool installed = backend["status"].asString("") == "installed";
                const std::string name = backend["name"].asString(id) + (installed ? std::string() : std::string("  (") + Tr("Not installed") + ")");
                if (ImGui::Selectable((name + "##" + id).c_str(), id == chosen)) choose(id);
            }
            ImGui::EndCombo();
        }
        if (!chosen.empty() && chosenBackend && (*chosenBackend)["status"].asString("") != "installed" && (*chosenBackend)["status"].asString("") != "detecting") {
            ImGui::SameLine();
            Status(kBad, kIconCircleX, Tr("Not installed"));
        }

        // The model belongs to a CLI: with Automatic the CLI's own default is used.
        const std::string model = settingsConnection["askModel"].asString("");
        std::string modelLabel = model.empty() ? std::string(Tr("CLI default")) : model;
        if (chosenBackend) {
            for (const Json& item : (*chosenBackend)["models"].items()) {
                if (item["id"].asString("") == model) modelLabel = item["label"].asString(model);
            }
        }
        FieldLabel(Tr("Model"), column);
        ImGui::SetNextItemWidth(field);
        ImGui::BeginDisabled(chosen.empty());
        if (ImGui::BeginCombo("##askModel", modelLabel.c_str())) {
            auto choose = [&](const std::string& id) {
                if (id != model && Ok(Call("team.settings", ObjectOf({{"askModel", Json(id)}})))) settingsPolled = -100;
            };
            if (ImGui::Selectable(Tr("CLI default"), model.empty())) choose(std::string());
            if (chosenBackend) {
                for (const Json& item : (*chosenBackend)["models"].items()) {
                    const std::string id = item["id"].asString("");
                    if (ImGui::Selectable((item["label"].asString(id) + "##" + id).c_str(), id == model)) choose(id);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (chosen.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", Tr("Choose an agent to choose its model"));

        // What it may do without asking.
        struct Level {
            const char* id;
            const char* label;
            const char* doc;
        };
        static const Level kLevels[] = {
            {"read", "Read only", "Looks at files and the scene, changes nothing."},
            {"edit", "Edit (recommended)", "Edits project files and the scene through the engine tools; also runs oe exec / oe script / oe render. "
                                           "Other shell commands are refused (Codex: sandboxed to the project)."},
            {"full", "Full access", "Any shell command without asking (Claude: bypassPermissions, Codex: no sandbox). Use only on a trusted project."},
        };
        const std::string askAccess = settingsConnection["askAccess"].asString("edit");
        const Level* current = &kLevels[1];
        for (const Level& level : kLevels) {
            if (askAccess == level.id) current = &level;
        }
        FieldLabel(Tr("Permissions"), column);
        ImGui::SetNextItemWidth(field);
        if (ImGui::BeginCombo("##askAccess", Tr(current->label))) {
            for (const Level& level : kLevels) {
                if (ImGui::Selectable(Tr(level.label), askAccess == level.id) && askAccess != level.id) {
                    if (Ok(Call("team.settings", ObjectOf({{"askAccess", Json(level.id)}})))) settingsPolled = -100;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr(level.doc));
            }
            ImGui::EndCombo();
        }
        FieldHelp(Tr(current->doc), column);

        // Standing instructions: typed freely, saved when the field is left.
        if (!settingsInstructionsLoaded && settingsConnection.has("askInstructions")) {
            settingsInstructions = settingsConnection["askInstructions"].asString("");
            settingsInstructionsLoaded = true;
        }
        FieldLabel(Tr("Instructions"), column);
        ImGui::InputTextMultiline("##askInstructions", &settingsInstructions, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.5f));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            const Json saved = Call("team.settings", ObjectOf({{"askInstructions", Json(settingsInstructions)}}));
            if (Ok(saved)) settingsPolled = -100;
            else Notify(saved["error"]["message"].asString(""), true);
        }
        FieldHelp(Tr("Given to the agent with every conversation, e.g. the language to answer in or what to check after a change."), column);
    }
    EndCard();

    // ----- The CLIs on this PC
    BeginCard("##cliCard", kIconTerminal, Tr("Installed agent CLIs"),
              Tr("Agents are the coding-agent CLIs installed on this PC. The editor starts them for the Team, Agent Chat and the Scene view's Ask bubbles."));
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(font * 0.5f, font * 0.4f));
    if (ImGui::BeginTable("##agents", 4, flags)) {
        ImGui::TableSetupColumn(Tr("Agent"), ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn(Tr("Installed"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(Tr("Login"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(Tr("Engine connection"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (const Json& backend : teamBackends.items()) {
            const std::string id = backend["id"].asString("");
            const std::string status = backend["status"].asString("");
            ImGui::PushID(id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(WithIcon(kIconBot, backend["name"].asString(id)).c_str());

            ImGui::TableNextColumn();
            if (status == "installed") {
                Status(kGood, kIconCircleCheck, backend["version"].asString(Tr("installed")));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", backend["path"].asString("").c_str());
            } else if (status == "detecting") {
                ImGui::TextDisabled("%s", Tr("Checking..."));
            } else {
                Status(kBad, kIconCircleX, status == "notInstalled" ? Tr("Not installed") : Tr("Not working"));
                if (backend.has("message")) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", backend["message"].asString("").c_str());
                    ImGui::PopTextWrapPos();
                }
                CopyableCommand(backend["installHint"].asString(""));
            }

            ImGui::TableNextColumn();
            const std::string login = backend["login"].asString("");
            if (status != "installed") {
                ImGui::TextDisabled("-");
            } else if (login == "loggedIn") {
                Status(kGood, kIconCircleCheck, backend["loginDetail"].asString(Tr("Signed in")));
            } else if (login == "loggedOut") {
                Status(kBad, kIconCircleX, Tr("Not signed in"));
                ImGui::TextDisabled("%s", Tr("Sign in from a terminal:"));
                CopyableCommand(backend["loginHint"].asString(""));
            } else {
                Status(kWarn, kIconCircleHelp, Tr("Unknown"));
                if (backend.has("loginDetail")) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", backend["loginDetail"].asString("").c_str());
                    ImGui::PopTextWrapPos();
                }
            }

            ImGui::TableNextColumn();
            const std::string engineStatus = backend["engine"]["status"].asString("");
            if (status != "installed") {
                ImGui::TextDisabled("-");
            } else if (!backend["runsTurns"].asBool(true)) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", Tr("Detected only: the editor cannot run this CLI yet."));
                ImGui::PopTextWrapPos();
            } else if (engineStatus == "connected") {
                Status(kGood, kIconPlug, Tr("MCP connected"));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(Tr("In the latest turn (%s)"), backend["engine"]["time"].asString("").c_str());
            } else if (!engineStatus.empty()) {
                Status(kBad, kIconTriangleAlert, Format(Tr("MCP %s"), engineStatus.c_str()));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", Tr("The latest turn had no engine tools and could only use the shell fallback."));
                ImGui::PopTextWrapPos();
            } else {
                ImGui::TextDisabled("%s", Tr("No turn yet"));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Shown after the CLI's first turn in this editor session"));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    if (teamBackends.size() == 0) ImGui::TextColored(kHelp, "%s", Tr("Looking for agent CLIs..."));
    if (ImGui::Button(WithIcon(kIconRefreshCw, Tr("Check again")).c_str())) {
        const Json refreshed = Call("team.backends", ObjectOf({{"refresh", Json(true)}, {"async", Json(true)}}), true);
        if (Ok(refreshed)) teamBackends = refreshed["result"];
        settingsPolled = time;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Detect the CLIs and their login again, e.g. after installing one or signing in"));
    ImGui::SameLine();
    bool inherit = settingsConnection["inheritUserTools"].asBool(false);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + font);
    if (ImGui::Checkbox(Tr("Load my own MCP servers and skills of the CLIs"), &inherit)) {
        if (Ok(Call("team.settings", ObjectOf({{"inheritUserTools", Json(inherit)}})))) settingsPolled = -100;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Off: turns load only the ownengine server (faster, fewer tokens)"));
    ImGui::TextColored(kHelp, "%s", Tr("Team agents: each agent's access is set in its profile (Team panel)."));
    EndCard();

    // ----- How agents reach the editor
    BeginCard("##engineCard", kIconPlug, Tr("Engine access"));
    const int port = settingsConnection["apiPort"].asInt(0);
    if (port > 0) {
        Status(kGood, kIconCircleCheck, Format(Tr("Editor API on port %d"), port));
    } else {
        Status(kWarn, kIconTriangleAlert, Tr("No editor API: agents can only edit files"));
    }
    if (!settingsConnection["oeFound"].asBool(true)) {
        Status(kBad, kIconCircleX, Tr("The oe program next to the editor is missing: agents get no engine tools"));
    }
    if (port > 0) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kHelp, "%s", Tr("MCP server each turn gets (engine tools such as entity_create, editor_state):"));
        ImGui::PopTextWrapPos();
        CodeLine(settingsConnection["mcp"].asString(""));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kHelp, "%s", Tr("Shell fallback when a CLI has no MCP tools (oe is on the turn's PATH):"));
        ImGui::PopTextWrapPos();
        CodeLine(settingsConnection["shell"].asString(""));
    }
    (void)style;
    EndCard();
}

}  // namespace oe
