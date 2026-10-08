#include "editor/EditorInternal.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include "editor/EditorNodeCanvas.h"
#include "editor/EditorVfxGraph.h"

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "editor/EditorText.h"
#include "render/GpuRenderer.h"
#include "render/Mesh.h"
#include "render/RenderScene.h"
#include "scene/Vfx.h"
#include "scene/VfxExpression.h"

namespace oe {
namespace {
void SameLineIfFits(const char* label, bool checkbox = false) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float width = ImGui::CalcTextSize(label).x + (checkbox ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x :
        ImGui::GetStyle().FramePadding.x * 2);
    if (right - ImGui::GetItemRectMax().x >= width + ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine();
}

bool ValueWidget(const Json& field, Json& value) {
    const std::string type = field["type"].asString();
    if (type == "bool") {
        bool v = value.asBool(); if (ImGui::Checkbox("##value", &v)) { value = v; return true; }
    } else if (type == "float" || type == "int") {
        const float low = field["min"].asFloat(), high = field["max"].asFloat();
        if (type == "int") {
            int v = value.asInt();
            if (ImGui::DragInt("##value", &v, 0.2f, static_cast<int>(low), static_cast<int>(high), "%d", ImGuiSliderFlags_AlwaysClamp)) {
                value = v; return true;
            }
        } else {
            float v = value.asFloat();
            if (ImGui::DragFloat("##value", &v, 0.02f, low, high, "%.3g", ImGuiSliderFlags_AlwaysClamp)) { value = v; return true; }
        }
    } else if (type == "vec3" || type == "color") {
        float v[3] = {value[0].asFloat(), value[1].asFloat(), value[2].asFloat()};
        if (type == "color" && value.isString()) {
            ParticleCurve color; std::string error;
            if (ParseParticleCurve(Json(Json::Array{Json(Json::Array{0, value})}), true, color, &error))
                for (int axis = 0; axis < 3; ++axis) v[axis] = axis == 0 ? color.values[0].x : axis == 1 ? color.values[0].y : color.values[0].z;
        }
        bool changed = type == "color" ? ImGui::ColorEdit3("##value", v) : ImGui::DragFloat3("##value", v, 0.02f);
        if (changed) { value = Json(Json::Array{v[0], v[1], v[2]}); return true; }
    } else if (field["options"].size()) {
        bool changed = false;
        if (ImGui::BeginCombo("##value", value.asString().c_str())) {
            for (const Json& option : field["options"].items()) if (ImGui::Selectable(option.asString().c_str(), option == value)) {
                value = option; changed = true;
            }
            ImGui::EndCombo();
        }
        return changed;
    } else if (type == "json") {
        std::string text = value.dump();
        if (ImGui::InputText("##value", &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string error; Json parsed = Json::parse(text, &error);
            if (error.empty()) { value = parsed; return true; }
        }
    } else if (type == "string") {
        std::string text = value.asString();
        if (ImGui::InputText("##value", &text, ImGuiInputTextFlags_EnterReturnsTrue)) { value = text; return true; }
    }
    return false;
}

Json ParameterField(const Json& value, const Json& spec, const Json& document, const Json& schema) {
    Json field = Json::MakeObject();
    field["type"] = value.isBool() ? "bool" : value.isNumber() ? "float" : value.isString() ? "string" : "json";
    if (value.isArray() && value.size()==3) field["type"]="vec3";
    const std::string target = spec["targets"].size()>0 ? spec["targets"][0]["to"].asString() : std::string();
    const size_t separator = target.find('.');
    if (separator != std::string::npos && !value.isNumber()) {
        const std::string block = target.substr(0, separator), property = target.substr(separator + 1);
        for (int kind = 0; kind < 2; ++kind) for (const Json& entry : document[kind ? "trails" : "emitters"].items()) {
            if (entry["name"].asString() != block) continue;
            for (const Json& candidate : schema["components"][kind]["fields"].items())
                if (candidate["name"].asString() == property) field = candidate;
        }
    }
    if (spec.has("min")) field["min"] = spec["min"];
    if (spec.has("max")) field["max"] = spec["max"];
    return field;
}

bool CurveWidget(NativeEditor::Impl& editor, const std::string& name, Json& keys, const Json& start, const Json& end, bool signedValue = false) {
    const bool color = name == "colorOverLife";
    bool changed = false;
    if (keys.size() == 0) {
        ImGui::TextDisabled("%s", Tr("Using start/end values"));
        if (ImGui::Button(Tr("Create Curve"))) {
            keys = Json(Json::Array{Json(Json::Array{0, start}), Json(Json::Array{1, end})}); changed = true;
        }
        editor.EffectsRect("create:" + name);
        return changed;
    }
    ParticleCurve curve; std::string error;
    if (!ParseParticleCurve(keys, color, curve, &error)) { ImGui::TextWrapped("%s", error.c_str()); return false; }
    const ImVec2 origin = ImGui::GetCursorScreenPos(), size(std::max(120.0f, ImGui::GetContentRegionAvail().x), color ? 48.0f : 96.0f);
    ImGui::InvisibleButton("##curvegraph", size); editor.EffectsRect("curve:" + name);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, origin + size, IM_COL32(22, 27, 36, 255), 3);
    float maximum = name == "opacityOverLife" ? 1 : 0.1f;
    for (const Json& key : keys.items()) if (!color) maximum = std::max(maximum, (signedValue ? std::abs(key[1].asFloat()) : key[1].asFloat()) * 1.2f);
    if (ImGui::IsItemActivated()) editor.effectsCurveRange = maximum;
    if (ImGui::IsItemActive()) maximum = editor.effectsCurveRange;
    auto normalized = [&](float v) { return signedValue ? (v / maximum + 1) * 0.5f : v / maximum; };
    if (color) {
        for (int pixel = 0; pixel < static_cast<int>(size.x); ++pixel) {
            const Vec3 c = curve.Sample(static_cast<float>(pixel) / size.x);
            const ImU32 rgb = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 1));
            draw->AddLine(origin + ImVec2(static_cast<float>(pixel), 0), origin + ImVec2(static_cast<float>(pixel), size.y - 10), rgb);
        }
    } else {
        for (int grid = 1; grid < 4; ++grid)
            draw->AddLine(origin + ImVec2(0, size.y * grid / 4), origin + ImVec2(size.x, size.y * grid / 4), IM_COL32(45, 51, 64, 255));
        for (int pixel = 1; pixel < static_cast<int>(size.x); ++pixel) {
            const float a = normalized(curve.Sample(static_cast<float>(pixel - 1) / size.x).x);
            const float b = normalized(curve.Sample(static_cast<float>(pixel) / size.x).x);
            draw->AddLine(origin + ImVec2(static_cast<float>(pixel - 1), size.y * (1 - a)),
                          origin + ImVec2(static_cast<float>(pixel), size.y * (1 - b)), IM_COL32(107, 203, 247, 255), 2);
        }
    }
    const int count = static_cast<int>(keys.size());
    int& selectedKey = editor.effectsCurveKeys[std::to_string(editor.effectsBlock) + name];
    selectedKey = std::max(0, std::min(count - 1, selectedKey));
    if (ImGui::IsItemActivated()) {
        editor.effectsCurveRange = maximum;
        const float t = Clamp((ImGui::GetIO().MousePos.x - origin.x) / size.x, 0, 1);
        float nearest = 2;
        for (int key = 0; key < count; ++key) if (std::abs(keys[key][0].asFloat() - t) < nearest) {
            nearest = std::abs(keys[key][0].asFloat() - t); selectedKey = key;
        }
    }
    if (ImGui::IsItemActive()) maximum = editor.effectsCurveRange;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const int key = selectedKey;
        const float low = key > 0 ? keys[key - 1][0].asFloat() + 0.001f : 0;
        const float high = key + 1 < count ? keys[key + 1][0].asFloat() - 0.001f : 1;
        if (low <= high) keys[key][0] = Clamp((ImGui::GetIO().MousePos.x - origin.x) / size.x, low, high);
        if (!color) keys[key][1] = Clamp(((1 - (ImGui::GetIO().MousePos.y - origin.y) / size.y) * (signedValue ? 2 : 1) - (signedValue ? 1 : 0)) * maximum, signedValue ? -maximum : 0, maximum);
        changed = true;
    }
    editor.EffectsMerge("curve:" + name);
    for (int key = 0; key < count; ++key) {
        const ImVec2 point = origin + ImVec2(keys[key][0].asFloat() * size.x, color ? size.y - 5 : size.y * (1 - normalized(keys[key][1].asFloat())));
        draw->AddCircleFilled(point, 5, key == selectedKey ? IM_COL32(255, 211, 95, 255) : IM_COL32(219, 232, 243, 255));
    }
    ImGui::TextDisabled("0                         1");
    ImGui::SetNextItemWidth(65);
    int selected = selectedKey;
    if (ImGui::SliderInt(Tr("Key"), &selected, 0, count - 1)) selectedKey = selected;
    const int key = selectedKey;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (color) {
        ImGui::TextUnformatted(Tr("Key Color")); ImGui::SetNextItemWidth(-FLT_MIN);
        float rgb[3] = {curve.values[key].x, curve.values[key].y, curve.values[key].z};
        if (ImGui::ColorEdit3("##keycolor", rgb)) { keys[key][1] = Json(Json::Array{rgb[0], rgb[1], rgb[2]}); changed = true; }
    } else {
        ImGui::TextUnformatted(Tr("Key Value")); ImGui::SetNextItemWidth(-FLT_MIN);
        float value = keys[key][1].asFloat();
        if (ImGui::DragFloat("##keyvalue", &value, 0.01f, signedValue ? -65504.0f : 0, name == "opacityOverLife" ? 1.0f : 65504.0f,
                             "%.3g", ImGuiSliderFlags_AlwaysClamp)) { keys[key][1] = value; changed = true; }
    }
    editor.EffectsRect("key:" + name);
    editor.EffectsMerge("key:" + name);
    ImGui::BeginDisabled(count >= 16);
    if (ImGui::Button(Tr("Add Key"))) {
        float t = 0.5f, gap = -1;
        for (int i = 1; i < count; ++i) if (keys[i][0].asFloat() - keys[i - 1][0].asFloat() > gap) {
            gap = keys[i][0].asFloat() - keys[i - 1][0].asFloat(); t = (keys[i][0].asFloat() + keys[i - 1][0].asFloat()) * 0.5f;
        }
        if (count == 1) t = keys[0][0].asFloat() > 0.5f ? 0.0f : 1.0f;
        const Vec3 sample = curve.Sample(t);
        keys.push(Json(Json::Array{t, color ? Json(Json::Array{sample.x, sample.y, sample.z}) : Json(sample.x)}));
        std::sort(keys.items().begin(), keys.items().end(), [](const Json& a, const Json& b) { return a[0].asFloat() < b[0].asFloat(); });
        changed = true;
    }
    editor.EffectsRect("add:" + name); editor.EffectsMerge("add:" + name); ImGui::EndDisabled(); ImGui::SameLine();
    if (ImGui::Button(Tr("Remove Key"))) { keys.items().erase(keys.items().begin() + key); changed = true; }
    editor.EffectsRect("remove:" + name); editor.EffectsMerge("remove:" + name); ImGui::SameLine();
    const bool vectorCurve = name == "positionOverLife" || name == "velocityOverLife" || name == "rotationOverLife" || name == "scaleOverLife";
    if (ImGui::Button(Tr(vectorCurve || signedValue ? "Use Birth Values" : "Use Start/End"))) { keys = Json::MakeArray(); changed = true; }
    return changed;
}

