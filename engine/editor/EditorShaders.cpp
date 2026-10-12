#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include "editor/EditorShaderGraph.h"
#include "editor/EditorNodeCanvas.h"
#include "editor/EditorText.h"
#include "imgui_internal.h"
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

// How a node card is laid out, in canvas units.
const float kNodeWidth = 200, kPinRow = 24, kValueRow = 26;

// What kind of thing a node is: its header color, the color of the wires that leave it and the
// group it is listed under in the Add Node list.
struct NodeKind {
    const char* group;
    ImU32 header, wire;
};
const NodeKind kInputKind{"Inputs", IM_COL32(52, 112, 84, 255), IM_COL32(120, 214, 160, 255)};
const NodeKind kValueKind{"Values", IM_COL32(134, 100, 46, 255), IM_COL32(238, 196, 110, 255)};
const NodeKind kTextureKind{"Texture", IM_COL32(104, 76, 142, 255), IM_COL32(196, 160, 240, 255)};
const NodeKind kMathKind{"Math", IM_COL32(52, 86, 134, 255), IM_COL32(112, 202, 242, 255)};
const NodeKind kVectorKind{"Vector", IM_COL32(40, 108, 116, 255), IM_COL32(110, 220, 224, 255)};
const ImU32 kSurfaceHeader = IM_COL32(150, 84, 72, 255);

struct NodeInfo {
    const char* op;
    const NodeKind* kind;
    const char* pins[3];
    const char* doc;  // English; shown translated
};
const NodeInfo kNodes[] = {
    {"uv", &kInputKind, {}, "Texture coordinates of the surface (0..1 across the mesh)"},
    {"position", &kInputKind, {}, "World position of the surface point"},
    {"normal", &kInputKind, {}, "World direction the surface faces"},
    {"time", &kInputKind, {}, "Seconds of game time: animates anything it feeds"},
    {"baseColor", &kInputKind, {}, "The material's base color and opacity"},
    {"particleAge", &kInputKind, {}, "Effect particles: age from 0 (born) to 1 (gone)"},
    {"particleRandom", &kInputKind, {}, "Effect particles: a random number that stays with the particle"},
    {"particleSpeed", &kInputKind, {}, "Effect particles: speed in the world"},
    {"particleCustom0", &kInputKind, {}, "Effect particles: custom channel 0"},
    {"particleCustom1", &kInputKind, {}, "Effect particles: custom channel 1"},
    {"particleCustom2", &kInputKind, {}, "Effect particles: custom channel 2"},
    {"particleCustom3", &kInputKind, {}, "Effect particles: custom channel 3"},
    {"constant", &kValueKind, {}, "A fixed number or color"},
    {"uniform", &kValueKind, {}, "A named value that materials and scripts can change"},
    {"texture", &kTextureKind, {"UV"}, "Samples the material's base texture at the given coordinates"},
    {"add", &kMathKind, {"A", "B"}, "A + B"},
    {"subtract", &kMathKind, {"A", "B"}, "A - B"},
    {"multiply", &kMathKind, {"A", "B"}, "A x B"},
    {"divide", &kMathKind, {"A", "B"}, "A / B (0 where B is 0)"},
    {"min", &kMathKind, {"A", "B"}, "The smaller of A and B"},
    {"max", &kMathKind, {"A", "B"}, "The larger of A and B"},
    {"sin", &kMathKind, {"In"}, "Sine wave of the input: swings between -1 and 1"},
    {"cos", &kMathKind, {"In"}, "Cosine wave of the input: swings between -1 and 1"},
    {"floor", &kMathKind, {"In"}, "Rounds down to a whole number"},
    {"fract", &kMathKind, {"In"}, "The part after the decimal point: repeats 0..1"},
    {"abs", &kMathKind, {"In"}, "Makes negative values positive"},
    {"clamp", &kMathKind, {"Value", "Min", "Max"}, "Keeps Value between Min and Max"},
    {"mix", &kMathKind, {"A", "B", "Blend"}, "Blends from A (Blend 0) to B (Blend 1)"},
    {"step", &kMathKind, {"Edge", "Value"}, "0 where Value is below Edge, otherwise 1"},
    {"dot", &kVectorKind, {"A", "B"}, "How much two directions agree: 1 same way, 0 at a right angle"},
    {"normalize", &kVectorKind, {"In"}, "Scales a vector to length 1"},
    {"swizzle", &kVectorKind, {"In"}, "Reorders or repeats the channels (x y z w) of the input"},
};
const NodeInfo* InfoOf(const std::string& op) {
    for (const NodeInfo& info : kNodes) if (op == info.op) return &info;
    return nullptr;
}
bool HasValueRow(const Json& node) { return node.has("value") || node.has("name"); }
ImVec2 NodeSize(const Json& node) {
    const float pins = static_cast<float>(node["args"].size());
    return ImVec2(kNodeWidth, EditorNodeCanvas::kHeader + pins * kPinRow + (HasValueRow(node) ? kValueRow : 0) + (pins > 0 || HasValueRow(node) ? 10 : 6));
}
const ImVec2 kSurfaceSize(kNodeWidth, EditorNodeCanvas::kHeader + 4 * kPinRow + 10);

