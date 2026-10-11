# Agent team

Status: **in progress.** This document is the design of the feature; the work log (docs/DESIGN.md §5) is
[TEAM_LOG.md](TEAM_LOG.md). Only the milestones ticked there exist in the code; everything else is design.

An agent team is a set of named AI agents that live in a project. Each has a profile (picture,
description, instructions) and runs on a coding-agent CLI installed on the user's PC (Claude Code,
Codex, ...) with a chosen model. The editor shows the roster, lets the user add, edit and remove
agents, and has a team chat where the user calls agents and gives them work. While an agent works,
the editor shows what it is doing; images an agent produces appear in the chat.

The engine does not talk to any model provider itself. It starts the CLI the user already
installed and logged in to, as a child process, and reads its machine-readable event stream. The
agent drives the open editor through the existing path: `oe mcp --connect <port>`.

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
| `canCall` | callable agent ids; default none. The lead may delegate to everyone; other agents need explicit permission |
| `maxBudgetUsd` | Claude per-turn allowance, 0 = disabled (omitted on disk), otherwise 0.000001..1000 USD. Unsupported on other backends |
| `lead` | agent that receives messages with no mention; empty = such messages reach nobody. The first agent added to an empty team becomes the lead; removing the lead leaves it empty |
| `maxConcurrent` | 1..8, default 3 |
| `maxHops` | 0..16, default 0 (off); a blocked handoff notice gives `team.settings {maxHops: 2}` |
| `inheritUserTools` | false by default; opt in to user MCP servers and skills. Codex user-config isolation is partial |
| `rotateAboveInputTokens` | 0 disables automatic conversation rotation (default); otherwise 1..2,000,000 input tokens |

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

### 3.3 The default team of a new project

`oe new <dir>` writes `.oe/team/team.json` with five agents (`engine/team/TeamDefaults.cpp`,
`SeedDefaultTeam`; `--no-team` skips it; a project that already has agents is never touched).
An existing project without agents (made before teams, or with `--no-team`) gets the same team
from `team.add_defaults` or the **Add the default team** button of the empty Team panel:

| Agent | CLI / model | Role |
|---|---|---|
| Lead (the lead) | Claude, `sonnet` | gets messages without a mention; small changes itself, larger ones through `team_delegate` with checkable acceptance lines |
| Core | Claude, `opus` | hard gameplay code, rules, balance; reached by delegation or by name |
| Content | Claude, `sonnet` | scenes, prefabs, UI, texts |
| Art | Codex, default model | images and sound |
| QA | Claude, `sonnet` | play-tests through the editor, edits nothing |