bool ExpressionWidget(NativeEditor::Impl& editor,const std::string& block,Json& expressions) {
    int& stage=editor.effectsCurveKeys["expression:stage"];
    int& channel=editor.effectsCurveKeys["expression:channel"];
    const char* stages[]={"birth","update"};const auto& names=VfxChannelNames();
    ImGui::TextUnformatted(Tr("Expression Stage"));ImGui::SetNextItemWidth(-FLT_MIN);
    if(ImGui::BeginCombo("##expressionstage",Tr(stage?"Update":"Birth"))) {
        for(int i=0;i<2;++i) if(ImGui::Selectable(Tr(i?"Update":"Birth"),stage==i)) stage=i;
        ImGui::EndCombo();
    }
    editor.EffectsRect("expression-stage");
    ImGui::TextUnformatted(Tr("Channel"));ImGui::SetNextItemWidth(-FLT_MIN);
    if(ImGui::BeginCombo("##expressionchannel",names[static_cast<size_t>(channel)])) {
        for(size_t i=0;i<names.size();++i) {
            if(ImGui::Selectable(names[i],channel==static_cast<int>(i))) channel=static_cast<int>(i);
            editor.EffectsRect(std::string("expression-channel:")+names[i]);
        }
        ImGui::EndCombo();
    }
    editor.EffectsRect("expression-channel");
    const char* name=names[static_cast<size_t>(channel)];
    const std::string id=editor.effectsPath+":"+block+":"+stages[stage]+":"+name;
    const std::string canonical=static_cast<const Json&>(expressions)[stages[stage]][name].asString();
    auto found=editor.effectsExpressionText.find(id);
    if(found==editor.effectsExpressionText.end()) found=editor.effectsExpressionText.emplace(id,std::make_pair(canonical,canonical)).first;
    auto& text=found->second;
    if(text.first!=canonical) text=std::make_pair(canonical,canonical);
    ImGui::TextDisabled("%s",Tr(channel<5?"Vector expression (vec3)":"Scalar expression"));
    ImGui::InputTextMultiline("##expression",&text.second,ImVec2(-FLT_MIN,110));editor.EffectsRect("expression-source");
    bool changed=false;
    if(ImGui::Button(Tr("Apply Expression"))) {
        if(text.second.empty()) {expressions[stages[stage]].erase(name);}
        else expressions[stages[stage]][name]=text.second;
        changed=true;
    }
    editor.EffectsRect("expression-apply");ImGui::SameLine();
    if(ImGui::Button(Tr("Clear Expression"))) {expressions[stages[stage]].erase(name);text.second.clear();changed=true;}
    editor.EffectsRect("expression-clear");
    ImGui::TextWrapped("%s",Tr("Expressions use normalized age, stable random and named parameters. See vfx.schema for inputs and snippets."));
    return changed;
}

