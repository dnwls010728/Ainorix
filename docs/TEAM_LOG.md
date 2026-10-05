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

## Usage baseline M — 2026-10-05

Measured manually in `build/scratch/team-usage/`, with Claude Code **2.1.289**,
`--model haiku` (resolved `claude-haiku-4-5-20251001`). Installed Codex:
**0.159.3**; no Codex model was run. Real CLIs were not run by `oe_tests`.
No editor/API was attached. Original samples and their local teams were unchanged.

Input-side tokens below are new input + cache creation + cache reads, summed over
the CLI result, including repeated cached context on tool iterations. Costs are
reported USD list-price estimates from `total_cost_usd`, not subscription charges.

| Condition | New input | Cache creation | Cache reads | Input-side total | Output | Cost USD | Turn duration |
|---|---:|---:|---:|---:|---:|---:|---:|
| M1 before | 9 | 21,612 | 16,349 | 37,970 | 162 | 0.0456779 | 7.6 s (team); 3.052 s (CLI) |
| M2 (a), standalone | 273 | 45,689 | 1,859,216 | 1,905,178 | 14,069 | 0.3479176 | 170.415 s (CLI) |
| M2 (b), team before | 129 | 43,936 | 806,374 | 850,439 | 9,664 | 0.2169584 | 108.4 s (team); 103.527 s (CLI) |

Durations come from the engine's turn wall clock and/or CLI `duration_ms`, as
labelled; standalone process startup is excluded from its reported CLI duration.
Raw JSON streams, prompts and command results remain in the ignored scratch folder.
A PATH-local capture shim forwarded unchanged arguments/stdin to the real Claude
executable and teed stdout for engine runs; it did not change user CLI configuration.

Manifest:

- M1: fresh `oe new` project, one agent `Solo`, Claude/haiku/full, empty instructions
  and description, defaults maxConcurrent=3/maxHops=4. Exact message: `Say ok`.
  Sent with `team.send {text: "Say ok", wait: 120}`.
- M2 (a): fresh copy of Wickbound excluding `.oe`, standalone `claude -p --model
  haiku --output-format stream-json --verbose --permission-mode bypassPermissions`.
  Sub-agents were permitted on haiku but none were used.
- M2 (b): another fresh copy excluding `.oe`; lead `Solo`, Claude/haiku/full,
  empty instructions/description; `Reviewer`, Claude/haiku/full, empty instructions,
  description `Verify script.check and task acceptance when asked.` Defaults
  maxConcurrent=3/maxHops=4. Same task sent to the lead with wait=600. Reviewer
  received no turn; neither run delegated. This measures the roster/prompt overhead,
  not a forced multi-worker workflow. Both inherit the existing user CLI tools/skills.

Exact common M2 task (the executable placeholder was replaced with the absolute
built `oe.exe` path when running):

> Add a Bat enemy. Copy the slime prefab and script, set speed 3, put one in scenes/run.scene.json, and make script.check clean. In this Wickbound version the slime is prefabs/enemy_gloom.prefab.json with scripts/run/enemy.lua: copy those to prefabs/enemy_bat.prefab.json and scripts/run/bat.lua. Keep the copied enemy behaviour and set its base speed to 3 in the copied script. The Bat prefab must use scripts/run/bat.lua. Put one instance named Bat in scenes/run.scene.json. Do not create new art. Verify the new script with <oe.exe> exec . script.check. Report changed files and verification output. You may use sub-agents on haiku only. Work only inside this scratch project; do not change the original sample or engine.

The slime reference needed this fixed normalization: Wickbound has no file named
`slime`, but `enemy_gloom` is its slime. Both conditions used the same normalization.

Acceptance, independently checked: both created the copied prefab/script with base
speed 3 and both `script.check {path:"scripts/run/bat.lua"}` results had zero errors
and warnings. **Both failed the scene acceptance**: (a) wrote only a `Prefab` marker
and Transform, without the prefab's entities/components; (b) wrote an unknown
`PrefabInstance` component. `Prefab` remembers the source of an already-expanded
instance; it does not instantiate it. Therefore neither scene contains a working
Bat enemy. These are model faults, not successful cost baselines. Do not use their
cost ratio (0.624x) as proof that the 1.5x quality-and-cost target is met, or apply the
early-stop gate to a failed task. E1 may proceed; M2 needs successful runs before E2.

Harness fault: the first standalone launch mistakenly used the repository working
directory. It was terminated by its exact PID before completion and restarted in
the scratch project. Its partial usage is unavailable and excluded; no source/sample
changes remain: the aborted launch created one untracked Bat prefab in the original
Wickbound folder; it was removed after confirming its path and creation time. No
process was killed by image name.

Validation: `build.bat` succeeded; `build/bin/oe_tests` passed **198 tests, 0 failed
checks**. `CLAUDE.md` and `AGENTS.md` were not changed by this milestone.