All have access `edit` (no escalation), `maxHops` stays 0, and `rotateAboveInputTokens` is 150000 so
a long chat conversation starts fresh. The instructions are project-neutral (they point to the
project's `AGENTS.md`) and say how to work with delegation: tasks end with `team_report` and evidence,
an `@mention` does not hand work on. An agent whose CLI is not installed shows as offline.

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
- Login: an installed CLI is then asked whether the user is signed in: `claude auth status`
  (JSON `loggedIn`, `authMethod`, `subscriptionType`), `codex login status` (exit 0 and
  "Logged in using ChatGPT"); Gemini CLI has no such command, so a Google sign-in is
  `~/.gemini/oauth_creds.json` and an API key `GEMINI_API_KEY` / `GOOGLE_API_KEY`. The entry gets
  `login` (`loggedIn` | `loggedOut` | `unknown`), `loginDetail` ("claude.ai (pro)", "ChatGPT")
  and `loginHint` (the command that signs in). A login failure that slips through still appears
  on the first turn as an error message with the hint to run the CLI once in a terminal.
- `engine`: how the ownengine MCP server came up in the CLI's latest turn this session
  (`{status: connected | failed | pending | ..., time, agent}`, from Claude's `init` event),
  absent before the first turn. `runsTurns: false` marks a CLI that is detected but has no turn
  adapter yet (Gemini CLI).
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

2026-10-03, Windows 11: Claude Code 2.1.288 (native `claude.exe`), codex-cli 0.159.3 and Gemini CLI
0.31.0 (npm `.cmd` shims). Turns were run by hand through `oe exec <project> team.send {text, wait}`
and through `oe mcp <project> --port <p>` + `POST /api/call`: new and resumed conversations for both
CLIs, two agents in parallel with `@all`, `--append-system-prompt-file`, the standing context on stdin
for Codex, tool activity lines, and attaching to the engine through the generated MCP configuration
(Codex refuses engine tools without `default_tools_approval_mode='approve'`). Not run with real CLIs:
`read` and `full` access, a failing login, cancel. Detection of all three takes about 3.5 s (the npm
shims start Node), hence the 10 s timeout and the rule that the editor never calls it on the UI thread.

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
3. The prompt given to the CLI contains unread messages addressed to that agent (including
   the triggering message), bounded to the last 40 messages or 32 KiB, each as
   `[Sender] text`. The standing context is passed as the system prompt addition
   (`.oe/team/tmp/<id>.system.md`): the agent's name, description and instructions, the roster
   (ids, names and descriptions of teammates), the project path, where to save images
   (`.oe/team/images/`), and whether the editor is reachable through the `ownengine` MCP server.
   Unrelated chat stays in the log. With an attached API, an omission count points to
   `team_messages` for context on demand; without an API that tool hint is omitted.
4. The process runs in the project directory with the environment variables
   `OE_TEAM_AGENT=<id>` and `OE_TEAM_HOP=<n>` and, when the hosting tool serves the HTTP API
   (`oe editor`, `oe mcp --port`), an MCP server entry that starts `oe mcp --connect <port>`
   (`.oe/team/tmp/<id>.mcp.json` for Claude, `-c` overrides for Codex; `--agent <id>` is T5).
   Without an API port (`oe exec`, `oe mcp` on stdio only) the agent works on the files.
   Claude defaults to `--strict-mcp-config --disable-slash-commands`, using an empty
   MCP config when unattached. Codex defaults to `--ignore-user-config` and
   `-c skills.include_instructions=false`; discovered project/system skills may remain.
   `inheritUserTools:true` omits these restrictions. Never mention a teammate merely
   to accept, acknowledge receipt, make small talk or talk about them.
5. Usage is recorded atomically in `.oe/team/usage.jsonl`, with agent/backend/session,
   UTC date, input/output/cache tokens and per-turn cost. Claude reports session-cumulative
   cost: the persisted prior result is subtracted. Missing baselines/counters and Codex
   costs remain unknown. Failed/cancelled turns are included; clearing chat retains usage.
   Legacy replies with no known cost scope show unavailable rather than a cumulative bill.
   Stream lines become events (§5.3). The final assistant text becomes the agent's chat message;
   the backend's session id is stored in `sessions.json` (with the id of the last chat message
   the agent has seen) so the next turn resumes the same conversation, also after a restart. A
   failed turn drops the stored conversation, so a broken or expired one cannot fail every
   later turn; changing an agent's backend starts a new conversation.
   `team.reset {id}` clears only that idle agent's conversation. It preserves the chat,
   seen watermark, usage, profiles and other agents; busy/queued agents must be cancelled
   or allowed to finish first. The roster context menu provides Reset Conversation.
   With `rotateAboveInputTokens` enabled, the next addressed turn starts fresh when
   the previous input-size estimate meets the threshold, and appends a notice.
   Claude uses the last reported API iteration input (including caches); otherwise
   the whole turn input is a conservative estimate. This is not the context-window
   limit. Unknown usage cannot trigger rotation. The estimate survives reopening.
   Rotation makes no model call of its own and may discard useful context or lose
   cache savings, so it is disabled by default. Invalid sessions files are preserved.
6. `team.cancel {id}` (the Stop button) kills the process tree and drops the agent's waiting
   message; the chat gets a "stopped" notice. Closing the tool cancels all turns (the job
   object ends them even if the tool crashes).
7. A turn ends with an `error` message when the CLI reports a failure, exits without a result
   (the text carries the exit code and the end of its stderr), cannot be started, or runs
   longer than 30 minutes (`TeamHost::turnTimeoutSeconds`) or has no stdout/stderr
   output for 10 minutes (`TeamHost::idleTimeoutSeconds`). Any output resets only the idle timer. If the result arrived but the
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
mention is shown but not delivered, with a notice naming who was not called and explaining
`team.settings {maxHops: 2}`. For targets the sender may call (`canCall`, or every teammate
for the lead), the notice also says to hand work off by calling `team_delegate`; mentions
are not delivered. Targets outside that permission get no delegation advice. The same
notice applies to agent-attributed `team.send` calls. Notices are visible through
`team.messages` and the chat UI, but excluded from subsequent turn prompts; there is no
automatic retry or conversion to a task. `maxHops: 0` turns the feature off.
This bounds loops and cost. The agent itself and offline teammates are
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

### 5.5 Task turns

Chat turns tell the lead and agents with `canCall` to call `team_delegate` rather than answer with an `@mention` when `maxHops` is 0; this instruction follows the stable prefix and is omitted when mentions are enabled or the worker has no delegation permission.

Delegation runs beside chat. Workers receive only role/project context and the
bounded task brief, never chat history or a resume id; their final text/session
never enters the chat conversation. `team_delegate` in the MCP bridge waits on
its I/O thread while the engine/editor continues updating. The worker must call
`team_report` to submit summary, changed paths and command/result/output evidence;
printing JSON in the final answer does not submit a report. A successful turn
without a tool report becomes done with unverified:true and its final text as
summary. A report is also unverified until the caller checks acceptance.
CLI errors/abnormal exits are failed; absolute deadline expiry is timed_out.
Cancellation propagates down the caller's task tree; there is no automatic retry.
Tasks cannot extend an ancestor deadline. Chat and task usage share the E5 ledger,
including failures/cancellations and per-session Claude budget accounting.

### 5.6 Stable prefix

BuildTeamStandingContext is the single prefix builder for chat and task turns.
It contains role, instructions, sorted callable agent ids/names and project facts.
Runtime state, connection details, time, counts, unread chat and task requirements
follow the prefix. The callable list follows canCall permissions (the lead may
call everyone). State/history changes leave identical prefix bytes. Changing role,
instructions, callable agents or project facts intentionally changes the prefix.

## 6. Safety

- **Process arguments**: argv arrays, no shell strings, no user text in arguments (§2.1).
- **Access levels** map to the CLIs' own permission systems; the engine adds no sandbox itself.

  | `access` | Claude Code | Codex | Meaning |
  |---|---|---|---|
  | `read` | `--permission-mode plan` | `sandbox_mode='read-only'`, engine tools not approved | reads and answers, no edits |
  | `edit` (default) | `--permission-mode acceptEdits` + `--allowedTools mcp__ownengine Bash(oe exec:*) Bash(oe script:*) Bash(oe render:*)` (and the same for PowerShell) | `sandbox_mode='workspace-write'` + `sandbox_workspace_write.network_access=true` + engine tools approved | edits files in the project and drives the editor (engine tools, or `oe exec --connect <port>` from the shell); other shell commands are refused |
  | `full` | `--permission-mode bypassPermissions` | `sandbox_mode='danger-full-access'` + engine tools approved | anything; the dialog shows a warning and asks for confirmation when selected |

  A headless turn cannot ask for permission, so a refused action appears as a `notice` in the
  chat. Team agents use their profile's `access`; direct conversations (`team.ask`: Agent Chat,
  Ask bubbles) use the team setting `askAccess` (default `edit`, Settings > Agents).
