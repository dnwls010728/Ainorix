// Settings window (docs/EDITOR.md "Settings"): Edit > Settings. The Agents tab shows, for each
// agent CLI, whether it is installed and signed in and how its last turn reached the editor (the
// ownengine MCP server, or the `oe exec --connect` shell fallback), and sets what agents asked
// directly may do. Reads team.backends / team.connection, writes through team.settings.
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

}  // namespace

void NativeEditor::Impl::SettingsWindow() {
    if (!showSettings) return;
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(font * 46.0f, font * 34.0f), ImGuiCond_FirstUseEver);
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
            if (!teamAvailable) {
                ImGui::TextDisabled("%s", Tr("Agent teams need `oe editor`."));
            } else {
                SettingsAgentsTab();
            }
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

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr("Agents are the coding-agent CLIs installed on this PC. The editor starts them for the Team, Agent Chat and the Scene view's Ask bubbles."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(WithIcon(kIconRefreshCw, Tr("Check again")).c_str())) {
        const Json refreshed = Call("team.backends", ObjectOf({{"refresh", Json(true)}, {"async", Json(true)}}), true);
        if (Ok(refreshed)) teamBackends = refreshed["result"];
        settingsPolled = time;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Detect the CLIs and their login again, e.g. after installing one or signing in"));
    ImGui::Spacing();

    // ----- One row per CLI
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
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
    if (teamBackends.size() == 0) ImGui::TextDisabled("%s", Tr("Looking for agent CLIs..."));

    // ----- How agents reach the editor
    ImGui::Spacing();
    ImGui::SeparatorText(Tr("Engine access"));
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
        ImGui::TextDisabled("%s", Tr("MCP server each turn gets (engine tools such as entity_create, editor_state):"));
        ImGui::Indent();
        ImGui::TextUnformatted(settingsConnection["mcp"].asString("").c_str());
        ImGui::Unindent();
        ImGui::TextDisabled("%s", Tr("Shell fallback when a CLI has no MCP tools (oe is on the turn's PATH):"));
        ImGui::Indent();
        ImGui::TextUnformatted(settingsConnection["shell"].asString("").c_str());
        ImGui::Unindent();
    }

    // ----- Permissions
    ImGui::Spacing();
    ImGui::SeparatorText(Tr("Permissions"));
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
    const char* current = "edit";
    for (const Level& level : kLevels) {
        if (askAccess == level.id) current = level.label;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr("Agent Chat and Ask bubbles"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (ImGui::BeginCombo("##askAccess", Tr(current))) {
        for (const Level& level : kLevels) {
            if (ImGui::Selectable(Tr(level.label), askAccess == level.id) && askAccess != level.id) {
                if (Ok(Call("team.settings", ObjectOf({{"askAccess", Json(level.id)}})))) settingsPolled = -100;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr(level.doc));
        }
        ImGui::EndCombo();
    }
    for (const Level& level : kLevels) {
        if (askAccess == level.id) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", Tr(level.doc));
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::TextDisabled("%s", Tr("Team agents: each agent's access is set in its profile (Team panel)."));
    bool inherit = settingsConnection["inheritUserTools"].asBool(false);
    if (ImGui::Checkbox(Tr("Load my own MCP servers and skills of the CLIs"), &inherit)) {
        if (Ok(Call("team.settings", ObjectOf({{"inheritUserTools", Json(inherit)}})))) settingsPolled = -100;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Off: turns load only the ownengine server (faster, fewer tokens)"));
}

}  // namespace oe
