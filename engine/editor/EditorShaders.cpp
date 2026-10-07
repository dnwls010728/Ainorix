#include "editor/EditorInternal.h"
#include <algorithm>
#include <cmath>
#include "editor/EditorShaderGraph.h"
#include "editor/EditorText.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "render/GpuRenderer.h"
#include "render/Mesh.h"
#include "render/Material.h"
#include "render/ShaderGraph.h"

namespace oe {
namespace {
const char* kOutputs[] = {"color", "emissive", "normal", "offset"};
const ImU32 kWire = IM_COL32(112, 202, 242, 255);

std::string Sibling(const std::string& path, const std::string& suffix) {
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) dot = path.find_last_of('.', dot - 1);
    return path.substr(0, dot) + suffix;
}

Json NewGraph() {
    return Json::parse(R"({"format":"ownengine.shader","uniforms":{},"nodes":[{"op":"baseColor"}],"color":0})");
}

bool VectorField(const char* label, Json& value) {
    ImGui::PushID(label); ImGui::TextUnformatted(label);
    bool scalar = value.isNumber();
    float v[4];
    for (int i = 0; i < 4; ++i) v[i] = scalar ? value.asFloat() : value[i].asFloat();
    bool changed = scalar ? ImGui::DragFloat("##value", v, 0.02f, -65504, 65504) :
                            ImGui::DragFloat4("##value", v, 0.02f, -65504, 65504);
    if (changed) value = scalar ? Json(v[0]) : Json(Json::Array{v[0], v[1], v[2], v[3]});
    ImGui::PopID(); return changed;
}

std::shared_ptr<const Texture> SurfacePreview(const Json& graph, const Json& material, std::string* error) {
    ShaderGraph compiled;
    if (!CompileShaderGraph(graph, compiled, error)) return {};
    auto uniforms = compiled.defaults;
    if (!ShaderUniforms(compiled, material["shaderUniforms"], uniforms, error)) return {};
    auto image = std::make_shared<Texture>();
    image->width = image->height = 128;
    image->texels.resize(128 * 128);
    ShaderInputs input;
    const Json& color = material["baseColor"];
    input.baseColor = color.isArray() ? Vec4(color[0].asFloat(), color[1].asFloat(), color[2].asFloat(), 1) : Vec4(1, 1, 1, 1);
    input.baseColor.w = material["opacity"].asFloat(1);
    input.time = 1;
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        float px = (x - 63.5f) / 58, py = (63.5f - y) / 58;
        float background = ((x / 12 + y / 12) & 1) ? 0.14f : 0.2f;
        Vec3 rgb(background, background, background);
        if (px * px + py * py <= 1) {
            float z = std::sqrt(std::max(0.0f, 1 - px * px - py * py));
            input.position = Vec4(px, py, z, 1); input.normal = Vec4(px, py, z, 0);
            input.uv = Vec4(static_cast<float>(x) / 127, static_cast<float>(y) / 127, 0, 1);
            ShaderSurface surface = EvaluateShaderGraph(compiled, input, uniforms);
            float alpha = Clamp(surface.color.w, 0, 1);
            rgb = Vec3(surface.color.x + surface.emissive.x, surface.color.y + surface.emissive.y,
                       surface.color.z + surface.emissive.z) * alpha + rgb * (1 - alpha);
        }
        auto channel = [](float c) { return static_cast<uint32_t>(Clamp(c, 0, 1) * 255 + 0.5f); };
        image->texels[static_cast<size_t>(y * 128 + x)] = channel(rgb.x) | (channel(rgb.y) << 8) | (channel(rgb.z) << 16) | 0xFF000000;
    }
    return image;
}
}

