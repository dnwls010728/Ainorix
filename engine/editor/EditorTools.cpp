// Command-backed clipboard, source-prefab workflow, history and asset previews.
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"

#include <algorithm>
#include <cstring>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "render/Mesh.h"
#include "render/GpuRenderer.h"

namespace oe {

void NativeEditor::Impl::CopySelection() {
    if (selection.empty() || InPlaySession()) return;
    Json ids = Json::MakeArray();
    for (EntityId id : selection) ids.push(id);
    Json response = Call("entity.copy", ObjectOf({{"ids", ids}}));
    if (Ok(response)) {
        ImGui::SetClipboardText(response["result"].dump().c_str());
        Notify(Tr("Copied selected entities"));
    }
}

void NativeEditor::Impl::PasteSelection() {
    if (InPlaySession()) return;
    const char* text = ImGui::GetClipboardText();
    if (!text || std::strlen(text) > 8 * 1024 * 1024) {
        Notify(Tr("Clipboard does not contain valid entities"), true);
        return;
    }
    std::string error;
    Json document = Json::parse(text, &error);
    if (!error.empty()) {
        Notify(Tr("Clipboard does not contain valid entities"), true);
        return;
    }
    Json response = Call("entity.paste", ObjectOf({{"document", document}}));
    if (Ok(response)) {
        selection.clear();
        for (const Json& id : response["result"]["roots"].items()) selection.push_back(static_cast<EntityId>(id.asNumber()));
        selectedId = kNullEntity;
        Refresh(true);
        scrollToRow = Primary();
        Notify(Tr("Pasted entities"));
    }
}

void NativeEditor::Impl::BrowseFile(NativeEditor::FilePurpose purpose) {
    if (InPlaySession() || !engine.EditingPrefab().empty()) return;
    FileDialogOptions request;
    request.save = purpose == NativeEditor::FilePurpose::SaveScene;
    request.directory = engine.ProjectDir();
    request.title = Tr(purpose == NativeEditor::FilePurpose::OpenScene ? "Open Scene" :
                       purpose == NativeEditor::FilePurpose::SaveScene ? "Save Scene As" : "Import Asset...");
    if (purpose != NativeEditor::FilePurpose::ImportAsset) {
        request.pattern = "*.scene.json";
        request.extension = "scene.json";
        if (request.save) request.filename = sceneName + ".scene.json";
    }
    const FileDialogResult result = window ? window->ChooseFile(request) : FileDialogResult{};
    if (result.status == FileDialogResult::Status::Cancelled) return;
    if (result.status == FileDialogResult::Status::Unavailable) {
        if (request.save) openSaveAs = true;
        else Notify(Tr("Native file dialogs unavailable; use Assets or drop a file."));
        return;
    }
    if (result.status == FileDialogResult::Status::Error) {
        Notify(result.error, true);
        return;
    }
    if (purpose == NativeEditor::FilePurpose::ImportAsset) {
        droppedFiles.push_back(result.path);
        ImportDroppedFiles();
        return;
    }
    try {
        const std::string full = engine.ResolvePath(result.path);
        const std::string path = RelativePath(full, engine.ProjectDir());
        if (path.size() < 11 || path.compare(path.size() - 11, 11, ".scene.json") != 0) {
            Notify(Tr("Choose a .scene.json file inside the project."), true);
            return;
        }
        if (request.save) {
            if (Ok(Call("scene.save", ObjectOf({{"path", Json(path)}})))) {
                Notify(Format(Tr("Saved %s"), path.c_str()));
                Refresh(true); RefreshAssets(true);
                const PendingAction action = pending;
                pending = {};
                if (action.kind != PendingAction::Kind::None) RunAction(action);
            }
        } else RequestAction({PendingAction::Kind::LoadScene, path});
    } catch (const std::exception& exception) {
        Notify(exception.what(), true);
    }
}

void NativeEditor::Impl::HistoryPanel() {
    if (requestHistoryFocus) {
        ImGui::SetNextWindowFocus();
        requestHistoryFocus = false;
    }
    if (!ImGui::Begin(TrId("History").c_str(), &showHistory)) { ImGui::End(); return; }
    const Json response = Call("history.list", Json(), true);
    const Json& history = response["result"];
    const int cursor = history["cursor"].asInt();
    ImGui::TextDisabled("%s", Tr("Select an entry to undo or redo to that point."));
    ImGui::BeginDisabled(InPlaySession());
    if (ImGui::Selectable(Tr("Initial state"), cursor == 0)) Call("history.go", ObjectOf({{"cursor", Json(0)}}));
    int index = 0;
    for (const Json& entry : history["entries"].items()) {
        ++index;
        ImGui::PushID(index);
        const std::string label = std::to_string(index) + "  " + entry["command"].asString();
        if (!entry["applied"].asBool()) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        if (ImGui::Selectable(label.c_str(), cursor == index)) {
            Call("history.go", ObjectOf({{"cursor", Json(index)}}));
            selectedId = kNullEntity; Refresh(true);
        }
        if (!entry["applied"].asBool()) ImGui::PopStyleVar();
        HelpTooltip(entry["args"].dump().substr(0, 1024));
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::End();
}

std::shared_ptr<const Texture> NativeEditor::Impl::AssetThumbnail(const std::string& path) {
    if (thumbnails.size() >= 128 && !thumbnails.count(path)) {
        auto oldest = std::min_element(thumbnails.begin(), thumbnails.end(), [](const auto& left, const auto& right) {
            return left.second.lastUsed < right.second.lastUsed;
        });
        thumbnails.erase(oldest);
    }
    Thumbnail& thumbnail = thumbnails[path];
    thumbnail.lastUsed = time;
    const int64_t modified = FileModifiedTime(engine.ResolvePath(path));
    // Periodic regeneration catches dependencies (textures/graphs used by a
    // model, material or prefab), while a per-frame budget bounds first-use work.
    if (previewBudget > 0 && (modified != thumbnail.modified || time - thumbnail.generated > 5)) {
        --previewBudget;
        Json result = Call("asset.preview", ObjectOf({{"path", Json(path)}, {"size", Json(64)}, {"pixels", Json(true)}}), true);
        thumbnail.generated = time;
        thumbnail.modified = modified;
        thumbnail.image.reset();
        if (Ok(result)) {
            auto texture = std::make_shared<Texture>();
            texture->width = texture->height = 64;
            for (const Json& pixel : result["result"]["pixels"].items()) texture->texels.push_back(static_cast<uint32_t>(pixel.asNumber()));
            thumbnail.image = texture;
            thumbnail.error.clear();
        } else thumbnail.error = result["error"]["message"].asString();
    }
    return thumbnail.image;
}

void NativeEditor::FocusHistoryPanel() { impl_->showHistory = true; impl_->requestHistoryFocus = true; }
void NativeEditor::EditPrefab(const std::string& path) { impl_->RequestAction({PendingAction::Kind::EditPrefab, path}); }
void NativeEditor::ClosePrefab() { impl_->RequestAction({PendingAction::Kind::ClosePrefab, ""}); }
void NativeEditor::BrowseFile(FilePurpose purpose) { impl_->BrowseFile(purpose); }

}  // namespace oe