- **Engine tools and the shell fallback**: each turn gets the editor's MCP server
  (`<oe> mcp --connect <port> --agent <id>`). When a CLI starts without its tools (2026-10-09:
  Claude Code with Opus dropped the whole server because a few command schemas named the types
  `entity` and `bool`, which JSON Schema does not have; fixed in `Params::Prop`, checked by
  `CommandSchemasUseJsonSchemaTypes`), the prompt names `oe exec --connect <port> <command>
  '<json>'`, oe's folder is appended to the turn's `PATH` (appended: it never shadows system
  commands) and edit access allows exactly `oe exec`, `oe script` and `oe render`. The turn's
  steps then show "Engine MCP server: failed" and a notice follows the reply. Interactive approval (the CLI's permission-prompt hook surfaced as chat buttons) is
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
- Messages render as Markdown (`EditorMarkdown.h`, drawn in `EditorTeamChat.cpp`): headings,
  bullet/numbered/task lists with nesting, quotes, rules, pipe tables, fenced code on a dark well
  with its language (right-click copies it), and inline bold, italic, `code`, strike and links.
  There is no bold/italic face: bold is a doubled stroke, italic a softer tone. Links open http(s)
  pages in the browser and project scripts, scenes, effects and materials in their editor; other
  schemes are ignored. @mentions and file references keep their tinted chips. Project scripts and
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

### 7.4 Asking about an object (Scene bubbles)

Right-click in the Scene view (a click, not a fly/pan drag; also on the gizmo, which only takes
the left button). The menu lists two targets, each a submenu with **Ask the Team** and **Ask the
Agent** (beside it the name of the CLI that answers). The agent is not chosen here: it is the one
of Settings > Agents (§7.6), or the first installed CLI that can run turns while none is set:

- the **object** under the pointer (choosing selects it; the right click alone does not), and
- **this spot**: the point on the surface under the pointer (`render.pick` returns it with the
  surface normal) or, over empty space, where the pointer's ray meets the ground plane (y = 0;
  z = 0 in 2D). For "put a tree here".

Where the Scene view draws the game's UI (its **UI** toggle), a right click on a UI element is
about the UI instead: the object is that element, and the spot is a place on the canvas (`(x, y)
px`: reference pixels from the top-left corner, with the canvas size as shown, told to the agent
as a UI position; thread `p<hash>-<cli>` as well). Their bubbles stand at the element's rectangle
or at that pixel.

Either opens a bubble that stays next to its object or spot while the camera moves (a pin marks
the spot; several bubbles may be open, those of one object side by side; deleting the object
closes its bubbles, a spot's stays; bubbles move out of each other's way). The inputs of
bubbles, Agent Chat and Team Chat behave alike (`ChatInput` in `EditorTeamChat.cpp`): Enter
sends, Shift+Enter or Ctrl+Enter adds a line, `#` completes a project file with a preview, and
`@` completes an agent of the team (team inputs) or a scene object (direct conversations, which
have no teammates; a name with spaces is inserted quoted). The list shows for the input being
typed in; Up/Down choose, Tab or a click takes, and the caret stays after the inserted word.

- **Team**: the text goes to the team chat as `[Name #id] text` or `[This spot (x, y, z) on Name] text` (`team.send`), so the lead
  answers unless someone is @mentioned. The bubble shows what it sent and the replies addressed
  to the person since then, plus the activity of working agents; the Team Chat has all of it.
