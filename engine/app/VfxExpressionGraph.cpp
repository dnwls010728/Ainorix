#include "app/VfxExpressionGraph.h"

#include <cmath>
#include <functional>
#include <vector>

#include "api/Commands.h"
#include "scene/VfxExpression.h"

namespace oe {
namespace {
struct Operation { const char* name; const char* symbol; int arity; };
const Operation kOperations[] = {
    {"constant","",0},{"input","",0},{"parameter","",0},{"component","",1},{"vec3","",3},{"negate","-",1},
    {"add","+",2},{"subtract","-",2},{"multiply","*",2},{"divide","/",2},{"less","<",2},{"less_equal","<=",2},
    {"greater",">",2},{"greater_equal",">=",2},{"equal","==",2},{"not_equal","!=",2},
    {"min","",2},{"max","",2},{"abs","",1},{"clamp","",3},{"lerp","",3},{"select","",3},
    {"sin","",1},{"cos","",1},{"pow","",2},{"exp","",1},{"sqrt","",1},{"floor","",1},{"fract","",1},
    {"dot","",2},{"length","",1},{"curve","",-1}
};
static_assert(sizeof(kOperations)/sizeof(kOperations[0])==static_cast<size_t>(VfxExprOp::Curve)+1,"Expression operation palette");
[[noreturn]] void Fail(const std::string& message) {
    throw ApiError("invalid_expression_graph",message,"Keep expression text; connect finite scalar/vector nodes without cycles, then retry.");
}
bool Name(const std::string& name) {
    if(name.empty() || name.size()>128) return false;
    for(size_t i=0;i<name.size();++i) {
        const char c=name[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_' || (i>0 && c>='0' && c<='9'))) return false;
    }
    return true;
}
int Index(const Json& value,int size) {
    if(!value.isNumber() || !std::isfinite(value.asNumber()) || value.asNumber()<0 || value.asNumber()>=size || value.asNumber()!=static_cast<double>(value.asInt()))
        Fail("node reference must be an integer within the graph");
    return value.asInt();
}
Json Graph(const VfxExpressionProgram& program) {
    Json graph=Json::MakeObject();graph["format"]="oe.vfx.expression_graph";graph["version"]=1;graph["nodes"]=Json::MakeArray();
    for(const auto& instruction:program.code) {
        Json node=Json::MakeObject();node["op"]=kOperations[static_cast<size_t>(instruction.op)].name;
        node["type"]=instruction.type==VfxValueType::Scalar?"scalar":"vector";node["args"]=Json::MakeArray();
        for(int arg:instruction.args) node["args"].push(arg);
        if(instruction.op==VfxExprOp::Constant) node["literal"]=instruction.name;
        if(instruction.op==VfxExprOp::Input || instruction.op==VfxExprOp::Parameter) node["name"]=instruction.name;
        if(instruction.op==VfxExprOp::Component) node["axis"]=static_cast<int>(instruction.value.x);
        graph["nodes"].push(node);
    }
    graph["output"]=static_cast<int>(program.code.size())-1;return graph;
}
std::string Source(const Json& graph,std::vector<std::string>* nodeSources=nullptr) {
    if(!graph.isObject() || graph["format"].asString()!="oe.vfx.expression_graph" || !graph["version"].isNumber() || graph["version"].asNumber()!=1)
        Fail("expected oe.vfx.expression_graph version 1");
    const Json& nodes=graph["nodes"];const int count=static_cast<int>(nodes.size());
    if(!nodes.isArray() || count<1 || count>64) Fail("graph needs 1..64 nodes");
    std::vector<int> visiting(static_cast<size_t>(count));std::vector<std::string> text(static_cast<size_t>(count));
    std::function<std::string(int,int)> visit=[&](int index,int depth)->std::string {
        if(depth>32) Fail("graph depth exceeds 32");
        if(visiting[static_cast<size_t>(index)]==1) Fail("graph contains a cycle at node "+std::to_string(index));
        if(visiting[static_cast<size_t>(index)]==2) return text[static_cast<size_t>(index)];
        visiting[static_cast<size_t>(index)]=1;const Json& node=nodes[index];
        const std::string op=node["op"].asString();const Operation* operation=nullptr;
        for(const auto& candidate:kOperations) if(op==candidate.name) operation=&candidate;
        if(!node.isObject() || !operation) Fail("unknown operation at node "+std::to_string(index));
        const Json& args=node["args"];
        if(!args.isArray() || (operation->arity>=0 && args.size()!=static_cast<size_t>(operation->arity)) ||
            (operation->arity<0 && (args.size()<3 || args.size()>33 || args.size()%2==0))) Fail("wrong argument count at node "+std::to_string(index));
        std::vector<std::string> arguments;for(const Json& arg:args.items()) arguments.push_back(visit(Index(arg,count),depth+1));
        std::string result;
        if(op=="constant") {
            result=node["literal"].asString();bool dot=false,digit=false;
            if(result!="pi") {
                for(char c:result) {if(c=='.' && !dot) dot=true;else if(c>='0' && c<='9') digit=true;else Fail("constant needs an unsigned decimal literal or pi");}
                if(!digit) Fail("constant needs an unsigned decimal literal or pi");
            }
        } else if(op=="input" || op=="parameter") {
            const std::string name=node["name"].asString();if(!Name(name)) Fail("invalid input/parameter name");
            result=(op=="parameter"?"param.":"")+name;
        } else if(op=="component") {
            const int axis=Index(node["axis"],3);const std::string parentOp=nodes[Index(args[0],count)]["op"].asString();
            if(parentOp!="input" && parentOp!="parameter") Fail("component reads require a named vector input or parameter");
            result=arguments[0]+"."+std::string(1,"xyz"[axis]);
        } else if(op=="negate") result="(-"+arguments[0]+")";
        else if(operation->symbol[0]) result="("+arguments[0]+operation->symbol+arguments[1]+")";
        else {
            result=op+"(";for(size_t i=0;i<arguments.size();++i) {if(i) result+=",";result+=arguments[i];}result+=")";
        }
        if(result.size()>1024) Fail("expanded graph source exceeds 1024 bytes");
        visiting[static_cast<size_t>(index)]=2;text[static_cast<size_t>(index)]=result;return result;
    };
    const std::string output=visit(Index(graph["output"],count),0);
    // Structural validation includes disconnected nodes; the command also compiles each node for types.
    for(int i=0;i<count;++i) visit(i,0);
    if(nodeSources) *nodeSources=text;
    return output;
}
VfxExpressionProgram Compile(const std::string& source,VfxValueType type,const Json& params) {
    VfxExpressionProgram program;VfxExpressionError error;
    if(!CompileVfxExpression(source,type,params,program,&error)) throw ApiError(error.code,error.message,error.hint);
    return program;
}
}  // namespace
Json VfxExpressionGraphSchema() {
    Json schema=Json::MakeObject();schema["format"]="oe.vfx.expression_graph";schema["version"]=1;
    schema["maxNodes"]=64;schema["maxDepth"]=32;schema["operations"]=Json::MakeArray();
    for(const auto& operation:kOperations) {
        Json entry=Json::MakeObject();entry["op"]=operation.name;entry["arity"]=operation.arity;schema["operations"].push(entry);
    }
    schema["storage"]="Transient authoring view only; assets store source, layout sidecars store positions.";
    schema["inputs"]=Json::MakeObject();
    for(const char* name:{"age","lifetime","emitterTime","systemTime","index","count","random","speed","birthSize","birthOpacity"}) schema["inputs"][name]="scalar";
    for(const char* name:{"birthPosition","birthVelocity","birthRotation","birthScale","birthColor"}) schema["inputs"][name]="vector";
    return schema;
}
Json VfxExpressionGraphCall(const Json& args,bool toSource) {
    const std::string type=args["type"].asString("scalar");if(type!="scalar" && type!="vector") Fail("type must be scalar or vector");
    const VfxValueType expected=type=="scalar"?VfxValueType::Scalar:VfxValueType::Vector;
    const Json params=args.has("params")?args["params"]:Json::MakeObject();
    if(!params.isObject() || params.size()>32) Fail("params needs at most 32 numeric entries");
    for(const auto& p:params.members()) {
        if(p.second.isArray() && p.second.size()==3) {
            for(const Json& v:p.second.items()) if(!v.isNumber() || !std::isfinite(v.asFloat()) || std::abs(v.asFloat())>65504) Fail("invalid vector parameter");
        } else if(!p.second.isNumber() || !std::isfinite(p.second.asFloat()) || std::abs(p.second.asFloat())>65504) Fail("invalid scalar parameter");
    }
    std::vector<std::string> nodeSources;
    const std::string original=toSource?Source(args["graph"],&nodeSources):args["source"].asString();
    Json nodeTypes=Json::MakeArray();
    if(toSource) for(const std::string& source:nodeSources) {
        VfxExpressionProgram node;VfxExpressionError error;
        if(CompileVfxExpression(source,VfxValueType::Scalar,params,node,&error)) nodeTypes.push("scalar");
        else if(CompileVfxExpression(source,VfxValueType::Vector,params,node,&error)) nodeTypes.push("vector");
        else throw ApiError(error.code,error.message,error.hint);
    }
    const auto program=Compile(original,expected,params);const Json graph=Graph(program);
    std::string source;
    try { source=Source(graph);const auto roundTrip=Compile(source,expected,params);
        if(roundTrip.code.size()!=program.code.size()) Fail("conversion changed the program");
        for(size_t i=0;i<program.code.size();++i) {
            const auto& a=program.code[i];const auto& b=roundTrip.code[i];
            if(a.op!=b.op || a.type!=b.type || a.args!=b.args || !(a.value==b.value) || a.name!=b.name) Fail("conversion changed a compiled value");
        }
    } catch(const ApiError& error) {
        throw ApiError("unsupported_expression_graph",error.what(),"Keep this expression in text view. Conversion preserves its document unchanged.");
    }
    Json result=Json::MakeObject();result["source"]=source;result["graph"]=graph;result["type"]=type;
    if(toSource) result["nodeTypes"]=nodeTypes;
    result["instructions"]=static_cast<int>(program.code.size());return result;
}
}  // namespace oe
