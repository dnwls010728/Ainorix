# Agent team

Status: **in progress.** This document is the plan and the work log for the feature
(docs/DESIGN.md §5). Only the milestones ticked below exist in the code; everything else is design.

An agent team is a set of named AI agents that live in a project. Each has a profile (picture,
description, instructions) and runs on a coding-agent CLI installed on the user's PC (Claude Code,
Codex, ...) with a chosen model. The editor shows the roster, lets the user add, edit and remove
agents, and has a team chat where the user calls agents and gives them work. While an agent works,
the editor shows what it is doing; images an agent produces appear in the chat.

The engine does not talk to any model provider itself. It starts the CLI the user already
installed and logged in to, as a child process, and reads its machine-readable event stream. The
agent drives the open editor through the existing path: `oe mcp --connect <port>`.

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

## 1. Requirements

| # | Requirement | Where it is answered |
|---|---|---|
| R1 | A team is made of agents; each has a profile | §3 |
| R2 | Profile picture: several built-in presets first, or an uploaded image | §3.2 |
| R3 | Per agent: description, instructions | §3.1 |
| R4 | Per agent: which agent CLI (Claude, Codex, ...) from a dropdown; an entry is selectable only when that CLI is installed on this PC | §4.1, §7.2 |
| R5 | Per agent: a model from the ones the chosen CLI supports | §4.2 |
| R6 | The editor has a team chat to call agents and give instructions | §5, §7.3 |
| R7 | Images an agent generates can be seen in the editor | §5.4 |
| R8 | The state of every agent and what it is working on is visible | §5.3 |
| R9 | The editor lists agents and can add and delete them | §7.1 |

Non-goals for the first version: calling provider HTTP APIs directly, storing API keys, agents in
packaged games or on web/Android, voice, and merging conflicting file edits of parallel agents.

## 2. Architecture

```
 Editor (oe_editor)                         oe process                          child processes
 ┌───────────────┐  Impl::Call  ┌──────────────────────────────┐  stdin/stdout  ┌──────────────┐
 │ Team panel    │─────────────▶│ team.* commands              │──────────────▶ │ claude -p …  │
 │ Team Chat     │◀─────────────│  TeamStore   (profiles)      │◀────────────── │ codex exec … │
 └───────────────┘  team.state  │  TeamSession (turns, chat)   │   JSON lines   └──────┬───────┘
                                │  Backends    (adapters)      │                       │ MCP (stdio)
                                │  platform/Process.h          │                ┌──────▼───────┐
                                │ HTTP API 127.0.0.1:7777 ◀────┼────────────────│ oe mcp       │
                                └──────────────────────────────┘   /api/call    │  --connect   │
                                                                                └──────────────┘
```

- **`engine/team/`** (new library `oe_team`, linked by `oe` and `oe_tests`, never by the player):
  portable C++17. `TeamStore` (profiles and files), `Backends` (descriptor table, command-line
  building, stream parsers), `TeamSession` (state machine, routing, chat log),
  `TeamCommands.cpp` (`RegisterTeamCommands(CommandRegistry&)`, called by the `oe` tool next to
  `RegisterBuiltinCommands`; the player never registers it).
- **`engine/platform/Process.h`** (new, the same pattern as `platform/Network.h`): the only OS
  code. Win32 in `platform/win32/ProcessWin32.cpp`; every other platform returns `Unavailable`
  until ported, and all backends then report `notInstalled` with that reason.
- **`engine/editor/EditorTeam.cpp`**: the two panels. They read `team.state` / `team.messages`
  and write only through `Impl::Call`, like every other panel (DESIGN.md invariant 1), so an
  agent attached over MCP can manage the team exactly as the person in the editor can.
- **Threads**: one reader thread per child process pushes complete lines into a mutex-guarded
  queue. `TeamSession::Update()` drains it on the main thread (called from the editor frame and
  from the `oe mcp` loop next to `Engine::RunPostedJobs`). Nothing in `engine/team` touches the
  simulation; wall-clock time is used only for message timestamps and timeouts, so the
  determinism invariant is unaffected.

### 2.1 `platform/Process.h`

```cpp
struct ProcessOptions {
    std::string executable;              // absolute path from FindExecutable
    std::vector<std::string> arguments;  // argv entries, never a shell string
    std::string workingDirectory;
    std::vector<std::pair<std::string, std::string>> environment;  // added to the parent's
    bool pipeStdin = true;
};
class Process {  // owned by TeamSession; destroying it kills the whole process tree
public:
    virtual ~Process() = default;
    virtual bool Write(const std::string& bytes, std::string* error) = 0;  // stdin
    virtual void CloseInput() = 0;
    virtual bool ReadLine(std::string& line, bool& isStderr) = 0;  // blocking; false at EOF
    virtual bool Running(int* exitCode) = 0;
    virtual void Kill() = 0;
};
bool PlatformProcessSupported();  // false: every backend reports `unavailable`
// Searches PATH (and PATHEXT on Windows); empty when not found.
std::string PlatformFindExecutable(const std::string& name);
std::unique_ptr<Process> PlatformStartProcess(const ProcessOptions& options, std::string* error);
```

