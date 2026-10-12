#include "editor/EditorInternal.h"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>

#include "app/Engine.h"
#include "editor/EditorNodeCanvas.h"
#include "editor/EditorText.h"
#include "scene/VfxExpression.h"

namespace oe {
namespace {
Json Defaults(const Json& document) {
    Json params=Json::MakeObject();
    for(const auto& entry:document["params"].members()) {
        const Json& value=entry.second["default"];bool numeric=value.isNumber();
        if(value.isArray() && value.size()==3) {numeric=true;for(const Json& axis:value.items()) numeric=numeric && axis.isNumber();}
        if(numeric) params[entry.first]=value;
    }
    return params;
}
const char* Stage(int stage) { return stage?"update":"birth"; }
}
bool NativeEditor::Impl::EffectsValueApply(const Json& candidate,const std::string& merge) {
    const int channel=effectsCurveKeys["expression:channel"],stage=effectsCurveKeys["expression:stage"];
    Json args=ObjectOf({{"graph",candidate},{"type",channel<5?"vector":"scalar"},{"params",Defaults(effectsDocument)}});
    const Json result=Call("vfx.expression_source",args,true);
    if(!Ok(result)) { effectsValueError=result["error"]["message"].asString();return false; }
    Json document=effectsDocument;
    document["emitters"][effectsBlock]["emitter"]["expressions"][Stage(stage)][VfxChannelNames()[static_cast<size_t>(channel)]]=result["result"]["source"];
    args=ObjectOf({{"vfx",document}});if(!merge.empty()) args["merge"]=merge;
    const Json replaced=Call("vfx.edit_replace",args,true);
    if(!Ok(replaced)) { effectsValueError=replaced["error"]["message"].asString();return false; }
    effectsValueGraph=candidate;
    for(size_t i=0;i<effectsValueGraph["nodes"].size();++i) effectsValueGraph["nodes"][i]["type"]=result["result"]["nodeTypes"][i];
    effectsValueSource=result["result"]["source"].asString();effectsValueError.clear();effectsError.clear();return true;
}
std::string NativeEditor::Impl::EffectsValueSync() {
    // The expression that is shown (emitter, stage, channel) as a graph; made again when any of them or the
    // expression's text changes.
    int& stage=effectsCurveKeys["expression:stage"];int& channel=effectsCurveKeys["expression:channel"];
    stage=std::clamp(stage,0,1);channel=std::clamp(channel,0,static_cast<int>(kVfxChannelCount)-1);
    const std::string block=effectsDocument["emitters"][effectsBlock]["name"].asString();
    const std::string prefix="value:"+block+":"+Stage(stage)+":"+VfxChannelNames()[static_cast<size_t>(channel)]+":";
    const std::string canonical=static_cast<const Json&>(effectsDocument)["emitters"][effectsBlock]["emitter"]["expressions"][Stage(stage)][VfxChannelNames()[static_cast<size_t>(channel)]].asString();
    const Json parameterTypes=VfxParameterTypes(Defaults(effectsDocument));
    if(effectsValueKey!=prefix || effectsValueSource!=canonical || effectsValueParamTypes!=parameterTypes) {
        const bool changedChannel=effectsValueKey!=prefix;
        effectsValueKey=prefix;effectsValueSource=canonical;effectsValueSelected=-1;effectsValueLink=-1;
        effectsValueParamTypes=parameterTypes;
        const Json result=Call("vfx.expression_graph",ObjectOf({{"source",canonical.empty()?(channel<5?"vec3(0,0,0)":"0"):canonical},
            {"type",channel<5?"vector":"scalar"},{"params",Defaults(effectsDocument)}}),true);
        effectsValueGraph=Ok(result)?result["result"]["graph"]:Json();
        effectsValueError=Ok(result)?std::string():result["error"]["message"].asString();
        if(changedChannel && !effectsGraphPositions.count(prefix+"output")) effectsValueFit=true;
    }
    return prefix;
}
void NativeEditor::Impl::EffectsValuePalette(const std::string& prefix,ImVec2 spawn) {
    ImGui::BeginDisabled(effectsValueGraph.isNull());
    if(ImGui::Button(Tr("Add Value Node"))) ImGui::OpenPopup("##valuepalette");EffectsRect("value-add");
    if(ImGui::BeginPopup("##valuepalette")) {
        for(const Json& operation:effectsSchema["expressions"]["valueGraph"]["operations"].items()) {
            const std::string op=operation["op"].asString();
            if(ImGui::Selectable(op.c_str()) && effectsValueGraph["nodes"].size()<64) {
                Json node=ObjectOf({{"op",op},{"args",Json::MakeArray()},{"type","scalar"}});
                int arity=operation["arity"].asInt();if(arity<0) arity=5;
                for(int i=0;i<arity;++i) node["args"].push(-1);
                if(op=="constant") node["literal"]="1";
                if(op=="input") node["name"]="age";
                if(op=="parameter") {const Json params=Defaults(effectsDocument);
                    node["name"]=params.members().empty()?"parameter":params.members().front().first;}
                if(op=="component") node["axis"]=0;
                if(op=="vec3") node["type"]="vector";
                effectsValueSelected=static_cast<int>(effectsValueGraph["nodes"].size());effectsValueGraph["nodes"].push(node);
                effectsGraphPositions[prefix+std::to_string(effectsValueSelected)]=spawn;effectsLayoutDirty=true;
            }
            EffectsRect("value-palette:"+op);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();if(ImGui::Button(Tr("Apply Nodes"))) EffectsValueApply(effectsValueGraph);EffectsRect("value-apply");
    ImGui::EndDisabled();
    ImGui::SameLine();if(ImGui::Button(Tr("Reset Nodes"))) {effectsValueKey.clear();effectsValueError.clear();}EffectsRect("value-reset");
}
// `sink`: the pin of an emitter node the value ends in (the structure graph); without it the output is a card.
void NativeEditor::Impl::EffectsValueCanvas(EditorNodeCanvas& canvas,float zoom,const std::string& prefix,ImVec2 origin,const ImVec2* sink) {
    if(effectsValueGraph.isNull()) return;
    const int channel=effectsCurveKeys["expression:channel"];
    {
        Json& nodes=effectsValueGraph["nodes"];const int count=static_cast<int>(nodes.size());
        std::vector<int> depths(static_cast<size_t>(count));
        // Compiler-produced graphs are topological; pending forward links are laid out conservatively.
        for(int i=0;i<count;++i) for(const Json& arg:nodes[i]["args"].items())
            if(arg.asInt()>=0 && arg.asInt()<i) depths[static_cast<size_t>(i)]=std::max(depths[static_cast<size_t>(i)],depths[static_cast<size_t>(arg.asInt())]+1);
        std::map<int,float> depthRows;int maxDepth=0,leaf=0;std::vector<ImVec2> placed;
        for(int i=0;i<count;++i) {
            const int depth=depths[static_cast<size_t>(i)];maxDepth=std::max(maxDepth,depth);
            float row=depthRows[depth],x=depth*220.0f;
            if(depth==0) {row=static_cast<float>(leaf%3)*150;x=-static_cast<float>(leaf/3)*220;++leaf;}
            else depthRows[depth]+=std::max(110.0f,68+nodes[i]["args"].size()*22.0f)+40;
            placed.push_back(ImVec2(x,row));
        }
        const ImVec2 base=sink?origin-ImVec2((maxDepth+1)*220.0f,0):ImVec2(0,0);
        for(int i=0;i<count;++i) if(!effectsGraphPositions.count(prefix+std::to_string(i)))
            effectsGraphPositions[prefix+std::to_string(i)]=base+placed[static_cast<size_t>(i)];
        if(!effectsGraphPositions.count(prefix+"output")) effectsGraphPositions[prefix+"output"]=ImVec2((maxDepth+1)*220.0f,40);
        if(effectsValueFit && !sink) {
            ImVec2 low(100000,100000),high(-100000,-100000);
            for(int i=0;i<=count;++i) {const auto p=effectsGraphPositions[prefix+(i==count?"output":std::to_string(i))];
                const float cardHeight=i==count?110:std::max(110.0f,68+nodes[i]["args"].size()*22.0f);
                low.x=std::min(low.x,p.x);low.y=std::min(low.y,p.y);high.x=std::max(high.x,p.x+190);high.y=std::max(high.y,p.y+cardHeight);}
            canvas.Fit(low,high);effectsValueFit=false;effectsLayoutDirty=true;
        }
        std::vector<ImVec2> outputs(static_cast<size_t>(count));
        for(int i=0;i<count;++i) outputs[static_cast<size_t>(i)]=canvas.Point(effectsGraphPositions[prefix+std::to_string(i)]+ImVec2(190,54));
        for(int i=0;i<count;++i) for(size_t slot=0;slot<nodes[i]["args"].size();++slot) {
            const int from=nodes[i]["args"][slot].asInt(-1);if(from>=0 && from<count)
                canvas.Wire(outputs[static_cast<size_t>(from)],canvas.Point(effectsGraphPositions[prefix+std::to_string(i)]+ImVec2(0,54+slot*22.0f)),IM_COL32(112,202,242,255));
        }
        const int output=effectsValueGraph["output"].asInt(-1);
        if(output>=0 && output<count) canvas.Wire(outputs[static_cast<size_t>(output)],sink?*sink:canvas.Point(effectsGraphPositions[prefix+"output"]+ImVec2(0,54)),IM_COL32(129,224,163,255),sink?1.6f:1.0f);
        int target=-1,slot=-1;bool clear=false;
        ImGui::PushID("##valuenodes");
        for(int i=0;i<(sink?count:count+1);++i) {
            ImGui::PushID(i);const std::string key=prefix+(i==count?"output":std::to_string(i));
            std::string title=i==count?std::string(Tr("Channel Output"))+": "+VfxChannelNames()[static_cast<size_t>(channel)]:nodes[i]["op"].asString();
            const size_t arity=i==count?1:nodes[i]["args"].size();const std::string badge="#"+std::to_string(i);
            if(canvas.Card(effectsGraphPositions[key],ImVec2(190,std::max(110.0f,68+arity*22.0f)),title.c_str(),
                i==count?IM_COL32(48,104,71,255):IM_COL32(49,70,108,255),effectsValueSelected==i,true,i==count?nullptr:badge.c_str())) effectsLayoutDirty=true;
            EffectsRect("value-node:"+std::to_string(i));
            if(ImGui::IsItemClicked()) effectsValueSelected=i;
            const ImVec2 p=canvas.Point(effectsGraphPositions[key]);
            const std::string detail=i==count?(channel<5?"vector":"scalar"):nodes[i]["op"].asString()=="constant"?nodes[i]["literal"].asString():
                nodes[i].has("name")?nodes[i]["name"].asString():nodes[i]["type"].asString();
            canvas.draw->AddText(ImGui::GetFont(),ImGui::GetFontSize()*zoom,p+ImVec2(12,std::max(110.0f,68+arity*22.0f)-24)*zoom,IM_COL32(216,222,233,255),detail.c_str());
            if(i<count) {
                canvas.Pin("##out",canvas.Point(effectsGraphPositions[key]+ImVec2(190,54)),true);EffectsRect("value-out:"+std::to_string(i));
                if(ImGui::IsItemActivated()) effectsValueLink=i;
            }
            for(size_t a=0;a<arity;++a) {
                const std::string label=i==count?"value":nodes[i]["op"].asString()=="curve"?(a==0?"time":std::string(a%2?"t":"v")+std::to_string((a-1)/2)):std::to_string(a);
                canvas.draw->AddText(ImGui::GetFont(),ImGui::GetFontSize()*zoom,p+ImVec2(12,47+a*22.0f)*zoom,IM_COL32(182,197,213,255),label.c_str());
                const bool connected=i==count?output>=0:nodes[i]["args"][a].asInt(-1)>=0;
                canvas.Pin(("##in"+std::to_string(a)).c_str(),canvas.Point(effectsGraphPositions[key]+ImVec2(0,54+a*22.0f)),connected);
                EffectsRect("value-in:"+std::to_string(i)+":"+std::to_string(a));
                if(ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && effectsValueLink>=0) {target=i;slot=static_cast<int>(a);}
                if(ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {target=i;slot=static_cast<int>(a);clear=true;}
            }
            ImGui::PopID();
        }
        ImGui::PopID();
        if(effectsValueLink>=0 && effectsValueLink<count && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            canvas.Wire(outputs[static_cast<size_t>(effectsValueLink)],ImGui::GetIO().MousePos,IM_COL32(255,213,116,255));
        if(!sink) canvas.Footer(Tr("Connect values to channel output. Incomplete nodes stay pending. Assets store expression text only."));
        if(target>=0) {
            Json candidate=effectsValueGraph;
            if(target==count) candidate["output"]=clear?-1:effectsValueLink;
            else candidate["nodes"][target]["args"][slot]=clear?-1:effectsValueLink;
            if(!EffectsValueApply(candidate)) effectsValueGraph=candidate; // pending topology never changes the document
        }
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) effectsValueLink=-1;
    }
}
void NativeEditor::Impl::EffectsValueProperties(const std::string& prefix) {
    const int stage=effectsCurveKeys["expression:stage"],channel=effectsCurveKeys["expression:channel"];
    const std::string canonical=effectsValueSource;
    if(effectsValueSelected>=0 && effectsValueSelected<static_cast<int>(effectsValueGraph["nodes"].size())) {
        Json candidate=effectsValueGraph;Json& node=candidate["nodes"][effectsValueSelected];const std::string op=node["op"].asString();
        ImGui::TextUnformatted(op.c_str());bool changed=false;std::string merge;
        if(op=="constant") {
            std::string value=node["literal"].asString();ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::InputText(Tr("Literal"),&value,ImGuiInputTextFlags_EnterReturnsTrue)) {node["literal"]=value;changed=true;}
            EffectsRect("value-literal");
            if(value!="pi") {
                float number=static_cast<float>(std::atof(value.c_str()));ImGui::SetNextItemWidth(-FLT_MIN);
                const bool numberChanged=ImGui::DragFloat("##valuenumber",&number,.01f,0,65504,"%.4g",ImGuiSliderFlags_AlwaysClamp);
                const std::string numberMerge=EffectsMerge(prefix+std::to_string(effectsValueSelected));
                if(numberChanged) {
                    std::ostringstream literal;literal<<std::fixed<<std::setprecision(9)<<number;
                    std::string valueText=literal.str();while(valueText.size()>1 && valueText.back()=='0') valueText.pop_back();
                    if(valueText.back()=='.') valueText.pop_back();node["literal"]=valueText;changed=true;merge=numberMerge;
                }
                EffectsRect("value-number");
            }
        } else if(op=="input" || op=="parameter") {
            const auto inputOptions=op=="input"?effectsSchema["expressions"]["valueGraph"]["inputs"]:Json();
            const Json parameterOptions=Defaults(effectsDocument);
            const char* label=op=="input"?Tr("Particle Input"):Tr("Parameter");
            if(ImGui::BeginCombo(label,node["name"].asString().c_str())) {
                if(op=="input") for(const auto& entry:inputOptions.members()) {
                    if(ImGui::Selectable(entry.first.c_str())) {node["name"]=entry.first;node["type"]=entry.second;changed=true;}
                }
                else for(const auto& entry:parameterOptions.members()) if(ImGui::Selectable(entry.first.c_str())) {node["name"]=entry.first;changed=true;}
                ImGui::EndCombo();
            }
        } else if(op=="component") {
            int axis=node["axis"].asInt();if(ImGui::Combo(Tr("Axis"),&axis,"X\0Y\0Z\0")) {node["axis"]=axis;changed=true;}
        }
        for(size_t a=0;a<node["args"].size();++a) {
            int from=node["args"][a].asInt(-1);ImGui::PushID(static_cast<int>(a));ImGui::SetNextItemWidth(85);
            if(ImGui::InputInt((std::string(Tr("Input"))+" "+std::to_string(a)).c_str(),&from)) {node["args"][a]=from;changed=true;}
            ImGui::PopID();
        }
        if(op=="curve") {
            ImGui::TextWrapped("%s",Tr("Curve inputs: time, then age/value pairs. Ages must be constant and ascending."));
            if(ImGui::Button(Tr("Add Key")) && node["args"].size()<33) {node["args"].push(-1);node["args"].push(-1);changed=true;}
            ImGui::SameLine();if(ImGui::Button(Tr("Remove Key")) && node["args"].size()>3) {node["args"].items().resize(node["args"].size()-2);changed=true;}
        }
        if(changed && !EffectsValueApply(candidate,merge)) effectsValueGraph=candidate;
        if(ImGui::Button(Tr("Remove Node"))) {
            const int removed=effectsValueSelected;
            candidate=effectsValueGraph;candidate["nodes"].items().erase(candidate["nodes"].items().begin()+removed);
            for(Json& entry:candidate["nodes"].items()) for(Json& arg:entry["args"].items()) {
                const int from=arg.asInt(-1);arg=from==removed?-1:from>removed?from-1:from;
            }
            const int from=candidate["output"].asInt(-1);candidate["output"]=from==removed?-1:from>removed?from-1:from;
            effectsValueSelected=-1;
            if(!EffectsValueApply(candidate)) effectsValueGraph=candidate;
        }
        EffectsRect("value-remove");
    }
    ImGui::Separator();ImGui::TextUnformatted(Tr("Expression Source"));
    const std::string textKey=effectsPath+":"+prefix;auto& text=effectsExpressionText[textKey];
    if(text.first!=canonical) text=std::make_pair(canonical,canonical);
    ImGui::InputTextMultiline("##valuesource",&text.second,ImVec2(-FLT_MIN,100));EffectsRect("value-source");
    if(ImGui::Button(Tr("Apply Expression"))) {
        Json document=effectsDocument;Json& expressions=document["emitters"][effectsBlock]["emitter"]["expressions"];
        if(text.second.empty()) expressions[Stage(stage)].erase(VfxChannelNames()[static_cast<size_t>(channel)]);
        else expressions[Stage(stage)][VfxChannelNames()[static_cast<size_t>(channel)]]=text.second;
        EffectsReplace(document);
    }
    EffectsRect("value-source-apply");
}
void NativeEditor::Impl::EffectsValues() {
    if(effectsBlock>=static_cast<int>(effectsDocument["emitters"].size())) {
        ImGui::TextWrapped("%s",Tr("Value nodes require a particle emitter."));return;
    }
    if(effectsLayoutPath!=effectsPath) EffectsGraphLayout(false);
    int& stage=effectsCurveKeys["expression:stage"];int& channel=effectsCurveKeys["expression:channel"];
    ImGui::SetNextItemWidth(110);
    if(ImGui::BeginCombo("##valuestage",Tr(stage?"Update":"Birth"))) {
        for(int i=0;i<2;++i) if(ImGui::Selectable(Tr(i?"Update":"Birth"),stage==i)) stage=i;
        ImGui::EndCombo();
    }
    EffectsRect("value-stage");ImGui::SameLine();ImGui::SetNextItemWidth(120);
    if(ImGui::BeginCombo("##valuechannel",VfxChannelNames()[static_cast<size_t>(channel)])) {
        for(size_t i=0;i<kVfxChannelCount;++i) {
            if(ImGui::Selectable(VfxChannelNames()[i],channel==static_cast<int>(i))) channel=static_cast<int>(i);
            EffectsRect(std::string("value-channel:")+VfxChannelNames()[i]);
        }
        ImGui::EndCombo();
    }
    EffectsRect("value-channel");
    const std::string prefix=EffectsValueSync();
    ImGui::SameLine();EffectsValuePalette(prefix,ImVec2(40,40));
    const std::string canonical=effectsValueSource;
    if(ImGui::Button(Tr("Auto Arrange"))) {
        for(auto it=effectsGraphPositions.begin();it!=effectsGraphPositions.end();) {
            if(it->first.compare(0,prefix.size(),prefix)==0) it=effectsGraphPositions.erase(it);else ++it;
        }
        effectsValueFit=true;effectsLayoutDirty=true;
    }
    EffectsRect("value-arrange");ImGui::SameLine();if(ImGui::Button(Tr("Fit Graph"))) effectsValueFit=true;EffectsRect("value-fit");
    if(canonical.empty()) ImGui::TextDisabled("%s",Tr("Channel uses authored values until nodes are applied."));
    if(!effectsValueError.empty()) ImGui::TextColored(ImVec4(1,.55f,.45f,1),"%s",effectsValueError.c_str());
    if(effectsValueGraph.isNull()) { ImGui::TextWrapped("%s",Tr("Keep this expression in text view; conversion was refused."));return; }
    const float available=ImGui::GetContentRegionAvail().x;
    const float inspector=std::min(300.0f,available*0.32f);
    ImGui::BeginChild("##valuegrapharea",ImVec2(std::max(160.0f,available-inspector-8),0));
    {
        EditorNodeCanvas canvas("##valuecanvas",effectsValuePan,effectsValueZoom);
        const ImVec2 oldPan=effectsValuePan;const float oldZoom=effectsValueZoom;canvas.Navigate();
        effectsLayoutDirty=effectsLayoutDirty || oldPan.x!=effectsValuePan.x || oldPan.y!=effectsValuePan.y || oldZoom!=effectsValueZoom;
        EffectsValueCanvas(canvas,effectsValueZoom,prefix,ImVec2(0,0),nullptr);
    }
    ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("##valueproperties",ImVec2(0,0),ImGuiChildFlags_Borders);
    EffectsValueProperties(prefix);
    ImGui::Separator();if(effectsScene) EffectsPreview(true);
    ImGui::EndChild();
    if(effectsGraphPositions.size()>4096) {effectsGraphPositions.clear();effectsValueFit=true;effectsLayoutDirty=true;}
    if(effectsLayoutDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) EffectsGraphLayout(true);
}
}  // namespace oe