std::string Sibling(const std::string& path, const std::string& suffix) {
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) dot = path.find_last_of('.', dot - 1);
    return path.substr(0, dot) + suffix;
}

Json NewGraph() {
    return Json::parse(R"({"format":"ownengine.shader","uniforms":{},"nodes":[{"op":"baseColor"}],"color":0})");
}

// A row of a properties table: the label on the left, the widget that follows fills the right column.
void PropertyLabel(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && *tooltip) ImGui::SetItemTooltip("%s", tooltip);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}
bool BeginProperties(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}

// A number or a four-vector, in the right column of a properties table.
bool VectorField(const char* label, Json& value) {
    ImGui::PushID(label);
    PropertyLabel(label);
    bool scalar = value.isNumber();
    float v[4];
    for (int i = 0; i < 4; ++i) v[i] = scalar ? value.asFloat() : value[i].asFloat();
    bool changed = scalar ? ImGui::DragFloat("##value", v, 0.02f, -65504, 65504, "%.3g") :
                            ImGui::DragFloat4("##value", v, 0.02f, -65504, 65504, "%.3g");
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
    shaderSelected = -1; shaderLink = -1; shaderError.clear(); shaderPreviewKey.clear();
    ShaderArrange();
}

void NativeEditor::Impl::ShaderArrange() {
    const Json& document = shaderDocument;
    const int count = static_cast<int>(document["nodes"].size());
    shaderPositions.assign(static_cast<size_t>(count), ImVec2());
    // How far a node is from the inputs (longest path): the Surface Output is one column past the deepest.
    std::vector<int> depths(static_cast<size_t>(count), 0);
    for (int pass = 0; pass < count; ++pass) {
        bool changed = false;
        for (int i = 0; i < count; ++i) {
            for (const Json& input : document["nodes"][i]["args"].items()) {
                const int source = input.asInt(-1);
                if (source < 0 || source >= count) continue;
                const int depth = depths[static_cast<size_t>(source)] + 1;
                if (depth > depths[static_cast<size_t>(i)]) { depths[static_cast<size_t>(i)] = depth; changed = true; }
            }
        }
        if (!changed) break;
    }
    int surface = 0;
    for (int depth : depths) surface = std::max(surface, depth + 1);
    // Who reads each node, and at which pin (the Surface Output is consumer `count`).
    struct Use { int node, pin; };
    std::vector<std::vector<Use>> uses(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const Json& args = document["nodes"][i]["args"];
        for (int pin = 0; pin < static_cast<int>(args.size()); ++pin) {
            const int source = args[pin].asInt(-1);
            if (source >= 0 && source < count) uses[static_cast<size_t>(source)].push_back({i, pin});
        }
    }
    for (int pin = 0; pin < 4; ++pin) {
        const int source = document[kOutputs[pin]].asInt(-1);
        if (source >= 0 && source < count) uses[static_cast<size_t>(source)].push_back({count, pin});
    }
    // Columns: every node goes as far right as its consumers allow, so an input or a constant sits just left
    // of what reads it instead of in one tall column at the far left with wires across the whole graph.
    std::vector<int> order(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) order[static_cast<size_t>(i)] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depths[static_cast<size_t>(a)] > depths[static_cast<size_t>(b)]; });
    std::vector<int> column(static_cast<size_t>(count), 0);
    for (int i : order) {
        int latest = INT_MAX;
        for (const Use& use : uses[static_cast<size_t>(i)]) latest = std::min(latest, (use.node == count ? surface : column[static_cast<size_t>(use.node)]) - 1);
        column[static_cast<size_t>(i)] = latest == INT_MAX ? depths[static_cast<size_t>(i)] : latest;
    }
    const float columnGap = kNodeWidth + 80, rowGap = 20;
    shaderOutputPos = ImVec2(static_cast<float>(surface) * columnGap, 0);
    auto pinHeight = [&](const Use& use) {
        const ImVec2 at = use.node == count ? shaderOutputPos : shaderPositions[static_cast<size_t>(use.node)];
        return at.y + EditorNodeCanvas::kHeader + 5 + (static_cast<float>(use.pin) + 0.5f) * kPinRow;
    };
    // Right to left: a column is ordered by where its nodes are read, each node level with its consumers'
    // pins, pushed down only where it would overlap the one above.
    for (int c = surface - 1; c >= 0; --c) {
        std::vector<int> nodes;
        std::vector<float> wanted(static_cast<size_t>(count), 0);
        float loose = 0;  // nodes nobody reads go under the others
        for (int i = 0; i < count; ++i) {
            if (column[static_cast<size_t>(i)] != c) continue;
            nodes.push_back(i);
            float sum = 0;
            for (const Use& use : uses[static_cast<size_t>(i)]) sum += pinHeight(use);
            wanted[static_cast<size_t>(i)] = uses[static_cast<size_t>(i)].empty() ? 1e6f + (loose += 1) : sum / static_cast<float>(uses[static_cast<size_t>(i)].size());
        }
        std::stable_sort(nodes.begin(), nodes.end(), [&](int a, int b) { return wanted[static_cast<size_t>(a)] < wanted[static_cast<size_t>(b)]; });
        float y = -1e9f, shift = 0;
        int placed = 0;
        for (int i : nodes) {
            const float height = NodeSize(document["nodes"][i]).y;
            const bool read = !uses[static_cast<size_t>(i)].empty();
            const float top = read ? wanted[static_cast<size_t>(i)] - EditorNodeCanvas::kHeader * 0.5f : y;
            y = std::max(y, top);
            if (y < -1e8f) y = 0;
            shaderPositions[static_cast<size_t>(i)] = ImVec2(static_cast<float>(c) * columnGap, y);
            if (read) { shift += top - y; ++placed; }
            y += height + rowGap;
        }
        // Pushing down only would let a crowded column drift below its consumers: move it back up by the average.
        if (placed) for (int i : nodes) shaderPositions[static_cast<size_t>(i)].y += shift / static_cast<float>(placed);
    }
    shaderFit = true; shaderFitReadable = true;
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

