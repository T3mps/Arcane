# AGENTS.md

Canonical agent guidance for this repository lives in [CLAUDE.md](CLAUDE.md)
-- read that file. It covers the repository layout, build commands, workspace
rules (/MD, MKS units, shaders-as-data, dispatcher TU ownership), test
conventions, and the engine-as-SDK contract.

## Servitor: live engine checks (all agents and models: Claude, Grok, Codex/GPT)

When a change can be seen in a running host (pixels, Play/Edit, camera/UI,
hot reload, cvars, logs/Problems, entity/ECS state), verify it with
**Servitor**: `Binaries\<Cfg>-windows-x86_64-md\Servitor\servitor.exe`, a
loopback CLI (not MCP). Read `.claude/skills/arcane-servitor/SKILL.md` first;
the summary is in [CLAUDE.md](CLAUDE.md) under "Servitor". The core loop:

`launch runtime --headless -- --fixed-time 0` -> `wait settled` ->
`screenshot` / `compare` / `step N` / `entity get` / `ecs query` / `logs`,
then `quit`.

Pass `--project ReferenceProject` and `--client <name>` on every call,
`launch`, `status` and `quit` included, with a name unique to you (agent name
plus a session or run id; never a shared name like `claude-code`).
`status --client NAME` lists only that client's hosts (use `status` alone to see
everything running). `quit --client NAME` quits that client's host (`--host`
narrows, several need `--yes`, `--pid N` wins); with none, `--host KIND` quits
the KIND host launched without `--client`, never another client's; with none
and no `--host` it is exit 4. Under `--json`/`--format json|ndjson` all output
is on stdout and stderr stays empty. Hosts Servitor launched exit after
`--remote-idle-exit` seconds idle; an editor the user opens by hand never does.
`--owner-pid` is opt-in (Servitor never sets it). Branch on the exit code
(0 ok, 2 usage, 3 permission, 4 no host, 5 retry, 6 failed, 7 host died,
8 check failed).
Never drive an editor the user has open, and never `--bless` through Servitor.
Codex: allow localhost network access in the sandbox, or every call fails
with exit 4/7.