- **One agent**: a direct conversation outside the roster and the chat (`team.ask`, thread
  `e<entity>-<cli>`, or `p<hash of the spot to 0.1 m>-<cli>`). The bubble shows the conversation, the running activity and a Stop button
  (`team.thread_cancel`). The turn runs like a team turn: in the project directory, with
  `edit` access, the editor's MCP server (`--agent ask-<cli>`, so edits show as notices with the
  CLI's name), usage recorded under `ask-<cli>`. The prompt starts with the object (`entity #id
  "Name" (components) in scene ...`, read with `entity.get`) or the spot (world position, the
  entity and normal of the surface, and that "here" means that position) and the CLI's own conversation is
  resumed by the next message. Conversations are kept in `.oe/team/threads/<thread>.json` (the
  last 200 messages); `team.clear` removes them, `team.threads` lists them.

Files: the paperclip beside an input (bubbles, Agent Chat, Team Chat) opens the file chooser, and
files dropped on a bubble or a chat panel are attached to that input's next message (at most 8,
shown as chips above the input; click one to remove it). They travel like Team Chat attachments
(`team.send` / `team.ask {attachments}`): a project file in place, another one copied into
`.oe/team/attachments/`, named in the prompt as `Attached image|file: <path>`, and shown under the
message (pictures as thumbnails that open the viewer).

### 7.5 Agent Chat panel

A tab beside Team Chat (View > Agent Chat) to talk to the agent directly, outside the team: the
name of the CLI that answers and its model (both set in Settings > Agents; the panel has no
pickers for them and follows the setting when it changes), a conversation picker and **New**, the conversation (Markdown,
"N tools · T s · $cost" under each reply, the running activity) and the input (Enter sends,
Ctrl+Enter adds a line; Stop while a turn runs). It uses `team.ask` conversations named
`chat-<cli>` (New: `chat-<cli>-<n>`); the picker also lists the CLI's Scene bubble conversations
(object or spot), which can be continued here. The prompt says it comes from this panel and names
the entity selected in the editor, if any. Like the bubbles, nothing goes to the team chat.
The gear opens Settings > Agents.

### 7.6 Settings > Agents

Edit > Settings (or the gear in Agent Chat) opens the Settings window. Its Agents tab lists
each CLI with **Installed** (version, path as tooltip, or the install command), **Login**
(signed in with the method, or the command that signs in, with Copy) and **Engine connection**
(whether the ownengine MCP server connected in the CLI's latest turn this session, or that the
turn had only the shell fallback), and **Check again** (`team.backends {refresh}`). Under it,
**Agent Chat and Ask bubbles**: the one agent that is asked directly. **Agent** (`team.settings
{askBackend}`: Automatic = the first installed CLI that can run turns, or one CLI), **Model**
(`askModel`, of the chosen CLI; with Automatic the CLI's default) and **Instructions**
(`askInstructions`, at most 4 KiB, saved when the field is left): standing instructions added to
the agent's system prompt in every direct conversation, e.g. the language to answer in. A CLI
without a system prompt file (Codex) reads them when a conversation starts, so a change reaches
its running conversations only after **New**. Then:
the editor API port, the MCP command line every turn gets and the shell fallback
(`team.connection`), the permission of Agent Chat and Ask bubbles (`team.settings
{askAccess}`: read only, edit, full access) and whether turns also load the user's own MCP
servers and skills (`inheritUserTools`). `oe editor <project> --screenshot f.png --settings
agents` shows it.

## 8. Commands

All in area `team`, registered only by the `oe` tool. None is `mutates` (the team is not part of
the scene, so no undo entry and no scene revision); team changes bump the team revision.

| Command | Params | Result |
|---|---|---|
| `team.backends` | `{refresh?, async?}` | array of backends: status, version, path, message, models, model source, image support, install hint, `runsTurns`, `login`/`loginDetail`/`loginHint`, `engine` (MCP status of the latest turn) |
| `team.connection` | | `{apiPort, oe, oeFound, mcp, shell, askAccess, inheritUserTools, askBackend, askModel, askInstructions, askAgent}`: how turns reach the editor; `askAgent` = the CLI that answers direct conversations now |
| `team.presets` | | avatar preset ids |
| `team.list` | | `{version, revision, lead, maxConcurrent, maxHops, agents: [profile], error?}` |
| `team.get` | `{id}` | one profile: every field, plus `avatarPath` (project-relative) for an uploaded picture |
| `team.add` | `{name, description?, instructions?, backend?, model?, access?, maxBudgetUsd?, canCall?, avatar?}` | the new profile (id generated; backend defaults to the first one, model to that backend's default) |
| `team.update` | `{id, values}` | the profile; partial update like `component.set`. A new backend without a model resets the model |
| `team.remove` | `{id}` | `{removed: id}`; removes profile and uploaded picture (T3: also cancels its turn and drops its session) |
| `team.move` | `{id, index}` | the team (as `team.list`) with the agent at position `index` (0 = first, clamped to the end) |
| `team.avatar` | `{id, preset}` or `{id, source}` | the profile |
| `team.settings` | `{lead?, maxConcurrent?, maxHops?, inheritUserTools?, rotateAboveInputTokens?, askAccess?, askBackend?, askModel?, askInstructions?}` | team settings; without arguments it only reads |

Every `id` argument accepts the agent's id or its name.
| `team.send` | `{text, attachments?, from?, wait?}` | `{message, started: [ids], queued: [ids], offline: [ids]}`; with `wait` also `replies` and `state` |
| `team.cancel` | `{id}` or `{all:true}` | `{cancelled: [ids]}` |
| `team.state` | | `{revision, running, agents}`: per agent state, activity, task, elapsed, tools, error and hint |
| `team.messages` | `{after?, limit?}` | Read omitted chat on demand: `{messages, first, last}`: with `after`, the oldest messages with a larger id; without, the newest `limit` (default 100, at most 500). `first`/`last` are the oldest and newest ids in the log (`first` 0 = empty), so a viewer can drop what was cleared |
| `team.usage` | `{id?, date?}` | per-agent/per-day totals with reportedTurns and costReportedTurns; date is YYYY-MM-DD or today (UTC), omit for all days |
| `team.delegate` | `{to, goal, acceptance?, files?, context?, maxDurationMs?}` | fresh worker task; to accepts @id, id or name |
| `team.report` | `{id, summary, changed, evidence}` | attributed worker evidence; never automatically verified |
| `team.task` | `{id}` | task record and state |
| `team.tasks` | | stored task records |
| `team.reset` | `{id}` | `{reset: true}`; clears only the selected idle agent's conversation; keeps chat/usage/profiles/other sessions |
| `team.clear` | | stops every turn, empties the chat log and stored sessions |

Errors follow the usual shape, with hints that say how to fix the call, for example
`backend_not_installed` ("Install it with `npm i -g @openai/codex`, then `team.backends
{refresh:true}`"), `unknown_agent`, `agent_busy`, `team_full`, `invalid_model`. The profile
commands use `unknown_agent`, `name_taken`, `team_full`, `unknown_backend`, `invalid_model`,
`unknown_preset`, `invalid_image`, `not_found`, `team_file_invalid`, `write_failed` and
`invalid_argument`.

## 9. Tests

All in `tests/tests.cpp`; `oe_tests` never runs a real CLI (network, cost, login). Stand-in backends
(`.cmd` scripts printing a recorded stream, injected through `RegisterTeamCommands(registry,
backends)`) cover turns: `TeamStreamParsers` (recorded Claude/Codex lines, argv without `"`, `%`, `!`),
`TeamSessionTurns` (routing, resume, queueing, cancel, crash, timeout, hops, images, attachments),
`TeamStoreProfiles`, `TeamAvatars`, `ProcessRunsChildren`, `TeamBackendParsing`,
`TeamBackendDetection`, `NativeEditorTeam`, `NativeEditorTeamChat`, `EditorAvatarPresets`,
`EditorTranslations`. Real CLIs are checked by hand and the result goes in the work log.

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