void NativeEditor::Impl::ShaderRect(const std::string& name) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    shaderRects[name] = {a.x, a.y, b.x - a.x, b.y - a.y};
}

// The Add Node list: searchable, grouped by kind, with what each node does. The node appears where the
// list was opened (under the mouse on the canvas, or in the middle of the view from the toolbar).
void NativeEditor::Impl::ShaderPalette() {
    if (shaderPaletteOpen) { ImGui::OpenPopup("##nodepalette"); shaderPaletteOpen = false; shaderPaletteFilter.clear(); }
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 20, 0), ImVec2(ImGui::GetFontSize() * 30, ImGui::GetFontSize() * 28));
    if (!ImGui::BeginPopup("##nodepalette")) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##nodesearch", WithIcon(kIconSearch, Tr("Search nodes")).c_str(), &shaderPaletteFilter);
    const bool full = shaderDocument["nodes"].size() >= 48;
    if (full) ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "%s", Tr("A graph holds at most 48 nodes."));
    auto matches = [&](const NodeInfo& info) {
        if (shaderPaletteFilter.empty()) return true;
        auto has = [&](const std::string& text) {
            return std::search(text.begin(), text.end(), shaderPaletteFilter.begin(), shaderPaletteFilter.end(),
                [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }) != text.end();
        };
        return has(info.op) || has(HumanizeName(info.op)) || has(Tr(info.doc));
    };
    const NodeKind* group = nullptr;
    for (const NodeInfo& info : kNodes) {
        if (!matches(info)) continue;
        if (info.kind != group) {
            group = info.kind;
            ImGui::Spacing();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(group->wire), "%s", Tr(group->group));
            ImGui::Separator();
        }
        const std::string label = HumanizeName(info.op);
        const bool chosen = ImGui::Selectable(("  " + label + "##" + info.op).c_str(), false, full ? ImGuiSelectableFlags_Disabled : 0);
        ShaderRect(std::string("palette:") + info.op);
        ImGui::SameLine(ImGui::GetFontSize() * 9.5f);
        ImGui::TextDisabled("%s", Tr(info.doc));
        if (!chosen) continue;
        const std::string op = info.op;
        Json node = ObjectOf({{"op", op}});
        const int arity = EditorShaderArity(op);
        if (arity) { node["args"] = Json::MakeArray(); for (int i = 0; i < arity; ++i) node["args"].push(-1); }
        if (op == "constant") node["value"] = Json(Json::Array{1, 1, 1, 1});
        if (op == "swizzle") node["value"] = Json(Json::Array{0, 1, 2, 3});
        if (op == "uniform") {
            if (!shaderDocument["uniforms"].size()) shaderDocument["uniforms"]["parameter1"] = 1;
            node["name"] = shaderDocument["uniforms"].members()[0].first;
        }
        shaderDocument["nodes"].push(node);
        shaderPositions.push_back(shaderPalettePos);
        shaderSelected = static_cast<int>(shaderPositions.size()) - 1; ShaderRecord();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void NativeEditor::Impl::ShaderCanvas() {
    EditorNodeCanvas canvas("##shadercanvas", shaderPan, shaderZoom);
    shaderRects["canvas"] = {canvas.origin.x, canvas.origin.y, canvas.size.x, canvas.size.y};
    const int count = static_cast<int>(shaderDocument["nodes"].size());
    const Json& document = shaderDocument;
    if (shaderFit) {
        ImVec2 low = shaderOutputPos, high = shaderOutputPos + kSurfaceSize;
        for (int n = 0; n < count; ++n) {
            const ImVec2 p = shaderPositions[static_cast<size_t>(n)], size = NodeSize(document["nodes"][n]);
            low.x = std::min(low.x, p.x); low.y = std::min(low.y, p.y);
            high.x = std::max(high.x, p.x + size.x); high.y = std::max(high.y, p.y + size.y);
        }
        canvas.Fit(low, high);
        // A large graph is not shrunk until its text is unreadable: it opens at a readable size on the Surface
        // Output, where the result is, and the rest is a drag away (Fit Graph shows all of it).
        if (shaderFitReadable && shaderZoom < 0.45f) {
            shaderZoom = 0.7f;
            shaderPan = ImVec2(canvas.size.x - 40 - (shaderOutputPos.x + kSurfaceSize.x) * shaderZoom,
                               canvas.size.y * 0.5f - (shaderOutputPos.y + kSurfaceSize.y * 0.5f) * shaderZoom);
        }
        shaderFit = shaderFitReadable = false;
    }
    canvas.Navigate();
    ImDrawList* draw = canvas.draw; ImGuiIO& io = ImGui::GetIO();
    auto point = [&](ImVec2 p) { return canvas.Point(p); };
    auto output = [&](int node) { return point(shaderPositions[static_cast<size_t>(node)] + ImVec2(kNodeWidth, EditorNodeCanvas::kHeader * 0.5f)); };
    auto input = [&](int node, int pin) {
        ImVec2 pos = node < 0 ? shaderOutputPos : shaderPositions[static_cast<size_t>(node)];
        return point(pos + ImVec2(0, EditorNodeCanvas::kHeader + kPinRow * 0.5f + 5 + static_cast<float>(pin) * kPinRow));
    };
    auto kindOf = [&](int node) {
        const NodeInfo* info = InfoOf(document["nodes"][node]["op"].asString());
        return info ? info->kind : &kMathKind;
    };
    // Wires of the selected node stand out; the others step back while one is selected.
    auto wire = [&](int source, ImVec2 to, int target) {
        const float emphasis = shaderSelected < 0 ? 1.0f : (source == shaderSelected || target == shaderSelected) ? 1.6f : 0.6f;
        canvas.Wire(output(source), to, kindOf(source)->wire, emphasis);
    };
    for (int n = 0; n < count; ++n) {
        const Json& args = document["nodes"][n]["args"];
        for (int pin = 0; pin < static_cast<int>(args.size()); ++pin) {
            int source = args[pin].asInt(-1); if (source >= 0 && source < count) wire(source, input(n, pin), n);
        }
    }
    for (int pin = 0; pin < 4; ++pin) {
        int source = document[kOutputs[pin]].asInt(-1);
        if (source >= 0 && source < count) wire(source, input(-1, pin), -2);
    }
    if (shaderLink >= 0 && shaderLink < count) canvas.Wire(output(shaderLink), io.MousePos, IM_COL32(255, 213, 116, 255), 1.6f);
    const float text = ImGui::GetFontSize() * shaderZoom;
    const bool labels = text >= 7.0f;
    ImGui::PushFont(nullptr, text);
    bool overNode = false;
    for (int n = 0; n <= count; ++n) {
        bool surface = n == count;
        ImVec2& position = surface ? shaderOutputPos : shaderPositions[static_cast<size_t>(n)];
        const Json& node = surface ? Json() : document["nodes"][n];
        const std::string op = surface ? std::string() : node["op"].asString();
        const NodeInfo* info = surface ? nullptr : InfoOf(op);
        int pins = surface ? 4 : static_cast<int>(node["args"].size());
        const ImVec2 size = surface ? kSurfaceSize : NodeSize(node);
        ImVec2 p = point(position);
        const std::string title = surface ? Tr("Surface Output") : HumanizeName(op);
        const std::string badge = surface ? std::string() : "#" + std::to_string(n);
        ImGui::PushID(n);
        canvas.Card(position, size, title.c_str(), surface ? kSurfaceHeader : (info ? info->kind->header : kMathKind.header),
                    !surface && shaderSelected == n, false, badge.c_str());
        ShaderRect(surface ? "surface" : "node:" + std::to_string(n));
        if (ImGui::IsItemHovered()) {
            overNode = true;
            if (info && shaderLink < 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) ImGui::SetItemTooltip("%s", Tr(info->doc));
        }
        if (ImGui::IsItemClicked() && !surface) shaderSelected = n;
        for (int pin = 0; pin < pins; ++pin) {
            ImVec2 pinPos = input(surface ? -1 : n, pin);
            const std::string label = surface ? HumanizeName(kOutputs[pin]) : (info && info->pins[pin]) ? std::string(info->pins[pin]) : std::string(1, static_cast<char>('A' + pin));
            int source = surface ? document[kOutputs[pin]].asInt(-1) : node["args"][pin].asInt(-1);
            if (labels) draw->AddText(pinPos + ImVec2(13 * shaderZoom, -text * 0.5f), source >= 0 ? IM_COL32(226, 232, 242, 255) : IM_COL32(150, 160, 178, 255), label.c_str());
            ImGui::PushID(pin); canvas.Pin("##input", pinPos, source >= 0);
            ShaderRect(surface ? std::string("surface:") + kOutputs[pin] : "in:" + std::to_string(n) + ":" + std::to_string(pin));
            if (ImGui::IsItemHovered()) overNode = true;
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
            ImVec2 pinPos = output(n); canvas.Pin("##output", pinPos, true);
            ShaderRect("out:" + std::to_string(n));
            if (ImGui::IsItemHovered()) overNode = true;
            if (ImGui::IsItemActivated()) shaderLink = n;
            if (HasValueRow(node) && labels) {
                // What the node holds, readable without opening its properties: a number, a color, a name.
                const ImVec2 row = p + ImVec2(10, EditorNodeCanvas::kHeader + static_cast<float>(pins) * kPinRow + 4) * shaderZoom;
                const ImVec2 rowEnd = p + ImVec2(kNodeWidth - 10, EditorNodeCanvas::kHeader + static_cast<float>(pins) * kPinRow + 4 + kValueRow - 6) * shaderZoom;
                draw->AddRectFilled(row, rowEnd, IM_COL32(22, 25, 31, 255), 4 * shaderZoom);
                std::string value;
                float indent = 8 * shaderZoom;
                if (node.has("name")) value = node["name"].asString();
                else if (op == "swizzle") { for (int i = 0; i < 4; ++i) value += "xyzw"[std::max(0, std::min(3, node["value"][i].asInt()))]; }
                else if (node["value"].isNumber()) value = Format("%.3g", node["value"].asFloat());
                else {
                    const ImVec4 color(node["value"][0].asFloat(), node["value"][1].asFloat(), node["value"][2].asFloat(), 1);
                    const float swatch = rowEnd.y - row.y - 6 * shaderZoom;
                    draw->AddRectFilled(row + ImVec2(4, 3) * shaderZoom, row + ImVec2(4 * shaderZoom + swatch, 3 * shaderZoom + swatch),
                                        ImGui::ColorConvertFloat4ToU32(ImVec4(Clamp(color.x, 0, 1), Clamp(color.y, 0, 1), Clamp(color.z, 0, 1), 1)), 3 * shaderZoom);
                    indent += swatch + 4 * shaderZoom;
                    value = Format("%.2g  %.2g  %.2g  %.2g", node["value"][0].asFloat(), node["value"][1].asFloat(), node["value"][2].asFloat(), node["value"][3].asFloat());
                }
                draw->PushClipRect(row, rowEnd, true);
                draw->AddText(ImVec2(row.x + indent, (row.y + rowEnd.y - text) * 0.5f), IM_COL32(238, 204, 150, 255), value.c_str());
                draw->PopClipRect();
            }
        }
        ImGui::PopID();
    }
    ImGui::PopFont();
    canvas.Footer(Tr("Drag an output to an input to connect. Right-click an input to disconnect. Right-click or double-click the canvas to add a node. Drag the canvas to move, wheel to zoom."));
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Escape)) shaderLink = -1;
    // The empty canvas: a click deselects, a right-click or double-click opens the Add Node list there.
    const bool empty = ImGui::IsWindowHovered() && !overNode;
    if (empty && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) shaderPressedEmpty = true;
    if (shaderPressedEmpty && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (empty && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, 3.0f)) shaderSelected = -1;
        shaderPressedEmpty = false;
    }
    if (empty && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        shaderPalettePos = canvas.Position(io.MousePos); shaderPaletteOpen = true;
    }
    if (shaderPaletteAtMouse) {  // from the toolbar: the middle of what is in view
        shaderPalettePos = canvas.Position(canvas.origin + canvas.size * 0.5f) - ImVec2(kNodeWidth * 0.5f, 40);
        shaderPaletteAtMouse = false; shaderPaletteOpen = true;
    }
    ShaderPalette();
}

