# Agent team — work log

The design is in [TEAM.md](TEAM.md) (section numbers such as "§5.1" refer to it). Continue from the first unchecked
line (docs/DESIGN.md §5).

## Implementation status (work log)

- [x] T1: `platform/Process.h` (spawn, pipes, kill, find executable) with the Win32 implementation
      (`win32/ProcessWin32.cpp`) and the stub `null/ProcessNone.cpp` elsewhere; `engine/team/`
      (library `oe_team`): backend descriptors, detection, Codex model discovery, user overrides
      (`Backends.cpp`), `team.backends` (`TeamCommands.cpp`, registered by `oe exec/script/editor/mcp/api`).
      Tests: `ProcessRunsChildren`, `TeamBackendParsing`, `TeamBackendDetection` (stand-in `.cmd`
      CLIs). Checked by hand with the installed CLIs, see §4.3. Not done: the editor does not call
      `team.backends` on a worker thread yet (the command blocks while the CLIs print their
      version, 3.5 s on the build host; T4), and no macOS/Linux process backend (Later).
- [x] T2: `TeamStore` (`engine/team/TeamStore.cpp`: profiles in `.oe/team/team.json`, validation,
      atomic writes, reload when another process changed the file, avatars: preset names +
      upload with center crop to 256x256); `team.list/get/add/update/remove/avatar/presets/settings`;
      `ImageDimensions` in `assets/Assets.h` (size check before decoding). Tests:
      `TeamStoreProfiles`, `TeamAvatars`. Not done: the 12 preset pictures themselves
      (`engine/editor/avatars/*.png`, embedded and drawn by the editor) come with T4, so until then
      a preset is only a name; reordering agents (the panel's drag) has no command yet (T4).
- [x] T3: `TeamSession` (`engine/team/TeamSession.cpp`): one turn per agent as a child process,
      routing (`@id`/`@name`, `@all`, lead), `maxConcurrent` queue, agent states and activity,
      `chat.jsonl`, conversation resume through `sessions.json`, cancel, turn timeout (30 min);
      adapters in `TeamAdapters.cpp` (Claude Code and Codex command lines + stream parsers);
      `team.send/cancel/state/messages/clear`; `oe mcp` updates the team every loop iteration.
      Tests: `TeamStreamParsers` (recorded lines), `TeamSessionTurns` (stand-in `.cmd` CLIs).
      Checked by hand with the real CLIs, see §4.3. Not done: the editor frame does not call
      the team update yet (T4; polling `team.state` drives it meanwhile); agent attribution of
      API calls (T5); images (T6); agent-to-agent turns and the hop limit (T7: `OE_TEAM_HOP` is
      always 0, an agent's `@mention` in a reply starts nothing). Unverified: whether Claude's
      `total_cost_usd` is per turn or cumulative for a resumed conversation (stored as reported);
      Codex `file_change` / `web_search` items were written from the format, not recorded.
- [x] T4: Editor **Team** panel (`engine/editor/EditorTeam.cpp`: roster with picture, status dot,
      lead mark, backend and model, activity, elapsed time and Stop; add / edit / delete; drag to
      reorder through the new `team.move`) and the profile dialog (backend combo with CLIs that
      are not installed disabled, model combo with Custom, access with a full-access
      confirmation, avatar picker: presets + upload) with Korean and Japanese rows. The editor
      frame updates the team (`NativeEditor::Options::onFrame`), and CLI detection no longer
      blocks: `team.backends {async}`. Tests: `NativeEditorTeam` (window events, a screenshot per
      language in `build/editor-team-*.png`), `EditorAvatarPresets`; CLI check:
      `oe editor <project> --screenshot f.png --team [--agent new|<id>]`. **Only one preset
      picture exists so far** (`fox`, generated with Codex's image tool, see §3.2): the other
      eleven are still to be made, and every new agent gets the fox until then. Not done:
      validation errors appear as one message above the buttons, not under the field they
      name; selecting a row does not fill the chat input yet (T5); the lead is marked with the
      word "Lead", not a crown icon. Not verified: the picture picker
      (preset grid, Upload with the Windows file chooser or the path field) and drag
      reordering were not driven by a test or by hand; the commands behind them
      (`team.avatar`, `team.move`) are tested.
- [x] T5: Editor **Team Chat** panel (`engine/editor/EditorTeamChat.cpp`): messages with picture,
      name, local time, a turn summary that opens to its steps, fenced code in the monospace
      font, buttons for project scripts and scenes named in a message; one live line per running
      or queued agent with Stop; input where Enter sends, Shift+Enter breaks the line and Tab
      (or a click) completes the `@mention` being typed; unread count in the tab title. A click
      on a roster row puts `@id ` into an empty input. Agent attribution:
      `oe mcp --connect <port> --agent <id>` sends `agent` with every `/api/call`,
      `Engine::PostCall(name, args, agent)` hands it to the remote-call observer
      (`RemoteCallAgent()`), and the editor's notice reads "Mina: component.set Player" with the
      agent's color on the notice and on the Hierarchy dot. Tests: `NativeEditorTeamChat`
      (typed text, Tab, Enter, Shift+Enter, a real click on Stop, the unread count, screenshots
      in `build/editor-team-chat-*.png`), attribution in `NativeEditorTeam`. Also fixed here: a
      turn's exit code is read once the CLI has exited, not when its output ends (the same race
      as in detection; a crash was sometimes reported as "exited with code 0"). Not done: the
      completion list is a row of buttons above the input, not a popup at the caret; paths open
      through buttons under the message, not as links inside the text; a mention is completed
      to the agent's id, not its name. Not verified: attribution end to end with a real CLI
      (the flag is passed and the HTTP field is read; the pieces are tested separately), and
      the roster click that fills the input (no test drives it).
- [x] T6: Images in chat: a reply carries the png/jpg files its turn created or changed in
      `.oe/team/images/` and `assets/` (`attachments` on the message); the chat shows them as
      thumbnails, a click opens the viewer (fit / actual size, checkerboard, size and path,
      **Import to assets**, **Show in folder**, **Copy path**). Tests: `TeamSessionTurns` (a
      stand-in CLI that writes a picture), `NativeEditorTeamChat` (viewer). Not done: images
      named by stream events (§5.4 route 1): `codex exec --json` shows no image-generation item
      and no stream of ours carried one, so only the file route exists; an agent must save into
      one of the two folders. Not verified: a picture really generated by a CLI arriving in the
      chat (only the stand-in was run).
- [x] T6b (added 2026-10-04): what the person gives the agents. **Attachments**: files dropped on
      the Team Chat panel (Win32 reports where a drop landed; elsewhere they are still imported)
      or passed as `team.send {attachments}`; project files are referred to in place, others are
      copied into `.oe/team/attachments/`; agents get the paths under the message. **File
      references**: typing `#` + part of a name lists matching project files with thumbnails,
      a larger preview beside the list (pictures, model / material / prefab thumbnails, the
      first lines of scripts and text), Tab or a click inserts the path. **Mention preview**: the
      `@` list shows picture, name, role and state. Checked by hand with the real CLIs: Claude
      Code (haiku) and Codex each read an attached PNG and named what is on it. Tests:
      `TeamSessionTurns` (attachments), `NativeEditorTeamChat` (drop events, `#` + Tab). Not
      verified: an actual drag from Explorer (tests inject the drop event with its position);
      non-image attachments with a real CLI.
- [x] T7a (2026-10-04): agent-to-agent mentions with the hop limit. A reply that mentions a
      teammate (`@id`, `@name`, `@all`) gives that teammate a turn; messages carry `hop` (0 from
      the person, an agent's reply one more than what it answered) and a reply whose hop exceeds
      `maxHops` is not handed on (a notice says to whom it was not). `OE_TEAM_HOP` is the hop
      of the message a turn answers. The standing context tells agents that an `@id` calls the
      teammate and to write names without `@` otherwise. Test: `TeamSessionTurns` (ping-pong
      chain cut at maxHops 2, maxHops 0). Checked with real CLIs: a Claude reply that mentioned
      a Codex agent started its turn. The concurrency limit and session resume were done in T3.
- [x] T7b (2026-10-04): READMEs (Korean, English, Japanese) describe the team feature and
      `oe exec/script --connect`. A `team.send` made by a team agent through the API
      (`--agent <id>`) is posted as that agent, whatever `from` says, with the hop of its
      running turn + 1, so it is cut at `maxHops` like a mention in a reply
      (`Engine::RemoteCallAgent()` is set while the posted call runs). Test: `TeamSessionTurns`.
      Not verified: a real CLI calling `team.send` through MCP.
- [ ] U (usage, postponed on 2026-10-04): a team spends far more tokens than one agent that
      delegates. Measurements, causes and the options U1-U6 are in §11; nothing of it is
      implemented and one decision is open (agent-to-agent turns on or off by default).
- [ ] Later: interactive permission prompts in chat, Gemini CLI adapter, per-agent git worktrees,
      macOS/Linux process backend.