## E1 attempt — rejected on measurement, 2026-10-05

- [x] M: baseline measurements recorded above, including M2 model failures.
- [ ] E1: attempted and tested, then discarded; measured result did not satisfy the keep gate.

Implemented provisionally: maxHops default 0 and an enable command in both blocked
handoff notices; strictly typed `inheritUserTools` setting, default false and omitted
from stored JSON when false; Claude `--strict-mcp-config` and
`--disable-slash-commands`, with an atomic empty MCP configuration without an editor
and opt-in restoration of user tools; no acknowledgement/acceptance/chat mentions in
standing context; output-based 600 s idle timeout plus the existing 1800 s absolute
cap. Chat resume behaviour remained intact; no E2 worker/task work was started.

Codex investigation: installed **0.159.3** documents `--ignore-user-config` on both
`exec` and `exec resume`; it skips CODEX_HOME/config.toml while retaining login there.
The provisional adapter used it plus `-c skills.include_instructions=false`.
The latter controls the automatic skills instruction block in the
[official configuration schema](https://learn.chatgpt.com/docs/config-schema.json).
This is **not complete skill isolation**: auto-discovered skills and project/system
configuration may remain; no full equivalent to Claude's flags was established.
`inheritUserTools:true` omitted both overrides. These adapter changes were discarded
with the rest of E1; no real Codex model was run, no user config/auth file was changed.

M1 after: fresh `oe new` scratch project, same Solo/Claude/haiku/full profile and
exact `Say ok` message with wait=120, no editor, lean default. Both before and after
used the same installed CLI and inherited the repository/user standing instructions.
Raw result and team reply agree on the cost.

| M1 | Input | Cache creation | Cache reads | Input-side total | Output | Cost USD | Team wall time | CLI duration |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Before | 9 | 21,612 | 16,349 | 37,970 | 162 | 0.0456779 | 7.6 s | 3.052 s |
| After attempted E1 | 9 | 32,782 | 0 | 32,791 | 74 | 0.065943 | 5.3 s | 2.360 s |

Input-side total fell **13.64%**, while reported cost rose **44.37%**. Tool/skill/MCP
counts from init were **116/127/9 before**, **30/0/0 after**. The approximately
19,000-token idle target was not met. Before included cache hits and after was cold;
this prevents attributing the cost increase solely to the flags. It also means this
measurement does not establish a cost improvement. No extra runs were used to select
a favourable cache state. Under the user's keep-only-on-improvement rule, E1 was
removed rather than presenting token reduction alone as a successful milestone.

Verification of the provisional code: build succeeded without compiler warnings;
`TeamStoreProfiles`, `TeamStreamParsers`, `TeamSessionTurns` and new stand-in-only
`TeamLeanTurns` cover defaults, persistence/validation/omission, opt-in flags on
new/resumed turns, an empty MCP config, no-ack context, blocked mentions, silence,
stdout/stderr progress resetting idle, and the absolute cap despite progress.
The first run had two incorrect test expectations: `team.send {wait}` includes error
messages in `replies`. The assertions were corrected; no production change was needed.
API documentation was regenerated, then restored with the rejected source/tests.
The discarded source/API/test snapshot remains only under ignored
`build/scratch/team-usage/rejected-e1/` for review. Earlier unrelated working-tree
changes were preserved, including both identical agent instruction files.

Final verification: provisional E1 passed **199 tests, 0 failed checks** after the
assertion fix. Restored source passed `build.bat` and **198 tests, 0 failed checks**.
SHA-256 checks confirmed the tests and API files exactly match their pre-E1 snapshots;
`CLAUDE.md` and `AGENTS.md` hashes match. `engine/team/` and Wickbound have no remaining
changes from this task. Only measurement/rejection notes are retained. Stop here;
E2 and later steps remain unimplemented and await the user's next instruction.

## 2026-10-05 — E2 measured and rejected (no retained implementation)

The user explicitly authorized E2 after E1 had been reverted. E2 was implemented
and tested provisionally, then restored under §11.5–11.6's measurement/acceptance
gate. E1 remains rejected; E3 and later were not started. There is no checked E2
milestone or production task API in this commit.

Provisional scope: versioned atomic task files; fresh worker conversations with
short role/brief and no chat/resume; `team.delegate/report/task/tasks`; attributed
worker evidence; `canCall` default none with lead access; recursive cancellation,
absolute deadline, interrupted-task failure without retry; nonblocking command and
blocking MCP I/O-thread polling; unverified final-text fallback without an API.
Existing CMake `CONFIGURE_DEPENDS` glob included TeamTasks.cpp. API was regenerated
and §§3.1, 5, 8 and the two identical agent guides updated provisionally, then all
restored with the implementation. The discarded source/docs/tests are available
only in ignored `build/scratch/team-e2/rejected-e2/`.

CLI: Claude Code **2.1.289**, `haiku`, resolved model
`claude-haiku-4-5-20251001`, full/bypassPermissions, streamed JSON. Codex was not
run. All real turns were manual in copies under `build/scratch/team-e2/`; tests
used stand-in `.cmd` backends only. No original sample or project team.json changed.
Each condition used the same prompt bytes (SHA-256
`4468214b1f7f58d9fb217f6f27863d675f016c8fa2752eee2a41a06651e62f0b`).

Manifest: (a) one fresh standalone haiku CLI, native sub-agents permitted but none
used; (c) Lead + Worker, both Claude/haiku/full, first added agent as lead,
`maxConcurrent:3`, explicit `maxHops:0`, default empty canCall. Chat's default
maxHops was not changed by E2. Host `oe mcp <scratch> --port <unused loopback port>`;
parent stdin stayed open and each host was closed by its own handle/PID. API calls
used no blocking `team.send` wait, so the worker could reach the editor.

| Condition | New input | Cache creation | Cache read | Total input side | Output | USD | Wall seconds | Outcome |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| (a), fresh single agent | 257 | 49,391 | 1,874,346 | 1,923,994 | 13,792 | 0.3554336 | 189.457 | Bat acceptance passed |
| (c), initial attempt | 101 | 51,874 | 606,794 | 658,769 | 8,913 | 0.2090934 | 82.043 | Invalid E2 comparison: `to:"@worker"` rejected, no task/worker started; lead did the work itself |
| (c), fresh copy after target handling fix | 296 | 106,088 | 1,970,435 | 2,076,819 | 23,435 | 0.5266905 | 248.179 | Bat acceptance passed; E2 report contract failed; capture encoding fault |

The first c run exposed inconsistent target syntax: team chat teaches `@id`, but
delegate accepted only bare ids/names. The provisional implementation accepted
an optional @ prefix and its stand-in test covered this. The second attempt was
on a new copy, not an automatic task retry. It produced exactly one task, without
engine retries. Per-process second-c result totals:

| Agent | New input | Creation | Read | Output | USD |
|---|---:|---:|---:|---:|---:|
| Lead | 127 | 60,197 | 841,077 | 10,420 | 0.2567287 |
| Worker | 169 | 45,891 | 1,129,358 | 13,015 | 0.2699618 |

The worker's saved task prompt had no chat history and the lead's blocking tool
returned the terminal record while API polling remained responsive. However,
the worker printed a JSON `team_report` section in its final answer instead of
calling the tool. It was correctly stored as `failed`, `reportReceived:false`,
`verified:false`, summary “Worker finished without team.report; the task is
unverified and failed.” This is a model/prompt failure of the handoff contract,
not evidence of a successful report. The lead then checked the Bat itself.

A separate harness fault affected the lead: the PowerShell capture proxy decoded
some Unicode incorrectly, breaking the final JSON result string. Its numeric
usage/cost prefix remained valid JSON; the Lead row above comes from that intact
prefix, not an estimate. Worker result JSON was valid. Because the lead's full
result was malformed, the engine did not receive a clean successful final turn.
No measurement is presented as a clean E2 success or a causal before/after result.
Do not reuse this text-decoding proxy; preserve raw UTF-8 bytes in a later harness.

Independent checks on (a) and second (c): one Bat root, Script.path
`scripts/run/bat.lua`, Collider2D and RigidBody2D, all five copied children
(Shadow/Aura/Body/Flash/Eyes), prefab script path, base speed 3, and actual
`oe exec <scratch> script.check {"path":"scripts/run/bat.lua"}` returned
zero errors and warnings. The prior M2 (a)/(b) rows remain failed baselines;
they cannot prove a quality-matched improvement over the old team.

Second c was **1.481825x** the valid single-agent cost, within the approximate
1.5x ceiling, but input was **+7.943%**, cost **+48.183%**, wall **+30.995%**.
The required report did not arrive through MCP and the capture had a harness
fault. There is no demonstrated improvement over a valid old-team baseline.
Under the keep-only-on-improvement rule, all provisional E2 changes were removed
and no favourable additional model run selected. A later E2 attempt needs an
explicit tool-call report instruction, byte-preserving capture and a valid
quality-matched before-team condition before claiming improvement.

Provisional verification: `build.bat` succeeded without compiler warnings;
**200 tests, 0 failed checks**, including TeamTaskRecords and TeamDelegationTurns.
They covered persisted/interrupted/corrupt task records, bounded evidence, polling
and transport errors, fresh workers preserving chat resume, no worker chat,
permissions/@ target, attributed/duplicate/missing reports, cancellation and deadline.

Final restoration verification: `build.bat` succeeded without compiler warnings;
**198 tests, 0 failed checks**. Shared tests/API/TEAM/guides/CLI source matched their
pre-E2 snapshot byte for byte. `engine/team/` has no remaining diff; both agent
guides remain identical. Existing unrelated edits remain unstaged. Only this
measurement/rejection record is committed for the E2 step.