Settled on 2026-10-05 (usage and cost, §11):

5. **`maxHops` default 0**: agent-to-agent `@mention` turns are off until a team sets it (for
   example `team.settings {maxHops: 2}`).
6. **Delegation beside the chat**: `team.delegate` is how a lead moves work; the person still calls
   agents by `@mention` in the chat, and `@mention` turns between agents stay available.
7. **Workers always start a fresh conversation**: a task turn never resumes and never sees the
   chat. The chat conversations of the lead and the person stay resumed.

## 11. Usage and cost

A team spends clearly more than one agent told "core work on Opus, the rest on Sonnet, images
through Codex" (observed 2026-10-04 on the Wickbound team). E1–E6 are implemented; historical rejection measurements are retained in TEAM_LOG.md.

### 11.1 Measured

One turn that only answers "ok" (Claude Code 2.1.288, `--model haiku`, input side = new + cached
input tokens):

| How the turn is started | Input-side tokens | Cost | Loaded |
|---|---|---|---|
| As the team starts it today | ~27,000 | $0.056 | 116 tools, 120 skills, 7 MCP servers |
| + `--mcp-config <empty> --strict-mcp-config` | ~22,900 | $0.034 | 31 tools |
| + `--disable-slash-commands` | ~19,000 | $0.027 | 30 tools, 0 skills |
| `--bare` | not usable | - | needs an API key, not the subscription login |

If the measurement folder is inside the repository, Claude loads the parent `CLAUDE.md`; compare the 19k goal against an outside-repository project (2026-10-05: 22,977 tokens, see TEAM_LOG.md).

Codex reports tokens but no cost (31,220 input tokens for a one-command turn, 27,904 cached).

### 11.2 Why it costs more

1. Every turn is a new CLI process with its own base context (19,000 to 27,000 tokens), per turn
   and per agent. One delegating agent pays it once and starts sub-agents with small prompts.
2. A handoff is a full turn in another conversation: the receiver re-reads what the sender read.
3. The chat is pushed into every turn (up to 40 messages / 32 KiB), needed or not.
4. Messages without a mention go to the lead, usually the most expensive model.
5. Agent-to-agent turns are on by default (`maxHops` 4); every `@id` in a reply calls someone.

Long-lived CLI processes would save start-up time, not tokens (the conversation is resent on every
request either way).

### 11.3 What Buzz does (reference)