void NativeEditor::Impl::ShaderProperties() {
    Json ordered;
    std::string validation;
    bool valid = OrderEditorShaderGraph(shaderDocument, ordered, &validation);
    if (valid) {
        std::string key = ordered.dump() + shaderMaterial.dump();
        if (key != shaderPreviewKey) { shaderPreview = SurfacePreview(ordered, shaderMaterial, &validation); shaderPreviewKey = key; }
        if (shaderPreview) {
            // As large as the panel allows, centered.
            const float side = std::max(96.0f, std::min(220.0f, ImGui::GetContentRegionAvail().x));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - side) * 0.5f));
            ImGui::Image(ViewTexture(engine.Gpu()->ImageView(shaderPreview)), ImVec2(side, side));
            ImGui::SetItemTooltip("%s", Tr("Surface preview: fixed time, white texture, no lighting or vertex displacement."));
        } else if (!validation.empty()) ImGui::TextWrapped("%s", validation.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5f, 0.42f, 1));
        ImGui::TextWrapped("%s %s", kIconTriangleAlert, validation.c_str());
        ImGui::PopStyleColor();
    }
    int count = static_cast<int>(shaderDocument["nodes"].size());
    if (ImGui::CollapsingHeader(WithIcon(kIconSlidersHorizontal, Tr("Node Properties")).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (shaderSelected < 0 || shaderSelected >= count) {
            ImGui::TextDisabled("%s", Tr("Click a node to edit it."));
        } else {
            ImGui::PushID(shaderSelected);
            Json& node = shaderDocument["nodes"][shaderSelected];
            const std::string op = node["op"].asString();
            const NodeInfo* info = InfoOf(op);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(info ? info->kind->wire : kMathKind.wire), "%s", HumanizeName(op).c_str());
            ImGui::SameLine(); ImGui::TextDisabled("#%d", shaderSelected);
            if (info) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", Tr(info->doc)); ImGui::PopTextWrapPos(); }
            if (BeginProperties("##nodeprops")) {
                if (op == "constant") {
                    bool scalar = node["value"].isNumber();
                    PropertyLabel(Tr("Kind"));
                    if (ImGui::BeginCombo("##kind", Tr(scalar ? "Number" : "Color / Vector"))) {
                        for (int choice = 0; choice < 2; ++choice) {
                            if (!ImGui::Selectable(Tr(choice ? "Number" : "Color / Vector"), scalar == (choice == 1)) || scalar == (choice == 1)) continue;
                            float value = node["value"].isNumber() ? node["value"].asFloat() : node["value"][0].asFloat();
                            node["value"] = choice ? Json(value) : Json(Json::Array{value, value, value, value}); ShaderRecord();
                        }
                        ImGui::EndCombo();
                    }
                    VectorField(Tr("Value"), node["value"]);
                    if (ImGui::IsItemDeactivatedAfterEdit()) ShaderRecord();
                    if (!node["value"].isNumber()) {
                        // The same four numbers as a color, which is what most constants are.
                        float color[4] = {node["value"][0].asFloat(), node["value"][1].asFloat(), node["value"][2].asFloat(), node["value"][3].asFloat()};
                        PropertyLabel(Tr("Color"));
                        if (ImGui::ColorEdit4("##color", color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
                            node["value"] = Json(Json::Array{color[0], color[1], color[2], color[3]});
                        if (ImGui::IsItemDeactivatedAfterEdit()) ShaderRecord();
                    }
                }
                if (op == "uniform") {
                    std::string name = node["name"].asString();
                    PropertyLabel(Tr("Uniform"));
                    if (ImGui::BeginCombo("##uniform", name.c_str())) {
                        for (const auto& uniform : shaderDocument["uniforms"].members()) {
                            if (ImGui::Selectable(uniform.first.c_str(), name == uniform.first)) { node["name"] = uniform.first; ShaderRecord(); }
                        }
                        ImGui::EndCombo();
                    }
                }
                if (op == "swizzle") {
                    // One choice per output channel instead of four numbers.
                    for (int channel = 0; channel < 4; ++channel) {
                        ImGui::PushID(channel);
                        const char* names[] = {"X / R", "Y / G", "Z / B", "W / A"};
                        PropertyLabel(Format(Tr("Output %s"), names[channel]).c_str());
                        int from = std::max(0, std::min(3, node["value"][channel].asInt()));
                        if (ImGui::BeginCombo("##from", names[from])) {
                            for (int i = 0; i < 4; ++i) if (ImGui::Selectable(names[i], from == i) && from != i) { node["value"][channel] = i; ShaderRecord(); }
                            ImGui::EndCombo();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            if (ImGui::Button(WithIcon(kIconTrash2, Tr("Delete Node")).c_str())) {
                RemoveEditorShaderNode(shaderDocument, shaderSelected);
                shaderPositions.erase(shaderPositions.begin() + shaderSelected); shaderSelected = -1; shaderLink = -1; ShaderRecord();
            }
            ImGui::SetItemTooltip("%s", Tr("Delete key"));
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader(WithIcon(kIconBraces, Format("%s (%d / 8)###uniforms", Tr("Uniform Defaults"), static_cast<int>(shaderDocument["uniforms"].size()))).c_str())) {
        std::string remove;
        if (BeginProperties("##uniformprops")) {
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
            ImGui::EndTable();
        }
        if (shaderDocument["uniforms"].size()) ImGui::TextDisabled("%s", Tr("Right-click a value: number, vector or remove."));
        if (!remove.empty()) { shaderDocument["uniforms"].erase(remove); shaderMaterial["shaderUniforms"].erase(remove); ShaderRecord(); }
        const float add = ImGui::CalcTextSize(Tr("Add Uniform")).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - add));
        ImGui::InputTextWithHint("##uniformname", Tr("Uniform name"), &shaderUniformName);
        ImGui::SameLine();
        ImGui::BeginDisabled(shaderDocument["uniforms"].size() >= 8 || shaderUniformName.empty() || shaderDocument["uniforms"].has(shaderUniformName));
        if (ImGui::Button(Tr("Add Uniform"))) { shaderDocument["uniforms"][shaderUniformName] = 1; ShaderRecord(); }
        ImGui::EndDisabled();
    }
    if (ImGui::CollapsingHeader(WithIcon(kIconPalette, Tr("Material")).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (BeginProperties("##materialprops")) {
            float color[3] = {1, 1, 1};
            if (shaderMaterial["baseColor"].isArray()) for (int i = 0; i < 3; ++i) color[i] = shaderMaterial["baseColor"][i].asFloat(1);
            PropertyLabel(Tr("Base Color"));
            if (ImGui::ColorEdit3("##basecolor", color, ImGuiColorEditFlags_NoInputs)) shaderMaterial["baseColor"] = Json(Json::Array{color[0], color[1], color[2]});
            float roughness = shaderMaterial["roughness"].asFloat(1), metallic = shaderMaterial["metallic"].asFloat();
            PropertyLabel(Tr("Roughness"));
            if (ImGui::SliderFloat("##roughness", &roughness, 0, 1, "%.2f")) shaderMaterial["roughness"] = roughness;
            PropertyLabel(Tr("Metallic"));
            if (ImGui::SliderFloat("##metallic", &metallic, 0, 1, "%.2f")) shaderMaterial["metallic"] = metallic;
            bool unlit = shaderMaterial["unlit"].asBool();
            PropertyLabel(Tr("Unlit"));
            if (ImGui::Checkbox("##unlit", &unlit)) shaderMaterial["unlit"] = unlit;
            float opacity = shaderMaterial["opacity"].asFloat(1);
            PropertyLabel(Tr("Opacity"));
            if (ImGui::SliderFloat("##opacity", &opacity, 0, 1, "%.2f")) shaderMaterial["opacity"] = opacity;
            std::string alpha = shaderMaterial["alphaMode"].asString("opaque");
            PropertyLabel(Tr("Alpha Mode"));
            if (ImGui::BeginCombo("##alpha", alpha.c_str())) {
                for (const char* mode : {"opaque", "mask", "blend"}) if (ImGui::Selectable(mode, alpha == mode)) shaderMaterial["alphaMode"] = mode;
                ImGui::EndCombo();
            }
            std::string texture = shaderMaterial["baseTexture"].asString("");
            PropertyLabel(Tr("Base Texture"));
            const float pick = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(-(pick + ImGui::GetStyle().ItemInnerSpacing.x));
            if (ImGui::InputText("##basetexture", &texture)) shaderMaterial["baseTexture"] = texture;
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::Button("...##picktexture", ImVec2(pick, pick))) ImGui::OpenPopup("##shadertexture");
            ImGui::SetItemTooltip("%s", Tr("Pick Texture"));
            std::string picked;
            if (AssetPicker("##shadertexture", "texture", texture, picked)) shaderMaterial["baseTexture"] = picked;
            for (const auto& uniform : shaderDocument["uniforms"].members()) {
                if (uniform.first == "cameraPosition" || uniform.first == "lightDirection" || uniform.first == "playing") continue;
                Json value = shaderMaterial["shaderUniforms"].has(uniform.first) ? shaderMaterial["shaderUniforms"][uniform.first] : uniform.second;
                if (VectorField(uniform.first.c_str(), value)) shaderMaterial["shaderUniforms"][uniform.first] = value;
            }
            ImGui::EndTable();
        }
        if (!shaderExistingMaterial.empty()) {
            if (auto preview = AssetThumbnail(shaderExistingMaterial)) {
                ImGui::Image(ViewTexture(engine.Gpu()->ImageView(preview)), ImVec2(96, 96));
                ImGui::SetItemTooltip("%s", Tr("The saved material, lit"));
            }
        }
    }
}

void NativeEditor::Impl::ShadersPanel() {
    shaderEditorFocused = false;
    if (focusShaders) { ImGui::SetNextWindowFocus(); focusShaders = false; }
    // Keep unsaved documents visible; switching documents has an explicit save/discard prompt.
    if (!ImGui::Begin(WithIcon(kIconWorkflow, TrId("Shader Graph")).c_str(), &showShaders)) { ImGui::End(); return; }
    shaderEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (ImGui::Button(WithIcon(kIconPlus, Tr("New Shader")).c_str())) OpenShader("");
    ShaderRect("new");
    if (shaderDocument.isNull()) {
        ImGui::TextWrapped("%s", Tr("Double-click a shader or material in Assets, or create a new shader."));
        ImGui::End(); return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonWidth = [&](const std::string& label) { return ImGui::CalcTextSize(label.c_str(), nullptr, true).x + style.FramePadding.x * 2; };
    const bool dirty = shaderDocument != shaderSaved || shaderMaterial != shaderMaterialSaved;
    const std::string saveLabel = WithIcon(kIconSave, Tr("Save Shader")) + (dirty ? " *" : "");
    ImGui::SameLine(); ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - buttonWidth(saveLabel) - style.ItemSpacing.x));
    ImGui::InputTextWithHint("##shaderpath", Tr("Project path ending in .shader.json"), &shaderSavePath);
    ImGui::SetItemTooltip("%s", Tr("Project path ending in .shader.json"));
    ImGui::SameLine();
    if (dirty) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.82f, 0.4f, 1));
    const bool save = ImGui::Button(saveLabel.c_str());
    if (dirty) ImGui::PopStyleColor();
    ShaderRect("save");
    ImGui::SetItemTooltip("%s", dirty ? Tr("Unsaved changes (Ctrl+S)") : "Ctrl+S");
    if (save || (shaderEditorFocused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) SaveShader();

    Json ordered; std::string validation;
    bool valid = OrderEditorShaderGraph(shaderDocument, ordered, &validation);
    const std::string materialLabel = WithIcon(kIconPalette, Tr(shaderExistingMaterial == shaderMaterialPath ? "Save Material" : "Create Material"));
    const std::string applyLabel = WithIcon(kIconCircleCheck, Tr("Apply to Selected Mesh"));
    ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x - buttonWidth(materialLabel) - buttonWidth(applyLabel) - style.ItemSpacing.x * 2));
    ImGui::InputTextWithHint("##materialpath", Tr("Material path"), &shaderMaterialPath);
    ImGui::SetItemTooltip("%s", Tr("Material path"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button(materialLabel.c_str())) SaveShaderMaterial();
    ShaderRect("save-material");
    ImGui::EndDisabled(); ImGui::SameLine();
    bool mesh = selectedId != kNullEntity && selected["components"].has("MeshRenderer");
    ImGui::BeginDisabled(!valid || !mesh);
    if (ImGui::Button(applyLabel.c_str())) {
        if (SaveShaderMaterial()) Call("component.set", ObjectOf({{"id", Json(selectedId)}, {"type", "MeshRenderer"},
            {"values", ObjectOf({{"material", shaderExistingMaterial}, {"shaderUniforms", Json::MakeObject()}})}}));
    }
    ShaderRect("apply");
    ImGui::EndDisabled();
    if (!mesh) ImGui::SetItemTooltip("%s", Tr("Select an entity with a MeshRenderer first"));

    if (ImGui::Button(WithIcon(kIconPlus, Tr("Add Node")).c_str())) shaderPaletteAtMouse = true;
    ShaderRect("add");
    ImGui::SetItemTooltip("%s", Tr("Also: right-click or double-click the canvas"));
    ImGui::SameLine(); if (ImGui::Button(WithIcon(kIconLayoutGrid, Tr("Arrange Nodes")).c_str())) ShaderArrange();
    ShaderRect("arrange");
    ImGui::SameLine(); if (ImGui::Button(WithIcon(kIconSquareDashed, Tr("Fit Graph")).c_str())) shaderFit = true;
    ShaderRect("fit");
    auto history = [&](bool redo) {
        if (redo ? shaderHistoryIndex + 1 < shaderHistory.size() : shaderHistoryIndex > 0) {
            shaderHistoryIndex = redo ? shaderHistoryIndex + 1 : shaderHistoryIndex - 1;
            shaderDocument = shaderHistory[shaderHistoryIndex]; shaderSelected = -1; shaderLink = -1;
            if (shaderPositions.size() != shaderDocument["nodes"].size()) ShaderArrange();
        }
    };
    ImGui::SameLine(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine(); ImGui::BeginDisabled(shaderHistoryIndex == 0);
    if (ImGui::Button(Tr("Undo"))) history(false); ShaderRect("undo"); ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Z");
    ImGui::SameLine(); ImGui::BeginDisabled(shaderHistoryIndex + 1 == shaderHistory.size());
    if (ImGui::Button(Tr("Redo"))) history(true); ShaderRect("redo"); ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Y");
    if (shaderEditorFocused && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) history(false);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) history(true);
        const int count = static_cast<int>(shaderDocument["nodes"].size());
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && shaderSelected >= 0 && shaderSelected < count) {
            RemoveEditorShaderNode(shaderDocument, shaderSelected);
            shaderPositions.erase(shaderPositions.begin() + shaderSelected); shaderSelected = -1; shaderLink = -1; ShaderRecord();
        }
    }
    // The state of the graph, always in view: what stops it from being saved, or how much room is left.
    ImGui::SameLine(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (valid) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "%s %s", kIconCircleCheck, Tr("No problems"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Format(Tr("%d / 48 nodes"), static_cast<int>(shaderDocument["nodes"].size())).c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5f, 0.42f, 1));
        const std::string problem = std::string(kIconTriangleAlert) + " " + validation;
        ImGui::PushTextWrapPos(0);  // one line; the whole text is in the tooltip and in the properties panel
        ImGui::TextUnformatted(problem.c_str(), problem.c_str() + std::min<size_t>(problem.size(), problem.find('\n')));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", validation.c_str());
    }
    float properties = std::min(ImGui::GetFontSize() * 21.0f, ImGui::GetContentRegionAvail().x * 0.4f);
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