void NativeEditor::Impl::OpenShader(const std::string& path, bool discard) {
    showShaders = true; focusShaders = true;
    if (!discard && !path.empty() && !shaderDocument.isNull() && (path == shaderPath || path == shaderExistingMaterial)) return;
    if (!discard && !shaderDocument.isNull() && (shaderDocument != shaderSaved || shaderMaterial != shaderMaterialSaved)) {
        shaderPendingPath = path; shaderPendingOpen = shaderOpenPrompt = true;
        return;
    }
    Json graph = NewGraph(), material = DefaultMaterialJson();
    std::string actualPath = path, materialPath;
    try {
        if (!path.empty()) {
            std::string text, error;
            if (!ReadTextFile(engine.ResolvePath(path), text)) { Notify(Tr("Cannot read shader asset."), true); return; }
            Json file = Json::parse(text, &error);
            if (!error.empty()) { Notify(error, true); return; }
            if (AssetManager::KindOf(path) == "material") {
                Material check;
                if (!MaterialFromJson(file, [&] (const std::string& p, std::string* err) { return engine.Assets().GetTexture(p, err); },
                    check, &error, [&] (const std::string& p, std::string* err) { return engine.Assets().GetShader(p, err); })) {
                    Notify(error, true); return;
                }
                for (const auto& field : file.members()) material[field.first] = field.second;
                material["baseColor"] = Json(Json::Array{check.baseColor.r, check.baseColor.g, check.baseColor.b});
                materialPath = path; actualPath = file["shader"].asString("");
                if (!actualPath.empty()) {
                    if (!ReadTextFile(engine.ResolvePath(actualPath), text)) { Notify(Tr("Cannot read shader asset."), true); return; }
                    graph = Json::parse(text, &error);
                    if (!error.empty()) { Notify(error, true); return; }
                }
            } else if (AssetManager::KindOf(path) == "shader") graph = file;
            else { Notify(Tr("Choose a shader or material asset."), true); return; }
            ShaderGraph check;
            if (!CompileShaderGraph(graph, check, &error)) { Notify(error, true); return; }
        }
    } catch (const std::exception& error) { Notify(error.what(), true); return; }
    if (!graph.has("uniforms")) graph["uniforms"] = Json::MakeObject();
    shaderDocument = graph; shaderSaved = actualPath.empty() ? Json() : graph;
    shaderPath = actualPath;
    shaderSavePath = actualPath.empty() ? materialPath.empty() ? "materials/new.shader.json" : Sibling(materialPath, ".shader.json") : actualPath;
    if (actualPath.empty()) {
        std::string seed = shaderSavePath;
        for (int suffix = 1; FileExists(engine.ResolvePath(shaderSavePath)); ++suffix)
            shaderSavePath = Sibling(seed, "_" + std::to_string(suffix) + ".shader.json");
    }
    shaderMaterial = material;
    shaderMaterialSaved = material;
    shaderMaterialPath = materialPath.empty() ? Sibling(shaderSavePath, ".mat.json") : materialPath;
    if (materialPath.empty()) {
        for (int suffix = 1; FileExists(engine.ResolvePath(shaderMaterialPath)); ++suffix)
            shaderMaterialPath = Sibling(shaderSavePath, "_" + std::to_string(suffix) + ".mat.json");
    }
    shaderExistingMaterial = materialPath;
    shaderHistory = {graph}; shaderHistoryIndex = 0;
    shaderSelected = 0; shaderLink = -1; shaderError.clear(); shaderPreviewKey.clear();
    ShaderArrange();
}

void NativeEditor::Impl::ShaderArrange() {
    const Json& document = shaderDocument;
    shaderPositions.clear();
    std::vector<int> depths(shaderDocument["nodes"].size(), 0);
    std::map<int, int> rowsAtDepth;
    int maxDepth = 0;
    for (size_t pass = 0; pass < depths.size(); ++pass) {
        bool changed = false;
        for (int i = 0; i < static_cast<int>(depths.size()); ++i) {
            for (const Json& input : document["nodes"][i]["args"].items()) {
                int source = input.asInt(-1);
                if (source < 0 || source >= static_cast<int>(depths.size())) continue;
                int depth = depths[static_cast<size_t>(source)] + 1;
                if (depth > depths[static_cast<size_t>(i)]) { depths[static_cast<size_t>(i)] = depth; changed = true; }
            }
        }
        if (!changed) break;
    }
    for (int i = 0; i < static_cast<int>(depths.size()); ++i) {
        int depth = depths[static_cast<size_t>(i)]; maxDepth = std::max(depth, maxDepth);
        shaderPositions.emplace_back(static_cast<float>(depth) * 230, static_cast<float>(rowsAtDepth[depth]++) * 150);
    }
    shaderOutputPos = ImVec2(static_cast<float>(maxDepth + 1) * 230, 0);
    shaderPan = ImVec2(24, 24); shaderZoom = 0.8f;
}