[Buzz](https://github.com/block/buzz) (Block) is a Nostr-based chat where humans and agents work in
channels. Read on 2026-10-05 from its raw files (`VISION_AGENT.md`, `crates/buzz-acp/README.md` and
`TASKS.md`, `crates/buzz-acp/src/base_prompt.md` and `session_model_*.md`,
`benchmarks/harbor-buzz-orchestra/`), not from the code. It publishes no token budgets or model
routing; it spends less by structure, and its stated rule is "if you can delete it, delete it".
What carries over to a local game-dev team:

| Buzz | Why it is cheap or safe | Used here as |
|---|---|---|
| Only an `@mention` wakes an agent; `--respond-to owner-only` by default, so agents do not wake each other unless switched on | no turn nobody asked for | E1: `maxHops` 0 |
| Prompt rule: `@mention` only to hand work back or ask; never to accept, confirm receipt or talk *about* someone | no ack or courtesy turns | E1: standing-context rule |
| Events queued for a channel are drained into one batched prompt; at most one prompt in flight per channel | three messages, one turn | already so (§5.1) |
| Context is pulled: the agent reads history with a tool when it needs it | no tokens for history nobody reads | E3 |
| `buzz-acp run --task`: one prepared task (`version`, `taskId`, `prompt`, `maxDurationMs`), a fresh process and session, no history, no automatic channel context, no retry after failure or timeout, final text not auto-published, one JSON status line (`completed`, `failed`, `invalid`, `timed_out`, `cancelled`) | a worker is a bounded function call; "exit 0 is not proof of success" | E2: the task turn |
| Orchestrator persona: does not run commands; one step per message with the exact goal **and** the success check; relays the task's requirements verbatim and adds no constraints; verifies the worker's output itself. Worker persona: reports the command, exit code and trimmed output; never claims success without the verifying output; on failure reports verbatim and stops | handoffs that do not loop or invent work | E2: task brief and report format |
| Turn limits: idle timeout (silence, default 620 s, reset by any output) plus an absolute cap (7200 s) | a stuck turn stops spending | E1: idle timeout |
| Owner control commands `!cancel`, `!rotate`, `!shutdown`, consumed by the harness, never sent to the model | control without a model call | E6: per-agent reset |
| A session summarizes its own history at 90 % of its context window and continues | a long conversation stays bounded | E6: rotate a resumed conversation by size |
| A shared `AgentRuntime` owns the capabilities and prompt context of both entry points | one place for the prompt prefix | E4 |
| Benchmark harness: a team is a manifest (roster, count, model endpoint, byte-pinned prompt hash, generation settings); a cheap model on purpose, "a weak result is a prompt finding, not a model finding"; regression layer k=1, workflow layer k=3; a harness fault is told apart from a model fault | team cost and quality are measured per condition | §11.5 |

Not taken over: long-lived ACP processes (they save start-up time, not tokens), heartbeat prompts
(idle spending), Nostr identities and relay, one-process-per-session scale (32 agents). Buzz's
"ten agents in parallel without resource overhead" is about local processes, not tokens.

### 11.4 Proposal

Model the team as call and return instead of a chat room. Chat stays for the person and the lead;
work moves as small tasks; shared state (files, `scene.summary`, `script.errors`) is read on demand,
never pushed. Goal: the fixed task of §11.5 costs at most about 1.5x one agent that delegates.

| # | Change | Effect |
|---|---|---|
| E1 | `maxHops` default 0. Claude turns always get `--strict-mcp-config` (the engine's server, or an empty config without an editor) and `--disable-slash-commands`; team setting `inheritUserTools` (default false) restores the user's own MCP servers and skills; look up the Codex equivalent. Standing context: `@mention` only to hand work back or ask, never to acknowledge or when talking about someone. Turn timeout becomes idle (default 10 min without output) plus the existing 30 min cap | idle turn 27k to 19k tokens, no unasked or courtesy turns, stuck turns stop |
| E2 | `team.delegate {to, goal, acceptance?, files?, context?, maxDurationMs?}` writes `.oe/team/tasks/<id>.json` and starts the worker. The worker turn is fresh (no `--resume`, nothing from the chat), its prompt is the task plus a short role context, and it is never retried automatically. It ends with `team.report {id, summary, changed, evidence}`; evidence is the command, its result and a trimmed output, not a claim. States: `running`, `done`, `failed`, `timed_out`, `cancelled`; without an editor the final text is taken and marked unverified; "done" is not "verified" until the lead checks `evidence` against `acceptance`. The lead's context says: one step per task, give the exact goal and success check, pass the person's requirements verbatim, add none. `oe mcp --connect` exposes `team_delegate` as a blocking tool (it polls `team.task` on its own thread, so the editor stays responsive). Profile field `canCall: [ids]`, default none, the lead may call everyone. Cancelling the delegating turn cancels its tasks | no chat resend, one bounded turn per task, no improvising |
| E3 | Chat pull: a turn gets the messages addressed to the agent plus the one that triggered it, and a line saying how many were left out and that `team_messages` reads them (only when an editor is attached) | prompt no longer grows with the chat |
| E4 | Cache-stable prompt: the standing context is byte-identical between turns of an agent (role, instructions, only the agents it may call; no states, times, counts); everything volatile goes after it. Build it in one place used by chat turns and task turns | cached tokens from the second turn on |
| E5 | Usage: token counts on `TurnEvent` (Claude `result.usage`, Codex `turn.completed.usage`), `.oe/team/usage.jsonl`, `team.usage`, numbers in the Team panel and on each reply; profile `maxBudgetUsd` (Claude `--max-budget-usd`). First verify whether Claude's `total_cost_usd` is per turn or cumulative on a resumed conversation | shows where it goes; a hard stop per turn |
| E6 | `team.reset {id}` starts a fresh conversation for one agent (today only `team.clear` drops everything), with a Reset item in the roster menu; when E5 shows a resumed conversation above a size threshold (setting, off by default) the next turn starts fresh and says so in a notice | a long conversation cannot grow the base cost of every later turn |

Decided (§10, items 5 to 7): `maxHops` 0, delegation beside the chat, fresh worker turns.
Model choice is configuration, not code: a Sonnet lead, Opus
called by name for hard jobs, pictures through the agent whose backend `supportsImages`; a project's
`team.json` is local (git-ignored), so measured settings are described in the log.

Left out until a measurement or a real collision asks for it (Buzz's own rule is to delete what
does not pay rent): file claims between tasks, `owns` globs, independent QA tasks, `hardModel`
retry (a failed task is not retried automatically, as in Buzz), `effort`, a per-chain cost cap,
per-agent git worktrees. Not planned: provider APIs (§1), our own history summarizer, ACP or
long-lived processes.

### 11.5 Measuring

Measurements report rather than gate. Identical M2 runs have ranged from $0.19
to $0.47. Run k=3 per condition at the end, report median/range, exclude harness
faults with reasons and retain model failures as quality results.

- **M1 idle turn**: `oe exec <scratch> team.send '{"text":"Say ok","wait":120}'` on `haiku` in a
  scratch `oe new` project; read tokens from the CLI's `result` line, cost from `team.messages`.
- **M2 fixed task**: "Add a villager" (copy the `Farmer` entity with its blob shadow, place it on a free
  plaza tile facing left, `script.check` clean, `RPGVillageGame` still passing) on a copy of `games/RPG`, k=3 at
  the end per
  condition: (a) one Claude agent alone that may use sub-agents, (b) the team before, (c) the improved team
  after E4. Record tokens, cost, wall time and whether the acceptance lines held, in
  [TEAM_LOG.md](TEAM_LOG.md). Write each condition down the way Buzz's manifest does (agents,
  models, the exact prompts) so a later run is comparable; use the cheap models.
- Measurement reports, it does not gate. One run is noise: identical M2 runs on this PC cost
  between $0.19 and $0.47, because the model takes a different tool path each time. So measure
  once, at the end of E1 to E4, with k=3 per condition (median and range), and use it to find out
  where cost remains. A harness fault (CLI not logged in, capture or encoding error, crash) is
  told apart from a model fault and left out with its reason.
- Real CLIs run only by hand on the cheapest model; tests use stand-in backends.

Each of E1 to E6 is one commit that also updates §3.1, §5 and §8 and `docs/API.md`.

### 11.6 Hand-over for whoever implements E1 to E6

Read §5 (turns), §11 and docs/DESIGN.md (invariants, definition of done) first. Do the steps in
order, one commit each; step N may assume steps before it. Tick the box in
[TEAM_LOG.md](TEAM_LOG.md) when the step is implemented, tested and its "Done when" holds.

Rules:

- Build with `build.bat`, test with `build/bin/oe_tests`. After changing a header, **touch the
  `.cpp` files that include it** (`build.bat` misses header dependencies on this PC). Never kill
  `oe.exe` by image name (`taskkill /IM oe.exe` kills the user's MCP servers); kill by PID.
- `oe_tests` never runs real CLIs. Tests use stand-in `.cmd` backends (see `TeamSessionTurns` in
  `tests/tests.cpp`). Real CLIs only by hand, on `haiku` / the cheapest Codex model, in a scratch
  folder (`build/scratch/...`). Do not edit or commit a project's `.oe/team/team.json`; describe
  intended settings in the log.
- Commands: `Register(...)` with a `Params()` schema in `engine/team/TeamCommands.cpp`, no
  `mutates` (the team is not the scene), `ApiError(code, message, hint)` for caller mistakes, then
  `oe api --markdown > docs/API.md`. Add a test for every new behaviour.
- Editor strings go through `Tr("...")` with a Korean and Japanese row in
  `engine/editor/EditorText.cpp` (`EditorTranslations` test).
- `CLAUDE.md` and `AGENTS.md` stay identical (`AgentInstructionsInSync`): change both or neither.
- Command lines: no user text in arguments, no `"`, `%`, `!` (§2.1); format numbers yourself, no
  locale. `.oe/team/` files are written atomically (`WriteTextFileAtomic`); an unreadable file is
  never overwritten. Update this document (§3.1, §5, §8, §11) in the same commit as the behaviour.
- Do not revert a step because one measurement is not better: a step is reverted only when tests
  fail, an acceptance check regresses, or the cost clearly rises in every run. Structural changes
  (fewer turns, a smaller prompt, no chat resend) are kept on their reasoning; measure afterwards
  (§11.5) and record the numbers.

| Step | Main files | Done when |
|---|---|---|
| M | none (measure only) | M1 and M2 baselines in the log: numbers, CLI versions, date |
| E1 | `TeamStore.h/.cpp` (`maxHops` default, `inheritUserTools`), `TeamAdapters.cpp` (`ClaudeBuildTurn`, `CodexBuildTurn`), `Backends.h` (`TurnRequest`), `TeamSession.cpp` (`StartTurn`, `StandingContext`, timeouts), `TeamCommands.cpp` (`team.settings`) | idle turn at most about 19,000 input-side tokens; a mention in a reply is not delivered and the notice says how to turn it on; stand-in tests for the new flags |
| E2 | new `engine/team/TeamTasks.h/.cpp` (add to the `oe_team` target in `CMakeLists.txt`), `TeamStore.h` (`canCall`), `TeamSession.cpp` (`StartTask`, no `--resume`, no chat history), `TeamCommands.cpp`, `tools/oe/main.cpp` (`CmdMcp`: the blocking `team_delegate` wrapper) | by hand on a scratch project the lead delegates over `team_delegate` and gets the worker's report back as the tool result; the worker's prompt in `.oe/team/tmp` holds no chat text; M2 (c) within about 1.5x of (a) |
| E3 | `TeamSession::StartTurn` | prompt size no longer grows with the number of unrelated chat messages (test with 30 of them) |
| E4 | `TeamSession::StandingContext` / task context | the same inputs give byte-identical context after a teammate's state changed; the second real Claude turn shows cached tokens above 0 |
| E5 | `Backends.h` (`TurnEvent`), `TeamAdapters.cpp` (usage parsing), `TeamSession.cpp`, `TeamCommands.cpp` (`team.usage`), `EditorTeam.cpp`, `EditorTeamChat.cpp` | the Team panel shows tokens per agent and day and they agree with the CLI's own result lines; first record whether Claude's `total_cost_usd` is per turn or cumulative on a resumed conversation |
| E6 | `TeamSession.cpp`, `TeamCommands.cpp` (`team.reset`), `EditorTeam.cpp` (roster menu) | `team.reset` drops one agent's conversation and nothing else; the size rotation is off by default and tested |

After E1 to E4 are in, run the final measurement of §11.5 once (k=3). If the team is still far above
the target, use `team.usage` to see where the tokens go and fix that, instead of adding features.

### E3 checkpoint — 2026-10-05

Chat pull is implemented without new profile settings (§3.1). Routing is unchanged:
explicit recipients and the lead determine delivery. Prompts exclude unrelated
messages; completed turns advance their seen watermark past skipped history.
Thirty unrelated messages leave only the omission-count line in the next prompt.
E1/E2 remain reverted; no task API is implied by this checkpoint. Measurements and
acceptance are recorded in TEAM_LOG.md.

### E5 checkpoint — 2026-10-05

Usage accounting is implemented in the Team roster (today in UTC), each reply,
and team.usage. Input includes cache creation/read; cached is the read subset.
Known zero differs from unknown; partial totals carry reported-turn counts.
Profiles support a Claude turn budget, editable in the profile dialog. The CLI
ceiling adds the known session cost to that allowance. An unknown resumed baseline
starts fresh when a budget is enabled. Claude checks between requests and can
exceed the allowance by one request; this is not a prepaid spending reservation.
Changing backend requires clearing an unsupported budget. Usage survives clear,
reset and profile removal. The ledger is bounded to 20,000 records/16 MiB;
archive it and reopen when full. Unreadable or externally changed files are never
overwritten. E1/E2/E4 remain reverted. E5 measures accounting accuracy, not a claim
that provider spend falls because telemetry is shown; evidence is in TEAM_LOG.md.

### E6 checkpoint — 2026-10-05

Per-agent reset and optional input-size rotation are implemented. The threshold
defaults to 0 and is omitted on disk; reset never calls a model. Reset preserves
other agents, chat, usage and the selected agent's seen watermark. Busy and queued
agents refuse reset. Stand-in tests cover manual reset, threshold boundaries,
restart persistence and invalid-session-file preservation. The roster exposes
Reset Conversation and displays usage on two lines so cost fits a narrow panel.
Manual haiku measurements reduced resumed input from 86,138 to 38,049 tokens.
Cold-cache reset cost increased; automatic rotation is optional, not a promise
of lower cost on every turn. Full measurements and tradeoffs are in TEAM_LOG.md.

### E1 restoration — 2026-10-05

Implemented and retained under the revised measurement policy. Build warning-free;
203 stand-in/engine tests pass. E3/E5/E6 are preserved. Final k=3 measurements
are recorded separately in TEAM_LOG.md; historical rejection entries are historical.

### E2 restoration — 2026-10-05

Implemented and retained under the revised measurement policy. Build warning-free;
205 stand-in/engine tests pass. E3/E5/E6 are preserved. Final k=3 measurements
are recorded separately in TEAM_LOG.md; historical rejection entries are historical.

### E4 restoration — 2026-10-05

Implemented and retained under the revised measurement policy. Build warning-free;
206 stand-in/engine tests pass. E3/E5/E6 are preserved. Final k=3 measurements
are recorded separately in TEAM_LOG.md; historical rejection entries are historical.

### Final restoration measurements — 2026-10-05

E1/E2/E4 are restored alongside E3/E5/E6, superseding the historical rejections.
Final builds passed 203/205/206 tests with no warnings. Canonical M1/M2 use k=3
per condition; medians/ranges, exact prompts, raw UTF-8 and failure classifications
are in TEAM_LOG.md. M1 improved from 38,104 to 32,699 input tokens (19k not reached).
M2 median cost is 0.2910520 USD standalone and 0.3539734 USD improved team, about
1.22x; Bat acceptance is 3/3 and 2/3 respectively, versus 0/3 before restoration.
The improved team's failed run emitted @worker instead of calling team_delegate;
default maxHops 0 correctly blocked it. Failed runs remain in the distributions.
No favourable replacement run or measurement-driven source rollback was made.