bool VectorCurveWidget(NativeEditor::Impl& editor, const std::string& name, Json& keys) {
    bool changed = false;
    const bool scale = name == "scaleOverLife";
    if (keys.size() == 0) {
        ImGui::TextDisabled("%s", Tr("Using birth values"));
        if (ImGui::Button(Tr("Create Curve"))) {
            const Json value(Json::Array{scale ? 1 : 0, scale ? 1 : 0, scale ? 1 : 0});
            keys = Json(Json::Array{Json(Json::Array{0, value}), Json(Json::Array{1, value})});
            changed = true;
        }
        editor.EffectsRect("create:" + name); return changed;
    }
    ParticleCurve vector; std::string error;
    if (!ParseParticleVectorCurve(keys, vector, &error)) { ImGui::TextWrapped("%s", error.c_str()); return false; }
    int& axis = editor.effectsCurveKeys["axis:" + name];
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(i == 0 ? "X" : i == 1 ? "Y" : "Z", axis == i)) axis = i;
        editor.EffectsRect("axis:" + name + ":" + std::to_string(i));
    }
    Json scalar = Json::MakeArray();
    for (const Json& key : keys.items()) scalar.push(Json(Json::Array{key[0], key[1][axis]}));
    if (CurveWidget(editor, name, scalar, scale ? Json(1) : Json(0), scale ? Json(1) : Json(0), !scale)) {
        Json next = Json::MakeArray();
        for (const Json& key : scalar.items()) {
            const Vec3 sampled = vector.Sample(key[0].asFloat());
            Json value(Json::Array{sampled.x, sampled.y, sampled.z}); value[axis] = key[1];
            next.push(Json(Json::Array{key[0], value}));
        }
        keys = next; changed = true;
    }
    return changed;
}
}