void NativeEditor::Impl::ShaderRecord() {
    if (!shaderHistory.empty() && shaderHistory[shaderHistoryIndex] == shaderDocument) return;
    shaderHistory.resize(shaderHistoryIndex + 1);
    shaderHistory.push_back(shaderDocument);
    if (shaderHistory.size() > 65) shaderHistory.erase(shaderHistory.begin());
    shaderHistoryIndex = shaderHistory.size() - 1;
    shaderError.clear();
}

bool NativeEditor::Impl::SaveShader() {
    Json ordered;
    if (!OrderEditorShaderGraph(shaderDocument, ordered, &shaderError)) { Notify(shaderError, true); return false; }
    Json reply = Call("shader.create", ObjectOf({{"path", shaderSavePath}, {"graph", ordered}, {"overwrite", shaderSavePath == shaderPath}}));
    if (!Ok(reply)) return false;
    shaderPath = shaderSavePath; shaderSaved = shaderDocument;
    thumbnails.clear(); RefreshAssets(true); Notify(Format(Tr("Saved %s"), shaderPath.c_str()));
    return true;
}

bool NativeEditor::Impl::SaveShaderMaterial() {
    if (!SaveShader()) return false;
    Json values = shaderMaterial; values["shader"] = shaderPath;
    Json args = ObjectOf({{"path", shaderMaterialPath}, {"values", values}});
    bool edit = shaderMaterialPath == shaderExistingMaterial;
    Json reply = Call(edit ? "material.set" : "material.create", args);
    if (!Ok(reply)) return false;
    shaderMaterial = reply["result"]["values"]; shaderMaterialSaved = shaderMaterial; shaderExistingMaterial = shaderMaterialPath;
    selectedAsset = shaderMaterialPath; RefreshAssets(true); thumbnails.erase(shaderMaterialPath);
    Notify(Format(Tr("Saved %s"), shaderMaterialPath.c_str()));
    return true;
}

