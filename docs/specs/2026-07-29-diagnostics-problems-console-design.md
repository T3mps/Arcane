# Diagnostics: the Problems pane, the engine seam, and a real Console

**Status: design.** The Arcane Editor has exactly one diagnostic display
surface: `ImGui::Begin("Console")` (`EditorPanels.cpp:349-359`), a bare
`TextUnformatted` per line over a 512-entry deque of flat strings. The sink that
feeds it discards everything structured it is handed --
`m_console.Push(std::string(m.payload.data(), m.payload.size()))`
(`EditorApp.cpp:576`) throws away the spdlog level, logger name, and source
location that are sitting right there on the `log_msg`.

Everything else is a one-shot modal (dismissed = lost) or nothing at all.
Roughly a hundred `ARC_WARN` / `ARC_ERROR` sites across the engine report real,
actionable problems -- including silent permanent data loss -- with no UI
whatsoever.

This arc adds a structured diagnostic seam in the engine, a **Problems** pane
that owns current problem state, and a Console that is finally a real log
viewer.

## 0. Why this arc exists (the evidence)

Three findings drove the design. All were read this session, not recalled.

**The shader editor already fought this and lost.**
`ShaderEditorDocument::PublishDiagnostics` (`ShaderEditorDocument.cpp:3064-3120`)
flattens structured `ShaderDiag` records into log strings, and needed an FNV-1a
signature over the entire row set plus a synthetic `"diagnostics cleared"` line
to avoid spamming the console on every keystroke-triggered recompile. The
comment at `:3111-3114` concedes that severity survives only as the literal word
"error" embedded in the row text.

**A Problems panel was already anticipated, then never built.**
`ShaderEditorDocument.hpp:10-12` states outright: *"Diagnostics have NO panel of
their own"*. `:5-6` records that an errors panel **once existed and was
removed**, leaving `m_jumpToLine` (`:555`) as a dead click-to-line seam. Other
comments keep consumers alive explicitly "for the Problems panel".

**The highest-consequence diagnostic in the engine is console-only.**
`SceneSerializer.hpp:273-289` warns when a scene component is skipped (unknown
type, or reflected-but-unregistered because a plugin failed to load) and its own
comment says *"re-saving this scene will drop it permanently"*. That is silent
permanent data loss whose only trace is a line in a 512-entry ring buffer.

## 1. The model: two surfaces, two lifetimes

**The Console is a stream. Problems is a state.**

A log line is an event that happened at a time and is never untrue afterwards --
append-only, chronological, capped. A diagnostic is an assertion about how
things *are right now*, and it stops being true the moment the material
recompiles clean. Expressing the second in the first is precisely what forced
the FNV gate and the fake "cleared" line.

Problems is therefore built on **publication groups**. A producer owns a key and
republishes its *entire* set for that key; the store replaces that key's
contents atomically. Retraction is not a special case -- it is a republish with
an empty set. Individual rows are never added or removed, so there is no
reconciliation logic to get wrong and no staleness to chase.

Keys are producer-scoped strings:

| Key | Republished when |
|---|---|
| `material:<guid>` | every compile / regen of that document |
| `scene:<path>` | scene load |
| `plugin:<name>` | plugin load / reload / rollback |
| `assets` | asset-registry scan |
| `project` | project open |

## 2. The engine seam

`Arcane/Arcane/src/Arcane/Base/Diagnostics.hpp` (engine DLL), mirroring how
`Arcane::Log` already works: one exported storage, module-agnostic callers.

```cpp
enum class DiagSeverity : std::uint8_t { Info, Warning, Error };
enum class DiagScope    : std::uint8_t { Project, Assets, Scene, Plugin, Material, Shader };

struct DiagLocator                     // what clicking the row does
{
    enum class Kind : std::uint8_t { None, Entity, Asset, File, GraphNode };
    Kind          kind = Kind::None;
    std::uint64_t entity = 0;          // Kind::Entity
    Guid          asset;               // Kind::Asset
    std::string   file;                // Kind::File
    int           line = 0;
    int           col  = 0;
    Guid          ownerAsset;          // Kind::GraphNode -- the material
    std::uint32_t nodeId = 0;
};

struct Diagnostic
{
    DiagSeverity severity;
    DiagScope    scope;
    std::string  code;      // stable, dotted: "scene.component.unknown"
    std::string  message;
    std::string  detail;    // optional consequence line, dimmed in the UI
    DiagLocator  locator;
};
```