Output lines are cut at 1 MiB (`kMaxProcessLineBytes`); at most 4096 unread lines are queued,
after which the child is blocked on its pipe. Only the three standard handles are inherited
(`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), so processes started at the same time do not hold each
other's pipes open. `PlatformFindExecutable` does not search the current directory.

Win32 notes: `CreateProcessW` with `CREATE_NO_WINDOW`, a job object with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` so closing the editor cannot leave agents running, UTF-8
pipes. CLIs installed through npm are `.cmd` shims, which `CreateProcessW` cannot start directly;
they are run through `cmd.exe /d /s /c`. Because `cmd.exe` re-parses its command line, **user text
(prompts, instructions) is never placed in arguments**: it goes through stdin or a temporary file
in `.oe/team/tmp/`. Arguments contain only values the engine generates or validates against a
strict character set (model ids, session ids, paths inside the project). `PlatformStartProcess`
enforces this for command scripts: every argument is quoted (so `& | < > ^` stay literal) and an
argument containing `"`, `%`, `!` or a control character is refused. A project path with one of
those characters therefore cannot be passed to an npm-installed CLI; T3 must run it with the
project as working directory instead of naming the path.

## 3. Data

Everything is under `<project>/.oe/team/` (a dot folder: not packaged by `oe package`, and
`.oe/` is in `.gitignore`, so a team is local to the user's checkout).

```
.oe/team/team.json          profiles and team settings
.oe/team/avatars/<id>.png   uploaded pictures, 256x256
.oe/team/chat.jsonl         chat log, one message per line, append-only
.oe/team/images/            images produced by agents (default output folder)
.oe/team/attachments/       files the person attached from outside the project
.oe/team/sessions.json      backend session ids for resume
.oe/team/tmp/               prompt files and generated MCP configs, cleared on start
```

### 3.1 `team.json`

```json
{
  "version": 1,
  "lead": "mina",
  "maxConcurrent": 3,
  "maxHops": 4,
  "agents": [
    {
      "id": "mina",
      "name": "Mina",
      "avatar": "preset:fox",
      "description": "Gameplay programmer. Lua scripts and scene wiring.",
      "instructions": "Work only in scripts/ and scenes/. Run script.check before finishing.",
      "backend": "claude",
      "model": "sonnet",
      "access": "edit"
    }
  ]
}
```

| Field | Rules |
|---|---|
| `id` | stable slug `[a-z0-9_-]{1,32}`, generated from the name when the agent is added (ASCII letters and digits, other characters become `-`; a name without any gives `agent`; `-2`, `-3`, ... make it unique), never changed by a rename; used in files and `@mentions`. `all` and `user` are reserved |
| `name` | display name, 1..40 characters on one line, unique (ASCII case-insensitive); `all` and `user` are refused |
| `avatar` | `preset:<name>` or `file` (then `avatars/<id>.png`) |
| `description` | one or two lines shown in the roster and given to teammates; ≤ 400 bytes |
| `instructions` | the agent's standing instructions (system prompt addition); ≤ 16 KiB |
| `backend` | a backend id from §4; unknown ids keep loading and show as unavailable, but cannot be chosen through `team.add` / `team.update` |
| `model` | a model id of that backend (`[A-Za-z0-9._:\[\]-]{1,64}`), or empty for the CLI's default |
| `access` | `read` \| `edit` (default) \| `full`, see §6 |
| `lead` | agent that receives messages with no mention; empty = such messages reach nobody. The first agent added to an empty team becomes the lead; removing the lead leaves it empty |
| `maxConcurrent` | 1..8, default 3 |
| `maxHops` | 0..16, default 4 |

Limits: 16 agents per team, file ≤ 1 MiB. Ordered, pretty JSON; defaults are not written
(DESIGN.md invariant 8). Writes are atomic (`WriteTextFileAtomic`): a change is validated, written,
and only then visible. Unknown keys are ignored when loading. A file that does not validate
(broken JSON, duplicate ids, a newer `version`, ...) is shown as an empty team with the reason in
`team.list` `error`, and every change is refused with `team_file_invalid` until the file is
repaired or deleted, so the engine never overwrites it. The store compares the file's
modification time before each command, so an editor and an `oe exec` on the same project see
each other's changes.

### 3.2 Profile pictures

- **Presets**: built-in 128x128 pictures, one name per PNG file in `engine/editor/avatars/`
  (`TeamAvatarPresets()` lists the names; `EditorAvatarPresets` checks that each has its file).
  They are embedded at build time the way the Roboto font is (`CMakeLists.txt` writes
  `EmbeddedAvatars.cpp`), so there is nothing to install. Today there is one, `fox`; the plan
  is twelve (`owl`, `cat`, `robot`, `bear`, `rabbit`, `panda`, `penguin`, `frog`, `tiger`,
  `koala`, `octopus`). Adding one = drop `<name>.png` into the folder and add the name to
  `TeamAvatarPresets()`. `team.presets` lists the ids. A new agent gets the first preset not
  used by a teammate (the first preset again once all are taken). A stored preset name without
  a picture (a file from a newer version) keeps loading and draws the fallback.
  The fox was made with Codex's native image generation (2026-10-03; prompt: a friendly fox
  head mascot, flat vector style, teal background, no text), then padded with its own
  background colour so the ears survive the round crop, and scaled to 128x128.
- **Upload**: the avatar picker opens `Window::ChooseFile` (`*.png;*.jpg;*.jpeg`); where native
  dialogs are unavailable, a path field. `team.avatar {id, source}` decodes the file with
  stb_image, center-crops to a square, resamples to 256x256 (area average, alpha-weighted;
  `MakeAvatarImage`) and writes `avatars/<id>.png`. The source may be outside the project (it is
  only read); it is rejected above 16 MiB or 8192 px (checked from the image header before
  decoding). Choosing a preset afterwards, or removing the agent, deletes the uploaded file.
- **Fallback**: a missing or undecodable picture draws a colored disc (color from an FNV-1a hash
  of the id) with the first letter of the name. The same fallback is used on screenshots taken
  without a GPU texture.
- Pictures are drawn round (`AddImageRounded`) at 24 px in lists and chat, 64 px in the profile
  dialog, with a status dot at the lower right.

## 4. Backends

A backend is one agent CLI. The descriptor table is code (`engine/team/Backends.cpp`):

| id | Executable | Turn command (headless, JSON event stream) | Image generation |
|---|---|---|---|
| `claude` | `claude` | `claude -p --output-format stream-json --verbose [--model <m>] --append-system-prompt-file <f> --permission-mode <mode> [--mcp-config <f> --strict-mcp-config [--allowedTools mcp__ownengine]] [--resume <session>]`, prompt on stdin | no |
| `codex` | `codex` | `codex exec [resume] --json --skip-git-repo-check [-m <m>] -c sandbox_mode='<mode>' [-c mcp_servers.ownengine.command='<oe>' -c mcp_servers.ownengine.args=['mcp','--connect','<port>'] -c mcp_servers.ownengine.default_tools_approval_mode='approve'] [<session>] -`, prompt on stdin | yes |
| `gemini` (later) | `gemini` | `gemini -p --output-format stream-json -m <m>` (no adapter yet: its agents are `offline`) | to be checked |

Both command lines were run against the installed versions (§4.3). The process runs with the
project as working directory, so file arguments are project-relative (`.oe/team/tmp/<id>.*`) and
no project path appears on a command line. Codex values are TOML with single-quoted strings,
because its npm shim refuses double quotes (§2.1). Codex has no system prompt flag: the standing
context is sent on stdin before the first prompt of a conversation (`systemPromptFlag = false`).
`--strict-mcp-config` makes a Claude agent use this editor only, not another engine server from
the user's own configuration. A backend adapter is a few fields, so a flag change touches one
place (`engine/team/TeamAdapters.cpp`):

```cpp
struct Backend {
    std::string id, displayName, executable, installHint;
    std::vector<ModelInfo> models;                                   // built-in catalog
    bool supportsImages;
    std::vector<ModelInfo> (*discoverModels)();                      // optional, see §4.2
    bool systemPromptFlag;                                           // false: context on stdin
    std::function<ProcessOptions(const TurnRequest&)> buildTurn;     // argv only, no user text
    std::function<void(const std::string& line, TurnEvents& out)> parseLine;  // one stream line -> events
};
```

`TurnEvent` is the backend-neutral form: `Session` (conversation id), `Thinking`, `Tool` (verb,
detail, call id), `Text`, `Done` (final text, cost), `Failed` (message). A line a parser does not
understand gives no event; a `Failed` followed by `Done` (a retry the CLI made itself) is a
success.

### 4.1 Detection (R4)

`team.backends` returns, per backend:

```json
{"id":"codex","name":"Codex","status":"installed","version":"0.159.3",
 "path":"C:/Users/x/AppData/Roaming/npm/codex.cmd",
 "models":[{"id":"gpt-6.1-sol","label":"GPT-6.1-Sol","default":true,"contextWindow":272000}],
 "modelSource":"discovered","supportsImages":true,"installHint":"npm install -g @openai/codex"}
```

- `status`: `installed` | `notInstalled` | `error` (found, but `--version` failed or timed out
  after 10 s; `message` says why) | `unavailable` (this platform has no process backend).
  `version` and `path` are absent when unknown; `message` is present unless `installed`.
- Detection = `PlatformFindExecutable` + `<exe> --version` (no stdin), all backends in parallel.
  The command runs it on the first call and again with `team.backends {refresh:true}`; results
  are cached for the session. With `async:true` the command returns at once: detection runs on
  a background thread, entries report `status: "detecting"` (with the catalog models) and a
  later call returns the result. The Team panel uses that form (polled every quarter second
  while detecting) when it first opens and from **Refresh agents**, so the window never
  freezes; a plain call made meanwhile waits for the running detection.
- A CLI counts as installed when `--version` exits with 0, or when it printed a version and
  then kept running until the timeout (an update check, a background helper). The command is
  finished only when its output ended **and** the process exited: Claude Code closes its
  output a moment before it exits, which an earlier version misread as exit code -1 and
  reported as `error` (found on 2026-10-03 in the running editor). A non-zero exit is an
  `error` even if the message contains a number that looks like a version. The profile dialog
  labels `error` entries "(not working)" with the message as tooltip, `notInstalled` ones
  "(not installed)".
- Being installed does not prove the user is logged in. A login failure appears on the first
  turn as an error message in the chat with the hint to run the CLI once in a terminal.
- An agent whose backend is not installed stays in the team, shows state `offline` and cannot be
  mentioned; nothing is deleted when a CLI is uninstalled.

### 4.2 Models (R5)

The CLIs have no stable "list models" command, so each backend carries a built-in catalog
(`id`, label, `default`, optional `contextWindow`) that is updated with engine releases:
`claude`: `fable`, `opus`, `sonnet` (default), `haiku`: the CLI's own aliases, which follow new
releases, labelled "Fable", "Opus", "Sonnet", "Haiku" without a version. Rule (2026-10-03): a
version is shown only where it was detected on this PC, never guessed. Claude Code offers no
list to discover (its config only caches extra options), so its models show none; Codex's
discovered models carry theirs in the name. A later step could learn what an alias resolves to
from a turn (the stream's `init` event names the model, e.g. `claude-haiku-4-5-20251001`).
`codex`: none built in, its list is discovered (below), and when that fails only the CLI's
default is offered; `gemini`: none yet (the CLI's default or a custom id). `modelSource` in `team.backends` says which list an entry carries:

- `discovered`: read from the installed CLI's own files. Implemented for Codex, which keeps the
  list its server sent in `$CODEX_HOME/models_cache.json` (default `~/.codex/`): entries with
  `visibility: "list"`, in `priority` order, the first one as the default. Only that file is
  read; the engine never opens the CLI's credentials.
- `catalog`: the built-in list (or the user override below).

Three escape hatches keep this from going stale:

- Discovery, where the CLI offers a source.
- The model combo ends with **Custom...**, a text field accepting `[A-Za-z0-9._:\[\]-]{1,64}`.
- `%APPDATA%/OwnEngine/backends.json` (user level) replaces a backend's models (which also
  turns discovery off for it), its executable or its image badge without rebuilding:
  `{"claude":{"executable":"C:/tools/claude.exe","supportsImages":true,"models":[{"id":"...","label":"...","default":true}]}}`.
  (`supportsImages` is `false` for Claude Code because it has no image-generation tool of its
  own; it can still write image files with code. Codex has a native one.)
  An invalid file (unknown backend id, bad model id, two defaults) is ignored as a whole with a
  warning in the log.

Changing the backend of an agent resets its model to that backend's default. A stored model that
is no longer in the catalog is kept and shown as custom.

### 4.3 Verified on the build host

2026-10-03, Windows 11, checked with `oe exec samples/Hello team.backends` and each CLI's
`--help`. Turns (T3) were run with Claude Code 2.1.288 and codex-cli 0.159.3 on a scratch project
through `oe exec <project> team.send {text, wait}` and, with an engine to attach to, through
`oe mcp <project> --port <p>` + `POST /api/call`:

- a new conversation and a resumed one for both CLIs (a second `oe exec` read `sessions.json`;
  both agents remembered the first question), two agents in parallel with `@all`;
- `--append-system-prompt-file` (Claude followed the agent's instructions), the standing context
  on stdin (Codex knew its teammates and saw the teammate's reply in the chat);
- tool activity: Claude `Read` -> "Reading project.json", MCP -> "Engine: scene.summary";
- attaching to the engine: both called `scene_summary` through the generated MCP configuration
  and reported the scene's 16 entities. Codex refuses engine tools without
  `default_tools_approval_mode='approve'` ("approval is required and the policy is never").

Not run with real CLIs: `read` and `full` access, a failing login, cancel, images.

| CLI | Version | Found as | Flags confirmed in `--help` |
|---|---|---|---|
| Claude Code | 2.1.288 | `~/.local/bin/claude.exe` (native) | `-p`, `--output-format stream-json`, `--verbose`, `--model`, `--mcp-config`, `--strict-mcp-config`, `--resume <id>`, `--permission-mode`, `--allowedTools`, `--input-format`. `--append-system-prompt` is listed; the `-file` form is only mentioned in another flag's description: check it in T3 |
| Codex | codex-cli 0.159.3 | `%APPDATA%/npm/codex.cmd` (npm shim) | `codex exec [PROMPT]` (stdin when omitted or `-`), `--json`, `-m`, `-C <dir>`, `-s read-only\|workspace-write\|danger-full-access`, `-c key=value`, `--skip-git-repo-check`, `-o <file>`; resume is `codex exec resume <session id> [PROMPT]` |
| Gemini CLI | 0.31.0 | `%APPDATA%/npm/gemini.cmd` (npm shim) | `-p`, `-o stream-json`, `-m`, `--approval-mode default\|auto_edit\|yolo\|plan`, `-r <session>` |

Detection of all three takes about 3.5 s in total (the npm shims start Node), which is why the
timeout is 10 s and why the editor must not call it on the UI thread.

## 5. Sessions, chat and state

### 5.1 Turn

A turn is one run of an agent's CLI in response to a message.

1. `team.send {text, from?}` appends the message to the chat and resolves recipients: every
   `@id`/`@name` mention (ASCII case-insensitive, the longest match, not inside a word such as
   an e-mail address), `@all` for everyone, otherwise the `lead`. The sender is never a
   recipient. An offline recipient gets a notice in the chat instead of a turn; a message that
   addresses nobody is stored with a notice saying so.
2. For each recipient not already busy, a turn starts (others wait in state `queued` behind
   `maxConcurrent`, in roster order; a message to a busy agent is delivered when its turn ends).
3. The prompt given to the CLI = the chat messages (kind `message`, not its own) since that
   agent's previous successful turn, bounded to the last 40 messages or 32 KiB, each as
   `[Sender] text`. The standing context is passed as the system prompt addition
   (`.oe/team/tmp/<id>.system.md`): the agent's name, description and instructions, the roster
   (ids, names and descriptions of teammates), the project path, where to save images
   (`.oe/team/images/`), and whether the editor is reachable through the `ownengine` MCP server.
4. The process runs in the project directory with the environment variables
   `OE_TEAM_AGENT=<id>` and `OE_TEAM_HOP=<n>` and, when the hosting tool serves the HTTP API
   (`oe editor`, `oe mcp --port`), an MCP server entry that starts `oe mcp --connect <port>`
   (`.oe/team/tmp/<id>.mcp.json` for Claude, `-c` overrides for Codex; `--agent <id>` is T5).
   Without an API port (`oe exec`, `oe mcp` on stdio only) the agent works on the files.
5. Stream lines become events (§5.3). The final assistant text becomes the agent's chat message;
   the backend's session id is stored in `sessions.json` (with the id of the last chat message
   the agent has seen) so the next turn resumes the same conversation, also after a restart. A
   failed turn drops the stored conversation, so a broken or expired one cannot fail every
   later turn; changing an agent's backend starts a new conversation.
6. `team.cancel {id}` (the Stop button) kills the process tree and drops the agent's waiting
   message; the chat gets a "stopped" notice. Closing the tool cancels all turns (the job
   object ends them even if the tool crashes).
7. A turn ends with an `error` message when the CLI reports a failure, exits without a result
   (the text carries the exit code and the end of its stderr), cannot be started, or runs
   longer than 30 minutes (`TeamHost::turnTimeoutSeconds`). If the result arrived but the
   process does not end within 5 s (something it started holds the pipes), the tree is killed.

`team.send {wait: seconds}` blocks the command until the addressed agents are done and adds
`replies` and `state` to the result. It is for one-shot use (`oe exec`): in a tool that serves
the API it also blocks the main thread, so an agent attached through MCP cannot reach the engine
while it waits. There, send without `wait` and poll `team.state`.

Agent-to-agent: when an agent's final message mentions a teammate, that teammate gets a turn
(and sees the message, like any chat message since its last turn). Every message has a `hop`:
0 for the person's, and for a reply one more than the newest message its turn answered. A reply
is handed on only while its hop is at most `maxHops`, so one message of the person is followed
by its first turn plus at most `maxHops` agent-to-agent turns per branch; beyond that the
mention is shown but not delivered, with a notice naming who was not called. `maxHops: 0`
turns the feature off. This bounds loops and cost. The agent itself and offline teammates are
skipped (an offline one with a notice). Because every `@id` in a reply calls someone, the
standing context asks agents to write a name without `@` when they only talk about a teammate.

Parallel agents share one working tree. Engine edits are already serialized through the
main-thread job queue and undo; file edits made directly by two agents can collide. The first
version documents this and relies on instructions that split ownership; per-agent git worktrees
are listed under Later.

### 5.2 Chat message

One JSON object per line in `chat.jsonl`:

```json
{"id":128,"time":"2026-10-03T14:02:11Z","from":"mina","to":["user"],"kind":"message",
 "text":"Added double jump. script.check is clean.","attachments":[{"type":"image","path":".oe/team/images/jump.png","width":512,"height":512}],
 "turn":{"seconds":42,"tools":9,"costUsd":0.08}}
```

`from` is `user`, an agent id, or `system` (a notice about no agent in particular). `to` lists
the addressed agent ids; a reply is addressed to the sender of the newest message it answered.
`kind`: `message` | `notice` (stopped, offline, nobody addressed; later hop limit, permission
denied) | `error` (with `hint`). Tool activity is **not** stored as messages; it is live state
(§5.3) plus the per-turn summary `turn {seconds, tools, costUsd?, steps?}` on the reply, where
`steps` are the activity lines of the turn (at most 100) and `costUsd` is what the CLI reported
(Claude only). A reply is cut at 64 KiB, a sent message is at most 32 KiB. The log is trimmed to
the last 2,000 messages when the project opens. `team.clear` empties it and drops the stored
sessions; message ids keep counting within the session so a poller never sees an id twice.

### 5.3 Agent state (R8)

`team.state` returns `{revision, running, agents: [{id, state, activity, task, elapsed, tools,
error?, hint?}]}`; the panels poll it every frame and redraw when `revision` changed (it covers
the roster, the states and the chat; `elapsed` alone does not bump it). Every team command
handles pending agent output first, so polling is also what advances turns in a tool that does
not call the team update itself.

| State | Meaning | Dot |
|---|---|---|
| `offline` | backend not installed or unavailable | grey, hollow |
| `idle` | ready | grey |
| `queued` | has a message, waiting for a free slot | blue |
| `thinking` | turn running, the model is generating | amber, pulsing |
| `working` | turn running, a tool call is in progress | green, pulsing |
| `error` | last turn failed (kept until the next turn or dismissal) | red |

Next to the state: `activity` (one line describing the current step), `task` (the first line of
the message that started the turn), `elapsed`, and `tools` (count this turn). `activity` comes
from the event stream:

| Stream event (Claude `stream-json` / Codex `--json`) | Activity text |
|---|---|
| assistant text / reasoning item | "Thinking" |
| `tool_use` Read, Grep / file read | "Reading scripts/player.lua" |
| `tool_use` Edit, Write / `file_change` | "Editing scripts/player.lua" |
| `tool_use` Bash / `command_execution` | "Running `build.bat`" (truncated to 80 characters) |
| MCP call to `ownengine` | "Engine: component.set Player" |
| image generation item | "Generating an image" |
| `result` / `turn.completed` | turn ends: `idle` or `error` |

Calls that reach the editor over `oe mcp --connect --agent <id>` carry the agent id (the
optional `agent` field of the `/api/call` request body, an agent id `[a-z0-9_-]{1,32}`; anything
else is ignored), which `Engine::PostCall` keeps for the remote-call observer
(`Engine::RemoteCallAgent()`). The editor's notice then reads "Mina: component.set Player" with
the agent's color (derived from its id) as the notice border and as the Hierarchy dot, instead
of "API: ...". An id that is not in the team, and a request without the field, behave exactly as
before. The generated MCP configuration of every turn passes `--agent <id>`.

### 5.4 Images (R7)

An agent's image reaches the chat through the **files** route: `.oe/team/images/` and the
project's `assets/` are listed (path, modified time; at most 4,000 files) when a turn starts and
when it ends; new or changed `png`/`jpg`/`jpeg` files (what stb_image decodes) become
attachments of that agent's reply, at most 16, each claimed by one reply when turns run in
parallel. The standing context tells agents to save pictures in `.oe/team/images/`. The
**stream** route of the first design (an event naming an image file) is not implemented: no
recorded stream contains such an event.

The person's files reach the agents as attachments of a message (`team.send {attachments}`, at
most 8, 32 MiB each): a file inside the project keeps its path, a file from elsewhere is copied
to `.oe/team/attachments/<message id>-<n>-<name>` (name reduced to `[A-Za-z0-9._-]`). Each
message's attachments are listed under it in the prompt as `Attached image: <path>` /
`Attached file: <path>`; the agent opens them itself. A message may consist of attachments only.

An attachment is `{type: "image"|"file", path, width?, height?}` with a project-relative path;
entries of `chat.jsonl` whose path is absolute or contains `..` are dropped when loading.

The chat shows an image attachment as a thumbnail (at most 16 x 11 text heights, decoded once,
bounded 64-entry texture cache shared with profile pictures) and other files as buttons.
Clicking a thumbnail opens the viewer window (fit / actual size, checkerboard behind alpha,
size and path). Buttons: **Import to assets** (`asset.import {source}`, for files outside
`assets/`), **Show in folder**, **Copy path**. A picture over 16 MiB or 8192 px is listed by
name without a thumbnail. Whether a backend can generate images is the descriptor's
`supportsImages` (reported by `team.backends`); the profile dialog does not show it (decided
2026-10-03). The installed CLI's version appears in the agent combo's list, not beside the
combo: text there changed the dialog's width whenever another agent was chosen.

## 6. Safety

- **Process arguments**: argv arrays, no shell strings, no user text in arguments (§2.1).
- **Access levels** map to the CLIs' own permission systems; the engine adds no sandbox itself.

  | `access` | Claude Code | Codex | Meaning |
  |---|---|---|---|
  | `read` | `--permission-mode plan` | `sandbox_mode='read-only'`, engine tools not approved | reads and answers, no edits |
  | `edit` (default) | `--permission-mode acceptEdits` + `--allowedTools mcp__ownengine` | `sandbox_mode='workspace-write'` + engine tools approved | edits files in the project and drives the editor; other shell commands are refused |
  | `full` | `--permission-mode bypassPermissions` | `sandbox_mode='danger-full-access'` + engine tools approved | anything; the dialog shows a warning and asks for confirmation when selected |

  A headless turn cannot ask for permission, so a refused action appears as a `notice` in the
  chat. Interactive approval (the CLI's permission-prompt hook surfaced as chat buttons) is
  listed under Later.
- **Who may spawn**: `team.send`, `team.add/update/remove` are commands, so anything on
  127.0.0.1 can call them, the same trust boundary as the rest of the API. Recursion is bounded:
  a `team.send` that carries a team agent's id (the `agent` field `oe mcp/exec --connect --agent`
  adds) is posted as that agent with its turn's hop + 1 and counts against `maxHops`, and
  `maxConcurrent` caps processes. A caller that leaves the field out is the person (hop 0):
  the field is attribution, not authentication.
- **Secrets**: the engine stores no keys or tokens; each CLI uses its own login. The child
  inherits the parent environment. Chat logs and prompts stay in `.oe/team/` (git-ignored).
- **Paths**: avatars and attachments are written only inside `.oe/team/`; attachment paths are
  resolved with `Engine::ResolvePath` and rejected outside the project.
- **Data from agents is untrusted**: stream lines are length-limited (1 MiB) and parsed with
  `Json`; unknown event types are ignored; text is rendered as text, never executed.

## 7. Editor

Two panels in `EditorTeam.cpp`, toggled under View, docked by default with Team beside the
Hierarchy tab and Team Chat beside Console. All strings through `Tr()` with Korean and Japanese
rows.

### 7.1 Team panel (R9, R8)

```
┌ Team ─────────────────────────────── [+ Add] ┐
│ (●) Mina      Claude · sonnet                │
│  🦊  Editing scripts/player.lua       0:42 ■ │
│ (●) Jun       Codex · gpt-…                  │
│  🦉  Idle                                    │
│ (○) Sora      Gemini · not installed         │
└──────────────────────────────────────────────┘
```

A row: picture with status dot, name, backend and model, the activity line, elapsed time and a
stop button while a turn runs. Click = select (T5: the chat input gets `@name `); double-click or
right-click > Edit opens the profile dialog; right-click > Delete asks for confirmation (the chat
history is kept, the agent's session and uploaded picture are removed; Enter confirms, Escape
cancels); drag a row onto another to reorder (`team.move`); "Lead" after the name marks the lead
(right-click > Make Lead); right-click > Stop cancels a turn. The row's tooltip shows the
description and, for an offline or failed agent, the reason and the hint. An empty team shows
"Add your first agent". The panel is a tab beside the Hierarchy (View > Team); it exists only in
a tool that registered the `team.*` commands, and an old `editor.ini` gets the tab without a
layout reset.

### 7.2 Profile dialog (R1–R5)

```
┌ Agent ─────────────────────────────────────────────┐
│  ( 🦊 )   Name         [ Mina                    ] │
│  Change   Description  [ Gameplay programmer…    ] │
│           Agent        [ Claude Code          ▾ ]  │
│                          Claude Code  2.1.x        │
│                          Codex        0.52.0       │
│                          Gemini  (not installed)   │  <- disabled, tooltip = install hint
│           Model        [ sonnet               ▾ ]  │
│           Access       [ Edit project         ▾ ]  │
│  Instructions                                      │
│  ┌──────────────────────────────────────────────┐  │
│  │ Work only in scripts/ and scenes/. …         │  │
│  └──────────────────────────────────────────────┘  │
│                    [Refresh agents] [Cancel] [Save]│
└────────────────────────────────────────────────────┘
```

- **Change** opens the picture picker: the preset grid plus **Upload...**.
- **Agent** combo: installed backends are selectable and show their version; the others are
  drawn disabled (`ImGuiSelectableFlags_Disabled`) with "(not installed)" and a tooltip carrying
  the install command. If none is installed the dialog explains how to install one and Save
  stays enabled, so a team can be prepared first (the agent is then `offline`).
- **Model** combo lists the selected backend's catalog and **Custom...**; it is disabled while
  the backend is not installed.
- **Access** combo: Read only / Edit project / Full access; choosing Full access asks first.
- Save (or Enter in the Name field) calls `team.add` or `team.update`, then `team.avatar` for
  an uploaded picture; nothing is stored before that, and Cancel or Escape discards the form. A
  refused save keeps the dialog open with the command's message and hint above the buttons.

### 7.3 Team Chat panel (R6, R7)

```
┌ Team Chat ───────────────────────────────────────────┐
│ 🙂 You                                        14:01  │
│    @Mina add a double jump to the player             │
│ 🦊 Mina  ▸ 9 steps · 42 s                     14:02  │
│    Added double jump. script.check is clean.         │
│ 🦉 Jun                                        14:05  │
│    Here is the jump dust sprite.                     │
│    ┌────────┐                                        │
│    │ image  │  dust.png 512x512  [Import to assets]  │
│    └────────┘                                        │
│ 🦊 Mina is editing scripts/player.lua…  ■ Stop       │
├──────────────────────────────────────────────────────┤
│ [ @Mina …                                    ] [Send]│
└──────────────────────────────────────────────────────┘
```

- Input: multi-line, Enter sends, Shift+Enter is a new line. While a word that starts with `@`
  or `#` is being typed, a list floats above the input (always on top, also over a short
  panel): `@` lists the agents it could mean (picture, name, id, role, Offline / Thinking) and
  `all`; `#` lists the project files whose name starts with, or whose path contains, the text
  (thumbnail, name, path; up to 300). Eight rows show and the rest scroll. Up / Down move the
  choice (wrapping; the caret does not move while the list is open), the pointer chooses too,
  and Tab or a click takes the chosen entry: `@` becomes `@id `, `#` becomes the file's path.
  Offline agents are listed disabled. The chosen file is previewed beside the list: the
  picture, the asset thumbnail of a model, material or prefab, or the first 14 lines of a
  script or text file.
- References are tinted, in the input while typing and in the messages: an `@mention` in the
  agent's color (`@all` in the accent color), a path of an existing project file in green.
  It shows what will be understood as a reference before the message is sent.
- Files dropped on the panel are attached to the next message and appear as buttons above the
  input (click removes one; pointing at one previews a picture). A file dropped anywhere else
  is imported into the project, as before.
- A running or queued turn shows as a live line above the input: picture, Stop, then
  "Name: activity..." (Stop comes first so the changing text does not move it). When the turn
  ends the agent's message appears with a "N steps, T s" button that expands to the turn's
  tool list.
- Messages render plain text with fenced code blocks in the monospace font; project scripts and
  scenes named in a message (`scripts/x.lua`, `scenes/x.scene.json`, existing files only) get a
  button under the text that opens them. Notices are dimmed lines; errors are red with their
  hint. Times are shown on the PC's clock. The panel keeps the newest 1,000 messages.
- Auto-scroll sticks to the bottom unless the user scrolled up. The tab title counts messages
  that arrived while the tab was hidden ("Team Chat (2)").
- The panel is a tab beside the Console (View > Team Chat); an old `editor.ini` gets it without
  a layout reset.

`oe editor <project> --screenshot f.png --team` brings the Team panel forward for the headless
check, `--chat` the Team Chat (`--agent new` or `--agent <id>` also opens the profile dialog); a seeded `.oe/team/` in a
`TempProject` gives tests a fixed roster and log.

## 8. Commands

All in area `team`, registered only by the `oe` tool. None is `mutates` (the team is not part of
the scene, so no undo entry and no scene revision); team changes bump the team revision.

| Command | Params | Result |
|---|---|---|
| `team.backends` | `{refresh?, async?}` | array of backends: status, version, path, message, models, model source, image support, install hint |
| `team.presets` | | avatar preset ids |
| `team.list` | | `{version, revision, lead, maxConcurrent, maxHops, agents: [profile], error?}` |
| `team.get` | `{id}` | one profile: every field, plus `avatarPath` (project-relative) for an uploaded picture |
| `team.add` | `{name, description?, instructions?, backend?, model?, access?, avatar?}` | the new profile (id generated; backend defaults to the first one, model to that backend's default) |
| `team.update` | `{id, values}` | the profile; partial update like `component.set`. A new backend without a model resets the model |
| `team.remove` | `{id}` | `{removed: id}`; removes profile and uploaded picture (T3: also cancels its turn and drops its session) |
| `team.move` | `{id, index}` | the team (as `team.list`) with the agent at position `index` (0 = first, clamped to the end) |
| `team.avatar` | `{id, preset}` or `{id, source}` | the profile |
| `team.settings` | `{lead?, maxConcurrent?, maxHops?}` | team settings; without arguments it only reads |

Every `id` argument accepts the agent's id or its name.
| `team.send` | `{text, attachments?, from?, wait?}` | `{message, started: [ids], queued: [ids], offline: [ids]}`; with `wait` also `replies` and `state` |
| `team.cancel` | `{id}` or `{all:true}` | `{cancelled: [ids]}` |
| `team.state` | | `{revision, running, agents}`: per agent state, activity, task, elapsed, tools, error and hint |
| `team.messages` | `{after?, limit?}` | `{messages, first, last}`: with `after`, the oldest messages with a larger id; without, the newest `limit` (default 100, at most 500). `first`/`last` are the oldest and newest ids in the log (`first` 0 = empty), so a viewer can drop what was cleared |
| `team.clear` | | stops every turn, empties the chat log and stored sessions |

Errors follow the usual shape, with hints that say how to fix the call, for example
`backend_not_installed` ("Install it with `npm i -g @openai/codex`, then `team.backends
{refresh:true}`"), `unknown_agent`, `agent_busy`, `team_full`, `invalid_model`. The profile
commands use `unknown_agent`, `name_taken`, `team_full`, `unknown_backend`, `invalid_model`,
`unknown_preset`, `invalid_image`, `not_found`, `team_file_invalid`, `write_failed` and
`invalid_argument`.

## 9. Tests

- **Parsers** (`TeamStreamParsers`): recorded Claude and Codex stream lines as fixtures in
  `tests/tests.cpp`; each must produce the expected events, activity parts, final text and
  session id; truncated, foreign and unknown lines give no event. Also the two command lines
  (no double quote, percent or exclamation mark in any argument) and mention parsing.
- **TeamStore** (`TeamStoreProfiles`, `TeamAvatars`): add / update / remove, id and name
  uniqueness, limits, defaults left out of the file, unknown backend and keys kept loading,
  invalid file never overwritten, a second session sees changes, avatar crop and resize
  (pixels checked), refused pictures, upload removed with its agent.
- **TeamSession with stand-in backends** (`TeamSessionTurns`): backends whose executable is a
  `.cmd` script that saves its prompt and arguments and prints a recorded Claude stream (no
  `oe fake-agent` subcommand was needed: a `Backend` is injected through
  `RegisterTeamCommands(registry, backends)`). Covers routing (mention, `@all`, lead, sender
  excluded, nobody), prompt content and resume, context on stdin, the MCP config file, state and
  activity of a running turn, queueing behind a busy agent and behind `maxConcurrent`, cancel,
  process crash (`error` state with the stderr tail), a failure the CLI reports, offline agents,
  the turn timeout, `wait`, removal of a running agent, restart and `team.clear`. Hop limit and
  images come with T7 and T6.
- **Process** (`ProcessRunsChildren`): start, write stdin, read lines from both pipes, exit code,
  environment and working directory, kill tree, missing executable; `.cmd` shim start with
  quoted arguments and refusal of unsafe ones on Windows.
- **Backends** (`TeamBackendParsing`, `TeamBackendDetection`): version and Codex model cache
  parsing, overrides file, detection of stand-in `.cmd` CLIs (installed, failing, hanging,
  missing) and the `team.backends` cache and refresh.
- **Editor**: `NativeEditorTeam` drives the Team panel with window events: the tab beside the
  Hierarchy, background detection, adding an agent through the dialog (typed name, Enter), a
  backend that is not installed cannot be chosen, a refused save keeps the dialog open, Escape
  discards, editing keeps untouched fields, delete asks first; it writes a screenshot per
  language and one of the dialog to `build/editor-team-*.png`. `EditorAvatarPresets` checks the
  embedded pictures. `EditorTranslations` covers the new strings. Sending a message and
  stopping a turn from the UI come with the chat panel (T5).
- Real CLIs are not run by `oe_tests` (network, cost, login). T1 and T3 are checked by hand with
  the installed CLIs and the result is recorded in the work log.

## 10. Decisions

Settled on 2026-10-03; the sections above already describe these choices.

1. **Where the team is stored**: per project and local, `.oe/team/` (git-ignored). A committed,
   shared team file was rejected for the first version; chat and images are local in any case.
2. **Default access level**: `edit`. `full` is the only level in which an agent can run
   `build.bat` unattended, so agents meant to build and test are set to `full` by the user until
   interactive permission prompts exist.
3. **Preset art**: 12 embedded PNGs made for the engine. The procedural disc with an initial
   stays as the fallback only.
4. **Message with no mention**: goes to the lead.

## 11. Usage and cost (to do)

Written on 2026-10-04 after the first real use (the Wickbound team: Core on Opus, Content and QA
on Sonnet, Art on Codex). The person's observation: the team uses clearly more than a single
agent told "core work on Opus, the rest on Sonnet, images through Codex". Nothing in this
section is implemented; it records what was measured and what to do when the work is taken up.

### 11.1 Measured

One turn that only answers "ok" (Claude Code 2.1.288, `--model haiku`, this PC's own setup, a
scratch folder; input side = new + cached input tokens of the turn):

| How the turn is started | Input-side tokens | Cost | Loaded |
|---|---|---|---|
| As the team starts it today | ~27,000 | $0.056 | 116 tools, 120 skills, 7 MCP servers |
| + `--mcp-config <empty> --strict-mcp-config` | ~22,900 | $0.034 | 31 tools |
| + `--disable-slash-commands` | ~19,000 | $0.027 | 30 tools, 0 skills |
| `--bare` | not usable | - | "Not logged in": needs an API key, not the subscription login |

So a turn costs 19,000 to 27,000 tokens before it does anything, and the person's own MCP
servers and skills ride along on every team turn. Real turns seen while testing: $0.03 to $0.20
each on haiku. Codex reports tokens but no cost (31,220 input tokens for a one-command turn,
27,904 of them cached).

### 11.2 Why a team costs more than one delegating agent

1. **The base context is paid per turn and per agent.** Every turn is a new CLI process
   (`--resume` continues the conversation, and the prompt cache gives a discount, but the
   context is still sent). One agent pays it once per session and starts sub-agents only when
   it needs them, with small prompts.
2. **A handoff is a full turn in another conversation.** The receiver knows nothing of what the
   sender read and reads the files again.
3. **The chat is pushed into every turn**: all messages since the agent's last turn, up to 40
   or 32 KiB (§5.1), whether it needs them or not.
4. **The lead is the most expensive model.** Every message without a mention goes to it, also
   when all it does is pass the work on.
5. **Agent-to-agent turns are on by default** (`maxHops` 4), and every `@id` in a reply calls
   someone.

Keeping the CLI processes alive between turns would save start-up time and hook runs, but
hardly any tokens: the conversation is resent on every request either way.

### 11.3 What Buzz does (reference)

The feature was inspired by [Buzz](https://github.com/block/buzz) (Block). Read on 2026-10-04
from its README, `VISION_AGENT.md` and the `crates/buzz-acp` README through a summarizing
fetch, not from the code, so details may be off. It documents no token budgets and no model
routing; it spends less by structure:

| Buzz | Effect | OwnEngine today |
|---|---|---|
| Only an `@mention` wakes an agent | no turn for unrelated messages | same, but no mention = the lead |
| `--respond-to owner-only` by default: only the owner's messages reach the agent | agents do not wake each other unless that is switched on | the opposite: handoff is on |
| Queued messages of a channel are drained into one prompt | three messages, one turn | same (messages that arrive during a turn are delivered together) |
| Context is pulled: the agent reads history through the CLI / search when it needs it | no tokens for history nobody reads | the opposite: pushed (11.2 point 3) |
| Long-lived agent processes over ACP, a session per channel | no start-up per turn | a process per turn with `--resume` |
| A session summarizes its own history when the context fills (buzz-agent) | bounds a long conversation | left to Claude Code / Codex, which compact by themselves |

### 11.4 Options, by expected effect

| # | Change | Where | Effect, cost |
|---|---|---|---|
| U1 | Agent-to-agent turns off unless switched on: `maxHops` default 0 (or keep it on with a lower default, 2) | `TeamSettings`, `team.settings`, docs | The largest: each avoided hop is a whole turn. Changes the behaviour introduced in T7a, so it needs the person's decision (open) |
| U2 | Pull instead of push: a turn gets the messages addressed to the agent and the one that handed the work on; the rest it reads with `team.messages` | `TeamSession::StartTurn`, standing context | Fewer input tokens per turn, most in busy chats. An agent may miss context it would have needed and must be told how to fetch it |
| U3 | Lean turns: always `--strict-mcp-config` (the engine's server only, also without an editor) and `--disable-slash-commands`; a per-agent tool list by role (QA: engine tools only) | `ClaudeBuildTurn`, a `tools` field on the profile; the Codex equivalent to be looked up | Measured: about 30 % fewer tokens and about half the cost of an idle turn. Team agents lose the person's own skills and MCP servers; make it a team setting if that is not wanted |
| U4 | A cheap lead: route unmentioned messages to a Sonnet agent and call the Opus one by name; tell the lead to do small jobs itself instead of planning and handing on | team configuration and instructions (Wickbound: `.oe/team/team.json`), no engine change | Opus only where it is asked for |
| U5 | Budgets: `--max-budget-usd` per turn for Claude (the flag exists, `-p` only); a per-chain cap; `--effort low` for QA and content agents | profile fields, `ClaudeBuildTurn` | A hard stop instead of a surprise |
| U6 | Show it: tokens and cost per turn, per agent and per day in the Team panel and on each reply; Codex gives tokens only | `TurnEvent` (usage), `ChatMessage.turn`, `team.state`, the panels | Saves nothing by itself, but shows where it goes. Today only Claude's `costUsd` is kept, and whether that number is per turn or cumulative for a resumed conversation is still unverified (T3) |

Suggested order when resumed: U3 + U2 + U6 in the engine, U4 for the Wickbound team at the same
time, U1 once decided, U5 last.

### 11.5 A limit that stays

Even with all of the above a handoff costs at least one base context (about 19,000 tokens with
U3). A team pays off for parallel or long work; for a small change one agent is always cheaper.
The guidance for users belongs in the READMEs once the numbers after U2/U3 are known.