void NativeEditor::Impl::ShaderCanvas() {
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##shadercanvas", ImVec2(std::max(160.0f, size.x), std::max(140.0f, size.y)), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 origin = ImGui::GetCursorScreenPos(); size = ImGui::GetContentRegionAvail();
    if (shaderFit) {
        ImVec2 low = shaderOutputPos, high = shaderOutputPos + ImVec2(190, 160);
        for (ImVec2 p : shaderPositions) {
            low.x = std::min(low.x, p.x); low.y = std::min(low.y, p.y);
            high.x = std::max(high.x, p.x + 190); high.y = std::max(high.y, p.y + 140);
        }
        shaderZoom = Clamp(std::min((size.x - 40) / std::max(1.0f, high.x - low.x),
                                    (size.y - 40) / std::max(1.0f, high.y - low.y)), 0.08f, 1.0f);
        shaderPan = ImVec2(20, 20) - low * shaderZoom; shaderFit = false;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList(); ImGuiIO& io = ImGui::GetIO();
    bool hovered = ImGui::IsWindowHovered();
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) shaderPan += io.MouseDelta;
    if (hovered && io.MouseWheel != 0) {
        float before = shaderZoom; shaderZoom = Clamp(shaderZoom * std::pow(1.15f, io.MouseWheel), 0.08f, 1.5f);
        shaderPan = io.MousePos - origin - (io.MousePos - origin - shaderPan) * (shaderZoom / before);
    }
    float grid = 32 * shaderZoom;
    for (float x = std::fmod(shaderPan.x, grid); x < size.x; x += grid)
        draw->AddLine(origin + ImVec2(x, 0), origin + ImVec2(x, size.y), IM_COL32(48, 53, 63, 255));
    for (float y = std::fmod(shaderPan.y, grid); y < size.y; y += grid)
        draw->AddLine(origin + ImVec2(0, y), origin + ImVec2(size.x, y), IM_COL32(48, 53, 63, 255));
    auto point = [&](ImVec2 p) { return origin + shaderPan + p * shaderZoom; };
    auto output = [&](int node) { return point(shaderPositions[static_cast<size_t>(node)] + ImVec2(190, 24)); };
    auto input = [&](int node, int pin) {
        ImVec2 pos = node < 0 ? shaderOutputPos : shaderPositions[static_cast<size_t>(node)];
        return point(pos + ImVec2(0, 54 + static_cast<float>(pin) * 23));
    };
    auto wire = [&](ImVec2 from, ImVec2 to) {
        float bend = std::max(45.0f, std::fabs(to.x - from.x) * 0.5f);
        draw->AddBezierCubic(from, from + ImVec2(bend, 0), to - ImVec2(bend, 0), to, kWire, 2);
    };
    const int count = static_cast<int>(shaderDocument["nodes"].size());
    const Json& document = shaderDocument;
    for (int n = 0; n < count; ++n) {
        const Json& args = document["nodes"][n]["args"];
        for (int pin = 0; pin < static_cast<int>(args.size()); ++pin) {
            int source = args[pin].asInt(-1); if (source >= 0 && source < count) wire(output(source), input(n, pin));
        }
    }
    for (int pin = 0; pin < 4; ++pin) {
        int source = document[kOutputs[pin]].asInt(-1);
        if (source >= 0 && source < count) wire(output(source), input(-1, pin));
    }
    if (shaderLink >= 0 && shaderLink < count) wire(output(shaderLink), io.MousePos);
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * shaderZoom);
    for (int n = 0; n <= count; ++n) {
        bool surface = n == count;
        ImVec2& position = surface ? shaderOutputPos : shaderPositions[static_cast<size_t>(n)];
        int pins = surface ? 4 : static_cast<int>(document["nodes"][n]["args"].size());
        ImVec2 p = point(position), end = p + ImVec2(190, std::max(94.0f, 68 + static_cast<float>(pins) * 23)) * shaderZoom;
        draw->AddRectFilled(p, end, IM_COL32(30, 34, 42, 255), 5);
        draw->AddRectFilled(p, p + ImVec2(190, 36) * shaderZoom, surface ? IM_COL32(48, 91, 84, 255) : IM_COL32(52, 69, 93, 255), 5);
        draw->AddRect(p, end, !surface && shaderSelected == n ? kWire : IM_COL32(98, 109, 127, 255), 5, 1.5f);
        std::string title = surface ? Tr("Surface Output") : std::to_string(n) + "  " + shaderDocument["nodes"][n]["op"].asString();
        draw->AddText(p + ImVec2(10, 9) * shaderZoom, IM_COL32(240, 243, 248, 255), title.c_str());
        ImGui::PushID(n); ImGui::SetCursorScreenPos(p + ImVec2(8, 2) * shaderZoom);
        // The whole card is draggable; leave the pin hit areas outside its bounds.
        ImGui::InvisibleButton("##node", ImVec2(174 * shaderZoom, end.y - p.y - 4 * shaderZoom));
        if (ImGui::IsItemClicked() && !surface) shaderSelected = n;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) position += io.MouseDelta / shaderZoom;
        for (int pin = 0; pin < pins; ++pin) {
            ImVec2 pinPos = input(surface ? -1 : n, pin);
            const char* label = surface ? kOutputs[pin] : pin == 0 ? "A" : pin == 1 ? "B" : "C";
            draw->AddText(pinPos + ImVec2(12, -7) * shaderZoom, IM_COL32(205, 213, 224, 255), label);
            int source = surface ? document[kOutputs[pin]].asInt(-1) : document["nodes"][n]["args"][pin].asInt(-1);
            draw->AddCircleFilled(pinPos, 5 * shaderZoom, source >= 0 ? kWire : IM_COL32(110, 120, 139, 255));
            ImGui::SetCursorScreenPos(pinPos - ImVec2(8, 8) * shaderZoom); ImGui::PushID(pin);
            ImGui::InvisibleButton("##input", ImVec2(16, 16) * shaderZoom);
            // The source pin still owns the active ID if it is submitted after this target.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && shaderLink >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (surface) { shaderDocument[kOutputs[pin]] = shaderLink; ShaderRecord(); }
                else if (ConnectEditorShaderGraph(shaderDocument, shaderLink, n, pin, &shaderError)) ShaderRecord();
                else Notify(shaderError, true);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                if (surface && pin != 0) shaderDocument.erase(kOutputs[pin]);
                else if (surface) shaderDocument[kOutputs[pin]] = -1;
                else shaderDocument["nodes"][n]["args"][pin] = -1;
                ShaderRecord();
            }
            ImGui::PopID();
        }
        if (!surface) {
            ImVec2 pinPos = output(n); draw->AddCircleFilled(pinPos, 5 * shaderZoom, kWire);
            ImGui::SetCursorScreenPos(pinPos - ImVec2(8, 8) * shaderZoom);
            ImGui::InvisibleButton("##output", ImVec2(16, 16) * shaderZoom);
            if (ImGui::IsItemActivated()) shaderLink = n;
            const Json& node = document["nodes"][n];
            if (node.has("value") || node.has("name")) {
                std::string value = node.has("name") ? node["name"].asString() : node["value"].dump();
                if (value.size() > 24) value = value.substr(0, 21) + "...";
                draw->AddText(p + ImVec2(10, 65) * shaderZoom, IM_COL32(229, 193, 140, 255), value.c_str());
            }
        }
        ImGui::PopID();
    }
    ImGui::PopFont();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Escape)) shaderLink = -1;
    ImGui::EndChild();
}

