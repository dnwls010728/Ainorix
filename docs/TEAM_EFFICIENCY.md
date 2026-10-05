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

`maxHops: 0` still lets the person call any agent with `@core`, `@qa`, or `@art`; it only prevents an agent reply from automatically waking another agent. Raise it temporarily for work that genuinely benefits from autonomous handoff.

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
5. Keep `maxConcurrent` at `2` unless real parallel work exists.
6. Give Art only the visual brief and referenced files.
7. A handoff is a new full agent turn. Avoid a handoff whose only result is "looks good".

## Follow-up engine work

The next structural changes should be implemented separately because they affect prompt semantics and need dedicated tests:

- pull only messages addressed to an agent instead of pushing every unread team message;
- split human-readable `ChatMessage` from executable `AgentTask` so an `@mention` is not inherently a model invocation;
- add an explicit handoff command / task object and make automatic handoff opt-in;
- record input, cached input and output tokens per turn and aggregate them per agent / chain / day;
- add per-role tool policies and prevent nested sub-agent delegation unless explicitly enabled;
- make the scheduler aware of write scopes so safe read-only QA can run beside an editor while conflicting writers queue.

Those changes are intentionally not hidden behind this small compatibility patch; they should land with focused tests and migration notes.
