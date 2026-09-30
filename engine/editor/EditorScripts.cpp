// Native editor Scripts panel: Lua code editor (ImGuiColorTextEdit) with
// syntax highlighting, line numbers, find/replace, and problems from
// script.check (while typing) and script.errors (the running game).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "editor/EditorInternal.h"
#include "editor/EditorText.h"

#include "app/Engine.h"
#include "core/Log.h"
#include "script/ScriptHost.h"

namespace oe {

namespace {

constexpr double kCheckDelay = 0.35;  // seconds after the last keystroke
constexpr size_t kSquiggleProblem = 1;
const ImU32 kErrorColor = IM_COL32(240, 90, 80, 255);
const ImU32 kWarningColor = IM_COL32(235, 185, 60, 255);

std::string FileName(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

// Lua with the engine API (scene, input, time, ...) as known identifiers.
const TextEditor::Language* EngineLua(Engine& engine) {
    static TextEditor::Language language = [&engine] {
        TextEditor::Language l = *TextEditor::Language::Lua();
        l.name = "Lua (OwnEngine)";
        for (const std::string& g : ScriptHost::SandboxGlobals(engine)) l.identifiers.insert(g);
        return l;
    }();
    return &language;
}

// Dark palette matching the editor theme.
const TextEditor::Palette& CodePalette() {
    static TextEditor::Palette palette = [] {
        TextEditor::Palette p = TextEditor::GetDarkPalette();
        using C = TextEditor::Color;
        auto set = [&p](C c, ImU32 v) { p[static_cast<size_t>(c)] = v; };
        set(C::background, IM_COL32(24, 26, 31, 255));
        set(C::text, IM_COL32(220, 223, 228, 255));
        set(C::keyword, IM_COL32(198, 120, 221, 255));
        set(C::declaration, IM_COL32(198, 120, 221, 255));
        set(C::number, IM_COL32(209, 154, 102, 255));
        set(C::string, IM_COL32(152, 195, 121, 255));
        set(C::punctuation, IM_COL32(171, 178, 191, 255));
        set(C::identifier, IM_COL32(220, 223, 228, 255));
        set(C::knownIdentifier, IM_COL32(97, 175, 239, 255));
        set(C::comment, IM_COL32(110, 118, 132, 255));
        set(C::lineNumber, IM_COL32(95, 102, 115, 255));
        set(C::currentLineNumber, IM_COL32(200, 204, 212, 255));
        set(C::currentLineHighlight, IM_COL32(255, 255, 255, 10));
        set(C::currentLineHighlightBorder, IM_COL32(255, 255, 255, 18));
        set(C::selection, IM_COL32(66, 133, 244, 90));
        set(C::cursor, IM_COL32(230, 232, 238, 255));
        return p;
    }();
    return palette;
}

// Number of characters (UTF-8 code points) in `s`.
size_t Glyphs(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// script.check warnings come in English from the engine; show them in the interface language.
std::string LocalizeDiagnostic(const std::string& message) {
    struct Pattern {
        const char* prefix;
        const char* format;
    };
    static const Pattern patterns[] = {{"assigns global '", "assigns global '%s' - shared by every script; add 'local'?"},
                                       {"unknown global '", "unknown global '%s' - typo, or a missing 'local'?"}};
    for (const Pattern& p : patterns) {
        size_t n = std::strlen(p.prefix);
        if (message.compare(0, n, p.prefix) != 0) continue;
        size_t end = message.find('\'', n);
        if (end == std::string::npos) break;
        return Format(Tr(p.format), message.substr(n, end - n).c_str());
    }
    return message;
}

// "scripts/x.lua:12: message" -> 12 (0 when absent).
int LineInMessage(const std::string& message, const std::string& path) {
    size_t at = message.find(path + ":");
    if (at == std::string::npos) return 0;
    return std::atoi(message.c_str() + at + path.size() + 1);
}

}  // namespace

void NativeEditor::Impl::OpenScript(const std::string& path) {
    showScripts = true;
    for (size_t i = 0; i < scripts.size(); ++i) {
        if (scripts[i].path == path) {
            scripts[i].open = true;
            focusScript = static_cast<int>(i);
            return;
        }
    }
    Json r = Call("script.read", ObjectOf({{"path", Json(path)}}));
    if (!Ok(r)) return;
    ScriptTab t;
    t.path = path;
    t.saved = r["result"]["source"].asString("");
    t.editor = std::make_unique<TextEditor>();
    TextEditor& ed = *t.editor;
    ed.SetLanguage(EngineLua(engine));
    ed.SetPalette(CodePalette());
    ed.SetTabSize(2);
    ed.SetInsertSpacesOnTabs(true);
    ed.SetAutoIndentEnabled(true);
    ed.SetShowMatchingBrackets(true);
    ed.SetCompletePairedGlyphs(true);
    ed.SetShowWhitespacesEnabled(false);
    ed.SetText(t.saved);
    t.undoIndex = ed.GetUndoIndex();
    CheckScript(t);
    scripts.push_back(std::move(t));
    focusScript = static_cast<int>(scripts.size()) - 1;
}

void NativeEditor::Impl::CheckScript(ScriptTab& tab) {
    Json r = Call("script.check", ObjectOf({{"path", Json(tab.path)}, {"source", Json(tab.editor->GetText())}}), true);
    tab.diagnostics.clear();
    for (const Json& d : r["result"]["diagnostics"].items()) {
        ScriptDiagnostic s;
        s.line = d["line"].asInt(0);
        s.error = d["severity"].asString("") == "error";
        s.message = LocalizeDiagnostic(d["message"].asString(""));
        tab.diagnostics.push_back(s);
    }
    tab.editedAt = -1;
    tab.markersDirty = true;
}

void NativeEditor::Impl::UpdateScriptMarkers(ScriptTab& tab) {
    TextEditor& ed = *tab.editor;
    ed.ClearMarkers();
    ed.ClearSquiggles();
    std::vector<const ScriptDiagnostic*> all;
    for (const ScriptDiagnostic& d : tab.diagnostics) all.push_back(&d);
    for (const ScriptDiagnostic& d : tab.runtime) all.push_back(&d);
    for (const ScriptDiagnostic* d : all) {
        if (d->line <= 0 || static_cast<size_t>(d->line) > ed.GetLineCount()) continue;
        size_t line = static_cast<size_t>(d->line - 1);
        ImU32 col = d->error ? kErrorColor : kWarningColor;
        std::string tip = std::string(d->error ? Tr("Error") : Tr("Warning")) + ": " + d->message;
        ed.AddMarker(line, col, (col & 0x00FFFFFF) | 0x28000000, tip, tip);
        // Underline the line's code (without its indentation).
        std::string text = ed.GetLineText(line);
        size_t first = 0;
        while (first < text.size() && (text[first] == ' ' || text[first] == '\t')) ++first;
        size_t glyphs = Glyphs(text);
        if (glyphs > first) ed.AddSquiggle(TextEditor::DocPos(line, first), TextEditor::DocPos(line, glyphs), kSquiggleProblem, col, tip);
    }
    tab.markersDirty = false;
}

void NativeEditor::Impl::ScriptsPanel() {
    scriptEditorFocused = false;
    if (focusScript >= 0) ImGui::SetNextWindowFocus();  // bring the tab to front; Begin() skips hidden tabs
    if (!ImGui::Begin(TrId("Scripts").c_str(), &showScripts)) {
        ImGui::End();
        return;
    }
    if (scripts.empty()) {
        ImGui::TextDisabled("%s", Tr("Double-click a script in Assets (or use \"Edit Script\" on a Script component) to edit it here."));
        ImGui::TextDisabled("%s", Tr("Ctrl+S saves the script; it hot-reloads, also while the game runs."));
        ImGui::End();
        return;
    }
    // Errors of the running game, a few times a second.
    if (time - lastScriptErrorsPoll > 0.5) {
        lastScriptErrorsPoll = time;
        scriptErrors = Call("script.errors", Json(), true)["result"];
    }
    if (ImGui::BeginTabBar("##scripts", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (size_t i = 0; i < scripts.size(); ++i) {
            ScriptTab& t = scripts[i];
            if (!t.open) continue;
            TextEditor& ed = *t.editor;
            ImGuiTabItemFlags flags = t.modified ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (focusScript == static_cast<int>(i)) {
                flags |= ImGuiTabItemFlags_SetSelected;
                focusScript = -1;
                ed.SetFocus();
            }
            std::string label = FileName(t.path) + "###" + t.path;
            if (!ImGui::BeginTabItem(label.c_str(), &t.open, flags)) continue;

            // Runtime errors for this file (changes rebuild the markers).
            std::vector<ScriptDiagnostic> runtime;
            std::string key;
            for (const Json& e : scriptErrors.items()) {
                std::string msg = e["message"].asString("");
                if (e["script"].asString("") != t.path && msg.find(t.path) == std::string::npos) continue;
                ScriptDiagnostic d;
                d.line = LineInMessage(msg, t.path);
                d.error = true;
                d.runtime = true;
                size_t cut = msg.find(t.path + ":");
                d.message = msg;
                if (cut != std::string::npos) {
                    size_t colon = msg.find(':', cut + t.path.size() + 1);
                    if (colon != std::string::npos) d.message = msg.substr(colon + 1);
                    while (!d.message.empty() && d.message[0] == ' ') d.message.erase(0, 1);
                }
                key += msg + "\n";
                runtime.push_back(d);
            }
            if (key != t.runtimeKey) {
                t.runtimeKey = key;
                t.runtime = runtime;
                t.markersDirty = true;
            }

            // Toolbar: save / revert / find, then problems summary and cursor position.
            bool save = ImGui::Button(Tr("Save"));
            ImGui::SameLine();
            ImGui::BeginDisabled(!t.modified);
            if (ImGui::Button(Tr("Revert"))) {
                ed.SetText(t.saved);
                t.undoIndex = ed.GetUndoIndex();
                t.modified = false;
                CheckScript(t);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(Tr("Find / Replace"))) ed.OpenFindReplaceWindow();
            HelpTooltip("Ctrl+F");
            ImGui::SameLine();
            int errors = 0, warnings = 0;
            for (const ScriptDiagnostic& d : t.diagnostics) (d.error ? errors : warnings) += 1;
            errors += static_cast<int>(t.runtime.size());
            ImGui::AlignTextToFramePadding();
            if (errors + warnings == 0) {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "%s", Tr("No problems"));
            } else {
                if (errors) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kErrorColor), Tr("%d errors"), errors);
                if (errors && warnings) ImGui::SameLine();
                if (warnings) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kWarningColor), Tr("%d warnings"), warnings);
            }
            TextEditor::DocPos cursor = ed.GetMainCursorPosition();
            std::string where = Format(Tr("Ln %d, Col %d"), static_cast<int>(cursor.line) + 1, static_cast<int>(cursor.index) + 1) + "   " + t.path +
                                (t.modified ? Tr("  (modified)") : "");
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16, ImGui::GetWindowWidth() - ImGui::CalcTextSize(where.c_str()).x - 16));
            ImGui::TextDisabled("%s", where.c_str());

            // Problems list below the code (click to jump).
            size_t problems = t.diagnostics.size() + t.runtime.size();
            float listH = problems ? ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min<size_t>(5, problems)) + ImGui::GetStyle().WindowPadding.y * 2 : 0;

            // The code editor.
            if (t.markersDirty) UpdateScriptMarkers(t);
            ed.SetFindButtonLabel(Tr("Find"));
            ed.SetFindAllButtonLabel(Tr("Find All"));
            ed.SetReplaceButtonLabel(Tr("Replace"));
            ed.SetReplaceAllButtonLabel(Tr("Replace All"));
            ImGui::PushFont(monoFont, 0.0f);
            ed.Render("##code", ImVec2(0, problems ? -listH - ImGui::GetStyle().ItemSpacing.y : 0), ImGuiChildFlags_Borders);
            ImGui::PopFont();
            bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            scriptEditorFocused = scriptEditorFocused || focused;
            save = save || (focused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S));

            // Edits: modified flag now, syntax check once typing pauses.
            if (ed.GetUndoIndex() != t.undoIndex) {
                t.undoIndex = ed.GetUndoIndex();
                t.modified = ed.GetText() != t.saved;
                t.editedAt = time;
            }
            if (t.editedAt >= 0 && time - t.editedAt > kCheckDelay) CheckScript(t);

            if (problems) {
                ImGui::BeginChild("##problems", ImVec2(0, listH), ImGuiChildFlags_Borders);
                std::vector<const ScriptDiagnostic*> all;
                for (const ScriptDiagnostic& d : t.runtime) all.push_back(&d);
                for (const ScriptDiagnostic& d : t.diagnostics) all.push_back(&d);
                for (size_t k = 0; k < all.size(); ++k) {
                    const ScriptDiagnostic& d = *all[k];
                    ImGui::PushID(static_cast<int>(k));
                    ImVec4 col = ImGui::ColorConvertU32ToFloat4(d.error ? kErrorColor : kWarningColor);
                    std::string text = std::string(d.runtime ? Tr("Runtime error") : d.error ? Tr("Error") : Tr("Warning")) + "   " +
                                       (d.line > 0 ? Format(Tr("line %d"), d.line) + "   " : std::string()) + d.message;
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    if (ImGui::Selectable(text.c_str()) && d.line > 0) {
                        ed.SetCursor(TextEditor::DocPos(static_cast<size_t>(d.line - 1), 0));
                        ed.ScrollToLine(static_cast<size_t>(d.line - 1));
                        ed.SetFocus();
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }

            if (save) {
                std::string text = ed.GetText();
                if (Ok(Call("script.write", ObjectOf({{"path", Json(t.path)}, {"source", Json(text)}})))) {
                    t.saved = text;
                    t.modified = false;
                    paramSchemas.erase(t.path);  // the inspector re-reads the script's params
                    CheckScript(t);
                    bool broken = std::any_of(t.diagnostics.begin(), t.diagnostics.end(), [](const ScriptDiagnostic& d) { return d.error; });
                    Notify(Format(Tr(broken ? "Saved %s - it has errors" : "Saved %s"), t.path.c_str()), broken);
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    // Closing a tab with unsaved edits keeps it open (Revert first to discard them).
    scripts.erase(std::remove_if(scripts.begin(), scripts.end(), [](const ScriptTab& t) { return !t.open && !t.modified; }), scripts.end());
    for (ScriptTab& t : scripts) t.open = true;
    ImGui::End();
}

}  // namespace oe
