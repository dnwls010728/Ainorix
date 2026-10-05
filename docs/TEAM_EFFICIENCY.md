# Agent team efficiency

This note is the low-cost operating profile for the agent team. It complements `TEAM.md`; the team architecture and command reference stay there.

## Recommended profile

Use one normal worker for routine work and wake expensive specialists only when they add real value. For the current four-agent setup:

| Agent | Model | Role |
|---|---|---|
| Content | Sonnet 5.5 | Default worker / lead. Gameplay, scenes, UI, data, routine fixes and docs. |
| Core | Opus 5.5 | Explicitly requested for engine architecture, rendering, physics, networking, hard bugs and large refactors. |
| QA | Sonnet 5.5 | Explicit verification for risky changes, regressions and release checks; do not wake after every small edit. |
| Art | Sol 6.1 | Images and visual assets only; give it the request and references it needs, not general engineering chat. |

For normal work, set Content as the lead and stop automatic agent-to-agent chains:

```text
oe exec <project> team.settings '{"lead":"content","maxConcurrent":2,"maxHops":0}'
```

`maxHops: 0` still lets the person call any agent with `@core`, `@qa`, `@art`, or `team.handoff`; it prevents an agent turn from automatically starting another agent. Raise it temporarily when an agent itself must continue a short specialist chain.

## Explicit handoff

Use `team.handoff` when exactly one specialist should take the next task instead of relying on a free-form chat mention:

```text
oe exec <project> team.handoff '{"to":"core","task":"Review the renderer ownership model and report only architecture risks."}'
```

The command resolves `to` by agent id or name and sends one addressed turn through the existing `TeamSession`, so attachment handling, offline reporting, session resume, concurrency and `maxHops` stay unchanged. To guarantee a single recipient with the current mention-based router, the task may mention the selected teammate but rejects another teammate mention or `@all` with `ambiguous_handoff`.

This is the first compatibility step toward separating human-readable chat from executable tasks. `team.send` and reply mentions keep their existing behavior for now; the next routing milestone makes automatic reply handoff opt-in and removes unrelated unread chat from prompts.

## Lean Claude turns

A Claude Code turn loads a large fixed context before it does useful project work. Measurements in `TEAM.md` §11 showed roughly 27k input-side tokens for an idle team turn on the development machine, about 22.9k with the engine-only MCP configuration, and about 19k when slash commands / user skills were also disabled.

Set `OE_TEAM_LEAN=1` in the environment of the `oe` process to add Claude Code's `--disable-slash-commands` flag to team turns:

### PowerShell

```powershell
$env:OE_TEAM_LEAN = "1"
./build/bin/oe.exe editor <project>
```

### cmd.exe

```bat
set OE_TEAM_LEAN=1
build\bin\oe.exe editor <project>
```

Unset the variable (or set it to `0`, `false`, or `off`) when a team agent intentionally needs the user's Claude slash-command / skill catalog.

The switch is opt-in so existing projects and automated tests keep their current behavior. It affects Claude Code only; Codex has no equivalent flag wired into the backend yet.

## Cost rules

1. Finish with one agent whenever one agent can finish the task.
2. Treat Core / Opus as an expensive specialist, not a router.
3. Do not run QA automatically after every small change.
4. Keep `maxHops` at `0` for normal work; enable short chains only for a specific task.
5. Prefer `team.handoff` when exactly one specialist should wake.
6. Keep `maxConcurrent` at `2` unless real parallel work exists.
7. Give Art only the visual brief and referenced files.
8. A handoff is a new full agent turn. Avoid a handoff whose only result is "looks good".

## Focused routing test

The explicit handoff routing has a small CLI-independent test target so it can be compiled without logging into Claude or Codex. On Windows, run:

```bat
tests\team_efficiency\run.bat
```

The runner configures the normal repository root and injects only the focused test target through `CMAKE_PROJECT_TOP_LEVEL_INCLUDES`; this preserves the root `CMAKE_SOURCE_DIR` assumptions used by the engine build. The test verifies id/name resolution, one-recipient routing text, rejection of another teammate / `@all`, unknown-agent handling, and registration of the `team.handoff` command.

## Follow-up engine work

The next structural changes should be implemented separately because they affect prompt semantics and need dedicated tests:

- pull only messages addressed to an agent instead of pushing every unread team message;
- split human-readable `ChatMessage` from executable `AgentTask` so an `@mention` is not inherently a model invocation;
- make automatic reply handoff opt-in instead of treating every final-message mention as execution;
- record input, cached input and output tokens per turn and aggregate them per agent / chain / day;
- add per-role tool policies and prevent nested sub-agent delegation unless explicitly enabled;
- make the scheduler aware of write scopes so safe read-only QA can run beside an editor while conflicting writers queue.

Those changes should land as measured, focused changes with migration notes rather than changing prompt semantics inside the compatibility patch.
