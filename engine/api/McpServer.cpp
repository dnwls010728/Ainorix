#include "api/McpServer.h"

#include <istream>
#include <map>
#include <ostream>

#include "core/Log.h"

namespace oe {

namespace {

const char* kSupportedVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

std::string ToolName(const std::string& command) {
    std::string n = command;
    for (char& c : n) {
        if (c == '.') c = '_';
    }
    return n;
}

void Send(std::ostream& out, const Json& message) {
    out << message.dump() << "\n";
    out.flush();
}

Json Reply(const Json& id, Json result) {
    Json r = Json::MakeObject();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"] = std::move(result);
    return r;
}

Json ReplyError(const Json& id, int code, const std::string& message) {
    Json r = Json::MakeObject();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["error"]["code"] = code;
    r["error"]["message"] = message;
    return r;
}

Json TextContent(const std::string& text) {
    Json c = Json::MakeObject();
    c["type"] = "text";
    c["text"] = text;
    return c;
}

}  // namespace

int RunMcpServer(const CommandCaller& call, std::istream& in, std::ostream& out) {
    std::map<std::string, std::string> toolToCommand;
    auto refreshTools = [&]() {
        Json list = call("api.list", Json(Json::Object{{"verbose", true}}));
        Json tools = Json::MakeArray();
        toolToCommand.clear();
        for (const Json& c : list["result"].items()) {
            std::string name = c["name"].asString();
            toolToCommand[ToolName(name)] = name;
            Json t = Json::MakeObject();
            t["name"] = ToolName(name);
            t["description"] = c["summary"].asString() + (c["mutates"].asBool() ? " (edits the scene; undoable)" : "");
            t["inputSchema"] = c["params"];
            tools.push(t);
        }
        return tools;
    };

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::string parseError;
        const Json msg = Json::parse(line, &parseError);
        if (!parseError.empty()) {
            Send(out, ReplyError(Json(), -32700, parseError));
            continue;
        }
        const Json& id = msg["id"];
        std::string method = msg["method"].asString("");
        bool isRequest = msg.has("id") && !method.empty();
        if (!isRequest) continue;  // notifications (initialized, cancelled, ...) and responses

        if (method == "initialize") {
            std::string requested = msg["params"]["protocolVersion"].asString("");
            std::string version = kSupportedVersions[0];
            for (const char* v : kSupportedVersions) {
                if (requested == v) version = v;
            }
            Json result = Json::MakeObject();
            result["protocolVersion"] = version;
            result["capabilities"]["tools"]["listChanged"] = false;
            result["serverInfo"]["name"] = "ownengine";
            result["serverInfo"]["version"] = OE_VERSION;
            Json info = call("engine.info", Json());
            result["instructions"] = info["result"]["guide"].asString(
                "OwnEngine game engine. Call engine_info first, then scene_summary.");
            Send(out, Reply(id, result));
        } else if (method == "ping") {
            Send(out, Reply(id, Json::MakeObject()));
        } else if (method == "tools/list") {
            Json result = Json::MakeObject();
            result["tools"] = refreshTools();
            Send(out, Reply(id, result));
        } else if (method == "tools/call") {
            if (toolToCommand.empty()) refreshTools();
            std::string tool = msg["params"]["name"].asString("");
            auto it = toolToCommand.find(tool);
            if (it == toolToCommand.end()) {
                Send(out, ReplyError(id, -32602, "unknown tool '" + tool + "'"));
                continue;
            }
            Json envelope = call(it->second, msg["params"]["arguments"]);
            Json result = Json::MakeObject();
            Json content = Json::MakeArray();
            if (envelope["ok"].asBool()) {
                Json payload = envelope["result"];
                if (payload.isObject() && payload.has("png_base64")) {
                    Json image = Json::MakeObject();
                    image["type"] = "image";
                    image["data"] = payload["png_base64"];
                    image["mimeType"] = "image/png";
                    content.push(image);
                    payload.erase("png_base64");
                }
                content.push(TextContent(payload.dump()));
                result["isError"] = false;
            } else {
                content.push(TextContent(envelope["error"].dump()));
                result["isError"] = true;
            }
            result["content"] = content;
            Send(out, Reply(id, result));
        } else if (method == "resources/list" || method == "prompts/list") {
            Json result = Json::MakeObject();
            result[method == "resources/list" ? "resources" : "prompts"] = Json::MakeArray();
            Send(out, Reply(id, result));
        } else {
            Send(out, ReplyError(id, -32601, "method not found: " + method));
        }
    }
    return 0;
}

}  // namespace oe