void NativeEditor::Impl::EffectsRect(const std::string& name) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    effectsRects[name] = {a.x, a.y, b.x - a.x, b.y - a.y};
}

std::string NativeEditor::Impl::EffectsMerge(const std::string& control) {
    if (ImGui::IsItemActivated()) { ++effectsDragSerial; }
    return "effects:" + control + ":" + std::to_string(effectsDragSerial);
}

void NativeEditor::Impl::EffectsReplace(const Json& document, const std::string& merge) {
    Json args = ObjectOf({{"vfx", document}}); if (!merge.empty()) args["merge"] = merge;
    Json result = Call("vfx.edit_replace", args);
    if (Ok(result)) effectsError.clear(); else effectsError = result["error"]["message"].asString() + "\n" + result["error"]["hint"].asString();
}

void NativeEditor::Impl::OpenEffect(const std::string& path, bool create) {
    showEffects = focusEffects = true;
    if (path.empty()) return;
    Json result = Call("vfx.edit", ObjectOf({{"path", path}, {"new", create}, {"preset", effectsPreset}}), true);
    if (!Ok(result)) {
        if (result["error"]["code"].asString() == "unsaved_vfx") {
            effectsPendingPath = path; effectsPendingNew = create; effectsPrompt = true;
        } else Notify(result["error"]["message"].asString(), true);
    } else { effectsBlock = 0; effectsCurveKeys.clear(); effectsError.clear(); }
}

bool NativeEditor::Impl::EffectsSave() {
    if (!Ok(Call("vfx.save", Json()))) return false;
    RefreshAssets(true); Notify(Format(Tr("Saved %s"), effectsPath.c_str())); return true;
}

void NativeEditor::Impl::ResetEffectsPreview() {
    effectsScene = std::make_unique<Scene>(); effectsRoot = effectsScene->Create("Preview");
    effectsScene->Add<Transform>(effectsRoot);
    auto& system = effectsScene->Add<VFXSystem>(effectsRoot);
    system.definition = std::make_shared<VfxAsset>(ParseVfxAsset(effectsDocument));
    ConfigureVfx(*system.definition, Json::MakeObject(), system.emitters, system.trails);
    effectsFrame = 0; effectsAccumulator = 0; effectsLastTime = time;
}

