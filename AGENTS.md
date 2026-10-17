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

Pass `--project ReferenceProject` always and `--client <agent-name>` on every call
except `launch` and `status` (they refuse `--client`, exit 2), and branch on the
exit code (0 ok, 4 no host, 5 retry, 6 failed, 7 host died, 8 check failed).
Never drive an editor the user has open, and never `--bless` through Servitor.
Codex: allow localhost network access in the sandbox, or every call fails
with exit 4/7.
