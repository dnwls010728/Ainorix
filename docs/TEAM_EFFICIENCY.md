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

For normal work, set Content as the lead, keep concurrency conservative, and allow only a short explicit specialist chain:

```text
oe exec <project> team.settings '{"lead":"content","maxConcurrent":2,"maxHops":2}'
```

With `OE_TEAM_LEAN=1`, reply mentions do not start teammates, so `maxHops: 2` is only a safety cap for deliberate agent-originated calls such as `team.handoff`. Set it to `0` when agents must never continue a chain themselves; the person can still call any agent directly.

## Explicit handoff

Use `team.handoff` when exactly one specialist should take the next task instead of relying on a free-form chat mention:

```text
oe exec <project> team.handoff '{"to":"core","task":"Review the renderer ownership model and report only architecture risks."}'
```

The command resolves `to` by agent id or name and passes it as a structured recipient to `TeamSession::Send`. The task text is stored unchanged, so `@qa`, `@all`, or any other mention inside the task is context only and cannot widen the recipient set. Attachment handling, offline reporting, session resume, concurrency and `maxHops` stay unchanged.

This is a compatibility step toward separating human-readable chat from executable tasks. It gives lean turns an explicit execution signal without changing the normal `team.send` path.

## Lean team turns

A team turn loads a large fixed context before it does useful project work. Measurements in `TEAM.md` §11 showed roughly 27k input-side tokens for an idle Claude team turn on the development machine, about 22.9k with the engine-only MCP configuration, and about 19k when slash commands / user skills were also disabled.

Set `OE_TEAM_LEAN=1` in the environment of the `oe` process to enable the low-cost team policy:

1. Claude Code gets `--disable-slash-commands`, avoiding the user's slash-command / skill catalog.
2. `TeamSession::StartTurn` pushes only new chat messages whose `to` contains that agent, instead of every unread team message.
3. An agent's final-message `@mention` is text only; it does not automatically start another model turn. Use `team.handoff` for deliberate delegation.
4. The standing prompt tells agents about these lean semantics, so the model does not accidentally depend on automatic mention handoff.

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

Unset the variable (or set it to `0`, `false`, or `off`) for compatibility mode. With lean mode disabled, the original unread-chat push and reply-mention handoff behavior remain unchanged. The CLI flag portion applies to Claude Code; the context/handoff policy in `TeamSession` applies to all backends.

## Cost rules

1. Finish with one agent whenever one agent can finish the task.
2. Treat Core / Opus as an expensive specialist, not a router.
3. Do not run QA automatically after every small change.
4. Run normal development with `OE_TEAM_LEAN=1` and `maxHops` around `2`; use `0` when agents must never continue a chain themselves.
5. Prefer `team.handoff` when exactly one specialist should wake.
6. Keep `maxConcurrent` at `2` unless real parallel work exists.
7. Give Art only the visual brief and referenced files.
8. A handoff is a new full agent turn. Avoid a handoff whose only result is "looks good".

## Focused routing test

The explicit handoff routing and lean-policy switch have a small CLI-independent test target so they can be compiled without logging into Claude or Codex. On Windows, run:

```bat
tests\team_efficiency\run.bat
```

The runner configures the normal repository root and injects only the focused test target through `CMAKE_PROJECT_TOP_LEVEL_INCLUDES`; this preserves the root `CMAKE_SOURCE_DIR` assumptions used by the engine build. The test verifies the lean environment switch, id/name resolution, structured one-recipient routing even when task text contains other mentions or `@all`, unknown-agent handling, and registration of the `team.handoff` command.

## Follow-up engine work

The next structural changes should be implemented separately:

- split human-readable `ChatMessage` from executable `AgentTask`, so scheduling, budgets and permissions can be attached to a task independently of the stored chat message;
- record input, cached input and output tokens per turn and aggregate them per agent / chain / day;
- add per-role tool policies and prevent nested sub-agent delegation unless explicitly enabled;
- make the scheduler aware of write scopes so safe read-only QA can run beside an editor while conflicting writers queue;
- move the lean policy from an environment switch into persisted team/profile execution settings once the semantics have been exercised in real projects.

Those changes should land as measured, focused changes with migration notes rather than widening this compatibility-oriented patch.