void NativeEditor::Impl::EffectsPreview(bool compact) {
    ImGui::TextUnformatted(Tr("Isolated Preview"));
    if (ImGui::Button(Tr(effectsPlaying ? "Pause" : "Play"))) effectsPlaying = !effectsPlaying;
    EffectsRect("play"); SameLineIfFits(Tr("Restart"));
    if (ImGui::Button(Tr("Restart"))) ResetEffectsPreview(); EffectsRect("restart"); SameLineIfFits(Tr("Loop Preview"), true);
    ImGui::Checkbox(Tr("Loop Preview"), &effectsLoop); EffectsRect("loop");
    if (!compact) {
        if (ImGui::Checkbox(Tr("Preview Motion"), &effectsMotion)) ResetEffectsPreview(); EffectsRect("motion");
        ImGui::TextUnformatted(Tr("Timeline"));
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    int frame = effectsFrame;
    const bool scrub = ImGui::SliderInt("##timeline", &frame, 0, effectsCycleFrames, "%d", ImGuiSliderFlags_AlwaysClamp);
    EffectsRect("timeline");
    const int cap = effectsScene->Get<VFXSystem>(effectsRoot)->definition->maxParticles;
    // One scrub is bounded by the headless authoring work limit. Playback advances at most eight steps per frame.
    uint64_t work = static_cast<uint64_t>(cap);
    for (const auto& trail : effectsScene->Get<VFXSystem>(effectsRoot)->trails) work += static_cast<uint64_t>(trail.maxPoints);
    uint64_t expressionWork=0;
    for (const auto& emitter : effectsScene->Get<VFXSystem>(effectsRoot)->emitters) expressionWork+=VfxExpressionWork(emitter);
    const int maxFrame = std::min({effectsCycleFrames, static_cast<int>(20000000 / std::max<uint64_t>(1, work)),
        static_cast<int>(100000000 / std::max<uint64_t>(1,expressionWork))});
    auto step = [&] {
        ++effectsFrame;
        effectsScene->Get<Transform>(effectsRoot)->position = effectsMotion ? Vec3(std::sin(effectsFrame / 30.0f) * 1.5f, 0, 0) : Vec3();
        InputState input; UpdateSystems(*effectsScene, input, static_cast<float>(Engine::kFixedDt));
        UpdateLateSystems(*effectsScene, static_cast<float>(Engine::kFixedDt));
    };
    if (scrub) {
        effectsPlaying = false; frame = std::min(frame, maxFrame);
        if (frame < effectsFrame) ResetEffectsPreview();
        while (effectsFrame < frame) step();
    } else if (effectsPlaying) {
        effectsAccumulator += std::max(0.0, std::min(time - effectsLastTime, 0.1));
        int steps = 0;
        while (effectsAccumulator >= Engine::kFixedDt && steps++ < 8) {
            effectsAccumulator -= Engine::kFixedDt;
            if (effectsFrame >= effectsCycleFrames) {
                if (effectsLoop) ResetEffectsPreview(); else { effectsPlaying = false; break; }
            }
            step();
        }
    }
    effectsLastTime = time;
    if (!compact) {
        const auto& system = *effectsScene->Get<VFXSystem>(effectsRoot);
        size_t count = 0, points = 0;
        for (const auto& emitter : system.emitters) count += emitter.particles.size();
        for (const auto& trail : system.trails) points += trail.points.size();
        ImGui::Text(Tr("Particles: %d / %d   Trail points: %d"), static_cast<int>(count), cap, static_cast<int>(points));
        ImGui::ProgressBar(cap ? static_cast<float>(count) / cap : 0, ImVec2(-FLT_MIN, 5), "");
        ImGui::TextDisabled(Tr("Time: %.2f s"), effectsFrame * Engine::kFixedDt);
        if (ImGui::Button(Tr("Fit Preview"))) {
            Vec3 low(1e30f, 1e30f, 1e30f), high(-1e30f, -1e30f, -1e30f); bool found = false;
            for (const RenderItem& item : GatherRenderItems(*effectsScene, &engine.Assets(), Mat4{})) for (int corner = 0; corner < 8; ++corner) {
                const Vec3 p = item.world.TransformPoint(Vec3((corner & 1) ? item.boundsMax.x : item.boundsMin.x,
                    (corner & 2) ? item.boundsMax.y : item.boundsMin.y, (corner & 4) ? item.boundsMax.z : item.boundsMin.z));
                low = Vec3(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
                high = Vec3(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)); found = true;
            }
            if (found) { effectsTarget = (low + high) * 0.5f; effectsZoom = Clamp(Length(high - low) * 1.75f, 0.5f, 30); }
        }
        EffectsRect("fit");
        ImGui::TextUnformatted(Tr("Preview Distance"));
        ImGui::SetNextItemWidth(-FLT_MIN); ImGui::SliderFloat("##distance", &effectsZoom, 0.5f, 30, "%.1f");
    }
    const float previewWidth = compact ? std::max(64.0f, std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 4)) :
        std::max(120.0f, ImGui::GetContentRegionAvail().x);
    const int pixels = std::max(128, std::min(384, static_cast<int>(previewWidth)));
    RenderTarget target; target.Resize(pixels, pixels);
    RenderView view = MakeLookAtView(effectsTarget + Vec3(0, 0, effectsZoom), effectsTarget, 45, 1);
    view.drawUI = false; view.clearColor = Color(0.025f, 0.032f, 0.048f); view.shaderTime = effectsFrame / 60.0f;
    engine.Renderer().Render(*effectsScene, view, target);
    auto image = std::make_shared<Texture>(); image->width = pixels; image->height = pixels; image->texels = std::move(target.color);
    effectsImage = image;
    ImGui::Image(ViewTexture(engine.Gpu()->ImageView(effectsImage)), ImVec2(previewWidth, previewWidth)); EffectsRect("preview");
    if (!compact) ImGui::TextWrapped("%s", Tr("Preview is independent of the game. Motion moves the root for trails; changes restart the preview."));
}

void NativeEditor::Impl::EffectsParameterValue(const std::string& name, float fieldWidth) {
    Json document = effectsDocument;
    const Json& spec = document["params"][name];
    Json value = spec["default"];
    ImGui::SetNextItemWidth(fieldWidth > 0 ? fieldWidth : -FLT_MIN);
    const bool changed = ValueWidget(ParameterField(value, spec, document, effectsSchema), value);
    EffectsRect("param:" + name); const std::string merge = EffectsMerge("param:" + name);
    if (changed) { document["params"][name]["default"] = value; EffectsReplace(document, merge); }
}