API: `Publish(std::string_view key, std::span<const Diagnostic>)` and
`SetSink(...)`, both `ARCANE_API`. Storage lives in `Arcane.dll` only;
`ArcaneEditor.exe` is a separate binary and calls the exported functions, the
same shape `Arcane::Log` uses today.

`code` is stable and dotted on purpose. It is the natural home for a lint rule
id, so the HLSL linting arc publishes into this same pipe
(`code = "hlsl.implicit-truncation"`) instead of inventing a parallel channel,
and it is what any future per-rule suppression or docs link hangs off.

**The sink is mutex-guarded from day one**, not retrofitted. The async-boot arc
(`2026-07-29-async-boot-loading-screen-design.md`) runs `project_open` -- and
therefore the asset-registry scan, a producer -- on a worker thread. Same
requirement as the `ConsoleBuffer` mutex in §5.

## 3. Editor side: store, pane, navigation

### `DiagnosticStore`

Subscribes to the engine sink, holds `key -> vector<Diagnostic>` under a mutex,
and exposes grouped/filtered views. Pure data, no ImGui dependency, headless
testable. Editor-local producers publish through the same exported API rather
than a second path.

### The Problems pane

Docks beside Console at the bottom. Grouped by `DiagScope` with per-group
counts, errors sorted first, a severity filter, a search box, and a header
carrying total error/warning counts. Each row shows `message`, with `detail`
beneath in dimmed text -- so the scene-component case reads with its consequence
attached, which is the entire reason that warning exists.

### Navigation

Clicking a row routes through a `LocatorRouter` in `EditorApp`:

| Locator kind | Action |
|---|---|
| `Entity` | select in Outliner + focus Inspector |
| `Asset` | open its document, or reveal in the Assets browser |
| `File` | open the document and jump to the line |
| `GraphNode` | open the material, switch to the owning pass, select the node |

`File` **consumes the dormant `m_jumpToLine` seam** (`ShaderEditorDocument.hpp:555`),
dead since the old errors panel was removed. `GraphNode` reuses the machinery
the canvas badges already drive (`RebuildDiagBadges`, `ShaderEditorDocument.cpp:3454-3480`).
Two comments in the codebase that kept consumers alive "for the Problems panel"
stop being aspirational.

## 4. v1 producer migration

Seven producers move to the seam. The other ~90 `ARC_WARN` sites keep flowing to
Console only and migrate opportunistically -- this arc does not sweep them.

1. **Scene component skips** (`SceneSerializer.hpp:273-289`). Codes
   `scene.component.unknown` / `scene.component.unregistered`, Error, locator =
   the entity, and the "re-saving this scene will drop it permanently" line
   becomes `detail`. The highest-value single migration in this list.
2. **Shader/material diagnostics.** The full set `ForEachDiagnosticRow`
   (`ShaderEditorDocument.cpp:2995-3062`) produces, published as structured
   records with real severity and `GraphNode`/`File` locators instead of
   flattened strings.
3. **Plugin load.** Currently three genuinely distinct causes collapse into one
   bool and one generic line -- verified: `Plugin.cpp:24-28` checks seven
   required exports and returns bare `false` naming none of them; `:31` is
   `return out.ABIVersion() == kGamePluginABIVersion;`, discarding **both**
   version numbers; `Module.cpp:50-51` returns `nullopt` on `LoadLibraryW`
   failure without capturing `GetLastError()`. Split into
   `plugin.module.load-failed` (carrying the OS error),
   `plugin.export.missing` (naming the symbol), and `plugin.abi.mismatch`
   (naming both versions). Requires `ResolveGamePluginAbi` and `Module::Load` to
   report *why* rather than returning a bare bool / `nullopt`. **This is a real
   fix, not plumbing** -- today's modal advice is a guess.
4. **Asset registry scan** -- duplicate id (kept-first), file outside content
   root, id write-back failure. All currently invisible, all cause confusing
   downstream behavior.
5. **Unresolved asset GUIDs** (`Assets.cpp:376`) plus Inspector
   dangling-reference styling -- today a broken material reference renders as an
   ordinary grey GUID string with no indication anything is wrong.
6. **Material loader drops** -- malformed param / pass / graph entries silently
   discarded at load (`MaterialAsset.cpp`).
7. **Project open / manifest schema.** Feeds Problems *and* keeps the existing
   modal; a failed project open still deserves a blocking dialog.

## 5. Console polish

`ConsoleBuffer` stops storing bare strings:

```cpp
struct ConsoleEntry
{
    DiagSeverity      level;       // from m.level
    std::uint64_t     timestampMs; // from m.time
    std::string       category;    // derived, see below
    std::string       message;
    spdlog::source_loc source;     // from m.source
};
```

