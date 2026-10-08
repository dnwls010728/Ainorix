#include "app/Engine.h"

#include "app/Vfx.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "scene/Vfx.h"

namespace oe {
namespace {
Json CheckedDocument(Engine& engine, const Json& json) {
    const VfxAsset asset = ParseVfxAsset(json);
    std::vector<ParticleEmitter> emitters; std::vector<Trail> trails;
    ConfigureVfx(asset, Json::MakeObject(), emitters, trails);
    for (const auto& emitter : emitters) if (!emitter.texture.empty()) engine.ResolvePath(emitter.texture);
    for (const auto& trail : trails) if (!trail.texture.empty()) engine.ResolvePath(trail.texture);
    return asset.json;
}
}
const Json* Engine::VfxDocument(const std::string& path) const {
    if (vfxEditDocument_.isNull() || vfxEditProject_ != ProjectDir()) return nullptr;
    return ResolvePath(path) == ResolvePath(vfxEditPath_) ? &vfxEditDocument_ : nullptr;
}

Json Engine::VfxDocumentCall(const std::string& operation, const Json& args) {
    if (vfxEditProject_ != ProjectDir()) {
        vfxEditDocument_ = Json(); vfxEditSaved_ = Json(); vfxEditPath_.clear(); vfxEditHistory_.clear();
    }
    const bool open = !vfxEditDocument_.isNull();
    const bool dirty = open && vfxEditDocument_ != vfxEditSaved_;
    if (operation == "edit") {
        const std::string path = RelativePath(ResolvePath(args["path"].asString()), ProjectDir());
        if (AssetManager::KindOf(path) != "vfx") throw ApiError("invalid_vfx", "document path must end in .vfx.json");
        if (open && path == vfxEditPath_ && !args["discard"].asBool()) return VfxDocumentCall("state", Json());
        if (dirty && !args["discard"].asBool()) throw ApiError("unsaved_vfx", "Effects document has unsaved changes", "Call vfx.save, or vfx.edit with discard:true.");
        Json document; std::string text;
        if (args["new"].asBool()) {
            if (FileExists(ResolvePath(path))) throw ApiError("already_exists", "asset already exists", "Open it without new:true or choose another path.");
            document = VfxPresetDocument(*this, args["preset"].asString("fire"));
        } else {
            if (!ReadTextFile(ResolvePath(path), text)) throw ApiError("not_found", "cannot read " + path);
            std::string error; document = Json::parse(text, &error);
            if (!error.empty()) throw ApiError("invalid_vfx", error);
        }
        document = CheckedDocument(*this, document);
        vfxEditDocument_ = document; vfxEditSaved_ = args["new"].asBool() ? Json() : document;
        vfxEditPath_ = path; vfxEditProject_ = ProjectDir(); vfxEditFile_ = text;
        vfxEditHistory_ = {document}; vfxEditCursor_ = 0; vfxEditMerge_.clear(); Touch();
    } else if (operation != "state") {
        if (!open) throw ApiError("no_vfx_document", "no Effects document is open", "Call vfx.edit first.");
        if (operation == "replace" || operation == "edit_replace") {
            const Json document = CheckedDocument(*this, args["vfx"]);
            if (document != vfxEditDocument_) {
                const std::string merge = args["merge"].asString();
                const bool combine = !merge.empty() && merge == vfxEditMerge_ && vfxEditCursor_ > 0 && vfxEditCursor_ + 1 == vfxEditHistory_.size();
                vfxEditHistory_.resize(vfxEditCursor_ + 1);
                if (combine) vfxEditHistory_.back() = document;
                else vfxEditHistory_.push_back(document);
                vfxEditMerge_ = merge;
                if (vfxEditHistory_.size() > 65) vfxEditHistory_.erase(vfxEditHistory_.begin());
                vfxEditCursor_ = vfxEditHistory_.size() - 1; vfxEditDocument_ = document; Touch();
            }
        } else if (operation == "undo" || operation == "redo") {
            vfxEditMerge_.clear();
            if (operation == "undo" && vfxEditCursor_ > 0) --vfxEditCursor_;
            if (operation == "redo" && vfxEditCursor_ + 1 < vfxEditHistory_.size()) ++vfxEditCursor_;
            vfxEditDocument_ = vfxEditHistory_[vfxEditCursor_]; Touch();
        } else if (operation == "save") {
            std::string text; ReadTextFile(ResolvePath(vfxEditPath_), text);
            if (text != vfxEditFile_ || (vfxEditSaved_.isNull() && FileExists(ResolvePath(vfxEditPath_))))
                throw ApiError("asset_changed", "asset changed on disk while the document was open", "Reload with vfx.edit {discard:true}.");
            vfxEditDocument_ = SaveVfxDocument(*this, vfxEditPath_, vfxEditDocument_);
            vfxEditMerge_.clear(); vfxEditSaved_ = vfxEditDocument_; vfxEditFile_ = vfxEditSaved_.dump(2) + "\n"; Touch();
        } else if (operation == "close") {
            if (dirty && !args["discard"].asBool()) throw ApiError("unsaved_vfx", "Effects document has unsaved changes", "Save or close with discard:true.");
            vfxEditDocument_ = vfxEditSaved_ = Json(); vfxEditPath_.clear(); vfxEditHistory_.clear(); vfxEditCursor_ = 0; Touch();
        } else throw ApiError("invalid_vfx", "unknown document operation");
    }
    Json result = Json::MakeObject(); result["path"] = vfxEditPath_; result["document"] = vfxEditDocument_;
    result["open"] = !vfxEditDocument_.isNull(); result["dirty"] = result["open"].asBool() && vfxEditDocument_ != vfxEditSaved_;
    result["undo"] = static_cast<uint64_t>(vfxEditCursor_);
    result["redo"] = vfxEditHistory_.empty() ? 0 : static_cast<int>(vfxEditHistory_.size() - vfxEditCursor_ - 1);
    return result;
}
}  // namespace oe