void NativeEditor::Impl::EffectsInspector() {
    Json document = effectsDocument;
    ImGui::TextUnformatted(Tr("Parameters"));
    for (const auto& parameter : document["params"].members()) {
        ImGui::PushID(parameter.first.c_str()); ImGui::TextUnformatted(parameter.first.c_str());
        EffectsParameterValue(parameter.first);
        HelpTooltip(parameter.second["doc"].asString());
        ImGui::PopID();
    }
    if (ImGui::TreeNode(Tr("Parameter Definitions"))) {
        std::string json = document["params"].dump(2);
        // Text remains in ImGui's edit state until Enter; typed controls above are the common path.
        if (ImGui::InputTextMultiline("##paramsjson", &json, ImVec2(-FLT_MIN, 120), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine)) {
            std::string error; Json params = Json::parse(json, &error);
            if (error.empty()) { document["params"] = params; EffectsReplace(document); }
            else effectsError = error;
        }
        ImGui::TextWrapped("%s", Tr("Enter applies definitions. Targets use block.field; numeric targets accept scale."));
        ImGui::TreePop();
    }
    int budget = document["budget"]["maxParticles"].asInt(10000);
    ImGui::TextUnformatted(Tr("System Budget")); ImGui::SetNextItemWidth(-FLT_MIN);
    const bool budgetChanged = ImGui::DragInt("##budget", &budget, 1, 0, 40000, "%d", ImGuiSliderFlags_AlwaysClamp);
    EffectsRect("budget"); const std::string budgetMerge = EffectsMerge("budget");
    if (budgetChanged) { document["budget"]["maxParticles"] = budget; EffectsReplace(document, budgetMerge); }
    const int emitterCount = static_cast<int>(document["emitters"].size()), total = emitterCount + static_cast<int>(document["trails"].size());
    if (total == 0) return;
    effectsBlock = std::max(0, std::min(total - 1, effectsBlock));
    const bool trail = effectsBlock >= emitterCount; const int index = trail ? effectsBlock - emitterCount : effectsBlock;
    const char* array = trail ? "trails" : "emitters";
    const std::string blockName = document[array][index]["name"].asString();
    ImGui::SeparatorText(blockName.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string moduleLabel = effectsSchema["modules"][effectsModule].asString();
    const bool modulesOpen = ImGui::BeginCombo("##modules", Tr(moduleLabel.c_str()));
    EffectsRect("module");
    if (modulesOpen) {
        for (size_t i = 0; i < effectsSchema["modules"].size(); ++i) {
            const std::string module = effectsSchema["modules"][i].asString();
            if (ImGui::Selectable(Tr(module.c_str()), effectsModule == static_cast<int>(i))) effectsModule = static_cast<int>(i);
            EffectsRect("module:" + module);
        }
        ImGui::EndCombo();
    }
    EffectsBlockFields();
}

void NativeEditor::Impl::EffectsBlockFields() {
    const Json document = effectsDocument;
    const int emitterCount = static_cast<int>(document["emitters"].size());
    const bool trail = effectsBlock >= emitterCount;
    const int index = trail ? effectsBlock - emitterCount : effectsBlock;
    const char* array = trail ? "trails" : "emitters", *key = trail ? "trail" : "emitter";
    const std::string blockName = document[array][index]["name"].asString();
    const Json& values = document[array][index][key];
    const Json& component = effectsSchema["components"][trail ? 1 : 0];
    for (const Json& module : effectsSchema["modules"].items()) {
        if (module != effectsSchema["modules"][effectsModule]) continue;
        ImGui::PushID(module.asString().c_str());
        if (ImGui::CollapsingHeader(Tr(module.asString().c_str()), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const Json& field : component["fields"].items()) {
                if (field["module"] != module) continue;
                const std::string name = field["name"].asString();
                bool mapped = false;
                for (const auto& parameter : document["params"].members()) for (const Json& target : parameter.second["targets"].items())
                    if (target["to"].asString() == blockName + "." + name) mapped = true;
                static const std::map<std::string,const char*> labels = {{"material","Material"},{"mesh","Particle Mesh"},
                    {"meshScale","Mesh Scale"},{"batching","Batch Particles"},{"onDeath","Death Emitter"},{"deathCount","Death Burst"},
                    {"mode","Ribbon Mode"},{"baseAnchor","Base Anchor"},{"tipAnchor","Tip Anchor"},
                    {"baseOffset","Base Offset"},{"tipOffset","Tip Offset"},
                    {"positionOffset","Birth Offset"},{"rotation3D","Birth Rotation"},{"scale","Birth Scale"},{"pivot","Particle Pivot"},
                    {"positionOverLife","Position Curve"},{"velocityOverLife","Velocity Curve"},{"rotationOverLife","Rotation Curve"},{"scaleOverLife","Axis Scale Curve"}};
                const auto label = labels.find(name);
                ImGui::PushID(name.c_str()); ImGui::TextUnformatted(label == labels.end() ? name.c_str() : Tr(label->second));
                if (mapped) { ImGui::SameLine(); ImGui::TextDisabled("%s", Tr("Controlled by parameter")); }
                HelpTooltip(field["doc"].asString());
                Json value = values.has(name) ? values[name] : field["default"];
                ImGui::SetNextItemWidth(-FLT_MIN); ImGui::BeginDisabled(mapped);
                bool changed = false;
                if (name == "expressions") changed=ExpressionWidget(*this,blockName,value);
                else if (name == "sizeOverLife" || name == "colorOverLife" || name == "opacityOverLife") {
                    const std::string suffix = name == "sizeOverLife" ? "Size" : name == "colorOverLife" ? "Color" : "Opacity";
                    auto base = [&](const std::string& property) {
                        if (values.has(property)) return values[property];
                        for (const Json& f : component["fields"].items()) if (f["name"].asString() == property) return f["default"];
                        return Json();
                    };
                    changed = CurveWidget(*this, name, value, base("start" + suffix), base("end" + suffix));
                } else if (name == "positionOverLife" || name == "velocityOverLife" || name == "rotationOverLife" || name == "scaleOverLife") {
                    changed = VectorCurveWidget(*this, name, value);
                } else if (name.compare(0, 6, "custom") == 0 && name.find("OverLife") != std::string::npos) {
                    const std::string birth = name.substr(0, 7);
                    changed = CurveWidget(*this, name, value, values.has(birth) ? values[birth] : Json(0), values.has(birth) ? values[birth] : Json(0), true);
                } else changed = ValueWidget(field, value);
                EffectsRect("field:" + name); const std::string merge = name == "expressions" ? std::string() : EffectsMerge(blockName + "." + name);
                ImGui::EndDisabled();
                if (changed) {
                    Json patch = ObjectOf({{name.c_str(), value}});
                    Json result = Call("vfx.set", ObjectOf({{"path", effectsPath}, {key, blockName}, {"values", patch}, {"merge", merge}}));
                    if (Ok(result)) effectsError.clear(); else effectsError = result["error"]["message"].asString();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
}

void NativeEditor::Impl::EffectsPanel() {
    effectsEditorFocused = false; effectsRects.clear();
    effectsState = Call("vfx.edit_state", Json(), true)["result"];
    if (effectsState["document"] != effectsDocument || effectsState["path"].asString() != effectsPath) {
        if (effectsState["path"].asString() != effectsPath) {
            if (effectsLayoutDirty) EffectsGraphLayout(true);
            effectsExpressionText.clear();
            effectsValueKey.clear(); effectsValueGraph = Json(); effectsValueSelected = -1; effectsValueLink = -1;
            effectsValuePan = ImVec2(24,24); effectsValueZoom = 0.8f;
            effectsLayoutPath.clear(); effectsGraphPositions.clear(); effectsGraphModules.clear(); effectsGraphMaterials.clear(); effectsGraphAnchors.clear();
            effectsGraphLink.clear(); effectsGraphHint.clear(); effectsGraphSelected.clear();
            effectsGraphPan = ImVec2(24, 24); effectsGraphZoom = 0.8f;
        }
        effectsGraphHint.clear(); effectsGraphErrorNode.clear();
        effectsDocument = effectsState["document"]; effectsPath = effectsState["path"].asString();
        if (!effectsDocument.isNull()) {
            try { ResetEffectsPreview(); } catch (const std::exception& error) { effectsError = error.what(); effectsScene.reset(); }
        } else { effectsScene.reset(); effectsImage.reset(); }
    }
    if (effectsSchema.isNull()) effectsSchema = Call("vfx.schema", Json())["result"];
    if (effectsPresets.isNull()) effectsPresets = Call("vfx.presets", Json())["result"];
    if (focusEffects) { ImGui::SetNextWindowFocus(); focusEffects = false; }
    bool open = true;
    const bool visible = ImGui::Begin(TrId("Effects").c_str(), &open);
    if (!open) {
        if (effectsState["dirty"].asBool()) { effectsPendingClose = true; effectsPrompt = true; }
        else { Call("vfx.close", Json(), true); showEffects = false; }
    }
    if (!visible || !open) { ImGui::End(); return; }
    effectsEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (ImGui::Button(Tr("New Effect"))) ImGui::OpenPopup("##newvfx"); EffectsRect("new");
    if (ImGui::BeginPopup("##newvfx")) {
        ImGui::InputText(Tr("Asset Path"), &effectsNewPath);
        if (ImGui::BeginCombo(Tr("Preset"), effectsPreset.c_str())) {
            for (const Json& preset : effectsPresets.items()) if (ImGui::Selectable(preset["name"].asString().c_str(), effectsPreset == preset["name"].asString()))
                effectsPreset = preset["name"].asString();
            ImGui::EndCombo();
        }
        if (ImGui::Button(Tr("Create"))) { OpenEffect(effectsNewPath, true); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    SameLineIfFits(Tr("Open Effect"));
    if (ImGui::Button(Tr("Open Effect"))) ImGui::OpenPopup("##effectassets"); EffectsRect("open");
    if (ImGui::BeginPopup("##effectassets")) {
        for (const Json& asset : assets.items()) if (asset["kind"].asString() == "vfx" && ImGui::Selectable(asset["path"].asString().c_str())) {
            OpenEffect(asset["path"].asString()); ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (effectsDocument.isNull()) {
        ImGui::TextWrapped("%s", Tr("Open a VFX asset from Assets or create an effect from a preset.")); ImGui::End(); return;
    }
    SameLineIfFits(Tr("Save Effect")); if (ImGui::Button(Tr("Save Effect"))) EffectsSave(); EffectsRect("save");
    SameLineIfFits(Tr("Undo")); ImGui::BeginDisabled(effectsState["undo"].asInt() == 0);
    if (ImGui::Button(Tr("Undo"))) Call("vfx.undo", Json()); EffectsRect("undo"); ImGui::EndDisabled();
    SameLineIfFits(Tr("Redo")); ImGui::BeginDisabled(effectsState["redo"].asInt() == 0);
    if (ImGui::Button(Tr("Redo"))) Call("vfx.redo", Json()); EffectsRect("redo"); ImGui::EndDisabled();
    SameLineIfFits(Tr("Place in Scene")); ImGui::BeginDisabled(effectsState["dirty"].asBool());
    if (ImGui::Button(Tr("Place in Scene"))) {
        Json result = Call("vfx.spawn", ObjectOf({{"path", effectsPath}, {"position", Vec3Json(cam.target)}, {"autoDestroy", false}}));
        if (Ok(result)) { Refresh(true); SelectOnly(static_cast<EntityId>(result["result"]["id"].asInt())); }
    }
    EffectsRect("place"); ImGui::EndDisabled();
    if (effectsEditorFocused && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) EffectsSave();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) Call("vfx.undo", Json());
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) Call("vfx.redo", Json());
    }
    ImGui::TextWrapped("%s%s", effectsPath.c_str(), effectsState["dirty"].asBool() ? " *" : "");
    if (!effectsError.empty()) ImGui::TextColored(ImVec4(1, 0.55f, 0.45f, 1), "%s", effectsError.c_str());
    const int blockCount = static_cast<int>(effectsDocument["emitters"].size() + effectsDocument["trails"].size());
    effectsBlock = std::max(0, std::min(std::max(0, blockCount - 1), effectsBlock));
    ImGui::TextUnformatted(Tr("Effect Block")); ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##effectblock", effectsDocument["emitters"].size() + effectsDocument["trails"].size() ?
        (effectsBlock < static_cast<int>(effectsDocument["emitters"].size()) ? effectsDocument["emitters"][effectsBlock]["name"] :
            effectsDocument["trails"][effectsBlock - effectsDocument["emitters"].size()]["name"]).asString().c_str() : "")) {
        int index = 0;
        for (const char* list : {"emitters", "trails"}) for (const Json& block : effectsDocument[list].items()) {
            if (ImGui::Selectable(block["name"].asString().c_str(), effectsBlock == index)) effectsBlock = index;
            ++index;
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button(Tr("Add Emitter"))) {
        Json document = effectsDocument; Json block = ObjectOf({{"name", UniqueEditorVfxName(document, "emitter")}, {"emitter", Json::MakeObject()}});
        document["emitters"].push(block); EffectsReplace(document);
    }
    EffectsRect("add-emitter"); SameLineIfFits(Tr("Add Trail"));
    if (ImGui::Button(Tr("Add Trail"))) {
        Json document = effectsDocument; Json block = ObjectOf({{"name", UniqueEditorVfxName(document, "trail")}, {"trail", Json::MakeObject()}});
        document["trails"].push(block); EffectsReplace(document);
    }
    EffectsRect("add-trail");
    SameLineIfFits(Tr("Structure Graph"));
    if(ImGui::Checkbox(Tr("Structure Graph"), &effectsGraph) && effectsGraph) effectsValues = false;
    EffectsRect("graph-toggle");
    SameLineIfFits(Tr("Value Nodes"));
    if(ImGui::Checkbox(Tr("Value Nodes"), &effectsValues)) {
        if(effectsValues) effectsGraph = false;
        effectsValueFit = true;
    }
    EffectsRect("value-toggle");
    if(effectsValues) { EffectsValues(); ImGui::End(); return; }
    if (effectsGraph) {
        EffectsGraph(); ImGui::End(); return;
    }
    const float available = ImGui::GetContentRegionAvail().x;
    const float previewWidth = std::min(310.0f, available * 0.42f);
    ImGui::BeginChild("##effectproperties", ImVec2(std::max(150.0f, available - previewWidth - 8), 0), ImGuiChildFlags_Borders);
    EffectsInspector(); ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("##effectpreview", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (effectsScene) EffectsPreview(); ImGui::EndChild(); ImGui::End();
}

void NativeEditor::Impl::EffectsModals() {
    if (effectsPrompt) { ImGui::OpenPopup(TrId("Save Effect Changes?").c_str()); effectsPrompt = false; }
    if (ImGui::BeginPopupModal(TrId("Save Effect Changes?").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(Tr("Save the current effect before continuing?"));
        bool proceed = false;
        if (ImGui::Button(Tr("Save"))) proceed = EffectsSave(); EffectsRect("prompt:save");
        ImGui::SameLine(); if (ImGui::Button(Tr("Don't Save"))) proceed = true; EffectsRect("prompt:discard");
        ImGui::SameLine(); if (ImGui::Button(Tr("Cancel"))) {
            effectsPendingPath.clear(); effectsPendingClose = false; effectsPendingAction = {}; ImGui::CloseCurrentPopup();
        }
        EffectsRect("prompt:cancel");
        if (proceed) {
            Call("vfx.close", ObjectOf({{"discard", true}}));
            if (!effectsPendingPath.empty()) OpenEffect(effectsPendingPath, effectsPendingNew);
            if (effectsPendingClose) showEffects = false;
            const PendingAction action = effectsPendingAction;
            effectsPendingPath.clear(); effectsPendingClose = false; effectsPendingAction = {};
            ImGui::CloseCurrentPopup();
            if (action.kind != PendingAction::Kind::None) RequestAction(action);
        }
        ImGui::EndPopup();
    }
}
}  // namespace oe