void NativeEditor::Impl::ShaderProperties() {
    ImGui::PushItemWidth(-FLT_MIN);
    Json ordered;
    std::string validation;
    bool valid = OrderEditorShaderGraph(shaderDocument, ordered, &validation);
    ImGui::TextColored(valid ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(1, 0.45f, 0.4f, 1), "%s", Tr(valid ? "No problems" : "Error"));
    if (!valid) ImGui::TextWrapped("%s", validation.c_str());
    if (valid) {
        std::string key = ordered.dump() + shaderMaterial.dump();
        if (key != shaderPreviewKey) { shaderPreview = SurfacePreview(ordered, shaderMaterial, &validation); shaderPreviewKey = key; }
        if (shaderPreview) ImGui::Image(ViewTexture(engine.Gpu()->ImageView(shaderPreview)), ImVec2(128, 128));
        else if (!validation.empty()) ImGui::TextWrapped("%s", validation.c_str());
        HelpTooltip(Tr("Surface preview: fixed time, white texture, no lighting or vertex displacement."));
    }
    int count = static_cast<int>(shaderDocument["nodes"].size());
    if (shaderSelected >= 0 && shaderSelected < count && ImGui::CollapsingHeader(Tr("Node Properties"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID(shaderSelected);
        Json& node = shaderDocument["nodes"][shaderSelected];
        ImGui::Text("%d  %s", shaderSelected, node["op"].asString().c_str());
        if (node["op"].asString() == "constant") {
            bool scalar = node["value"].isNumber();
            if (ImGui::Checkbox(Tr("Scalar"), &scalar)) {
                float value = node["value"].isNumber() ? node["value"].asFloat() : node["value"][0].asFloat();
                node["value"] = scalar ? Json(value) : Json(Json::Array{value, value, value, value}); ShaderRecord();
            }
            VectorField(Tr("Value"), node["value"]);
            if (ImGui::IsItemDeactivatedAfterEdit()) ShaderRecord();
        }
        if (node["op"].asString() == "uniform") {
            std::string name = node["name"].asString();
            if (ImGui::BeginCombo(Tr("Uniform"), name.c_str())) {
                for (const auto& uniform : shaderDocument["uniforms"].members()) {
                    if (ImGui::Selectable(uniform.first.c_str(), name == uniform.first)) { node["name"] = uniform.first; ShaderRecord(); }
                }
                ImGui::EndCombo();
            }
        }
        if (node["op"].asString() == "swizzle") {
            int channels[4]; for (int i = 0; i < 4; ++i) channels[i] = node["value"][i].asInt();
            if (ImGui::DragInt4(Tr("Channels"), channels, 0.05f, 0, 3)) node["value"] = Json(Json::Array{channels[0], channels[1], channels[2], channels[3]});
            if (ImGui::IsItemDeactivatedAfterEdit()) ShaderRecord();
        }
        if (ImGui::Button(Tr("Delete Node"))) {
            RemoveEditorShaderNode(shaderDocument, shaderSelected);
            shaderPositions.erase(shaderPositions.begin() + shaderSelected); shaderSelected = -1; shaderLink = -1; ShaderRecord();
        }
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader(Tr("Uniform Defaults"))) {
        std::string remove;
        for (auto& uniform : shaderDocument["uniforms"].members()) {
            ImGui::PushID(uniform.first.c_str());
            VectorField(uniform.first.c_str(), uniform.second);
            if (ImGui::IsItemDeactivatedAfterEdit()) ShaderRecord();
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(Tr("Remove"))) remove = uniform.first;
                if (ImGui::MenuItem(Tr("Scalar"))) { Json scalar = uniform.second.isNumber() ? uniform.second : uniform.second[0]; uniform.second = scalar; ShaderRecord(); }
                if (ImGui::MenuItem(Tr("Vector"))) {
                    if (uniform.second.isNumber()) { float v = uniform.second.asFloat(); uniform.second = Json(Json::Array{v, v, v, v}); ShaderRecord(); }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (!remove.empty()) { shaderDocument["uniforms"].erase(remove); shaderMaterial["shaderUniforms"].erase(remove); ShaderRecord(); }
        ImGui::InputText(Tr("Uniform name"), &shaderUniformName);
        if (ImGui::Button(Tr("Add Uniform")) && shaderDocument["uniforms"].size() < 8 && !shaderUniformName.empty()) {
            if (!shaderDocument["uniforms"].has(shaderUniformName)) { shaderDocument["uniforms"][shaderUniformName] = 1; ShaderRecord(); }
        }
    }
    if (ImGui::CollapsingHeader(Tr("Material"), ImGuiTreeNodeFlags_DefaultOpen)) {
        float color[3] = {1, 1, 1};
        if (shaderMaterial["baseColor"].isArray()) for (int i = 0; i < 3; ++i) color[i] = shaderMaterial["baseColor"][i].asFloat(1);
        ImGui::TextUnformatted(Tr("Base Color"));
        if (ImGui::ColorEdit3("##basecolor", color)) shaderMaterial["baseColor"] = Json(Json::Array{color[0], color[1], color[2]});
        float roughness = shaderMaterial["roughness"].asFloat(1), metallic = shaderMaterial["metallic"].asFloat();
        ImGui::TextUnformatted(Tr("Roughness"));
        if (ImGui::SliderFloat("##roughness", &roughness, 0, 1)) shaderMaterial["roughness"] = roughness;
        ImGui::TextUnformatted(Tr("Metallic"));
        if (ImGui::SliderFloat("##metallic", &metallic, 0, 1)) shaderMaterial["metallic"] = metallic;
        bool unlit = shaderMaterial["unlit"].asBool();
        if (ImGui::Checkbox(Tr("Unlit"), &unlit)) shaderMaterial["unlit"] = unlit;
        float opacity = shaderMaterial["opacity"].asFloat(1);
        ImGui::TextUnformatted(Tr("Opacity"));
        if (ImGui::SliderFloat("##opacity", &opacity, 0, 1)) shaderMaterial["opacity"] = opacity;
        std::string alpha = shaderMaterial["alphaMode"].asString("opaque");
        ImGui::TextUnformatted(Tr("Alpha Mode"));
        if (ImGui::BeginCombo("##alpha", alpha.c_str())) {
            for (const char* mode : {"opaque", "mask", "blend"}) if (ImGui::Selectable(mode, alpha == mode)) shaderMaterial["alphaMode"] = mode;
            ImGui::EndCombo();
        }
        std::string texture = shaderMaterial["baseTexture"].asString("");
        ImGui::TextUnformatted(Tr("Base Texture"));
        if (ImGui::InputText("##basetexture", &texture)) shaderMaterial["baseTexture"] = texture;
        if (ImGui::Button(Tr("Pick Texture"))) ImGui::OpenPopup("##shadertexture");
        std::string picked;
        if (AssetPicker("##shadertexture", "texture", texture, picked)) shaderMaterial["baseTexture"] = picked;
        for (const auto& uniform : shaderDocument["uniforms"].members()) {
            if (uniform.first == "cameraPosition" || uniform.first == "lightDirection") continue;
            Json value = shaderMaterial["shaderUniforms"].has(uniform.first) ? shaderMaterial["shaderUniforms"][uniform.first] : uniform.second;
            ImGui::PushID(uniform.first.c_str());
            if (VectorField(uniform.first.c_str(), value)) shaderMaterial["shaderUniforms"][uniform.first] = value;
            ImGui::PopID();
        }
        if (!shaderExistingMaterial.empty()) {
            if (auto preview = AssetThumbnail(shaderExistingMaterial)) ImGui::Image(ViewTexture(engine.Gpu()->ImageView(preview)), ImVec2(96, 96));
        }
    }
    ImGui::PopItemWidth();
}

void NativeEditor::Impl::ShadersPanel() {
    shaderEditorFocused = false;
    if (focusShaders) { ImGui::SetNextWindowFocus(); focusShaders = false; }
    // Keep unsaved documents visible; switching documents has an explicit save/discard prompt.
    if (!ImGui::Begin(TrId("Shader Graph").c_str(), &showShaders)) { ImGui::End(); return; }
    shaderEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (ImGui::Button(Tr("New Shader"))) OpenShader("");
    if (shaderDocument.isNull()) {
        ImGui::TextWrapped("%s", Tr("Double-click a shader or material in Assets, or create a new shader."));
        ImGui::End(); return;
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 170));
    ImGui::InputText("##shaderpath", &shaderSavePath); HelpTooltip(Tr("Project path ending in .shader.json"));
    ImGui::SameLine();
    if (ImGui::Button(Tr("Save Shader")) || (shaderEditorFocused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) SaveShader();
    if (shaderDocument != shaderSaved || shaderMaterial != shaderMaterialSaved) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "*"); }
    if (ImGui::Button(Tr("Add Node"))) ImGui::OpenPopup("##nodepalette");
    if (ImGui::BeginPopup("##nodepalette")) {
        for (const std::string& op : EditorShaderOperations()) {
            if (ImGui::MenuItem(op.c_str(), nullptr, false, shaderDocument["nodes"].size() < 48)) {
                Json node = ObjectOf({{"op", op}});
                int arity = EditorShaderArity(op);
                if (arity) { node["args"] = Json::MakeArray(); for (int i = 0; i < arity; ++i) node["args"].push(-1); }
                if (op == "constant") node["value"] = Json(Json::Array{1, 1, 1, 1});
                if (op == "swizzle") node["value"] = Json(Json::Array{0, 1, 2, 3});
                if (op == "uniform") {
                    if (!shaderDocument["uniforms"].size()) shaderDocument["uniforms"]["parameter1"] = 1;
                    node["name"] = shaderDocument["uniforms"].members()[0].first;
                }
                shaderDocument["nodes"].push(node);
                shaderPositions.push_back(ImVec2(70 - shaderPan.x / shaderZoom, 80 - shaderPan.y / shaderZoom));
                shaderSelected = static_cast<int>(shaderPositions.size()) - 1; ShaderRecord();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(); if (ImGui::Button(Tr("Arrange Nodes"))) ShaderArrange();
    ImGui::SameLine(); if (ImGui::Button(Tr("Fit Graph"))) shaderFit = true;
    auto history = [&](bool redo) {
        if (redo ? shaderHistoryIndex + 1 < shaderHistory.size() : shaderHistoryIndex > 0) {
            shaderHistoryIndex = redo ? shaderHistoryIndex + 1 : shaderHistoryIndex - 1;
            shaderDocument = shaderHistory[shaderHistoryIndex]; shaderSelected = -1; shaderLink = -1;
            if (shaderPositions.size() != shaderDocument["nodes"].size()) ShaderArrange();
        }
    };
    ImGui::SameLine(); ImGui::BeginDisabled(shaderHistoryIndex == 0);
    if (ImGui::Button(Tr("Undo"))) history(false); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(shaderHistoryIndex + 1 == shaderHistory.size());
    if (ImGui::Button(Tr("Redo"))) history(true); ImGui::EndDisabled();
    if (shaderEditorFocused && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) history(false);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) history(true);
    }
    ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x - 340));
    ImGui::InputTextWithHint("##materialpath", Tr("Material path"), &shaderMaterialPath);
    HelpTooltip(Tr("Material path"));
    ImGui::SameLine();
    Json ordered; std::string validation;
    bool valid = OrderEditorShaderGraph(shaderDocument, ordered, &validation);
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button(Tr(shaderExistingMaterial == shaderMaterialPath ? "Save Material" : "Create Material"))) SaveShaderMaterial();
    ImGui::EndDisabled(); ImGui::SameLine();
    bool mesh = selectedId != kNullEntity && selected["components"].has("MeshRenderer");
    ImGui::BeginDisabled(!valid || !mesh);
    if (ImGui::Button(Tr("Apply to Selected Mesh"))) {
        if (SaveShaderMaterial()) Call("component.set", ObjectOf({{"id", Json(selectedId)}, {"type", "MeshRenderer"},
            {"values", ObjectOf({{"material", shaderExistingMaterial}, {"shaderUniforms", Json::MakeObject()}})}}));
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", Tr("Drag output to input; right-click input to disconnect. Middle drag: pan; wheel: zoom."));
    float properties = std::min(270.0f, ImGui::GetContentRegionAvail().x * 0.42f);
    ImGui::BeginChild("##graphleft", ImVec2(std::max(100.0f, ImGui::GetContentRegionAvail().x - properties - 8), 0));
    ShaderCanvas(); ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("##graphproperties", ImVec2(0, 0), ImGuiChildFlags_Borders); ShaderProperties(); ImGui::EndChild();
    if (shaderOpenPrompt) { ImGui::OpenPopup(TrId("Save Shader Changes?").c_str()); shaderOpenPrompt = false; }
    if (ImGui::BeginPopupModal(TrId("Save Shader Changes?").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(Tr("Save the current shader before opening another?"));
        bool proceed = false, discard = false;
        if (ImGui::Button(Tr("Save"))) proceed = shaderMaterial != shaderMaterialSaved ? SaveShaderMaterial() : SaveShader();
        ImGui::SameLine(); if (ImGui::Button(Tr("Don't Save"))) { proceed = discard = true; }
        ImGui::SameLine(); if (ImGui::Button(Tr("Cancel"))) { shaderPendingOpen = shaderPendingQuit = false; ImGui::CloseCurrentPopup(); }
        if (proceed) {
            bool quitting = shaderPendingQuit;
            shaderPendingOpen = shaderPendingQuit = false; ImGui::CloseCurrentPopup();
            if (quitting) {
                if (discard) { shaderDocument = Json(); shaderHistory.clear(); }
                RequestAction({PendingAction::Kind::Quit, ""});
            }
            else OpenShader(shaderPendingPath, true);
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}
}  // namespace oe
