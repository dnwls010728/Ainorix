#include "editor/EditorInternal.h"
#include <algorithm>
#include <cmath>
#include "app/Engine.h"
#include "assets/Assets.h"
#include "editor/EditorText.h"
#include "scene/SpriteClip.h"
#include "render/GpuRenderer.h"

namespace oe {
namespace {
void FrameSameLine(float width) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (right - ImGui::GetItemRectMax().x >= width + ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine();
}
float ButtonWidth(const char* label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2; }
}
void NativeEditor::Impl::AnimationSpriteFrames(const Json& state) {
    ImGui::BeginChild("##spriteFrames", ImVec2(0, 0));
    const Json response = Call("animation.get_clip", ObjectOf({{"id", Primary()}, {"clip", state["clip"]}, {"offset", animationFramePage * 64}}), true);
    if (!Ok(response)) {
        animationFramePage = 0;
        ImGui::TextWrapped("%s", response["error"]["message"].asString().c_str()); ImGui::EndChild(); return;
    }
    const Json info = response["result"]; Json clip = info["values"];
    auto edit = [&](Json args) {
        args["id"] = Primary(); args["clip"] = state["clip"];
        const Json result = Call("animation.set_clip", args);
        if (Ok(result)) {
            clip = result["result"]["values"];
            const std::string operation = args["operation"].asString();
            if (operation == "append") animationSequenceFrame = static_cast<int>(clip["frames"].size()) - 1;
            else if (operation == "insert") animationSequenceFrame = args["index"].asInt();
            else if (operation == "move") animationSequenceFrame = args["to"].asInt();
            animationSequenceFrame = std::clamp(animationSequenceFrame, 0, static_cast<int>(clip["frames"].size()) - 1);
            SpriteClipTiming updated; ParseSpriteClipTiming(clip, updated, nullptr);
            Call("animation.preview", ObjectOf({{"id", Primary()}, {"clip", state["clip"]}, {"playing", false},
                {"time", updated.starts[static_cast<size_t>(animationSequenceFrame)]}, {"loop", clip["loop"].asBool(true)}}));
        }
    };
    ImGui::SetNextItemWidth(140); ImGui::InputText("##newClip", &animationNewClip); AnimationRect("new-clip-name");
    FrameSameLine(ButtonWidth(Tr("Create Sprite Clip")));
    if (ImGui::Button(Tr("Create Sprite Clip"))) {
        const Json result = Call("animation.set_clip", ObjectOf({{"id", Primary()}, {"clip", animationNewClip}, {"operation", "create"},
            {"values", ObjectOf({{"frames", Json(Json::Array{0})}, {"fps", 8}, {"loop", true}})}}));
        if (Ok(result)) Call("animation.preview", ObjectOf({{"id", Primary()}, {"clip", animationNewClip}, {"playing", false}}));
    }
    AnimationRect("new-clip");
    if (!clip.isObject()) { ImGui::TextWrapped("%s", Tr("Create a clip to start adding frames.")); ImGui::EndChild(); return; }
    FrameSameLine(ImGui::CalcTextSize(Tr("Loop Clip")).x + ImGui::GetFrameHeight()); bool loop = clip["loop"].asBool(true);
    if (ImGui::Checkbox(Tr("Loop Clip"), &loop)) edit(ObjectOf({{"values", ObjectOf({{"loop", loop}})}})); AnimationRect("clip-loop");
    FrameSameLine(100); float clipFps = clip["fps"].asFloat(8); ImGui::SetNextItemWidth(80);
    if (ImGui::InputFloat(Tr("FPS"), &clipFps, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) edit(ObjectOf({{"values", ObjectOf({{"fps", clipFps}})}}));
    AnimationRect("clip-fps");
    if (clip.has("durations")) {
        FrameSameLine(ButtonWidth(Tr("Use FPS Timing"))); if (ImGui::Button(Tr("Use FPS Timing"))) edit(ObjectOf({{"operation", "uniform"}})); AnimationRect("uniform");
    }
    const bool shared = !info["asset"].asString().empty();
    ImGui::TextDisabled("%s", Tr(shared ? "Shared clip edits write immediately; no scene undo." : "Clip edits use scene undo; save the scene to keep them."));
    std::shared_ptr<const Texture> texture;
    ImTextureID textureId = ImTextureID();
    if (engine.Gpu() && !info["image"]["texture"].asString().empty()) {
        texture = engine.Assets().GetTexture(info["image"]["texture"].asString());
        if (texture) textureId = ViewTexture(engine.Gpu()->ImageView(texture));
    }
    const float side = ImGui::GetFrameHeight() * 1.5f;
    auto imageButton = [&](int index, const Json& rect) {
        bool pressed;
        if (texture) {
            const float x = rect[0].asFloat(), y = rect[1].asFloat(), w = rect[2].asFloat(), h = rect[3].asFloat();
            const ImVec2 uv0((x + 0.5f) / static_cast<float>(texture->width), (y + 0.5f) / static_cast<float>(texture->height));
            const ImVec2 uv1((x + w - 0.5f) / texture->width, (y + h - 0.5f) / texture->height);
            const float scale = side / std::max(1.0f, std::max(w, h));
            pressed = ImGui::ImageButton("##frame", textureId, ImVec2(w * scale, h * scale), uv0, uv1);
        } else pressed = ImGui::Button(std::to_string(index).c_str(), ImVec2(side, side));
        return pressed;
    };
    ImGui::TextUnformatted(Tr("Source Frames")); ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Previous Frames"))) animationFramePage = std::max(0, animationFramePage - 1); AnimationRect("source-prev");
    ImGui::SameLine(); if (ImGui::SmallButton(Tr("Next Frames"))) animationFramePage = std::min((info["frameCount"].asInt() - 1) / 64, animationFramePage + 1); AnimationRect("source-next");
    ImGui::SameLine(); ImGui::TextDisabled("%d / %d", animationFramePage + 1, (info["frameCount"].asInt() + 63) / 64);
    ImGui::BeginChild("##sourceFrames", ImVec2(0, side + 18), ImGuiChildFlags_Borders);
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (side + 14)));
    int column = 0;
    for (const Json& frame : info["frames"].items()) {
        const int index = frame["index"].asInt(); ImGui::PushID(index);
        if (column++ % perRow != 0) ImGui::SameLine();
        if (imageButton(index, frame["rect"])) edit(ObjectOf({{"operation", "append"}, {"frame", index}}));
        AnimationRect("source:" + std::to_string(index));
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("OE_ANIM_SOURCE", &index, sizeof(index)); ImGui::Text("%d", index); ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d", index);
        ImGui::PopID();
    }
    ImGui::EndChild();
    SpriteClipTiming timing; std::string error;
    if (!ParseSpriteClipTiming(clip, timing, &error)) { ImGui::TextUnformatted(error.c_str()); ImGui::EndChild(); return; }
    animationSequenceFrame = std::max(0, std::min(animationSequenceFrame, static_cast<int>(timing.starts.size()) - 1));
    ImGui::TextUnformatted(Tr("Clip Sequence")); ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Previous Sequence"))) animationSequenceFrame = std::max(0, animationSequenceFrame - 64);
    ImGui::SameLine(); if (ImGui::SmallButton(Tr("Next Sequence"))) animationSequenceFrame = std::min(static_cast<int>(clip["frames"].size()) - 1, animationSequenceFrame + 64);
    ImGui::BeginChild("##sequenceFrames", ImVec2(0, side + 28), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const size_t begin = static_cast<size_t>(animationSequenceFrame / 64 * 64);
    for (size_t i = begin; i < std::min(begin + 64, clip["frames"].size()); ++i) {
        ImGui::PushID(static_cast<int>(i)); if (i != begin) ImGui::SameLine();
        const int source = clip["frames"][i].asInt(); Json rect;
        if (info["image"]["atlas"].asBool()) {
            // A bounded query avoids storing every atlas rectangle in editor state.
            const Json page = Call("animation.get_clip", ObjectOf({{"id", Primary()}, {"clip", state["clip"]}, {"offset", source}, {"limit", 1}, {"sourceOnly", true}}), true);
            rect = page["result"]["frames"][0]["rect"];
        } else {
            const int cols = info["image"]["columns"].asInt(1), gridRows = info["image"]["rows"].asInt(1);
            const int frame = source % (cols * gridRows); const float w = static_cast<float>(info["textureWidth"].asInt()) / static_cast<float>(cols), h = static_cast<float>(info["textureHeight"].asInt()) / static_cast<float>(gridRows);
            rect = Json(Json::Array{static_cast<float>(frame % cols) * w, static_cast<float>(frame / cols) * h, w, h});
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(animationSequenceFrame == static_cast<int>(i) ? ImGuiCol_ButtonActive : ImGuiCol_Button));
        if (imageButton(source, rect)) {
            animationSequenceFrame = static_cast<int>(i);
            Call("animation.preview", ObjectOf({{"time", timing.starts[i]}, {"playing", false}}));
        }
        ImGui::PopStyleColor(); AnimationRect("sequence:" + std::to_string(i));
        const int index = static_cast<int>(i);
        if (ImGui::BeginDragDropSource()) { ImGui::SetDragDropPayload("OE_ANIM_SEQUENCE", &index, sizeof(index)); ImGui::Text("%d", source); ImGui::EndDragDropSource(); }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("OE_ANIM_SOURCE")) edit(ObjectOf({{"operation", "insert"}, {"index", index}, {"frame", *static_cast<const int*>(payload->Data)}}));
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("OE_ANIM_SEQUENCE")) { edit(ObjectOf({{"operation", "move"}, {"index", *static_cast<const int*>(payload->Data)}, {"to", index}})); animationSequenceFrame = index; }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    ImGui::SameLine(); ImGui::Button(Tr("Drop Frames Here"), ImVec2(std::max(side * 3, ButtonWidth(Tr("Drop Frames Here"))), side)); AnimationRect("append-drop");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("OE_ANIM_SOURCE")) edit(ObjectOf({{"operation", "append"}, {"frame", *static_cast<const int*>(payload->Data)}}));
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("OE_ANIM_SEQUENCE")) edit(ObjectOf({{"operation", "move"}, {"index", *static_cast<const int*>(payload->Data)}, {"to", static_cast<int>(clip["frames"].size()) - 1}}));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
    // A drop can change length/selection during this draw; footer timing must use the new sequence.
    ParseSpriteClipTiming(clip, timing, nullptr);
    animationSequenceFrame = std::clamp(animationSequenceFrame, 0, static_cast<int>(timing.starts.size()) - 1);
    if (ImGui::Button(Tr("Move Frame Left")) && animationSequenceFrame > 0) edit(ObjectOf({{"operation", "move"}, {"index", animationSequenceFrame}, {"to", animationSequenceFrame - 1}})); AnimationRect("frame-left");
    FrameSameLine(ButtonWidth(Tr("Move Frame Right"))); if (ImGui::Button(Tr("Move Frame Right")) && animationSequenceFrame + 1 < static_cast<int>(clip["frames"].size())) edit(ObjectOf({{"operation", "move"}, {"index", animationSequenceFrame}, {"to", animationSequenceFrame + 1}})); AnimationRect("frame-right");
    FrameSameLine(ButtonWidth(Tr("Remove Frame"))); if (ImGui::Button(Tr("Remove Frame"))) edit(ObjectOf({{"operation", "remove"}, {"index", animationSequenceFrame}})); AnimationRect("frame-remove");
    FrameSameLine(ImGui::CalcTextSize(Tr("Frame Seconds")).x + 110); ImGui::TextUnformatted(Tr("Frame Seconds")); ImGui::SameLine(); float duration = static_cast<float>(timing.durations[static_cast<size_t>(animationSequenceFrame)]); ImGui::SetNextItemWidth(100);
    if (ImGui::InputFloat("##frameSeconds", &duration, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) edit(ObjectOf({{"operation", "duration"}, {"index", animationSequenceFrame}, {"duration", duration}})); AnimationRect("frame-duration");
    ImGui::EndChild();
}
}  // namespace oe