Every one of those fields is already on the `log_msg` the sink receives and is
discarded one line later at `EditorApp.cpp:576`. **That single capture is the
enabling fix**; the rest is presentation.

`category` is derived from the consistent `"Subsystem: "` prefixes already used
across the codebase (`AssetRegistry:`, `Assets:`, `plugin:`, `scene load:`,
`SpriteMaterialCache:`, `PostChainCache:`, `LoadMaterialAsset:`, ...) via a
small lookup table, unknown falling back to `General`. This is stringly-typed
and deliberately so: it is zero engine churn, the prefixes are already de facto
stable, and anything that genuinely matters gets a real `DiagScope` through the
seam instead.

Panel gains:

- Toolbar: per-severity toggle buttons with live counts, search box, Clear, Copy
  selection, autoscroll pin, Collapse toggle.
- Severity-colored rows, word wrap, timestamp and category columns.
- Configurable line cap replacing the hardcoded 512 (`EditorApp.hpp:182`).
- **Collapse** folds identical `(level, category, message)` triples into one row
  with an xN badge, computed at draw time over the current buffer -- no storage
  change, and nothing is hidden.

`ConsoleBuffer` also gains its mutex here (`ConsoleBuffer.hpp:11-15` documents
the exact hazard: *"If a worker ever logs, wrap Push in the sink's lock"*).

## 6. What gets deleted

`PublishDiagnostics`, its FNV-1a signature gate, `m_emittedDiagSig`, and the
synthetic `"diagnostics cleared"` line (`ShaderEditorDocument.cpp:3064-3120`) --
roughly 60 lines whose whole job was making an append-only log imitate a
replaceable set. Publication groups do that natively.

The toolbar's `HasErrors()` status word (`:2001-2006`) **stays**: it is a useful
at-a-glance indicator, not a workaround.

Structured diagnostics go to Problems **only**. No duplication into the Console.

## 7. Testing

`DiagnosticStore` is pure and GPU-free. New tag `[diagnostics]`:

- Publishing a key **replaces** that key's set rather than appending.
- An empty republish clears the key; other keys are untouched.
- Grouping, per-scope counts, and filter/search predicates are correct.
- A worker publishing while the main thread reads is safe (shares the
  `ConsoleBuffer` concurrency test with the async-boot arc).

Plus:

- **Plugin-load cause split**: three distinct failures produce three distinct
  codes. Currently untestable, because all three are one bool.
- Console: category-derivation table, collapse counting, line-cap eviction.

Locator navigation is **desk-verify** -- it is ImGui-driven, and windowed runs
SIGSEGV under this box's virtual-display setup.

## 8. Sequencing

**Land this arc before the async-boot arc.** Boot introduces the first logging
worker and the first worker-side diagnostic producer, so it needs a thread-safe
`ConsoleBuffer` and a thread-safe diagnostic sink. Building them here means boot
inherits them rather than growing both mid-arc.

**Land this arc before HLSL linting.** Lint findings become `Diagnostic` records
with `code` = rule id, and the "hide engine-generated-line findings" policy
simplifies from *filter them out of a panel* to *do not publish them, log once*.
The linting arc gets a real surface instead of inventing one.

## 9. Desk-verify checklist

1. Break a material: Problems shows the errors grouped under Materials with
   counts; fix it, the group empties itself with no "cleared" line anywhere.
2. Console shows the same session's log stream *without* the shader rows.
3. Click a shader error row: the material opens, the right pass is selected, and
   the cursor lands on the offending line.
4. Click a scene-component row: the entity selects in the Outliner.
5. Load a scene whose plugin is not loaded: a scene-component-skip row appears
   with the permanent-data-loss detail visible.
6. Point a project at a stale game DLL: the plugin row names the ABI numbers;
   delete an export and it names the symbol; corrupt the DLL and it carries the
   OS error.
7. Console toolbar: severity toggles filter and counts match, search narrows,
   Collapse folds a repeated warning to one row with xN, Clear empties, Copy
   puts the selection on the clipboard, autoscroll pin holds position.
8. Inspector shows a dangling material reference as a problem, not grey text.

## 10. Non-goals

- Sweeping the remaining ~90 `ARC_WARN` sites (§4 migrates seven; the rest are
  opportunistic).
- Per-module loggers / a real category enum in `Arcane::Log` (the prefix table
  is the deliberate cheap answer; `DiagScope` covers what matters).
- Diagnostic suppression / "ignore this rule" UI. `code` is designed to support
  it later; nothing is built now.
- Persisting Problems across editor sessions -- it is current state by
  definition, rebuilt by its producers on load.
