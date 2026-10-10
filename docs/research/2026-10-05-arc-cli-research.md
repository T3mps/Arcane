# arc CLI -- research for the D27 spec

**Date:** 2026-10-05
**Scope:** roadmap item D27 (`docs/superpowers/specs/2026-10-05-arcane-1.0-roadmap.md`): `arc`, a command-line tool that attaches to a running ArcaneEditor, ArcaneRuntime or ArcaneServer (or launches one and then attaches) and drives it fully. It follows the Unity CLI model and is **not** an MCP server. Its main users are coding agents and CI; humans come second.
**Method:**
- **External survey:** four parallel research passes, all web-sourced, with every claim carrying a URL. Unreal facts were checked against the local UE 5.8.2 source at `D:\dev\_reference\UnrealEngine-5.8.2-release\Engine\` (written `UE\` below).
- **Arcane seams:** read on `main` at `c0fb42e1`, plus the `feat/settings-s7` branch for `RemoteCVarService`.
- **Reliability marks:** **[UNVERIFIED]** marks a claim the researcher could not confirm in a primary source. **[inference]** marks a conclusion that is ours, not the source's.

---

## Executive summary

1. **The industry has converged on one shape:**
   - an HTTP server bound to loopback inside the editor;
   - a descriptor file in the project tree that names the port and carries a per-session bearer token;
   - a JSON envelope;
   - a thin CLI;
   - commands registered in code, each with a schema;
   - an MCP adapter, if there is one, as a thin layer on top.

   Unity's Pipeline package does exactly this (ports 7800-7849, a `.unity-pipeline-port` descriptor, a 256-bit bearer token). Defold does it too (`.internal/editor.port`, OpenAPI, a token file). Bevy's BRP has the same shape minus the auth. `arc` should follow it rather than invent a new one.
2. **UE 5.8 already ships what D27 is reaching for, as MCP.** The experimental `ModelContextProtocol` plugin exposes tools derived from reflection (`meta=(AICallable)`), and includes a **"Playwright-style Slate UI automation toolset"** (`SlateInspectorToolset`: snapshot/ref/click/type/screenshot/WaitFor). It is the closest prior art to the ImGui half of `arc` (`UE\Plugins\Experimental\Toolsets\SlateInspectorToolset\...\SlateInspectorToolset.h:56-73`).
3. **The most common complaint agents have about every one of these systems is the same: errors that come back as successes, plus no readiness signal.** Unreal MCP and Unity's early job model both have it, and so do the UE and O3DE exit codes, which callers end up recovering by scraping logs. Arcane already has the antidote in its own codebase:
   - Rule 3, "no silently inert flags";
   - the seven-value `Verdict`;
   - `exitReason`;
   - `TriedPaths`;
   - HostWitness's precedence ("a killed host's leftover report is never believed").

   `arc` should carry that discipline onto the wire.
4. **Arcane has about 60% of the substrate already:**
   - a validated discovery lock (`EditorLock`, pid plus creation time);
   - a transport-agnostic cvar service (`RemoteCVarService`, S7);
   - a reflection-to-JSON codec that refuses rather than drops data, plus a JSON Schema generator (`ReflectionJson`, `Astra::JsonSchemaGenerator`);
   - runtime-typed component access (`GetComponentByHash`, `AddComponentByID`, and related calls);
   - an undo transaction API (`CommandStack::Begin/Snapshot/Commit/Cancel`);
   - structured, located diagnostics (`Diagnostics::Publish` with `DiagLocator`);
   - Playwright-parity image comparison and a settle (auto-wait) predicate;
   - two capture paths (viewport via `WriteAutoScreenshot`, backbuffer via `--screenshot`).

   **What is missing:**
   - a live control channel;
   - a named action registry (specified in settings §7.2 but not built);
   - any ImGui item addressing (the Test Engine hooks are commented out);
   - a JSON output mode in arcbuild;
   - an open-ended `--headless` (it currently requires `--frames`, precisely because there was no stop channel).
5. **The recommended smallest useful slice** (phase P0): `arc launch / status / wait / quit`, `arc cvar get/set/list/explain`, `arc logs`, `arc problems`, `arc entity get/query` (read-only), and `arc screenshot`. These go over a loopback HTTP JSON-RPC channel with a descriptor file, a token and a stable exit-code contract. That is enough to turn the golden-gate desk check and "is this entity where I think it is" into gates, without touching the editor's UI.

---

## Part 1: External survey

### 1.1 Unity CLI + the Pipeline package (the user's reference model)

**Architecture.** There are three layers:
- the standalone `unity` CLI binary;
- `com.unity.pipeline`, which "turns your running Editor into a local HTTP server";
- `eval`, which compiles Roslyn C# and runs it on the main thread "without recompilation or domain reload".

Sources: [blog](https://unity.com/blog/meet-the-unity-cli), [beginner's guide](https://unity.com/resources/a-beginners-guide-to-unity-cli-and-the-pipeline-package).

Status: the CLI is "experimental" ([intro](https://docs.unity.com/en-us/unity-cli/unity-cli)). It is at 1.0.0-beta.12 as of 2026-09-30 ([release notes](https://docs.unity.com/en-us/unity-cli/release-notes)), the Pipeline package is 0.8.0-exp.1, and the CLI launched on 2026-07-20.

| Dimension | Unity |
|---|---|
| **Transport** | `HttpListener` on `http://127.0.0.1:{port}/`. Clients must dial `127.0.0.1`, not `localhost`: Mono matches the Host header literally. Ports: Editor 7800-7849, test servers 7850-7899, Players 7900-7949 (first free port wins). Endpoints: `POST /api/exec`, `GET /api/status`, `GET /api/commands`, `GET /api/progress`, `GET /api/job?id=`, `POST /api/job/cancel` ([connectivity](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/connectivity.html), [changelog](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/changelog/CHANGELOG.html)). `exec` takes `{"command","parameters"}`, `{"commandLine"}` or `{"argv":[...]}`. Players have their own server, opt-in through `enableInBuilds` and compiled out unless `UNITY_EDITOR \|\| (UNITY_STANDALONE && DEBUG)` ([runtime setup](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/runtime-setup.html)). `unity mcp` is a stdio MCP server inside the CLI that bridges to the same HTTP API ([perflint](https://perflint.dev/blog/unity-cli-eval-mcp-guide/)). |
| **Discovery** | A descriptor at `<project>/Library/Pipeline/.unity-pipeline-port` holding `{pid, port, projectPath, projectName, unityVersion, mode, startedAt, lastHeartbeat, evalToken, capabilities[]}`. It is rewritten on every heartbeat and deleted on shutdown. Test servers write no descriptor, so they never hijack discovery ([connectivity](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/connectivity.html), [testing](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/testing.html)). `unity status` lists port/state/project/version/PID. `--until-ready` (default timeout 300 s, exit 6 on timeout) and a main-thread probe (`starting` vs `ready`) arrived in beta.10-12. When several Editors match, the CLI targets the main Editor by default and a clone through `--project-path` ([release notes](https://docs.unity.com/en-us/unity-cli/release-notes)). |
| **Auth/security** | `Authorization: Bearer <evalToken>`: 256 bits of CSPRNG output, compared in constant time, 401 when wrong. The token lives in `SessionState`, so it **survives domain reloads**. Before 0.4, a recompile rotated it and gave long-lived clients a 401. The server rejects non-loopback peers and **any request carrying an `Origin` header** (anti-browser/DNS rebinding). Since 0.8 it binds to `127.0.0.1` only; before that the LAN could reach it ([connectivity](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/connectivity.html), [changelog](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/changelog/CHANGELOG.html)). Writes are confined to an authoring root, with `..` refused ([safety](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/safety-and-mutations.html)). `eval` amounts to remote code execution gated only by the token. |
| **Command model / self-description** | `[CliCommand("name","desc", MainThreadRequired=true, RuntimeOnly=false)]` and `[CliArg(...)]` on static methods, hierarchical `/`-separated `Tags`, and `CommandRegistry.RegisterCommand()` at runtime ([creating commands](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/creating-commands.html)). Authoring commands take `ObjectRef` handles that accept "globalId/path/guid/instanceId/hierarchyPath" and return `AuthoringResult` identities that chain into the next command ([authoring](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/authoring-commands.html)). Destructive commands take `dry_run` (wins) and `confirm` ([safety](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/safety-and-mutations.html)). Listing: `unity command` with no name lists about 50 **tags** with counts (beta.12), filterable with `--tag/--query`. `unity list` gives tools with parameter schemas, and `unity commands --format json` is the CLI's own manifest ([reference](https://docs.unity.com/en-us/unity-cli/unity-cli-reference), [release notes](https://docs.unity.com/en-us/unity-cli/release-notes)). There were 148 built-in tools at beta.3 ([perflint](https://perflint.dev/blog/unity-cli-eval-mcp-guide/)). A transactional `batch` takes up to 200 ops, with `"$0.instanceId"` back-references ([batch](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/batch.html)). |
| **Output** | `human` (TTY default), `tsv` (pipe default), `json` (envelope `success, command, data, errors, warnings`), `ndjson` (progress frames, then a `result` frame whose `data.count` makes truncation detectable), and `github` (`::error::` annotations). Selected through `UNITY_FORMAT`. In json mode a failure still writes the full envelope to stdout, and callers branch on `errors[0].code`. Envelopes also carry `notifications[{code,message,remediation}]`. `--result-only` prints only the value ([reference](https://docs.unity.com/en-us/unity-cli/unity-cli-reference), [release notes](https://docs.unity.com/en-us/unity-cli/release-notes)). |
| **Streaming/long jobs** | Server side: `exec` with `"job":true` returns a job id; `/api/job` and `/api/job/cancel` follow it up; jobs do **not** survive domain reloads. CLI side: `--detach`, then `unity job wait\|status\|cancel` ([changelog](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/changelog/CHANGELOG.html)). Some commands are async on their own and come with status twins (`build`/`build_status`, `recompile`/`recompile_status`, `run_tests`/`test_status`) ([build](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/build-and-compilation.html)). `wait_for` evaluates a member/op/value condition every frame and can act **in the same frame** through `on_met`; it allows 8 concurrent waits, and a reload resolves them as `interrupted` ([wait](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/wait.html)). `unity watch test` reruns affected tests ([release notes](https://docs.unity.com/en-us/unity-cli/release-notes)). |
| **Errors / exit codes** | 0 ok · 1 general · 2 usage · 3 auth · 4 config required · 6 primary operation failed · 7 service/Editor unreachable after retry (retryable) · 8 `unity test`: tests ran and some failed · 130 SIGINT · 143 SIGTERM. Code 5 is undocumented ([reference](https://docs.unity.com/en-us/unity-cli/unity-cli-reference)). `recompile`: 0 compiled, 6 errors, 7 no Editor. An Editor-reported argument error counts as "an answer", so it exits 6, not 7 ([release notes](https://docs.unity.com/en-us/unity-cli/release-notes)). Server side: string `errorCode` (`INVALID_COMMAND_ARGS`), HTTP 401, and **503 while `settling`** (cold import or compile). The status `blocked_by_dialog` is reported while a modal is up ([connectivity](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/connectivity.html)). |
| **Screenshots** | `capture_game_view` (`source=screen` includes the overlay UI; `source=camera` renders one camera off-screen); returns inline base64, or path-only when `save_path` is set. `capture_scene_view`. `capture_editor_element` captures a UI Toolkit element **by CSS selector**, on 6000.7+ ([capture](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/capture.html)). When the main thread is blocked, the MCP path falls back to an OS-level desktop screenshot ([release notes](https://docs.unity.com/en-us/unity-cli/release-notes)). |
| **UI tree** | `get_scene_hierarchy` (nodes carry `instanceId + hierarchyPath`), `find_gameobjects`, `get_component_properties`, `get/set_selection`, `menu` ([scenes](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/scenes.html), [GameObjects](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/gameobjects-and-components.html)). **No documented editor-window visual-tree dump [UNVERIFIED].** Runtime input: `simulate_key` and `simulate_pointer` ([runtime](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/runtime.html)). |
| **Undo** | `AuthoringUndoScope` collapses a multi-step command into one Ctrl+Z. A transactional batch is one undo step, rolled back with `RevertAllDownToGroup` on failure, and non-undoable ops are refused with `not_batchable_transactional`. GameObject mutations are blocked in Play mode ([safety](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/safety-and-mutations.html), [batch](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/batch.html)). |
| **Versioning** | The descriptor's `capabilities[]` (`exec.argv`, `exec.commandLine`) is the feature negotiation. The CLI degrades gracefully against older packages and reports "package too old" rather than "not found" ([release notes](https://docs.unity.com/en-us/unity-cli/release-notes)). The package is 0.x-exp with breaking renames ([changelog](https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/changelog/CHANGELOG.html)). |
| **Why CLI over MCP / agent pain** | The in-editor MCP server is deprecated: "`unity command` and `unity eval` ... drive the Editor directly without MCP (faster, fewer tokens)" ([replace-MCP](https://docs.unity.com/en-us/unity-cli/replace-mcp-server-unity-cli)). Problems with the old bridge: "domain reloads caused socket drops and null exceptions" ([guide](https://unity.com/resources/a-beginners-guide-to-unity-cli-and-the-pipeline-package)); a 401 after every recompile; opaque capacity and licence errors ([forum](https://discussions.unity.com/t/unity-ai-assistant-2-7-0-mcp-server-capacity-limit/1718606)); a schema of about 96 KB, roughly 24k tokens ([m4bwav/unity-agent](https://github.com/m4bwav/unity-agent)); zero-tool sessions when the client starts before the Editor. Problems that persist with the CLI: foreground focus needed for asset refresh, modal dialogs, `eval` dying on reload, roughly 0.8 s per call ([Vindler](https://vindler.solutions/blog/unity-cli-agent-automation)), a 22 ms vs 118 ms persistent-vs-fresh-process gap ([agmazon](https://agmazon.com/blog/articles/technology/202609/unity-cli-mcp-guide-en.html)), and the need for `unity shell` as a warm process. Sandboxed agents (Codex) block localhost, so `unity mcp configure codex` relaxes the sandbox network policy ([release notes](https://docs.unity.com/en-us/unity-cli/release-notes)). Perflint's maxim: "registered commands for verdicts; eval for premises" ([perflint](https://perflint.dev/blog/unity-cli-eval-mcp-guide/)). |

### 1.2 Unreal Engine

**Remote Control API.**
- **Transport and discovery.**
  - HTTP on 30010 and WebSocket on 30020. The WebSocket bind defaults to **0.0.0.0** ("will open the connection to everyone on your network") (`UE\Plugins\VirtualProduction\RemoteControl\Source\RemoteControlCommon\Public\RemoteControlSettings.h:290-319`).
  - There is no discovery mechanism.
- **Routes** (`WebRemoteControl.cpp:588-825`; [HTTP reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/remote-control-api-http-reference-for-unreal-engine)):
  - `/remote/info`, `/remote/batch`, `/remote/object/call`, `/remote/object/property` (with `READ_ACCESS`, `WRITE_ACCESS` or `WRITE_TRANSACTION_ACCESS`), `/remote/object/describe`, `/remote/search/assets`, plus about 20 preset routes.
  - Objects are addressed by UObject path. **Presets** are curated façades that insulate callers from path churn ([preset reference](https://dev.epicgames.com/documentation/unreal-engine/remote-control-preset-api-http-reference-for-unreal-engine?lang=en-US)).
- **Self-description.** `GET /remote/info` lists every route with its verb and description (`WebRemoteControl.cpp:884-903`).
- **WebSocket protocol** (`WebSocketMessageHandler.cpp:322-380`; `RemoteControlWebSocketServer.cpp:28-71`):
  - The envelope is `{MessageName, Parameters, Id?, Passphrase?}`.
  - Messages include `preset.register`, `actors.register`, `object.call`, **`transaction.begin`/`transaction.end`**, `http` (tunnels any route) and `batch`.
  - Property and transaction events are pushed and batched at end of frame.
- **Undo, which is the best part of the design:**
  - `generateTransaction:true` wraps the call in a "Remote Call Transaction Wrap" undo entry (`WebRemoteControlInternalUtils.cpp:175`).
  - `transaction.begin`/`end` groups a slider drag into one undo step.
- **Auth.**
  - An MD5-hashed passphrase in a header, but only when `bRestrictServerAccess` is on, which defaults to **false**. **Localhost always bypasses it**, `AllowedOrigin` defaults to `"*"`, and the default allowlist is "all IPs" (`RemoteControlSettings.h:353-399`; `RemoteControlDefaultPreprocessors.cpp:235-290`).
  - Arbitrary function calls, console commands and Python are separate opt-in switches (`RemoteControlSettings.h:345-377`).
- **Errors:** HTTP status plus `{errorMessage}`.
- **Versioning:** no API version field was found **[UNVERIFIED that none exists]**.

**Python remote execution** (`UE\Plugins\Experimental\PythonScriptPlugin\Source\PythonScriptPlugin\Private\PythonScriptRemoteExecution.cpp:25-195`).
- **Discovery:** UDP multicast `239.0.0.1:6766`, bound to 127.0.0.1 with TTL 0, using `ping`/`pong` (`pong` carries user, machine, engine version, project).
- **Command channel:** `open_connection` makes the **editor connect to the client's TCP server**. The TCP channel carries `command {command, unattended, exec_mode: ExecuteFile|ExecuteStatement|EvaluateStatement}` and returns `command_result {success, result, output[{type, output}]}`.
- **Auth:** none. The feature is off by default (`PythonScriptPluginSettings.cpp:15-19,644`).
- **Pain points:**
  - Multicast discovery breaks on VPN adapters ([PyPI unreal-engine-mcp](https://pypi.org/project/unreal-engine-mcp/)).
  - No schema; the agent must already know the `unreal` API.
  - Output is a flat log rather than structured data.

**Console commands, `-ExecCmds` and CmdLink.**
- `-ExecCmds` queues `DeferredCommands` (`UE\Source\Runtime\Engine\Private\UnrealEngine.cpp:2552`). Gauntlet injects `-ExecCmds="Automation RunTests ..."` this way (`RunUnreal.cs:259-264`).
- **CmdLinkServer** is a Windows named pipe, `\\.\pipe\UnrealEngine-CLI[-<key>]`, enabled by `-cmdlink`. It runs console commands from `CmdLink.exe`. Async commands call `BeginAsyncCommand`/`EndAsyncCommand` so the pipe waits for completion (`UE\Plugins\CmdLinkServer\Source\CmdLinkServer\Private\CmdLinkServer.cpp:19-29,125,341-343`; `Public\CmdLinkServer.h:24-29`). **This is a small, clean named-pipe precedent for a CLI talking to a live editor.**

**Automation Test framework and Gauntlet.**
- **Commands:** `Automation List|RunTests|RunAll|RunFilter|SetFilter|Now|Quit|SoftQuit|StartRemoteSession ...` (`UE\Source\Developer\AutomationController\Private\AutomationCommandline.cpp:780-798`).
- **Reports:** `-ReportExportPath` writes `index.json` and `index.html` (`AutomationControllerManager.cpp:233,369`).
- **Exit codes:** **only 0 or -1** (failures set `GIsCriticalError`). The process prints `**** TEST COMPLETE. EXIT CODE: %d ****` because "some tools parse this" (`AutomationCommandline.cpp:493-504`).
- **Distribution:** workers are found over MessageBus UDP multicast `230.0.0.1:6666` (`UdpMessagingPrivate.h:13`). An experimental `AutomationControllerRpc` plugin also exists.
- **Screenshots:** comparison against approved ground truth, with SSIM, tolerances and per-Platform_RHI_ShaderModel buckets (`IScreenShotManager.h:60-87`, `ImageComparer.h:489-502`; [doc](https://dev.epicgames.com/documentation/unreal-engine/screenshot-comparison-tool-in-unreal-engine)).
- **Gauntlet** ([overview](https://dev.epicgames.com/documentation/unreal-engine/gauntlet-automation-framework-overview-in-unreal-engine)):
  - It spawns sessions and grades them through `GetExitCodeAndReason` (exit code plus log analysis), with `TestResult {Passed, Failed, TimedOut, WantRetry}` (`Gauntlet.UnrealTestNode.cs:390,2078,2252`).
  - Arcane's HostWitness already imitates and corrects its precedence cascade (Part 2.2).

**Unreal Insights / trace.**
- `UnrealTraceServer` records on TCP 1981 and exposes its store on 1989 (`StoreSettings.h:14-15`).
- A run can stream with `-tracehost=` or write `-tracefile`, and the console offers `Trace.Send/Trace.File` (`TraceAuxiliary.cpp:1977-2004,1540-1846`; [doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/trace-in-unreal-engine-5)).
- The format is binary and aimed at the GUI, not at agents.

**UE 5.8 "Unreal MCP": the headline prior art.**
- **Transport and safety.**
  - Streamable HTTP plus SSE at `http://127.0.0.1:8000/mcp`, disabled and experimental by default (`UE\Plugins\Experimental\ModelContextProtocol\...\ModelContextProtocolSettings.h:23-46`).
  - It checks the `Origin` header for localhost only, as an anti-rebinding measure, and has no other auth. Epic's own doc says: "There is no authentication layer" ([doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-mcp-in-unreal-editor)). Epic's Claude Code skill warns "Localhost is not a trust boundary" ([EpicGames skills](https://github.com/EpicGames/unreal-engine-skills-for-claude-code-plugin)).
- **Tools come from reflection:** a `UFUNCTION(meta=(AICallable))` on a `UToolsetDefinition` (`ToolsetDefinition.h:12-28`), or a Python `@toolset_registry.tool_call` using type hints. There are about 28 toolset plugins.
- **Tool-search mode.** `tools/list` returns only `list_toolsets`/`describe_toolset`/`call_tool`, which keeps the catalog small and the prompt cache warm (`ModelContextProtocolToolSearch.h:10-80`).
- **Tool interface.** `IModelContextProtocolTool` has an input schema, an optional output schema, `Run`/`RunAsync` (with a callback that must fire exactly once; the server hops back to the game thread) and `CancelAsync` (`IModelContextProtocolTool.h:23-101`).
- **`SlateInspectorToolset`, a Playwright-style UI tool set:**
  - `Snapshot(Ref, MaxDepth, bIncludeSourceLocations)`, `Observe/Unobserve`, `Screenshot(Ref)`, `Click`, `Hover`, `Type`, `PressKey`, `SelectOption`, `Drag`, `Windows`, `WaitFor` (non-blocking, polled) and `FillForm`.
  - Observers re-walk their subtree about every 100 ms to keep refs valid.
  - Input goes through Slate events "because AutomationDriver's synchronous API deadlocks when called from the game thread" (`SlateInspectorToolset.h:56-181`).
- **Undo and sandboxing.**
  - `UToolsetLibrary::UndoTransaction` rolls back failed scripts (`ToolsetLibrary.h:110-138`).
  - `FGlobalSandbox` provides file sandboxing (`SandboxLibrary.h:12-74`).
  - The AI assistant uses separate named undo buffers (`AIAssistantTransactionBufferManager.h:13-64`).
- **Agent pain:**
  - **"MCP formally returns success, while the text body contains an error."**
  - No readiness signal: early clients see a partial tool list.
  - Blueprint graphs are write-only.
  
  Source: [forum](https://forums.unrealengine.com/t/unreal-mcp-codex-issue-analysis-report/2730883).
  - Results drowned in thousands of runtime-error lines ([forum](https://forums.unrealengine.com/t/mcp-tool-calls-return-blueprint-runtime-errors-instead-of-the-tools-return-value/2833175)).
  - Epic's own skill says edits are "not always undoable, especially across compilation boundaries", calls "hang or fail" during compiles, and "always check the result" ([SKILL.md](https://github.com/EpicGames/unreal-engine-skills-for-claude-code-plugin/blob/main/skills/unreal-mcp/SKILL.md)).
  - New `UFUNCTION`s require an editor restart.
- **Other UI introspection.**
  - The Widget Reflector supports snapshots over MessageBus (`WidgetSnapshotService.h:15-30`).
  - The AI Assistant's `SlateQuerier` builds `widget_path` context (`AIAssistantSlateQuerier.h:31-70`).

### 1.3 Godot

- **Remote debugger.**
  - **The editor listens and the game connects to it.** The editor binds `127.0.0.1:6007` and walks to the next port up if 6007 is taken; it also supports `unix://`. The game connects through `--remote-debug tcp://host:port` ([editor_debugger_server.cpp](https://github.com/godotengine/godot/blob/master/editor/debugger/editor_debugger_server.cpp), [remote_debugger_peer.cpp](https://github.com/godotengine/godot/blob/master/core/debugger/remote_debugger_peer.cpp), [editor_settings.cpp](https://github.com/godotengine/godot/blob/master/editor/settings/editor_settings.cpp)).
  - **Framing:** a length prefix plus a binary-encoded Variant Array ([binary serialization](https://docs.godotengine.org/en/stable/tutorials/io/binary_serialization_api.html)).
  - **Routing:** messages are routed by prefix up to the first `:` to a registered capture, and **unknown prefixes are silently dropped** ([remote_debugger.cpp](https://github.com/godotengine/godot/blob/master/core/debugger/remote_debugger.cpp)).
  - **Extension points:** `EngineDebugger.register_message_capture` and `EditorDebuggerPlugin` ([EngineDebugger](https://docs.godotengine.org/en/stable/classes/class_enginedebugger.html), [EditorDebuggerPlugin](https://docs.godotengine.org/en/stable/classes/class_editordebuggerplugin.html)).
- **The `scene:` capture** offers:
  - `request_scene_tree`, `inspect_object(s)`, `set_object_property(_field)`;
  - `live_*` edits;
  - `next_frame`, `suspend_changed`, `speed_changed`;
  - `rq_screenshot`. The screenshot replies with **a PNG path**, not pixels ([scene_debugger.cpp](https://github.com/godotengine/godot/blob/master/scene/debugger/scene_debugger.cpp)).
- **What it lacks:** no schema, no versioning, no auth. Whether remote edits go through UndoRedo is **[UNVERIFIED]**.
- **Headless.** `--headless` means a dummy display and audio driver ([CLI tutorial](https://docs.godotengine.org/en/stable/tutorials/editor/command_line_tutorial.html)). In Godot 4 the rendering server is Dummy, so **viewport capture is impossible headless** ([forum](https://forum.godotengine.org/150510/godot-headless-not-same-godot-window-cant-render-the-backend)). Arcane's `--headless` renders the real graph offscreen, which is a genuine advantage.
- **Language servers.** The GDScript LSP is TCP 6005 and the DAP is 6006. Both run inside the editor and are TCP-only, so stdio clients need a bridge ([LSP source](https://github.com/godotengine/godot/blob/master/modules/gdscript/language_server/gdscript_language_server.cpp), [DAP header](https://github.com/godotengine/godot/blob/master/editor/debugger/debug_adapter/debug_adapter_server.h)).
- **MCP servers.** Every serious Godot MCP **bypasses the debugger protocol** and adds its own channel:
  - [KeeVeeG/godot-mcp](https://github.com/KeeVeeG/godot-mcp): WebSocket JSON-RPC to an editor plugin, mutations routed through `EditorUndoRedoManager`, a runtime autoload, ports auto-negotiated in 6505-6514, and timeouts during compiles.
  - [godot-mcp-pro](https://github.com/youichi-uda/godot-mcp-pro): tool presets of Full 187 / Lite 88 / Minimal 35 to fit context, and it advertises "All node/property operations support Ctrl+Z".
  - [godot-mcp-runtime](https://github.com/Erodenn/godot-mcp-runtime): screenshots as a 960x540 inline preview plus the full PNG on disk; a UI walk of visible Controls; input results that report which Control was hit and which signals fired.

### 1.4 Bevy Remote Protocol (BRP): the closest architectural cousin

- **Versions.** BRP arrived in 0.15 to serve "the upcoming Bevy Editor" ([0.15 notes](https://bevy.org/news/bevy-0-15/)). The current release is 0.19.1 ([docs.rs](https://docs.rs/bevy_remote/latest/bevy_remote/)) and main is 0.20-dev.
- **Transport.**
  - JSON-RPC 2.0 over HTTP POST at `127.0.0.1:15702`. On main the render sub-app gets its own server on 15703. Batches are supported, but streaming methods are refused inside a batch ([http.rs](https://github.com/bevyengine/bevy/blob/main/crates/bevy_remote/src/http.rs), [lib.rs](https://github.com/bevyengine/bevy/blob/main/crates/bevy_remote/src/lib.rs)).
- **Execution.**
  - Requests are drained in a `RemoteLast` schedule after `Last`.
  - Handlers are systems with **exclusive `&mut World`**, `fn(In<Option<Value>>, &mut World) -> BrpResult`, which means **every request applies at a frame boundary** (lib.rs).
- **Methods.**
  - The 0.17 rename moved `bevy/*` to `world.*` and broke every client ([PR #19377](https://github.com/bevyengine/bevy/pull/19377), [migration guide](https://bevy.org/learn/migration-guides/0-16-to-0-17/)).
  - Methods in 0.19.1: `world.query`, `world.get_components`, `insert/remove/mutate_components`, `spawn_entity`, `despawn_entity`, `reparent_entities`, `list_components`, `get_components+watch`, `list_components+watch`, `get/insert/remove/mutate/list_resources`, `trigger_event`, `write_message`, `observe+watch`, `schedule.list`, `schedule.graph`, `registry.schema`, `rpc.discover` ([builtin_methods](https://docs.rs/bevy_remote/0.19.1/bevy_remote/builtin_methods/index.html)).
  - Main adds `app.info`, `diagnostics.*`, **`stepping.*` (step_frame)**, `world.inspect*`, `world.summarize` and `registry.component_metadata` ([builtin_methods.rs](https://github.com/bevyengine/bevy/blob/main/crates/bevy_remote/src/builtin_methods.rs)).
- **Query shape.**
  - `world.query {data:{components, option, has}, filter:{with, without}, strict}`. Types are named by full `TypePath`.
  - With `strict:false`, the result carries `{components, errors}` rather than failing.
  - `mutate_components {entity, component, path, value}` uses reflect `GetPath` syntax.
- **Streaming.** A `+watch` method answers with SSE (`text/event-stream`), re-runs each frame, and returns deltas (`{components, removed, errors}`). There is **no job model**.
- **Reflection.**
  - Reads use `ReflectSerializer` and writes use `TypedReflectDeserializer` against the `AppTypeRegistry`, so only registered types are reachable.
  - `registry.schema` returns `JsonSchemaBevyType` (standard JSON Schema plus `typePath`, `reflectTypes`, `kind`, ...) ([JsonSchemaBevyType](https://docs.rs/bevy_remote/0.19.1/bevy_remote/schemas/json_schema/struct.JsonSchemaBevyType.html)).
  - **`rpc.discover` is OpenRPC with method names only**: "in methods array only the name ... is populated" ([PR #18068](https://github.com/bevyengine/bevy/pull/18068)).
- **Errors.** Standard JSON-RPC codes plus Bevy-specific ones: -23401 entity not found, -23402 component reflect error, -23403 component not present, -23404 self-reparent, -235xx resources (lib.rs).
- **Auth and extension.** No auth ("no authentication", [client.rs](https://github.com/bevyengine/bevy/blob/main/crates/bevy_remote/src/client.rs)). Custom methods are added through `RemotePlugin::with_method`. No undo.
- **Community tools fill the gaps:**
  - [bevy_brp_extras](https://github.com/natepiano/bevy_brp/blob/main/extras/README.md):
    - screenshots written atomically as PNG paths;
    - send_keys, type_text and mouse input;
    - `register_agent_tool`, a catalog that attaches descriptions and params/result schemas and fails as a whole if any entry is stale.
  - [bevy_brp_mcp](https://github.com/natepiano/bevy_brp/blob/main/mcp/README.md):
    - `brp_type_guide`, which exists because building payloads is hard;
    - `brp_launch`, which sends the launched app's stdout to log files;
    - one compatibility pin per Bevy release.
- **Lesson [inference]:** an ECS engine with reflection can get query/get/insert/mutate almost for free. Schemas for **commands** (not just types) and **visual and UI introspection** have to be designed in; they do not fall out of reflection.

### 1.5 O3DE

- **RemoteTools gem.**
  - AzNetworking TCP. Hosts `Listen(port)`, and clients join `127.0.0.1:<port>` with a 1 s retry ([RemoteToolsSystemComponent.cpp](https://github.com/o3de/o3de/blob/development/Gems/RemoteTools/Code/Source/RemoteToolsSystemComponent.cpp)).
  - Messages are binary `AZ::ObjectStream` through the SerializeContext. Services register with `RegisterToolingServiceHost/Client(Crc32, Name, port)` ([IRemoteTools.h](https://github.com/o3de/o3de/blob/development/Code/Framework/AzFramework/AzFramework/Network/IRemoteTools.h)).
  - Fixed ports: Lua 6777, ScriptCanvas 45641 ([ScriptRemoteDebuggingConstants.h](https://github.com/o3de/o3de/blob/development/Code/Framework/AzFramework/AzFramework/Script/ScriptRemoteDebuggingConstants.h)).
  - No auth and no discovery.
- **Legacy Remote Console** (from CryEngine).
  - TCP from port 4600, with the client scanning 8 ports.
  - Single-character message types.
  - **The entire protocol is "send command, then wait for this log line"**. For example, screenshots are `r_GetScreenShot 2` followed by waiting for `'Screenshot: '` ([remote_console_commands.py](https://github.com/o3de/o3de/blob/development/Tools/RemoteConsole/ly_remote_console/ly_remote_console/remote_console_commands.py)).
- **Editor Python.**
  - The EditorPythonBindings gem plus `azlmbr` EBus calls. Only BehaviorContext items scoped `Automation` or `Common` are exposed, which is **reflection-driven exposure with a scope flag** ([PythonUtility.h](https://github.com/o3de/o3de/blob/development/Gems/EditorPythonBindings/Code/Include/EditorPythonBindings/PythonUtility.h)).
  - Properties are addressed by pipe paths such as `"MeshComponentRenderNode|Mesh asset"`.
  - Scripts wait with `idle_wait`, because work is frame-pumped ([editor-automation.md](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/editor/editor-automation.md)).
- **Test framework.**
  - pytest launches the Editor with `-runpythontest <file> -BatchMode -autotest_mode [-rhi=null]` and runs 8 parallel editors.
  - **Exit codes are 0 or 0xF, and anything else is a crash.**
  - Per-test results come from `JSON_START(...)JSON_END` **embedded in the log** ([multi_test_framework.py](https://github.com/o3de/o3de/blob/development/Tools/LyTestTools/ly_test_tools/o3de/multi_test_framework.py), [utils.py](https://github.com/o3de/o3de/blob/development/AutomatedTesting/Gem/PythonTests/EditorPythonTestTools/editor_python_test_tools/utils.py)).
  - Atom golden-image comparison is in [atom_component_helper.py](https://github.com/o3de/o3de/blob/development/AutomatedTesting/Gem/PythonTests/Atom/atom_utils/atom_component_helper.py).
  - No Qt widget-tree remote API was found **[UNVERIFIED]**.

### 1.6 Playwright and CDP: the gold standard for UI piloting

- **CDP transport.**
  - Launch with `--remote-debugging-port` (0 picks a free port; the WebSocket URL is printed on stderr).
  - HTTP `/json/version` returns `webSocketDebuggerUrl`, `/json/list` lists targets, and **`/json/protocol` returns the running browser's full schema** ([CDP](https://chromedevtools.github.io/devtools-protocol/), [getting started](https://github.com/aslushnikov/getting-started-with-cdp)).
  - Messages are `{id, method, params}`, events carry no `id`, and flattened `sessionId`s address targets.
  - **Chrome 136 refuses the debug port on the default profile** because of cookie theft, which is a lesson in the opt-in for debug ports ([blog](https://developer.chrome.com/blog/remote-debugging-port)).
- **Accessibility domain** ([Accessibility.pdl](https://raw.githubusercontent.com/ChromeDevTools/devtools-protocol/master/pdl/domains/Accessibility.pdl)):
  - It is experimental.
  - `getFullAXTree` and the server-side `queryAXTree(accessibleName, role)`.
  - Node IDs are stable only while the domain is enabled.
- **Locators** ([locators](https://playwright.dev/docs/locators)).
  - Preference order: `getByRole`, then `getByText`/`Label`, then `getByTestId`.
  - **Strict mode:** an action whose locator matches more than one element throws. That is the key agent-safety property.
- **Auto-wait / actionability** ([actionability](https://playwright.dev/docs/actionability)). Before acting, Playwright checks that the element is:
  - Visible;
  - **Stable**, meaning the same box for two consecutive animation frames;
  - Receives Events, meaning it is the hit target;
  - Enabled;
  - Editable.
- **Tracing.** `trace.zip` holds actions, before/after DOM snapshots, console output and a filmstrip, and opens with `show-trace` ([trace viewer](https://playwright.dev/docs/trace-viewer)).
- **Visual diff.**
  - `toHaveScreenshot` waits for **two consecutive identical screenshots**, then compares with YIQ `threshold` (0.2), `maxDiffPixels`/`maxDiffPixelRatio`, `mask` and `animations:"disabled"`.
  - It updates with `--update-snapshots`, and golden names encode browser and OS ([test-snapshots](https://playwright.dev/docs/test-snapshots), [assertions](https://playwright.dev/docs/api/class-pageassertions)).
  - Arcane's `ImageCompare` is a constant-for-constant port of this.
- **Aria snapshots.** A YAML accessibility tree with partial and regex matching: **structural goldens that diff as text** ([aria-snapshots](https://playwright.dev/docs/aria-snapshots)).
- **Playwright MCP vs Playwright CLI.**
  - **MCP** uses the accessibility tree, not pixels, and `browser_click` takes `ref=e12` from the last snapshot ([playwright-mcp](https://github.com/microsoft/playwright-mcp)). Refs go stale when the page changes **[exact lifetime UNVERIFIED in Microsoft docs]**.
  - **CLI** (`@playwright/cli`) **writes snapshots to disk** and returns only URL, title and file path. The stated rationale: "CLI invocations are more token-efficient: they avoid loading large tool schemas and verbose accessibility trees into the model context" ([playwright-cli README](https://raw.githubusercontent.com/microsoft/playwright-cli/main/README.md)).
  - A later measurement found the two now cost about the same, because MCP also writes snapshots to disk and harnesses defer-load schemas ([Checkly, 2026-07-30](https://www.checklyhq.com/blog/mcp-vs-cli-token-efficiency/)).
  - **The real lesson: put large payloads in files and return handles. The transport doesn't decide token cost.**

### 1.7 Dear ImGui Test Engine

- **Item references** ([Named References](https://github.com/ocornut/imgui_test_engine/wiki/Named-References)):
  - `ImGuiTestRef{ID, Path}`.
  - Paths are `//Window/Button` (absolute) or relative to `SetRef`.
  - Special segments: `//$FOCUSED`, `$$5` (an int `PushID`), `$$(ptr)0x...`, `###id` (stable id independent of the label), the `**/` wildcard (searches by label across child windows), and the `\\/` escape.
- **Locating items.**
  - Items are known only through the hooks. `ImGuiTestEngineHook_ItemAdd(ctx, id, bb, lastItemData)` records rectangles for watched ids, and GatherTask / FindByLabelTask resolve wildcards ([imgui_te_engine.cpp](https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_te_engine.cpp)). So **an item must be submitted in a frame before it can be found** [inference].
  - `ImGuiTestItemInfo` has `{ID, DebugLabel, Window, ParentID, RectFull, RectClipped, ItemFlags, StatusFlags, FramesNotMoving, ...}` ([imgui_te_engine.h](https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_te_engine.h)).
  - Queries never return null; they set an error, unless `ImGuiTestOpFlags_NoError` is passed ([imgui_te_context.h](https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_te_context.h)).
- **Driving widgets.**
  - Simulated `io.AddMousePosEvent/AddMouseButtonEvent/AddKeyEvent/AddInputCharacter`, while **backend events are removed** so real input cannot interfere (imgui_te_engine.cpp).
  - Actions include `ItemClick`, `ItemInput(Value)`, `ItemCheck`, `ItemOpen`, `MenuClick("File/Save")`, `ComboClick("Combo/Item")`, `ItemDragAndDrop`, `KeyPress`, `WindowFocus` and `CaptureScreenshotWindow` (imgui_te_context.h).
  - The defaults amount to actionability checks: auto-open the parent path, auto-scroll, auto-uncollapse, wait until not moving, verify the hovered id (all opt-out through op flags).
- **Execution model.**
  - The test function is a coroutine that **never runs in parallel with the main thread** ([Overview](https://github.com/ocornut/imgui_test_engine/wiki/Overview)).
  - Speeds are Fast (teleport), Normal and Cinematic. It runs headless.
  - Results export as JUnit XML ([imgui_te_exporters.h](https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_te_exporters.h)).
  - Capture goes through a backend `ImGuiScreenCaptureFunc`, with stitch-scroll and video support ([imgui_capture_tool.h](https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_capture_tool.h)).
  - There is no golden diffing **[absence not exhaustively verified]**.
- **Hooks** (in `imgui_internal.h` under `IMGUI_ENABLE_TEST_ENGINE`; [imgui_internal.h](https://raw.githubusercontent.com/ocornut/imgui/master/imgui_internal.h)):
  - `ImGuiTestEngineHook_ItemAdd`, `_ItemInfo(ctx, id, label, statusFlags)`, `_Log` and `ImGuiTestEngine_FindItemDebugLabel`.
  - **`ImGuiItemStatusFlags_Openable/Opened/Checkable/Checked/Inputable` exist only under that define.** They are the only role-like metadata ImGui has, so **any in-house tool needs these same hooks** [inference].
- **Licence** ([LICENSE.txt v1.04](https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/LICENSE.txt)).
  - It is free if **any** of these hold:
    - you are a natural person;
    - you are a non-legal or not-for-profit entity;
    - you use it for education;
    - **you use it for Derivative Software released publicly under an OSI licence**;
    - you are an entity with turnover under USD 2M in the last fiscal year.
  - Otherwise there is a 45-day trial and then a paid licence from DISCO HELLO.
  - It does not distinguish internal tooling from shipped products.
  - For Arcane (MIT, public, `LICENSE`), vendoring falls under the OSI-derivative clause, and the author is a natural person [inference, not legal advice]. A **closed** downstream game studio over the threshold would need its own licence if it built on it.
- **ImGui's own tools.**
  - Metrics, the Debug Log, the **ID Stack Tool** ("hover items ... to query information about the source of their unique ID") and the Item Picker ([Debug Tools wiki](https://github.com/ocornut/imgui/wiki/Debug-Tools)).
  - `IMGUI_DISABLE_DEBUG_TOOLS` compiles them out.
- **Remote ImGui and accessibility.**
  - [netImgui](https://github.com/sammyfreg/netImgui) streams draw data and receives input back.
  - [RemoteImGui](https://github.com/JordiRos/remoteimgui) is historical.
  - Accessibility is still open ([imgui #4122](https://github.com/ocornut/imgui/issues/4122)), and there is no official AccessKit integration ([AccessKit](https://github.com/AccessKit/accesskit)).

### 1.8 Other prior art

- **Defold: the best-documented editor HTTP API, and the closest to `arc`.**
  - Editor API ([editor HTTP API](https://defold.com/manuals/editor-http-api)):
    - The port is in **`.internal/editor.port`**.
    - Clients should **poll `/openapi.json` until it is valid** ("do not assume that creating the process means the project is ready"). OpenAPI 3.0.3 is "the source of truth", and the docs advise against hard-coded endpoint lists.
    - Commands are `POST /command/<name>`. Since 1.13.2, long commands block and return `{success, issues[{message, severity, resource, range}]}`.
    - HTTP status codes: 200 done, **202 accepted, which is "not proof that the requested result exists"**, 403 not active in this state, 404 unavailable, 422 build or validation failed.
    - `/console/stream` streams the console.
    - `/preview/{path}` renders a resource to PNG, explicitly "not a screenshot of the running game".
    - `/eval` (editor Lua) needs a bearer token from **`.internal/editor.token`**, to be kept out of "prompts, reports, or logs".
    - Edits go through `editor.transact()`.
    - `/ref?q=` searches the docs, to save agent tokens.
  - The separate **engine service** is debug builds only, uses a dynamic port announced in a log line, provides `/ping`, `/info`, `/state` and `/post/<socket>/<msg>`, and has **no authentication**. The official **Automation Bridge** extension mounts `/automation-bridge/v1` (health/capabilities, scene inspection, input click, screenshot), and it is stripped from release builds ([engine service](https://defold.com/manuals/engine-service/)).
  - **Defold deliberately splits the editor plane from the game plane.**
- **RenderDoc.**
  - Target control on TCP 38920-38927, remote replay on 39920. The remote server "will allow any connection ... without authentication", mitigated by `remoteserver.conf` `whitelist/noexec` ([network capture](https://renderdoc.org/docs/how/how_network_capture_replay.html), [FAQ](https://renderdoc.org/docs/getting_started/faq.html)).
  - Python flow: `CreateTargetControl` → `TriggerCapture` → a `ReceiveMessage` loop ([remote_capture example](https://renderdoc.org/docs/python_api/examples/renderdoc/remote_capture.html)).
  - The Python API is "not considered locked" ([FAQ](https://renderdoc.org/docs/python_api/faq.html)), while the in-app C API is semver-versioned ([in-app API](https://renderdoc.org/docs/in_application_api.html)).
  - **Relevance:** a future `arc gpu capture` verb.
- **Tracy** (Arcane vendors it; roadmap D13).
  - The **app listens** on 8086 and walks upward if taken. It also sends UDP broadcast discovery on 8086, and binds INADDR_ANY unless `TRACY_ONLY_LOCALHOST` is set ([TracyProfiler.cpp](https://github.com/wolfpld/tracy/blob/master/public/client/TracyProfiler.cpp), [TracySocket.cpp](https://github.com/wolfpld/tracy/blob/master/public/common/TracySocket.cpp)).
  - The handshake uses the `"TracyPrf"` shibboleth with `ProtocolVersion=83`, and an **exact** version match is required ([TracyProtocol.hpp](https://github.com/wolfpld/tracy/blob/master/public/common/TracyProtocol.hpp), [NEWS](https://github.com/wolfpld/tracy/blob/master/NEWS)).
  - Headless capture: `tracy-capture -o out.tracy -a addr -p port -s seconds` ([capture.cpp](https://github.com/wolfpld/tracy/blob/master/capture/src/capture.cpp)).
  - v0.14.0 (2026-08-09) "Added MCP server" ([NEWS](https://github.com/wolfpld/tracy/blob/master/NEWS)); its transport and tools are **[UNVERIFIED]**.
  - **Relevance:** `arc profile capture` should shell out to `tracy-capture` rather than re-implement the protocol. Arcane builds should set `TRACY_ONLY_LOCALHOST`.
- **Flax.** No remote protocol was found. CLI flags `-headless`, `-null` (null renderer, other systems still run), `-std`, `-play`, `-build preset.target`, `-exit` ([command-line access](https://docs.flaxengine.com/manual/editor/advanced/command-line-access.html)).
- **Stride.** Only a connection router for remote shader compile (`127.0.0.1:31254`) ([compile shaders](https://doc.stride3d.net/4.3/en/manual/graphics/effects-and-shaders/compile-shaders.html)). No general remote API was found **[UNVERIFIED]**.

### 1.9 Cross-cutting lessons from the survey [inference]

1. **Convergent transport:**
   - loopback HTTP, with no Origin header accepted (Unity, Unreal MCP);
   - a descriptor file in the project tree (Unity, Defold);
   - a bearer token from a CSPRNG that survives hot reload (Unity 0.4, Defold).
2. **Self-describe commands with full parameter and result schemas.**
   - BRP's names-only `rpc.discover` forced the community to build parallel catalogs.
   - Defold's OpenAPI and UE's per-tool JSON Schema avoid that.
   - **List tag-first or by search**, to protect context (Unity beta.12, UE tool search, godot-mcp-pro presets).
3. **Never put an error inside a success** (the top Unreal MCP complaint). Never treat "accepted" as "done" (Defold 202). Never grade from logs (UE `EXIT CODE` scraping, O3DE `JSON_START`).
4. **Give a readiness and state signal that names why the host is busy:** `settling` / `blocked_by_dialog` / 503 (Unity), and a polled OpenAPI (Defold). Make the client retry only requests the host rejected *without running them* (Unity beta.12).
5. **Frame-boundary execution on the main thread is universal** (BRP `RemoteLast`, Unity main-thread gating, the ImGui TE coroutine, UE MCP hopping to the game thread). **Never block the main thread synchronously from a tool** (the SlateInspector deadlock note).
6. **Mutations go through the undo transaction layer, and a batch is one undo step** (UE `generateTransaction`, Unity `AuthoringUndoScope`, Defold `editor.transact`, Godot MCP's selling point).
7. **Return images and trees as files plus metadata, written atomically** (Godot `rq_screenshot`, BRP extras, Playwright CLI). Warn about black frames from occluded or minimized windows.
8. **Version the protocol and negotiate capabilities** (Unity `capabilities[]`, Defold `/health`, Tracy's exact match). Protocols drift (Bevy's 0.17 rename, RenderDoc's unlocked Python API).
9. **Keep the editor-control plane separate from the live-game plane** (Defold; Unity's separate Editor and Player port ranges). Compile the game plane out of shipping builds (Unity `DEBUG`, Defold release stripping).
10. **The ImGui item-hook API is the only route to item-level UI introspection** in an ImGui app, whether or not the Test Engine is used.

---

## Part 2: Arcane's existing seams (read on `main` at c0fb42e1, plus `feat/settings-s7`)

Every path below is relative to the engine checkout (`D:\dev\starworks\Arcane`). The overall finding first: **Arcane already has the *launch-and-grade* half of `arc` and almost none of the *attach-and-ask* half.** It has a well-disciplined one-shot automation surface (flags in, JSON report out, a verdict vocabulary, golden diffs), a discovery file the Hub already reads, a transport-agnostic cvar service, and an undo stack with transactions. What it doesn't have yet is a live control channel into a running host, a named action registry, a dynamic (runtime-typed) query API exposed outside C++, or any ImGui item addressing.

### 2.1 Headless hosts and the one-shot automation CLI
- **`ArcaneClient/src/Arcane/Host/HostConfig.{hpp,cpp}`**: the shared host option set, parsed through the generic `Arcane::Cli` (`ArcaneCore/src/Arcane/Cli/Cli.hpp`). It's a fairly complete "staged scenario" vocabulary: `--project`, `--scene <guid>`, `--plugin`, `--backend`, `--frames N`, `--headless`, `--fixed-dt`, `--fixed-time`, `--settle N`, `--settle-timeout MS`, `--compare <ref>`, `--bless`, `--max-diff-pixels`, `--max-diff-pixel-ratio`, `--screenshot`, `--report`, `--probe kind@x,y`, `--set name=value` (repeatable, applied at `SetBy::CommandLine`), `--dump-layout`, `--play-as`, `--view-mode`, `--select-name`, `--tool`, `--open-asset`, `--select-asset`, `--select-in-document`, `--window-size WxH`, and the diagnostics triggers `--crash-gpu` and `--hang-main`.
- **`--headless`** renders the real frame graph with no window shown and no swapchain (the `OffscreenVehicle`). It **requires `--frames N`**. The refusal's comment says why: "an agent spawning a bare `--headless` spawns a process it cannot stop" (`HostConfig.cpp` ~line 526). **`arc` removes that reason**, because its channel is a stop path. So the spec should let `--headless` run open-ended when the control channel is armed, and only then.
- **House rule 3: "no silently inert flags".** "A flag that silently does nothing is the worst failure mode an agent can meet: it exits 0 having done nothing, and an agent reads that as success" (`HostConfig.cpp` ~line 310). Parse refusals go to stderr with exit 2. `arc` should inherit this exactly: an unknown verb, flag or argument is a refusal, never a no-op.
- **Hosts:** `ArcaneRuntime` (`ArcaneRuntime/src/RuntimeApp.cpp`, `RuntimeFrame.cpp`), `ArcaneEditor` (`ArcaneEditor/src/App/EditorApp*.cpp`) and the Core-only `ArcaneServer` (`ArcaneServer/src/ServerApp.hpp`), which ticks a fixed-step loop with `--frames`/`--report` and a Ctrl-C clean-exit hook. One process can run several worlds: the editor's "client + embedded server" play runs a Client world and a DedicatedServer world on one `ProcessContext` (`VerifyReport.hpp`'s worlds block, report schemaVersion 6+). **`arc` therefore has to address a world, not only a process.**

### 2.2 The Servitor and the witness harness (the grader layer)
- **Servitor is "a mode plus a corpus", not a package** (`docs/specs/2026-08-25-package-tiering-design.md`). Its pieces:
  - `VerifyReport` (`ArcaneClient/src/Arcane/Host/VerifyReport.{hpp,cpp}`, **`kSchemaVersion = 13`**) emits facts only: probes (`brightness|luma|rgba|pick@x,y`, `census`), settle facts (`settleAttemptsUsed`, `settleBailReason`), `compare.resolvedLevel` and `compare.triedPaths`, the worlds, and foreign modules. Its contract: "the engine's job stops at emitting facts; there is no assertion DSL here".
  - The **verdict vocabulary** `Arcane::Verdict` (`ArcaneClient/src/Arcane/Host/Verdict.hpp`) has seven values: `Passed, PassedOnFallback, Failed, Errored, NotRun, Skipped, Indeterminate`, with `IsGreen()` (Skipped is not green). It is a wire contract pinned on both sides (`VerdictTest.cpp` and `golden-gate.ps1 -SelfTest`).
  - The **exit reasons** live in `RuntimeApp.cpp` (~line 1256): `frames-complete`, `device-lost`, `render-failed`, `validation-errors`, `compare-failed`, `settle-not-converged`, and others. The agent skill says to grade by `exitReason`, "never the raw exit code (codes collide)" (`.claude/skills/arcane-verify/SKILL.md`). **That is a lesson for `arc`'s exit-code design: put the fine detail in JSON and keep exit codes coarse but non-colliding.**
- **The host witness harness** (`docs/specs/2026-09-03-host-witness-harness-design.md`, `ArcaneTests/src/Helpers/HostWitness.{hpp,cpp}`) is a CreateProcess wrapper that returns `WitnessRun {exitCode, timedOut, progressingAtKill, reportFound, reportParsed, report}`. It grades in Gauntlet's precedence (killed, then exit code, then report presence, then contents), and it deliberately does not copy Gauntlet's "no report = pass" branch. The spec explicitly **parked the in-process half** ("a witness module riding the existing plugin ABI... the seam the agent-introspection goal needs") and recorded UE's TCP messaging channel as prior art "for whenever that unparks". `arc` is that unparking.
- **The golden gate** is `scripts/golden-gate.ps1`, with `scripts/automation-baselines.json` and `automation-exclusions.json`. It runs four lanes (`runtime-scene`, `f3-cull-blend`, `editor-ui`, `editor-ui-perspective`) on dx12 and vulkan and writes `golden-gate-summary.json`. The comparator `ArcaneCore/src/Arcane/Assets/ImageCompare.hpp` is a constant-for-constant port of Playwright's, with Playwright's own fixtures vendored as the conformance oracle (`ThirdParty/playwright-fixtures/`). `ReferenceImages` resolves references per backend with a shared fallback. **`arc see --compare` should call this same comparator and the same resolution, not a second one.**

### 2.3 The editor action registry (it doesn't exist on `main` yet)
- **Today there is no named-action registry.** Menu and shortcut intents are a bag of fields, `Arcane::Editor::MenuRequests` (`ArcaneEditor/src/Panels/EditorPanels.hpp:44`): `newScene`, `saveScene`, `rebuildModule`, `selectAll`, `duplicateSelection`, `togglePhysicsOverlay`, `requestCreateKind`, and so on. Input raises them, `EditorApp::ConsumeMenuRequests` folds them in at one site per verb ("so the keybind and the menu item cannot drift apart", `EditorApp.hpp` ~line 269), and `RunSceneAction` and `ConsumeAssetPanelActions` execute them. Panels report through `AssetPanelActions` and `PageAction`. Navigation has its own small vocabulary: `RouteAction {SelectEntity, OpenDocument, RevealAsset, OpenGraphNode, ShowInExplorer, OpenAsText}` (`ArcaneEditor/src/Panels/LocatorRoute.hpp`).
- **The settings arc specifies the registry** (`docs/superpowers/specs/2026-10-03-settings-and-cvar-completion-design.md` §7.2). Every editor command becomes a named action (`editor.view.frameAll`, `edit.undo`, `graph.delete`) with a display name, a context (Global, Viewport, Graph, Asset Browser, Text, Inspector), a default chord bound to a `keychord` cvar `editor.keys.<action>`, and a callback. The ~40 to 49 hard-coded key checks move onto it. **That registry is `arc act`'s natural command table.** The spec should require each action to also declare an argument schema, an availability predicate (the "greyed while playing/building" logic that lives in menu code today) and whether it is undoable. Without those, menus can call an action but an agent can't discover or validate one.
- `PluginHost` and the game module can contribute systems and components (`ArcaneCore/src/Arcane/Plugin/GameModule.hpp`, `SystemFactory`). A game-module action contribution point for `arc` is the analogue of Unity's custom-command attribute.

### 2.4 Reflection
- `ArcaneCore/src/Arcane/Reflection.hpp` is a facade over Astra reflection: `ARCANE_REFLECT_TYPE` / `FIELD` / `ATTR`, plus attributes `Range, Hidden, ReadOnly, DisplayName, Tooltip, Category, Serializable, ColorFormat, AngleFormat, Multiline, FilePath, DragSpeed, Deprecated, AliasName, Precision`, plus enums with display names and descriptions.
- `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp` is the reflection-to-JSON bridge over Astra's `IFieldVisitor`. It handles enums by name, glm vectors, matrices and quaternions as arrays, and nested structs, and it **refuses rather than drops**: an unsupported field type or a present-but-unreadable key latches an error. **It's exactly the codec `arc get` / `arc set` need**, and it already defines Arcane's component JSON shape (the scene file format), so agents see one JSON dialect everywhere.
- `ThirdParty/Astra/include/Astra/Reflection/JsonSchema.hpp` is `Astra::JsonSchemaGenerator`. It emits **JSON Schema draft 2020-12 from `TypeMeta`**, with tooltips as descriptions. That's the direct analogue of BRP's `registry.schema` and the self-description source for `arc schema component <name>`.
- The settings arc adds settings structs reflected with `ARCANE_REFLECT_TYPE_ATTR(Settings, ...)`, so settings and components share one metadata system.

### 2.5 Cvars, the console, and the remote cvar service
- `ArcaneCore/src/Arcane/Config/CVarRegistry.hpp` provides `Register`, `RegisterCommand`, `Find`, `Get`, `Set(handle, value, SetBy, module, Permission)`, `Publish` (the snapshot swap: a set stays pending until `Publish`), `Explain(name)` returning `CVarExplain {published, pending, setBy, history[]}`, `List()`, `ListCommands()`, `Execute(line, Permission, SetBy)` returning `ExecResult {ok, text}`, and `RevertCheats()`. `SetResult` covers `Applied / RefusedWeaker / Stale / TypeMismatch / Denied`. `ConsoleModel` (`Config/ConsoleModel.hpp`) is the widget-free console model that both the editor tab and the runtime overlay drive.
- **The audience x context model** (settings spec §3.2):
  - Audiences: `Editor` / `Game` / `PlayerSafe` / `Server`.
  - Contexts: `Editor` / `LocalHost` / `ServerAdmin` / `Client`. They come "from the session role, not from which console was used".
  - Flags: `Dev` (compiled out of Dist), `Hidden`, `Protected` (never readable outside Editor and ServerAdmin), `Cheat` (gated by `server.cheats`).
  - A game-installable `CVarPolicy` returns Allow, Deny or Default.
  - Every non-Editor set of a `Server` setting is audited.
- **`RemoteCVarService`** (branch `feat/settings-s7`, `ArcaneCore/src/Arcane/Config/RemoteCVarService.hpp`) is transport-agnostic: `Handle(RemoteCVarRequest{op: get|set|list|explain, name, value, callerId}) -> RemoteCVarResponse{ok, text}`, run in the `ServerAdmin` context. Protected values are never readable through it. Commands run only via `set`, and only with `ServerCanExecute`. Hidden names answer "unknown". Every set and command run, allowed or refused, goes to the injected audit sink. It runs on the registry's writer thread and publishes after a successful set.
  - **Two gaps for `arc`:** (a) the response is **text, not structured JSON**, so `arc` needs a structured variant or a JSON payload in `text`; (b) it is hard-wired to `ServerAdmin`, while `arc` attached to an editor should run as `Editor` and attached to a game as `LocalHost`.
- The `ArcaneServer` stdin console (S7) and Aphelyon's HMAC-signed `ServiceEndpoint` are the two transports already planned for that service. `arc` would be the third.

### 2.6 AutoScreenshot, capture and golden images
- `EditorApp::WriteAutoScreenshot()` (`ArcaneEditor/src/App/EditorApp.cpp` ~line 1880) writes `<project>/Saved/AutoScreenshot.png` from the viewport output on scene save and clean shutdown, for the Hub's cover thumbnail. It arms one extra viewport frame with `FrameDesc::capture` and calls `ReadCapture`, and it is "NEVER the chrome context". So a **viewport-only capture path already exists**, separate from the backbuffer path behind `--screenshot` ("captures the BACKBUFFER, after tonemap and ImGui"). That gives `arc see` two primitives for free: `viewport` (scene only) and `window` (full chrome). A per-ImGui-window crop is new work: crop the backbuffer to the ImGui window rect.
- The capture format is `kGraphOffscreenFormat`, BGRA8_UNORM and display-referred (`VerifyReport.hpp` header). The `--settle` machinery waits for quiescence: two byte-equal frozen-clock captures **and** `ShaderCompiler::IsIdle()`. **That's Arcane's "auto-wait"**, and `arc see` and `arc wait --settled` should reuse it.

### 2.7 Diagnostics capture
- `ArcaneCore/src/Arcane/Base/Diagnostics.hpp` has crash (SEH filter) and hang (heartbeat watchdog) triggers that share one report path. A dedicated crash thread writes the `.arcdiag` envelope (`DiagEnvelope.hpp`, versioned JSON with a GUID), the `.dmp`, the `.txt`, the GPU (DRED) section and a log tail, then hands off to `ArcaneCrashReporter`. Reports land in `<exe dir>/diagnostics`.
- **The same header holds the Problems feed's producer API:** `Diagnostics::Publish(key, span<Diagnostic>)` / `Clear(key)` "publication groups". A producer owns a key and republishes its entire set, which makes retraction trivial. Each `Diagnostic` has a severity, code, message and detail, and a `DiagLocator` that is an `Entity(id)`, `Asset(guid)`, `File(path, line, col)` or `GraphNode(owner, node)`. **That's a ready-made structured payload for `arc problems`**, and `DiagLocator` is already a cross-panel address form.
- For `arc`: when a host dies mid-session, the client should report the `.arcdiag` path, the way the HostWitness reports `timedOut` before exit code.

### 2.8 arcbuild and hot reload
- `arcbuild` (`arcbuild/src/`, spec `docs/specs/2026-09-13-arcbuild-driver-design.md`) has the verbs `build | probe | generate | rebuild | clean`. **Exit codes: `kExitOk 0`, `kExitRefused 2`, `kExitProbeRebuild 3`** (`arcbuild/src/Exit.hpp`), and child-tool exit codes pass through. Output is human text through `IOutput` (`Info / Always / Error / Child`). There's **no JSON mode**, and a unified output policy across hosts and arcbuild is recorded as owed debt (arcbuild II). The editor's Tools -> Rebuild Game Module spawns the same exe.
- **Hot reload:** `ArcaneCore/src/Arcane/Plugin/PluginHost.hpp` watches the game DLL (debounced mtime `Poll()`), loads versioned copies to dodge the PDB lock, ABI-checks, and `Reload(restoreState)` snapshots and restores state with rollback on failure. `arc build` should be "arcbuild, then wait for the host's reload event". That's the equivalent of Unity's `recompile` + domain-reload wait, and it needs `PluginHost` to publish a reload-generation counter and the last reload result.

### 2.9 Problems and Console panes
- `ArcaneEditor/src/Panels/DiagnosticStore.hpp` is the consumer side of the publication groups: pure data, no ImGui, mutex-guarded. It already has `MatchesDiagnosticFilter(d, SeverityMask, search)`, "one definition of 'matches'" shared by the panel, the store and the Console. `ProblemsPanel.cpp` and `ConsoleModel/ConsoleBuffer` are the presentation side.
- Logs: `Arcane/Base/Log.hpp` (file sink with `AttachFileSink` / `FileSinkPath`, bounded flush, the Mosaic sink). `arc logs --follow` needs a ring-buffer subscriber sink. That's a small addition beside the existing file sink.

### 2.10 Astra ECS and its query surface
- Astra is vendored header-only from `D:\dev\starworks\Astra` (`dev`, `c46a90ed`, synced 2026-10-02). Re-vendor with `scripts/sync-vendor.ps1 -Library Astra` (`scripts/sync-astra.ps1` is a shim). The checked-in `ThirdParty/Astra/VENDORED.txt` from that sync still names the shim; the next sync rewrites the stamp.
- **Typed queries** live in `Registry/Query.hpp`, `View.hpp` and `Relations.hpp`. They're C++ templates, unusable from a wire request.
- **Runtime-typed primitives already exist:** `GetComponentByHash(entity, typeHash)` and `HasComponentByHash` (an XXHash64 of the type name), `AddComponentByID` / `RemoveComponentByID` / `SetEnabledByID` / `IsEnabledByID`, the component-descriptor enumeration for an entity ("useful for editor/inspector UI"), and `InspectResources()` (resource descriptors). Combined with `MetaRegistry` + `ReflectionJson`, **that's enough to build BRP-style `query/get/insert/remove/mutate` without new Astra features.** A runtime-typed *multi-component filter* (`with/without` over component IDs) would be done most cheaply by archetype-mask filtering. If the Astra direction favours it, it belongs in Astra first ("commit in Astra repo FIRST, then sync").
- `Astra/Debug/Inspector.hpp` is a read-only snapshot facade (archetypes, columns, chunk layouts, byte accounting) "so tools never hold pointers into live engine state". **That's `arc ecs stats` for free.**
- **Addressing rule:** entities on the wire must be addressed by the `Arcane::Identity` GUID (`Scene/Components.hpp`, `Identity{id, name}`), never by raw Astra handles. That's the standing binding rule: "anything outliving registry state keys by Identity GUID". `VerifyReport`'s `pick` probe already reports "a DURABLE identity (Identity.id's Guid + Identity.name), not the raw hit-proxy uint32". The settled Astra direction also says one View API reads table, sparse, universal and relationship storages. A wire query must come out of that same API.

### 2.11 Undo
- `ArcaneClient/src/Arcane/Edit/Command.hpp` defines `ICommand {Undo, Redo, Label, AffectsScene, IsExpired, PayloadBytes}`.
- `CommandStack.hpp` is a transaction model: `Begin(label) -> TransactionId`, `SnapshotComponent(entity, descriptor)`, `Commit(owner)` / `Cancel(owner)`, `Undo` / `Redo`, `UndoLabel`, `StateId` / `SceneStateId`, and byte-budgeted spill payloads. `ComponentEditCommand` and `RegistryStateCommand` are the concrete scene commands, and `AssetFileCommand`s cover asset file ops.
- **`arc set`'s edit path = `Begin("arc: set Transform.position") -> SnapshotComponent -> write via ReflectionJson -> Commit`**, which is the inspector's own path. A batch = one transaction = one Ctrl+Z.
- The owed **entity-record undo arc** (GUID-keyed per-entity records replacing the whole-registry memento) lands before or with `arc`. `arc` should target the transaction API, not the memento.

### 2.12 Discovery and IPC prior art already in the tree
- **`EditorLock`** (`ArcaneCore/src/Arcane/Project/Project.{hpp,cpp}` ~line 147 / 620) writes `<project>/Saved/editor.lock` as JSON `{pid, start}`, where `start` is the process-creation FILETIME. `IsAlive()` validates pid **and** creation time, so a crash's stale lock is ignored. It's described as "Unity's lockfile model, minus the flaw its users hate". The **Hub mirrors the format** in `ArcaneHub/src-tauri/src/editorlock.rs` ("change BOTH"). **This is `arc`'s discovery file.** It only needs an endpoint, a token reference, a host kind and an ABI added, and it already solves the stale-lock problem Unity's users complain about.
- **Net/crypto:** `ArcaneCore/src/Arcane/Net/TcpSocket.hpp` (header-only, `CreateListenSocket(port, bindAddr)`), `Net/Protocol.hpp`, `Net/RateLimiter.hpp`, and `Crypto/Crypto.hpp` (`GenerateSecureToken`, PBKDF2-HMAC-SHA256, `ConstantTimeCompare`). The Aphelyon servers already run HMAC-signed JSON RPC on top of these. There's **no named-pipe or AF_UNIX code** in the engine today.
- **Threading:** `Base/ServiceThread.hpp` (one thread for long-lived blocking work, explicitly "NOT the fork-join pool") is the right home for `arc`'s listener. Requests must be marshalled to the main or registry-writer thread at a frame boundary, because cvar `Handle` and registry writes are main-thread work.

### 2.13 ImGui
- Dear ImGui **1.92.9 WIP, docking branch** (`ThirdParty/imgui`), plus `imgui-node-editor`. `imconfig.h` has `IMGUI_ENABLE_TEST_ENGINE` **commented out**. The Test Engine is **not vendored**.
- The **click tests show the pain directly**. `ArcaneTests/src/AssetStatusPanelClickTest.cpp` drives real panels through device-less ImGui frames with real mouse input, but it locates each button "without pixel-guessing or an ImGui test-engine (not vendored here)" by re-deriving ImGui's cursor math (`window->DC.CursorPos.x = ...`) from production formulas. That's the cost of having no item registry, and `arc ui` would pay it on every widget.
- `--dump-layout` (`ImGui::SaveIniSettingsToDisk`) is the only ImGui introspection export today: the dock layout, not the item tree.
- Separately, the user's post-1.0 plan is an **own immediate-mode UI and node library replacing ImGui and imgui-node-editor**. Any widget-addressing scheme `arc` adopts should be defined in Arcane's terms (an item registry fed by a hook) so it survives that replacement.

---

## Part 3: Synthesis

### 3.1 Recommended architecture at a glance

```
 agent / CI / human
        |  argv  (arc <noun> <verb> [args] [--format json|ndjson|human|github])
        v
 +-----------------+        reads   <project>/Saved/arc/<host>-<pid>.json  (descriptor: port, token, abi, caps, worlds, heartbeat)
 |   arc  (C++)    |------------------------------------------------------------+
 | ArcaneCore+Cli  |                                                            |
 +-----------------+                                                            |
        |  HTTP/1.1 on 127.0.0.1:<port>  Authorization: Bearer <token>          |
        |  POST /arc/v1/rpc   (JSON-RPC 2.0, batch)                             |
        |  GET  /arc/v1/stream?sub=logs|problems|job:<id>   (SSE)               |
        v                                                                       |
 +--------------------------- host process (Editor / Runtime / Server) --------+-----+
 | ArcListener (ServiceThread) --queue--> ArcDispatcher (main thread, frame boundary, |
 |                                         budget N/frame, state gate)               |
 |   ArcCommandRegistry  <-- engine commands (ArcaneCore/Client)                     |
 |                       <-- editor actions (settings s7.2 ActionRegistry)           |
 |                       <-- game-module commands (GameModule hook)                  |
 |   services: CVarRegistry/RemoteCVarService | Astra Registry + ReflectionJson     |
 |             CommandStack (undo txns) | Diagnostics/Log ring | Capture/ImageCompare|
 |             ArcUi item registry (ImGui ItemAdd/ItemInfo hooks) | PluginHost events |
 +-----------------------------------------------------------------------------------+
```

#### Transport

**Recommended: loopback HTTP/1.1 with JSON-RPC 2.0 bodies, plus SSE for streams.**

| Option | For | Against |
|---|---|---|
| **Loopback HTTP (recommended)** | What Unity, Defold, BRP and UE MCP all ship. Debuggable with `curl`. Reachable from WSL2 and from containers on the host with mirrored networking (Unity had to fix exactly that case). The same code works on Windows and Linux. Easy to put an MCP adapter over later. | A TCP port is visible to every local user, so it needs a token. It is browser-reachable in principle (DNS rebinding), so it must refuse any `Origin` header. Some agent sandboxes (Codex) block localhost networking by default. |
| Named pipe / AF_UNIX | The OS enforces access (pipe ACL to the current user; a 0600 socket file). No port allocation. Not reachable from a browser. UE's CmdLink and Unity's old MCP relay used it. | Two code paths (Windows pipes vs Unix sockets). Windows AF_UNIX works from 1803 but tooling is thin. Not reachable from WSL to Windows. No `curl` for debugging. Agent sandboxes restrict file sockets too. |
| WebSocket | Bidirectional and push-friendly (UE RC 30020, Godot MCPs). | A heavier server; reconnect logic in the client; overkill when SSE covers push. |
| Custom TCP framing (Tracy, Godot debugger) | Compact. | No ecosystem tooling; every client reimplements framing. |

Two rules for the HTTP option:
- **Bind `127.0.0.1` only**, and refuse non-loopback peers and any request carrying `Origin` (Unity's rule).
- Hand-roll the server on `Arcane::TcpSocket`, or vendor a header-only HTTP server. The listener runs on a `ServiceThread`.

Leave a seam so a pipe or socket transport can be added later. Protocol, envelope and dispatch must not know which transport carried a request; `RemoteCVarService` is already transport-agnostic in exactly this way.

#### Discovery

**One descriptor per host, `<project>/Saved/arc/<hostKind>-<pid>.json`**, written atomically at listener start, refreshed every second (heartbeat) and deleted on clean exit:

```json
{ "protocol": 1, "pid": 1234, "start": 133400000000000000, "hostKind": "editor|runtime|server",
  "port": 7820, "token": "<base64 32 bytes>", "engineVersion": "...", "abi": 52,
  "project": "D:/.../Aphelyon", "capabilities": ["ecs.query","ui.tree","capture.viewport"],
  "worlds": [{"id":"client","role":"Standalone"}], "state": "ready", "heartbeat": "2026-10-05T12:00:00Z" }
```

- **Validation:**
  - Validate with `EditorLock`'s `{pid, start}` liveness rule, so a crash's stale descriptor is ignored rather than reported as "already open".
  - A stale heartbeat that is older than about 10 s is treated as "hung", not "gone".
- **`editor.lock` stays as it is:**
  - Leave it untouched. Its format is mirrored in the Hub's `editorlock.rs`.
  - Multiple hosts (editor plus a separate-process runtime plus a server) can be live for one project at once, which a single lock cannot express.
- **Launch-and-attach:**
  - `arc launch` passes `--arc-descriptor <path>` (and optionally `--arc-port`).
  - The CLI then knows exactly which file to wait for, with no race against other hosts.
- **Ports:**
  - Dynamic: bind port 0 and write the OS-chosen port.
  - A fixed range such as Unity's 7800-7849 only helps humans guess. Don't use one.

#### Auth

- **The token:**
  - `Crypto::GenerateSecureToken(32)` per host process.
  - Checked with `ConstantTimeCompare`.
  - Sent as `Authorization: Bearer`.
- **Survives hot reload:**
  - The listener and the token live in the host executable, not in the game DLL.
  - So a module reload never rotates the token. Unity's 0.4 bug cannot happen here by construction.
- **Token storage:**
  - Kept in the descriptor, which sits under the user's own `Saved/`.
  - Defold's rule applies: never echo the token into logs or reports. `arc` redacts it in `--verbose` output.
- **Off in Dist:**
  - The whole subsystem is `#if !defined(ARCANE_DIST)`, compiled out as `Dev` cvars are.
  - In Debug and Release, the editor arms it by default (a Preferences cvar, `editor.arc.enable`, default on).
  - ArcaneRuntime and ArcaneServer arm it **only on `--arc`**. This mirrors Unity's player opt-in and Defold's debug-only engine service.

#### Command registry and self-description

`Arcane::Arc::CommandRegistry` lives in ArcaneCore. Each command declares:

| Field | Meaning |
|---|---|
| `name` | Dotted and stable (`ecs.query`, `entity.get`, `cvar.set`, `action.run`, `ui.click`, `capture.viewport`) |
| `summary`, `tags` | For tag-first listing |
| `args` | A **reflected C++ struct**. Its JSON Schema comes from `Astra::JsonSchemaGenerator` and it is parsed with `ReflectionJson`, so the schema and the parser cannot drift. This is the equivalent of UE's `AICallable` and Unity's `[CliArg]`. |
| `result` | A reflected struct, or a documented free-form JSON shape |
| `hosts` | Editor / Runtime / Server |
| `thread` | `Main` (default) or `Any` |
| `mutates` | none / undoable / non-undoable |
| `audience` | The minimum context (s3.6) |
| `available` | A predicate that returns a reason string when the command is unavailable, for example "playing", "building" or "no project". It is the same predicate the menu uses to grey out items. |

Three sources feed the registry:
1. **Engine commands** registered at host start.
2. **Editor actions** (settings §7.2 `ActionRegistry`). Each action automatically becomes `action.run {name}` and appears in `arc action list` with its context, chord and availability.
3. **Game-module commands**, registered through a `GameModule` hook. They are unregistered on module unload and re-registered on reload. A `commands-changed` event makes clients refresh (the lesson of Unity's zero-tool sessions).

Self-description has three levels:
- `arc commands --format json` is the CLI's own static manifest, available with no host running.
- `arc list [--tag t] [--query q]` is the host's live catalog, tags first.
- `arc describe <cmd>` returns the full args and result schema.
- There is also `arc schema component <Name>` (the reflected component's JSON Schema) and `arc schema resource <Name>`.

#### Verb set

All verbs share the same addressing:
- An entity is `guid:<Identity GUID>`, or `name:<Identity.name>` (strict: ambiguous names are refused), or `path:<a/b/c>` once hierarchy is first-class.
- A component is named by its reflected short name. Qualifying is only needed on a collision.
- A world is `--world client|server` when a process runs more than one.

| Group | Verbs | Backed by |
|---|---|---|
| **session** | `arc status [--until-ready]`, `arc launch editor\|runtime\|server [host flags...]`, `arc attach`, `arc quit [--force]`, `arc wait ready\|settled\|frames N\|reload\|condition <expr>` | descriptor, HostConfig, the settle predicate (`SettleBound.hpp`, `ShaderCompiler::IsIdle`) |
| **query** | `arc ecs query --with A,B --without C [--fields A.x]`, `arc ecs stats`, `arc entity get <ref> [--component C]`, `arc resource get <R>`, `arc scene tree`, `arc selection`, `arc asset list\|info\|refs <guid>`, `arc logs [--follow] [--since]`, `arc problems [--severity]`, `arc stats frame`, `arc world list` | Astra by-ID/hash API + `MetaRegistry` + `ReflectionJson`, `Astra::Debug::Inspector`, `DiagnosticStore`, a Log ring sink, `FramePerf`, AssetRegistry/AssetReferenceIndex |
| **act** | `arc entity set <ref> C.field=<json>`, `arc entity add\|remove\|spawn\|despawn\|duplicate`, `arc select <ref>`, `arc open <asset>`, `arc action list\|run <name>`, `arc cvar get\|set\|list\|explain`, `arc play [--topology]\|stop\|pause\|step N`, `arc undo\|redo`, `arc input key\|click\|move` (game input) | CommandStack transactions, EntityOps, the ActionRegistry, CVarRegistry/RemoteCVarService, PlayMode, the InputSnapshot injection seam |
| **see** | `arc screenshot viewport\|window\|panel <ref> [--out f.png]`, `arc ui tree [--window W] [--depth N] [--out f.yml]`, `arc ui click\|hover\|type\|check\|select\|drag <ref>`, `arc ui wait <ref> [--visible\|--gone]`, `arc compare <refName> [--bless]` | `WriteAutoScreenshot`'s viewport capture, backbuffer `ReadCapture`, the ArcUi item registry, `ImageCompare` + `ReferenceImages` |
| **build** | `arc build [--config]` (arcbuild, then wait for the host's reload event), `arc reload` (`PluginHost::ForceReload`), `arc generate` | arcbuild, PluginHost |
| **test** | `arc test [filter]` (ArcaneTests with JUnit/JSON), `arc gate golden [--lanes]` (golden-gate.ps1 summary), `arc probe <kind@x,y>` (live VerifyReport probes) | ArcaneTests, golden-gate, VerifyReport |
| **job** | `--detach` on any long verb, `arc job list\|status\|wait\|cancel <id>` | the host's job table (s3.5) |

**There is no `eval` verb.** Arcane has no embedded scripting language, and it is not worth adding one for this. Unity's `eval` is its riskiest surface ("remote code execution gated only by the token"). The answer to "a new question without a rebuild" is the generic ECS/reflection query plus game-module commands, which matches Perflint's "registered commands for verdicts" maxim.

#### Output contract

- **Formats:**
  - `--format human` is the TTY default.
  - `json` is the default when stdout is not a TTY. That is a deliberate departure from Unity's `tsv` default: agents parse JSON more reliably and Arcane has no shell-pipeline culture to serve.
  - `ndjson` is for streams and progress. **Every stream ends with a `{"type":"result", "count":N}` frame**, so a truncated stream is detectable.
  - `github` emits `::error file=..,line=..::` from `DiagLocator`s.
  - The format can also come from `ARC_FORMAT`.
- **Envelope:**

  ```json
  { "ok": true, "protocol": 1, "command": "entity.get",
    "host": {"kind":"editor","pid":1234,"world":"client","frame":81234},
    "data": { ... },
    "errors": [ {"code":"entity.not-found","message":"...","remediation":"...","locator":{...}} ],
    "warnings": [], "notifications": [],
    "undo": {"transaction":"arc: entity.set Transform.position","undoable":true} }
  ```

- **Envelope rules:**
  - Never `ok:true` with a non-empty `errors`.
  - Failures still write the full envelope to stdout.
  - Human diagnostics go to stderr.
  - `--result-only` prints only `data`.
- **Large payloads go to files.** UI trees, ECS dumps and screenshots are written atomically under `<project>/Saved/arc/out/` (or `--out`), and the envelope carries `{path, bytes, sha256, width, height}`. This is the Playwright CLI and Godot lesson.
- **Error codes:**
  - Use stable dotted strings: `arg.invalid`, `arg.unknown` (Rule 3), `auth.denied`, `permission.denied` (with the context and audience), `host.busy` (with `state`: `booting`/`reloading`/`compiling-shaders`/`modal`/`playing-transition`), `entity.not-found`, `entity.ambiguous`, `component.unknown`, `reflect.unreadable`, `ui.ambiguous`, `ui.not-found`, `ui.not-actionable` (with which actionability check failed), `undo.unavailable`, `verdict.failed`.
  - Map the JSON-RPC numeric codes underneath them.

#### Exit codes

Keep them coarse and non-colliding. The detail lives in the JSON, as the arcane-verify skill already teaches ("never the raw exit code").

| Code | Meaning | Alignment |
|---|---|---|
| 0 | ok (including verdict `Passed`, `PassedOnFallback`, and `Skipped` with a reason in JSON) | Unity 0 |
| 1 | internal error in `arc` itself | Unity 1 |
| 2 | usage / refusal: unknown flag, bad argument, unknown command | `Arcane::Cli` and arcbuild `kExitRefused = 2`, Unity 2 |
| 3 | auth: bad token, or permission denied for this context | Unity 3 |
| 4 | no host: nothing to attach to, or the descriptor is stale (try `arc launch`) | Unity 4 ("configuration required") |
| 5 | host busy / not ready within `--timeout`; retryable | (Unity leaves 5 unused) |
| 6 | the host answered and the operation failed (`errors[]` set; verdict `Errored`) | Unity 6 |
| 7 | host unreachable or died mid-command (with a `.arcdiag` path if one was written); retryable | Unity 7 |
| 8 | a check ran and did not pass: test failures, compare-failed, a wait condition false at timeout, or verdict `Failed` or `Indeterminate` | Unity 8 |
| 130 / 143 | SIGINT / SIGTERM | Unity |

- **Retry rule** (Unity beta.12): the client retries automatically only when the host provably did **not** run the request (busy, or a connection refused before send).
- **Grading precedence** follows HostWitness: died, then timed out, then exit, then envelope, then data.

#### ImGui widget addressing

Recommended: implement the three `IMGUI_ENABLE_TEST_ENGINE` hooks ourselves in an `ArcUi` item registry. Vendoring the Test Engine stays an open option (decision Q11).

- **Capture.** Define `IMGUI_ENABLE_TEST_ENGINE` in `imconfig.h` (non-Dist), which turns on `ItemAdd`, `ItemInfo` and `Log`. The hooks record, per frame, for each item:
  - `{id, label, window, parentId, ID path, rect, clipped rect, status flags (Openable/Opened/Checkable/Checked/Inputable), item flags, framesNotMoving}`.
  - This costs one small vector per frame, and only while `arc` is armed and a UI subscriber exists (`g.TestEngineHookItems` gates it).
- **Address syntax.** Adopt the Test Engine's ref-path grammar wholesale, so its docs teach `arc` users:
  - `//Window/Child/Button`, `**/` wildcards, `$$n` int ids, and `###id`.
  - Add Playwright-style **snapshot refs**: `arc ui tree` writes YAML with `[ref=e12]` per item. Refs are valid only for the frame generation they came from; a stale ref gives `ui.stale-ref`.
  - **Strict mode:** more than one match is `ui.ambiguous`, and the error lists the candidates.
- **Roles.** ImGui has no roles beyond the five status flags. Add an opt-in `ArcUi::Role` / test-id annotation (`ArcUi::Tag("inspector.transform.position.x")`, the analogue of `getByTestId`) for the widgets that desk checks care about: Inspector fields, Outliner rows, toolbar buttons, menu items. Arcane's widget layer (the `Widgets/` directory) is the single place to stamp these.
- **Actions.** Inject input through `io.AddMousePosEvent/AddMouseButtonEvent/AddKeyEvent/AddInputCharacter` at the frame boundary, removing backend events for the duration, as the Test Engine does. Before acting, run Playwright's actionability checks:
  - visible (non-zero clipped rect, window not collapsed);
  - stable (unmoved for 2 frames);
  - receives events (hovered id equals the target after the move);
  - enabled (not `ImGuiItemFlags_Disabled`).
  
  Then auto-open the parent menus and tree path, and auto-scroll. Any failure is `ui.not-actionable{check}`.
- **Headless.** The editor's `--headless` already runs the full ImGui chrome offscreen (the `editor-ui` golden lane proves it), so `arc ui` works in CI without a window. That's an advantage over Godot.
- **Structural goldens.** `arc ui tree --out` YAML can be committed and diffed like Playwright aria snapshots. It is much less brittle than `editor-ui.png`, and it covers owed defect 2 (the editor-ui reference being written by the thing under test).
- **Future-proofing.** The own-UI rewrite after 1.0 replaces ImGui, so the `ArcUi` registry is the contract and the ImGui hooks are just its first producer.

#### Undo-path integration

- **Every mutating command in the editor runs in one `CommandStack` transaction**: `Begin("arc: <command> <summary>")`, then `SnapshotComponent` for each touched (entity, component), then the write through `ReflectionJson`, then `Commit`. This is the Inspector's own path, so `arc` edits are indistinguishable from hand edits and one Ctrl+Z undoes them.
- **Batches.** `arc batch file.json` (and JSON-RPC batch) is **one transaction**, with `$N.path` back-references (Unity's batch). On any failure the whole batch is `Cancel`led and reverted, and a non-undoable op inside a transactional batch is refused up front (`undo.unavailable`, like Unity's `not_batchable_transactional`).
- **Asset and file operations** use the existing `AssetFileCommand` family. Destructive ones require `--confirm` and honour `--dry-run` (Unity's convention).
- **Play mode and the runtime host:**
  - Edits there are not undoable. Play's Stop restores the snapshot, and ArcaneRuntime has no CommandStack.
  - The envelope says `undoable:false`.
  - Edit-mode-only commands refuse in Play with a reason, as the menu items grey out.
- **Build order.** Build on the owed entity-record undo arc (GUID-keyed records), not the whole-registry memento. `arc` should land after it, or at least target only the transaction API so the memento's replacement is invisible to it.

#### Threading and state gating

- **Requests are queued by the listener thread and drained on the main thread at one fixed frame point.** Recommended: right after input and before simulate/update, so an `arc`-injected edit is seen by this frame's sim and render. Drain with a per-frame budget (Unity: 10) and leave long work to jobs.
- **Every response carries the frame number it was applied on**, so `arc wait frames N` and step semantics are exact.
- **The host publishes a `state`**: `booting`, `ready`, `reloading` (PluginHost), `compiling-shaders`, `modal` (when `ModalErrorQueue` or a dialog slot is open), `building`, `playing`, `paused`.
  - `status` and `ping` answer in any state, from the listener thread, using an atomic snapshot.
  - Main-thread commands answer `host.busy{state}` while the state forbids them (Unity's 503 / `blocked_by_dialog`).
- **The editor must keep ticking while unfocused or minimized while an `arc` session is active** (the equivalent of Unity's `set_autotick`). Verify that there is no idle throttling.

#### Jobs and streaming

- **The job table lives in the host exe**, so it survives game-module hot reload. That's an improvement on Unity, where jobs die on domain reload.
- **Job model:** a job is `{id, command, state: queued|running|done|failed|cancelled, progress, result-envelope}`.
- **Async vs blocking:** `--detach` returns `{job}`. Without it, the CLI blocks and renders progress (ndjson frames or human).
- **Streams:** `GET /arc/v1/stream` delivers SSE for logs, problems (publication-group deltas, keyed), jobs, `commands-changed` and `state`.
- **Watch:** `arc watch entity <ref> --component C` is a per-frame delta stream (BRP `+watch`).
- **Native-tool cases** (`arc build`, `arc test`): the CLI itself owns the subprocess and only uses the host for the reload wait, because arcbuild must keep working with no host running.

#### Security mapped onto audience x context

`arc` sessions **are** a context. The settings spec already says "the context comes from the session role, not from which console was used". The mapping:

| Host attached | arc session context | Effect |
|---|---|---|
| ArcaneEditor | `Editor` | Full read/write on every audience, including `Hidden` by exact name. `Protected` readable (the human is the owner); redacted in logs. |
| ArcaneRuntime (standalone or listen host) | `LocalHost` | `Game`: read, and write only `Cheat` with cheats on. `PlayerSafe`/`Server`: read/write. Exactly as the runtime console. |
| ArcaneServer | `ServerAdmin` | Through `RemoteCVarService`: audited, `Protected` never readable, commands only if `ServerCanExecute`. |
| a connected client process | `Client` | `arc` refuses to attach in this role in v1. A client's view belongs to its server's admin. |

- **The game's `CVarPolicy` applies on top**, unchanged.
- **Two capability levels inside a session**, carried in the descriptor and the token handshake:
  - `observe`: query, see, logs, status.
  - `drive`: act, input, cvar set, build, quit.
  
  `arc attach --read-only` requests an observe-only session. Agent harnesses can pin it for exploratory runs (godot-mcp-pro's Lite/Minimal presets and Defold's per-route token are the precedents).
- **Audit:** every `drive` call is appended to `<project>/Saved/arc/audit.ndjson` (who = `--client` name, command, args digest, outcome, frame). For `Server` cvars it also goes to the existing audit sink (S7).
- **Rule 3 on the wire:** unknown arguments are refused. A command unavailable in this host kind or context says why; it never silently no-ops.

### 3.2 Open design decisions (multiple choice; recommended pick marked)

| # | Decision | Options | **Recommended** and why |
|---|---|---|---|
| Q1 | Transport | (a) loopback HTTP+SSE · (b) named pipe / AF_UNIX · (c) WebSocket · (d) custom TCP | **(a).** The industry convergence (Unity, Defold, BRP, UE MCP), `curl`-debuggable, reachable from WSL, one code path for Win and Linux. Keep the dispatcher transport-agnostic so (b) can be added if sandboxing demands it. |
| Q2 | Wire envelope | (a) JSON-RPC 2.0 · (b) REST path per command (Defold) · (c) Unity-style `{command, parameters}` POST | **(a).** Batch and ids for free, standard error codes, BRP precedent, and a trivial MCP mapping later. The CLI hides it anyway. |
| Q3 | Discovery | (a) extend `editor.lock` · (b) new `Saved/arc/<host>-<pid>.json` per host · (c) fixed port range · (d) UDP broadcast | **(b).** Several hosts per project, and the Hub's mirrored `editor.lock` stays untouched. Reuse EditorLock's pid+start validation. |
| Q4 | Token storage | (a) in the descriptor · (b) separate token file with an ACL · (c) env var passed at launch only | **(a)** in v1. The file sits in the user's own project `Saved/`, and Unity does the same. Revisit (b) if `Saved/` ever lives on a shared drive. |
| Q5 | Arming per host | (a) on everywhere in non-Dist · (b) editor on by default, runtime/server on `--arc` · (c) off everywhere unless flagged | **(b).** The editor is the desk-check target. Games and servers should not open ports unasked (the Chrome 136 lesson). |
| Q6 | Dist | (a) compiled out · (b) present but disabled by a cvar · (c) available behind a key | **(a).** It matches `Dev` cvars and Unity's `DEBUG`-only player server. Live-ops admin on a shipped server is the S7 `RemoteCVarService` over Aphelyon's HMAC RPC, not `arc`. |
| Q7 | Client implementation | (a) C++ exe on ArcaneCore + `Arcane::Cli` · (b) Rust (shared with the Hub) · (c) Python | **(a).** The same `Json.hpp`, the same JSON Schema and reflection types, the same exit-code conventions as arcbuild and the hosts, ships in `bin/`, starts in milliseconds, no runtime to install. |
| Q8 | Warm process | (a) none · (b) `arc shell` (ndjson over stdin/stdout, one connection) · (c) a daemon | **(b) in phase P4.** A fresh process plus loopback HTTP is about 10-50 ms, far below Unity's ~0.8 s. Add `arc shell` only if measurements say so. |
| Q9 | MCP | (a) never · (b) `arc mcp`: a thin stdio adapter over the same catalog, later · (c) MCP first | **(b), late (P4).** The roadmap says "not MCP", but a 300-line adapter costs nothing once the catalog has schemas, and some harnesses only speak MCP. Use tool-search style (list/describe/call), as UE does. |
| Q10 | `eval` / scripting | (a) none · (b) embed Lua or JS for `arc eval` · (c) "run a console line" only | **(a) plus (c).** The console's `Execute(line, Permission, SetBy)` already exists and is permission-checked. Arbitrary code is the riskiest surface every other engine has. |
| Q11 | ImGui introspection | (a) own `ArcUi` registry over the three hooks · (b) vendor the Dear ImGui Test Engine · (c) pixel and OCR only | **(a)**, borrowing the Test Engine's ref grammar. No licence dependency for downstream closed studios above $2M (the free OSI clause covers Arcane itself, not every game built on it). It survives the post-1.0 own-UI replacement and is small. Revisit (b) for the in-process `[ui]` test suite, where its coroutine/runner/JUnit features pay for themselves. |
| Q12 | Entity addressing | (a) Identity GUID (+ strict name) · (b) raw Astra handle · (c) hierarchy path only | **(a).** It's the binding rule: anything outliving registry state keys by GUID. Handles may appear as diagnostics only. |
| Q13 | Component type names | (a) reflected short name, qualify on collision · (b) fully qualified always (BRP `TypePath`) | **(a).** Agents found BRP's full paths verbose (`brp_type_guide` exists for that reason). Collisions error with candidates. |
| Q14 | Query surface | (a) BRP-shaped JSON (`with/without/option/has`, `strict`) · (b) a string DSL · (c) both, DSL compiled to (a) | **(a)** on the wire, with CLI sugar (`--with A,B --without C`). It must go through Astra's one View API (the unified-storage direction), so relationship storage joins later without a new verb. |
| Q15 | Undo default | (a) always transactional in the editor · (b) opt-in `--undo` · (c) opt-out `--no-undo` | **(a), with no opt-out.** Desk checks must leave the user's undo history coherent. Non-undoable commands declare it and say so in the envelope. |
| Q16 | Play-mode mutations | (a) allowed, `undoable:false` · (b) refused · (c) per command | **(c).** Live tweaks (cvars, component pokes) are allowed in Play. Authoring commands (asset ops, scene save) refuse, as their menus grey out. |
| Q17 | Screenshot return | (a) file path + metadata · (b) inline base64 · (c) both behind a flag | **(a)**, with `--inline` for small crops. Token economy (Playwright CLI) and atomic writes (BRP extras). |
| Q18 | Default output format | (a) json when not a TTY, human on a TTY · (b) Unity's tsv when piped · (c) always json | **(a).** Agents parse JSON. Humans get human. |
| Q19 | Exit codes | (a) Unity-aligned table (s3.1) plus 5 = busy · (b) arcbuild's 0/2/3 only · (c) per-verb codes | **(a).** Unity-literate agents read it immediately, and it keeps `2 = refused` consistent with `Arcane::Cli` and arcbuild. (arcbuild's 3 = would-rebuild stays arcbuild-local; `arc build --probe` maps it into JSON.) |
| Q20 | Streaming | (a) SSE · (b) WebSocket · (c) long-poll | **(a).** BRP precedent, plain HTTP, one direction is all the client needs. |
| Q21 | Execution point in the frame | (a) after input, before sim · (b) end of frame (BRP `RemoteLast`) · (c) both, per command | **(a)** by default (edits visible this frame), with `capture.*` commands armed for the end of frame. |
| Q22 | Multi-world addressing | (a) `--world` on every world-scoped command · (b) a separate descriptor per world · (c) the default world only in v1 | **(a).** `VerifyReport` already enumerates worlds (schemaVersion 6+), and the embedded-server play mode is a real case today. |
| Q23 | Who owns the editor ActionRegistry | (a) settings arc S7.2 builds it; `arc` consumes it · (b) the `arc` arc builds it · (c) both, separately | **(a).** It is already specified there with contexts and chords. Add three fields to that spec now (args schema, availability reason, undoable) so `arc` doesn't retrofit them. |
| Q24 | Structured results from `RemoteCVarService` | (a) add a JSON response variant · (b) `arc` parses its text · (c) `arc` bypasses it with a direct CVarRegistry adapter | **(a).** One audited service, two renderings. Text parsing contradicts the facts-not-prose rule VerifyReport follows. |
| Q25 | Protocol versioning | (a) integer `protocol` + `capabilities[]` in the descriptor · (b) semver endpoint prefix `/arc/v1` · (c) both | **(c).** `/arc/v1` for breaking changes; `capabilities[]` for additive features (Unity's negotiation). The CLI refuses a too-new major with exit 4 and a remediation. |
| Q26 | Tests | (a) `arc test` drives ArcaneTests out of process · (b) in-host test runner over `arc` · (c) both | **(c), phased.** (a) first (cheap, JUnit exists). Then in-host `[arc]` scenario scripts: a `.arcscenario` JSON list of `arc` calls plus assertions, run by `arc run file`. They turn the desk-verify checklists into files. |
| Q27 | Hub interplay | (a) the Hub uses `arc` for "focus/open in running editor" · (b) the Hub keeps `editor.lock` only · (c) the Hub embeds an `arc` client | **(b)** for now. The editor is standalone, with the Hub an accessory and no process calls between them. Revisit after `arc` stabilises. |
| Q28 | Linux | (a) same HTTP loopback · (b) AF_UNIX on Linux | **(a)**, so the code stays identical (and survives the cloud Linux CI). |
| Q29 | Golden comparisons | (a) `arc compare` uses `ImageCompare` + `ReferenceImages` in-host · (b) the client compares files | **(a).** One comparator. Blessing uses the same resolved-level rule as `--bless`. |
| Q30 | Log access | (a) ring-buffer sink streamed over SSE · (b) tail the file sink · (c) both | **(a)**, with (b) as the fallback when the host is dead, reporting `FileSinkPath()` in the error envelope. |

### 3.3 Phased delivery

**P0, the jack-in core (the smallest useful slice).** Goal: one desk check becomes a gate.
- Listener, dispatcher, descriptor and token; `#if !ARCANE_DIST`.
- `--headless` allowed open-ended when `--arc` is armed: the stop channel removes the refusal's reason.
- **Verbs:**
  - `arc status [--until-ready]`, `arc launch`, `arc quit`, `arc wait ready|settled|frames N`;
  - `arc cvar get|set|list|explain` (through RemoteCVarService plus the JSON variant, in the session's context);
  - `arc logs [--follow]`, `arc problems`;
  - `arc entity get <guid|name>`, `arc ecs query --with ... --without ...` (read-only), `arc ecs stats`, `arc scene tree`, `arc selection`;
  - `arc screenshot viewport|window --out`, `arc compare <ref>`.
- The full envelope, the formats and the exit-code table.
- `arc commands --format json`, `arc list`, `arc describe`.
- **Acceptance:** a CI script launches the headless editor on ReferenceProject, waits for `settled`, reads `MeshCube`'s `Transform` as JSON, sets a cvar, compares the viewport to `runtime-scene`, and quits. Every step uses exit codes and no log scraping. A HostWitness-style `[arc]` Catch2 suite drives the real staged hosts.

**P1, act.**
- Mutations: `arc entity set|add|remove|spawn|despawn|duplicate` through CommandStack transactions; `arc batch` (one transaction with `$N` refs); `arc undo|redo`.
- The editor ActionRegistry (from the settings arc, with the three extra fields) exposed as `arc action list|run`.
- Editor control: `arc select`, `arc open`, and `arc play|stop|pause|step N` (with `--topology`).
- Game-module command registration and the `commands-changed` event.
- Input: `arc input key|click` (game input through the InputSnapshot seam).
- **Acceptance:** a scripted "duplicate, move, undo, assert restored" round trip in the editor.

**P2, see.**
- The `ArcUi` item registry over the ImGui hooks.
- UI verbs: `arc ui tree` (YAML with refs, written to file), `arc ui click|type|check|select|drag|hover|wait`, with strict mode and actionability.
- Screenshots and goldens: `arc screenshot panel <ref>` (backbuffer crop) and structural UI goldens.
- `ArcUi::Tag` annotations on Inspector, Outliner, toolbar and menus.
- **Acceptance:** convert two existing desk-verify checklists (for example `docs/2026-07-28-widget-layer-desk-verify.md` and the Servitor desk pass) into `.arcscenario` files that pass headless.

**P3, build, test and jobs.**
- `arc build` (arcbuild, then the reload wait), `arc reload`.
- Jobs: `--detach`, `arc job *`, and SSE progress.
- Tests: `arc test` (ArcaneTests, JUnit/JSON), `arc gate golden`, `arc run <scenario>`.
- `arc watch entity`.
- `--format github`, for the GitHub Actions migration (D26).

**P4, polish and reach.**
- `arc shell` (a warm process), only if latency measurements justify it.
- `arc mcp` (a thin stdio adapter, tool-search style).
- `arc profile capture` (shells out to `tracy-capture`, D13).
- A packaged agent skill (`.claude/skills/arcane-arc`, plus `arc skill show` for other agents).
- A read-only session preset; the audit log viewer; an optional AF_UNIX transport if sandboxes force it.

**Sequencing notes.**
- The settings arc's S7 (RemoteCVarService) and §7.2 (ActionRegistry) are P0/P1 dependencies, and the entity-record undo arc is a P1 dependency. All three are already ahead of D27 on the roadmap.
- Two cheap, independent changes help `arc` without waiting for it:
  - the JSON variant of `RemoteCVarService`;
  - the three extra ActionRegistry fields.
  
  Both are amendments to the in-flight settings spec.

---

## Sources

**Unity:**
- https://docs.unity.com/en-us/unity-cli/replace-mcp-server-unity-cli
- https://docs.unity.com/en-us/unity-cli/unity-cli
- https://docs.unity.com/en-us/unity-cli/unity-cli-reference
- https://docs.unity.com/en-us/unity-cli/release-notes
- https://docs.unity.com/en-us/unity-cli/unity-pipeline/unity-pipeline-package
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/connectivity.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/changelog/CHANGELOG.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/creating-commands.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/authoring-commands.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/safety-and-mutations.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/batch.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/wait.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/capture.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/build-and-compilation.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/scenes.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/gameobjects-and-components.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/runtime.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/commands/editor-lifecycle-and-observability.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/runtime-setup.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/testing.html
- https://docs.unity3d.com/Packages/com.unity.pipeline@0.8/manual/code-reload.html
- https://docs.unity3d.com/Packages/com.unity.ai.assistant@2.18/manual/integration/unity-mcp-overview.html
- https://unity.com/blog/meet-the-unity-cli
- https://unity.com/resources/a-beginners-guide-to-unity-cli-and-the-pipeline-package
- https://discussions.unity.com/t/announcing-the-unity-cli-a-new-way-to-connect-your-tools-and-agents/1731104
- https://discussions.unity.com/t/unity-cli-1-0-0-beta-4-is-rolling-out/1733720
- https://discussions.unity.com/t/unity-ai-assistant-2-7-0-mcp-server-capacity-limit/1718606
- https://perflint.dev/blog/unity-cli-eval-mcp-guide/
- https://vindler.solutions/blog/unity-cli-agent-automation
- https://agmazon.com/blog/articles/technology/202609/unity-cli-mcp-guide-en.html
- https://github.com/m4bwav/unity-agent

**Unreal** (plus local source under `D:\dev\_reference\UnrealEngine-5.8.2-release\Engine\` at the paths cited inline):
- https://dev.epicgames.com/documentation/en-us/unreal-engine/remote-control-api-http-reference-for-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/remote-control-preset-api-http-reference-for-unreal-engine?lang=en-US
- https://dev.epicgames.com/documentation/unreal-engine/python-settings-in-the-unreal-engine-project-settings
- https://dev.epicgames.com/documentation/unreal-engine/run-automation-tests-in-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/screenshot-comparison-tool-in-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/gauntlet-automation-framework-overview-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/running-gauntlet-tests-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/trace-in-unreal-engine-5
- https://dev.epicgames.com/documentation/unreal-engine/using-the-slate-widget-reflector-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-mcp-in-unreal-editor
- https://github.com/EpicGames/unreal-engine-skills-for-claude-code-plugin
- https://forums.unrealengine.com/t/unreal-mcp-codex-issue-analysis-report/2730883
- https://forums.unrealengine.com/t/mcp-tool-calls-return-blueprint-runtime-errors-instead-of-the-tools-return-value/2833175
- https://forums.unrealengine.com/t/run-automated-testing-from-command-line/294995
- https://www.strayspark.studio/blog/ai-blueprint-editing-describe-graph-closed-loop-ue5
- https://github.com/chongdashu/unreal-mcp
- https://github.com/runreal/unreal-mcp
- https://pypi.org/project/unreal-engine-mcp/

**Godot:**
- https://github.com/godotengine/godot/blob/master/editor/debugger/editor_debugger_server.cpp
- https://github.com/godotengine/godot/blob/master/core/debugger/remote_debugger_peer.cpp
- https://github.com/godotengine/godot/blob/master/core/debugger/remote_debugger.cpp
- https://github.com/godotengine/godot/blob/master/scene/debugger/scene_debugger.cpp
- https://github.com/godotengine/godot/blob/master/editor/settings/editor_settings.cpp
- https://github.com/godotengine/godot/blob/master/main/main.cpp
- https://github.com/godotengine/godot/blob/master/modules/gdscript/language_server/gdscript_language_server.cpp
- https://github.com/godotengine/godot/blob/master/editor/debugger/debug_adapter/debug_adapter_server.h
- https://docs.godotengine.org/en/stable/tutorials/io/binary_serialization_api.html
- https://docs.godotengine.org/en/stable/classes/class_enginedebugger.html
- https://docs.godotengine.org/en/stable/classes/class_editordebuggerplugin.html
- https://docs.godotengine.org/en/stable/classes/class_editorsettings.html
- https://docs.godotengine.org/en/stable/tutorials/editor/command_line_tutorial.html
- https://forum.godotengine.org/150510/godot-headless-not-same-godot-window-cant-render-the-backend
- https://github.com/Coding-Solo/godot-mcp
- https://github.com/KeeVeeG/godot-mcp
- https://github.com/youichi-uda/godot-mcp-pro
- https://github.com/Erodenn/godot-mcp-runtime

**Bevy:**
- https://docs.rs/bevy_remote/latest/bevy_remote/
- https://docs.rs/bevy_remote/0.19.1/bevy_remote/builtin_methods/index.html
- https://docs.rs/bevy_remote/0.19.1/bevy_remote/schemas/json_schema/struct.JsonSchemaBevyType.html
- https://github.com/bevyengine/bevy/blob/main/crates/bevy_remote/src/lib.rs
- https://github.com/bevyengine/bevy/blob/main/crates/bevy_remote/src/http.rs
- https://github.com/bevyengine/bevy/blob/main/crates/bevy_remote/src/builtin_methods.rs
- https://github.com/bevyengine/bevy/blob/main/crates/bevy_remote/src/client.rs
- https://github.com/bevyengine/bevy/blob/main/crates/bevy_remote/Cargo.toml
- https://github.com/bevyengine/bevy/pull/18068
- https://github.com/bevyengine/bevy/pull/19377
- https://bevy.org/news/bevy-0-15/
- https://bevy.org/news/bevy-0-19/
- https://bevy.org/learn/migration-guides/0-16-to-0-17/
- https://github.com/natepiano/bevy_brp/blob/main/extras/README.md
- https://github.com/natepiano/bevy_brp/blob/main/mcp/README.md
- https://github.com/Nub/bevy_mcp
- https://github.com/bevyengine/bevy_editor_prototypes/discussions/1
- https://docs.rs/bevy_remote_wasm

**O3DE:**
- https://github.com/o3de/o3de/blob/development/Gems/RemoteTools/Code/Source/RemoteToolsSystemComponent.cpp
- https://github.com/o3de/o3de/blob/development/Code/Framework/AzFramework/AzFramework/Network/IRemoteTools.h
- https://github.com/o3de/o3de/blob/development/Code/Framework/AzFramework/AzFramework/Script/ScriptRemoteDebuggingConstants.h
- https://github.com/o3de/o3de/blob/development/Tools/RemoteConsole/ly_remote_console/ly_remote_console/remote_console_commands.py
- https://github.com/o3de/o3de/blob/development/Gems/EditorPythonBindings/Code/Include/EditorPythonBindings/PythonUtility.h
- https://github.com/o3de/o3de/blob/development/Tools/LyTestTools/ly_test_tools/o3de/multi_test_framework.py
- https://github.com/o3de/o3de/blob/development/AutomatedTesting/Gem/PythonTests/EditorPythonTestTools/editor_python_test_tools/utils.py
- https://github.com/o3de/o3de/blob/development/AutomatedTesting/Gem/PythonTests/Atom/atom_utils/atom_component_helper.py
- https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/editor/editor-automation.md
- https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/testing/parallel-pattern/_index.md
- https://docs.o3de.org/docs/user-guide/scripting/lua/debugging-tutorial/

**Playwright / CDP:**
- https://chromedevtools.github.io/devtools-protocol/
- https://github.com/ChromeDevTools/devtools-protocol
- https://raw.githubusercontent.com/ChromeDevTools/devtools-protocol/master/pdl/domains/Accessibility.pdl
- https://github.com/aslushnikov/getting-started-with-cdp
- https://developer.chrome.com/blog/remote-debugging-port
- https://www.w3.org/TR/webdriver-bidi/
- https://fxdx.dev/cdp-retirement-in-firefox/
- https://playwright.dev/docs/locators
- https://playwright.dev/docs/actionability
- https://playwright.dev/docs/trace-viewer
- https://playwright.dev/docs/test-snapshots
- https://playwright.dev/docs/api/class-pageassertions
- https://playwright.dev/docs/aria-snapshots
- https://playwright.dev/docs/codegen
- https://github.com/microsoft/playwright-mcp
- https://github.com/microsoft/playwright-cli
- https://www.checklyhq.com/blog/mcp-vs-cli-token-efficiency/
- https://bug0.com/blog/playwright-cli-vs-playwright-mcp-ai-browser-testing-2026

**Dear ImGui / Test Engine:**
- https://github.com/ocornut/imgui_test_engine
- https://github.com/ocornut/imgui_test_engine/wiki/Named-References
- https://github.com/ocornut/imgui_test_engine/wiki/Automation-API
- https://github.com/ocornut/imgui_test_engine/wiki/Overview
- https://github.com/ocornut/imgui_test_engine/wiki/Setting-Up
- https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_te_context.h
- https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_te_engine.h
- https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_te_engine.cpp
- https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_te_exporters.h
- https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/imgui_capture_tool.h
- https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/LICENSE.txt
- https://www.dearimgui.com/licenses/
- https://raw.githubusercontent.com/ocornut/imgui/master/imgui_internal.h
- https://raw.githubusercontent.com/ocornut/imgui/master/imconfig.h
- https://github.com/ocornut/imgui/wiki/Debug-Tools
- https://github.com/ocornut/imgui/issues/4122
- https://github.com/sammyfreg/netImgui
- https://github.com/JordiRos/remoteimgui
- https://github.com/AccessKit/accesskit

**Other prior art:**
- https://defold.com/manuals/editor-http-api
- https://defold.com/manuals/engine-service/
- https://renderdoc.org/docs/how/how_network_capture_replay.html
- https://renderdoc.org/docs/getting_started/faq.html
- https://renderdoc.org/docs/python_api/index.html
- https://renderdoc.org/docs/python_api/faq.html
- https://renderdoc.org/docs/python_api/examples/renderdoc/remote_capture.html
- https://renderdoc.org/docs/in_application_api.html
- https://github.com/wolfpld/tracy/blob/master/public/client/TracyProfiler.cpp
- https://github.com/wolfpld/tracy/blob/master/public/common/TracySocket.cpp
- https://github.com/wolfpld/tracy/blob/master/public/common/TracyProtocol.hpp
- https://github.com/wolfpld/tracy/blob/master/NEWS
- https://github.com/wolfpld/tracy/blob/master/capture/src/capture.cpp
- https://docs.flaxengine.com/manual/editor/advanced/command-line-access.html
- https://docs.flaxengine.com/manual/editor/advanced/extending-editor.html
- https://doc.stride3d.net/4.3/en/manual/graphics/effects-and-shaders/compile-shaders.html

**Arcane** (local, `D:\dev\starworks\Arcane` on `main` at `c0fb42e1`, plus `feat/settings-s7`):

| Area | Paths |
|---|---|
| Roadmap and specs | `docs/superpowers/specs/2026-10-05-arcane-1.0-roadmap.md` (D27); `docs/superpowers/specs/2026-10-03-settings-and-cvar-completion-design.md` (s3.2, s7.2, s9); `docs/specs/2026-09-03-host-witness-harness-design.md`; `docs/specs/2026-08-25-package-tiering-design.md`; `docs/2026-08-26-servitor-closeout-and-desk-verify.md`; `docs/research/2026-08-31-ue-automation-framework-comparison.md` |
| Host and automation | `ArcaneClient/src/Arcane/Host/{HostConfig,VerifyReport,Verdict,SettleBound,ReferenceImages}.{hpp,cpp}`; `ArcaneRuntime/src/RuntimeApp.cpp`; `ArcaneServer/src/ServerApp.hpp`; `ArcaneTests/src/Helpers/HostWitness.{hpp,cpp}`; `ArcaneTests/src/AssetStatusPanelClickTest.cpp`; `scripts/golden-gate.ps1`; `.claude/skills/arcane-verify/SKILL.md`; `ThirdParty/playwright-fixtures/README.md` |
| Editor | `ArcaneEditor/src/Panels/EditorPanels.hpp` (MenuRequests); `ArcaneEditor/src/App/EditorApp.hpp` / `EditorApp.cpp` (WriteAutoScreenshot, `--dump-layout`); `ArcaneEditor/src/Panels/{DiagnosticStore,LocatorRoute}.hpp`; `ArcaneEditor/src/App/PlayMode.hpp` |
| Undo | `ArcaneClient/src/Arcane/Edit/{Command,CommandStack}.hpp` |
| Core | `ArcaneCore/src/Arcane/Config/{CVarRegistry,ConsoleModel}.hpp`; `ArcaneCore/src/Arcane/Config/RemoteCVarService.hpp` (branch `feat/settings-s7`); `ArcaneCore/src/Arcane/Reflection.hpp`; `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp`; `ArcaneCore/src/Arcane/Base/{Diagnostics,DiagEnvelope,ServiceThread,Log}.hpp`; `ArcaneCore/src/Arcane/Project/Project.{hpp,cpp}` (EditorLock); `ArcaneCore/src/Arcane/Net/TcpSocket.hpp`; `ArcaneCore/src/Arcane/Crypto/Crypto.hpp`; `ArcaneCore/src/Arcane/Plugin/PluginHost.hpp` |
| Hub | `ArcaneHub/src-tauri/src/editorlock.rs` |
| Build | `arcbuild/src/{Exit,Output}.hpp` |
| Astra | `ThirdParty/Astra/include/Astra/{Registry/Registry.hpp,Reflection/JsonSchema.hpp,Debug/Inspector.hpp}`; `ThirdParty/Astra/VENDORED.txt` |
| ImGui | `ThirdParty/imgui/imconfig.h`; `ThirdParty/imgui/imgui.h` (1.92.9 WIP, docking) |
