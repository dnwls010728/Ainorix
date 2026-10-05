from pathlib import Path

path = Path("tests/tests.cpp")
text = path.read_text(encoding="utf-8")
anchor = '''    // Agent to agent: a teammate mentioned in a reply gets a turn; the chain a person's message
    // starts is cut after maxHops such turns.
'''
if anchor not in text:
    raise SystemExit("TeamSessionTurns lean-test insertion anchor not found")

block = r'''    // Lean mode keeps unrelated unread chat out of a turn and makes reply mentions text-only.
    // Hold the only slot with Sloth so Jun and Mina are both pending before roster-order scheduling
    // starts Mina first; this makes the prompt-filter assertion deterministic.
    {
        const char* oldLeanValue = std::getenv("OE_TEAM_LEAN");
        const bool hadLeanValue = oldLeanValue != nullptr;
        const std::string savedLeanValue = oldLeanValue ? oldLeanValue : "";
        std::string savedReply;
        CHECK(ReadTextFile(JoinPath(dir, "reply.jsonl"), savedReply));
        _putenv_s("OE_TEAM_LEAN", "1");
        CHECK(LeanTeamTurnsEnabled());
        const std::string leanReply = R"J({"type":"result","subtype":"success","is_error":false,"result":"Lean done; FYI @jun.","session_id":"sess-1","total_cost_usd":0.01})J" "\n";
        CHECK(WriteTextFile(JoinPath(dir, "reply.jsonl"), leanReply));

        CHECK(Call(e, "team.settings", R"J({"maxConcurrent":1})J")["ok"].asBool());
        CHECK(Call(e, "team.send", R"J({"text":"@sloth hold the lean slot"})J")["result"]["started"].size() == 1);
        CHECK(until([&] { return stateOf("sloth")["state"].asString() == "working"; }));
        CHECK(Call(e, "team.send", R"J({"text":"@jun LEAN_UNRELATED"})J")["result"]["queued"].size() == 1);
        CHECK(Call(e, "team.send", R"J({"text":"@mina LEAN_TARGETED"})J")["result"]["queued"].size() == 1);
        CHECK(Call(e, "team.cancel", R"J({"id":"sloth"})J")["ok"].asBool());
        CHECK(until(settled));
        const std::string leanPrompt = fileText("mina.prompt.txt");
        CHECK(leanPrompt.find("[User] @mina LEAN_TARGETED") != std::string::npos);
        CHECK(leanPrompt.find("LEAN_UNRELATED") == std::string::npos);
        CHECK(leanPrompt.find("hold the lean slot") == std::string::npos);

        // Mina's reply mentions Jun, but in lean mode that text must not start Jun. There is no
        // explicit Jun message in this second chain, so exactly user -> Mina should be appended.
        CHECK(Call(e, "team.settings", R"J({"maxConcurrent":3})J")["ok"].asBool());
        const int leanBase = lastMessage()["id"].asInt();
        sent = Call(e, "team.send", R"J({"text":"@mina LEAN_NO_HANDOFF"})J");
        CHECK(sent["result"]["started"].size() == 1 && sent["result"]["started"][0].asString() == "mina");
        CHECK(until(settled));
        args = Json::MakeObject();
        args["after"] = leanBase;
        const Json leanChain = e.Call("team.messages", args)["result"]["messages"];
        CHECK(leanChain.size() == 2);
        if (leanChain.size() == 2) {
            CHECK(leanChain[0]["from"].asString() == "user" && leanChain[0]["to"].size() == 1 && leanChain[0]["to"][0].asString() == "mina");
            CHECK(leanChain[1]["from"].asString() == "mina" && leanChain[1]["text"].asString().find("@jun") != std::string::npos);
            CHECK(leanChain[1]["to"].size() == 1 && leanChain[1]["to"][0].asString() == "user");
        }
        CHECK(stateOf("jun")["state"].asString() == "idle");

        CHECK(WriteTextFile(JoinPath(dir, "reply.jsonl"), savedReply));
        _putenv_s("OE_TEAM_LEAN", hadLeanValue ? savedLeanValue.c_str() : "");
    }

'''
path.write_text(text.replace(anchor, block + anchor, 1), encoding="utf-8", newline="\n")
print("lean TeamSession regression tests inserted")
