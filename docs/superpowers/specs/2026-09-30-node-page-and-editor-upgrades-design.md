# Node page and editor upgrades: the 2026-09-30 critique phase

**Status:** Approved 2026-09-30 after a brainstorm with the user. The decisions are in section 9. Brief defaults are listed for review in 9.27, and drafting picks in 9.28.
**Answers:** the 1920x1080 editor critique, `.superpowers/critique-2026-09-30/editor-critique-final.md`: its top-10 list and its cheap fixes.
**Amends:**
- `docs/superpowers/specs/2026-09-28-input-actions-editor-redesign-design.md` s2.3: the Rebind column becomes always visible. That spec had it hover/selected-only (`:61`).
- `docs/superpowers/specs/2026-09-28-inspector-ownership-design.md` s3.5: this phase delivers its promised node page.

**Scheduled:** the next phase after inspector filters. The entity-record undo arc follows it (s10).

## 1. The problem and the goal

The critique grades the editor **C+**, "better built than it looks". The mechanics are careful: undo, PropertyGrid, rebind capture, the Console ring and crash capture. What the user sees and touches is not. Important state is hard to see, the asset-kind pages ignore PropertyGrid, everyday verbs are missing, and there is one silent data-loss path.

| Surface | Grade | Top-10 items |
|---|---|---|
| Shell & layout | B- | #5 accent/state, #10 start page |
| Inspector | C+ | #3 asset page fit, #4 PropertyGrid ports |
| Outliner & Viewport | B- | #1 data loss |
| Asset Browser / Graph / Status | C+ | #6 file ops + multi-select |
| Console & Problems | C+ | #8 |
| Shader editor | C+ | #2 previews, node page |
| Input Actions | B- | #9 |
| Mesh & Sprite | C- | #2 previews, #4 ports |
| Crash report & launch | C+ | #7 crash viewer, #10 start page |

**The centrepiece is the shader NODE page.** When exactly one node is selected, the Inspector shows a page with:
- the node's type and description;
- one row per input pin: wired, an editable literal, or the neutral default;
- its per-type settings, its outputs and its errors.

Edits apply live while dragging, with one undo step per gesture. This completes the inspector-ownership source model, where every surface feeds the one Inspector through PropertyGrid. It also forces the T2 foundations that the material, sprite, mesh and asset pages need anyway.

**Done means:**
- **Re-capture.** The critique's scenarios 01-20 are re-run with `.superpowers/critique-2026-09-30/capture.ps1` at 1920x1080 on the new default layout. Each run uses `--backend dx12` and a fresh `LOCALAPPDATA` per scenario, and the set adds a node-page capture. Each named top-10 or cheap-fix problem is absent from its capture. A problem no capture can show (the Warnings filter, the Space placeholders, the data-loss path, Play mode) is closed by its named test.
- **Goldens.** All five editor slots are re-blessed together once, in T4's batch; other tranches re-bless only the slots their change forces, one slot at a time (s2.6): `editor-ui`, `editor-ui-perspective`, `editor-input-doc`, `editor-asset-page` and `editor-material-page` (`scripts/golden-gate.ps1:335-363`).
- **Every tranche gate is green** (s2.1). The phase ends on T6's green gate.

**Out of scope** (s10): the entity-record undo arc, the configuration pass, per-node previews, Outliner sibling reordering and type icons, viewport snapping UI, Console source locations, sprite texture editing, Asset Graph edge labels, and a full-pane preview for imported meshes.

## 2. Plan-wide rules

### 2.1 Six gated tranches

One spec and one plan. Each tranche closes on its own green gate before the next starts.

| Tranche | Content | Section |
|---|---|---|
| **T1** correctness | Outliner data loss, preview wiring, undo stack correctness, Astra "now" batch | 3 |
| **T2** shared foundations | PropertyGrid rows, shared asset-reference field, header crumbs, `BeginPopupBelow`, `GraphFit`, shell path helper, link row, monospace font, toggle helper | 4 |
| **T3** Inspector pages | node page, honest preview status, material/sprite/mesh page ports, asset page fit, pinned header | 5 |
| **T4** shell state | accent, Play presence, top-strip status, dim text, "0 refused", Preferences, Asset Graph legend; **the batched golden re-bless** | 6 |
| **T5** asset file ops | rename, duplicate, delete, multi-select, move, all undoable | 7 |
| **T6** independent surfaces | crash viewer, Problems/Console, Input Actions, start page | 8 |

**The gate.** A tranche is green when all of these hold on its final commit:
1. **Build.** `Arcane.slnx` builds in Debug and Release (`-md`). The ReferenceProject and Aphelyon (`D:\dev\starworks\Aphelyon`) game modules build through `arcbuild` in both configs.
2. **ArcaneTests.** `~[gpu]` plus the `[witness]` cases, with `[shell]` excluded, in both configs. Run FROM THE EXE DIR and record each run's random-order seed.
3. **Golden gate.** `scripts/golden-gate.ps1` is green in both configs and ends on Debug. Delete the exe-dir `imgui.ini` first.
4. **Windowed desk check.** Check every surface the tranche touched at 1920x1080 on the user's layout. Vet the shader graph on a scratch copy of Aphelyon's `logo_showcase`.
5. **ABI hygiene.** If the tranche bumped the ABI (s2.3), restamp ReferenceProject and Aphelyon and rebuild both modules before steps 1-4.

Intermediate steps inside a tranche need only the build and ArcaneTests. The golden gate and the desk check run at the tranche's end, and T4's editor lanes are expected red until 6.10. A red gate is fixed forward inside its tranche, and nothing from the next tranche starts on a red one.

### 2.2 Astra first

1. Commit in the Astra repo (`D:\dev\starworks\Astra`, branch `dev`), with its tests.
2. Run `scripts\sync-astra.ps1` to re-vendor into `Arcane/ThirdParty/Astra`.
3. Stage the changed files BY NAME plus `VENDORED.txt`. Never stage the CRLF-only fan-out.

Arcane never edits `ThirdParty/Astra` directly. T1's "now" batch (s3.4) is the only Astra work in this phase.

### 2.3 ABI bumps

The constant is `kGamePluginABIVersion` (`ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:987`, today 46). Each bump adds a `vNN` history line (style of `:982`).
- A tranche that bumps takes exactly ONE bump, covering all its changes, numbered at merge.
- Every bump restamps `engine.abi` in ReferenceProject's and Aphelyon's `.arcproj` and rebuilds both modules. The host's ABI gate refuses the stale DLL.
- Only `EditorApp` and ArcaneTests derive from or call `ICommand`/`CommandStack`. Game modules still link the `ArcaneClient` export, so a vtable change is a bump even though no module overrides `ICommand`.

| Tranche | Boundary change | Where |
|---|---|---|
| T1 | `ICommand` gains `AffectsScene`, `IsExpired`, `PayloadBytes`; `CommandStack` API/layout (`UndoLimits`, `UndoPayload`, `Clear(reason)`) | `ArcaneClient/src/Arcane/Edit/Command.hpp:11-18`, `CommandStack.hpp` |
| T1 | Astra: `Registry` instance id (layout); `SetParent` returns `bool` and takes an index; retire/`Clear` allocator rules (header behaviour both sides must agree on) | s3.4 |
| T1 | `VerifyReport` schema 12 -> 13: `documents[]` (drafting pick, 9.28: moved from T3, where the brief lists it, to T1, whose witness reads it first) | `ArcaneClient/src/Arcane/Host/VerifyReport.hpp:230` |
| T3 | `GraphNodeTypeInfo::description` column; `GraphPinNeutralDefault` export | s5.1 |
| T5 | `AssetRegistry::Remove`/`Rebind`/`PeekId`; `Project::UnregisterAsset`/`RebindAsset`; `Runtime::UnregisterAsset`/`RebindMovedAsset`; `Assets::EvictPath`/`ForgetUnresolved` (appended virtuals) | s7.2 |
| T6 | `ConsoleModel` history and completion; `CVarRegistry::ListCommands`; `Log::SetLevel` export; `ArcaneClient` `DrawConsoleInputLine` | s8.2 |

### 2.4 Cvars

Every NEW tunable is an Archive cvar, never a `constexpr`. It carries help text, `min`/`max` when numeric, and a dotted category, and it persists through `Saved/Config` (`ArcaneCore/src/Arcane/Base/Runtime.cpp:524-530`). The configuration pass (s10) builds its settings window by enumerating the registry, so a constexpr would be invisible to it.
- **Registration.** `ARC_CVAR` (`ArcaneCore/src/Arcane/Config/CVarDecl.hpp:13-23`) sets no range and hard-codes `desc.module = "engine"` (`:21`). T1 adds `ARC_CVAR_RANGED(name, module, type, default, min, max, flags, help)` beside it (drafting pick, 9.28). It fills `CVarDesc::min`/`max` (`CVarRegistry.hpp:24-34`, fields `:28-29`), and editor cvars pass `"editor"`. Every editor cvar in this plan uses it.
- **Engine code stays cvar-agnostic,** with two named Core-subsystem exceptions. Both register through `CVarDesc` from inside a Core function with a runtime default, not at static init:
  - `log.level` registers in `Log::Init` and applies through `Log::SetLevel`. Its default is the level `Init` was given (`Log.cpp:209-231`; `Init` defaults to info, `Log.hpp:25`), so an explicit `Init(level)` is not overridden by the cvar default. An Archive value from `Saved/Config` applies through the callback when config loads.
  - `console.historySize` is registered by `CVarRegistry` and read by `ConsoleModel`; the console is the registry's own UI.
- **Engine-type tunables are pushed in by the editor.** The editor calls `CommandStack::SetLimits`, and the stack never reads a cvar.

| Cvar | Type | Default | Range | Tranche | Consumer |
|---|---|---|---|---|---|
| `editor.undo.maxSteps` | Int | 100 | 1..10000 | T1 | step eviction (today's ctor default, `CommandStack.hpp:43`) |
| `editor.undo.byteBudgetMB` | Int | 512 (drafting pick, 9.28) | 16..65536 | T1 | byte-budget eviction |
| `editor.undo.spillThresholdKB` | Int | 256 (drafting pick, 9.28) | 16..1048576 | T1 | payloads above it spill to `Saved/UndoCache/` |
| `editor.graph.fitMaxZoom` | Float | 1.0 | 0.1..2.0 (the zoom table, `GraphZoomLevels.hpp`) | T2 | `GraphFit` zoom cap |
| `editor.inspector.materialPreviewFraction` | Float | 0.45 (drafting pick, 9.28) | 0.2..0.8 | T3 | material preview box height |
| `editor.inspector.assetThumbMinPx` | Int | 64 | 32..140 | T3 | asset-page thumbnail floor |
| `editor.inspector.assetThumbHeightFraction` | Float | 0.30 (drafting pick, 9.28) | 0.1..0.6 | T3 | asset-page thumbnail height |
| `console.historySize` | Int | 64 (drafting pick, 9.28) | 1..1024 | T6 | console input-history ring |
| `log.level` | Int (spdlog trace..off) | 2 (info) = `Init`'s level | 0..6 | T6 | runtime `set_level` on every sink (a Console-only level stays possible later without changing this cvar, s8.2); Archive \| Dev |

### 2.5 GUID rule (binding)

- Anything that outlives the registry state it points into keys entities by `Identity` GUID.
- A live component may hold an Astra handle only if a visitor declares it and a remap pass rewrites it.
- Copies mint fresh GUIDs, and there is never same-id resurrection.
- **New code in this phase adds no handle holders.** T1 entity work either holds a handle within one frame's call or uses the existing `SceneRoot` resource. Rekeying the existing holders is the entity-record arc (s10).

### 2.6 Golden policy

- **One batched re-bless at T4's end** covers all five editor slots.
- **Other tranches re-bless only the slots their change forces,** one slot at a time, after reading the diff. The commit names the cause. The known cases:
  - T1: `editor-material-page`, only if the preview now renders there.
  - T2: the deleted material title line (`editor-material-page`). Wherever a slot shows an entity page: the deleted name line, the Color4 swatch position, and thumb + name asset fields. Label ellipsis wherever a label clipped, and `editor-input-doc` if the breadcrumb layout moves it (s4 intro).
  - T3: the graph-owned `.arcmat` fixture changes the `editor-ui` asset counts; the ports change `editor-material-page` and `editor-asset-page`; the pinned header changes any slot showing an Inspector.
  - T5: `editor-asset-page` (its icon row gains Rename/Duplicate/Delete), at s7.13 step 3.
- **T6 runs after T4 (s2.1),** so `editor-input-doc`'s Input Actions change is its own slot re-bless in T6, by the standard procedure. The brief's "folds into T4's batch if T6 lands after" cannot trigger under strict tranche order; flagged for the user.
- **Procedure (every re-bless):** bless the STAGED slot, copy it to the source tree IMMEDIATELY, gate both configs, end on Debug.
- **Harness determinism.** Machine-dependent UI (the Problems/Console badges and the chip, s8) is suppressed under the verify/screenshot harness.

## 3. T1 -- correctness

### 3.1 Outliner data loss

**Problem** (critique Outliner #1, #2; top-10 #1). The code trace confirms it, and it is worse than the critique says.
- **The unparent strip strands entities.** "(drop here to unparent)" (`ArcaneEditor/src/Panels/EditorPanels.cpp:2230`) calls `Edit::Reparent(registry, moving, Astra::Entity::Invalid())` (`:2243-2244`).
  - `Reparent` maps an invalid parent to `reg.RemoveParent(e)` (`ArcaneClient/src/Arcane/Edit/EntityOps.cpp:168-198`, at `:194`), so the entity becomes a registry root beside SceneRoot.
  - `SaveJson` walks only SceneRoot's subtree (`ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp:131-150`, seeded `:148`), so the next Save drops the entity.
  - The loss is silent: the Outliner still lists it at depth 0 (`ArcaneEditor/src/Panels/EntityList.cpp:117-120`).
  - It also renders FROZEN. Transform propagation rebuilds only from the root (`ArcaneCore/src/Arcane/Scene/TransformSystems.hpp:238-273`) and `Materialise` skips rows outside it (`:317-337`). The orphan keeps its stale `WorldTransform` while the gizmo follows the live graph.
  - `CreateEntityInScene` already fixed this exact failure for New Entity (`EntityOps.cpp:99-107`; regression `ArcaneTests/src/EntityOpsTest.cpp:497-535`). The strip was missed.
- **Deleting the Scene root, then saving, writes ONE EMPTY ENTITY.** No structural path checks for the root:
  1. `DeleteEntities` destroys it, and each child gets `RemoveParent` because the heir is Invalid (`EntityOps.cpp:128-166`, `:150`).
  2. The `SceneRoot` resource keeps the dead handle.
  3. `DoSaveScene` checks only that the resource exists (`ArcaneEditor/src/App/EditorAppScene.cpp:256`).
  4. `SaveJson` seeds `order = [dead]` (`SceneSerializer.hpp:148`).
  5. `GetEntityComponents(dead)` returns empty (`ThirdParty/Astra/include/Astra/Registry/Registry.hpp:932-937`).
- **The other root paths.**
  - Cut copies and deletes the whole scene (`EditorPanels.cpp:1521-1543`).
  - Duplicate nests a second scene, including a second Camera, under SceneRoot (`EntityOps.cpp:651-661`).
  - Dragging the root onto an orphan moves SceneRoot out of the saved subtree (drag source `EditorPanels.cpp:2185`).
  - After a root delete, Paste and New Entity parent to the dead handle (`EntityOps.cpp:103-106`, `:439-441`).
- **Precedent.** Select All already excludes the root (`ArcaneEditor/src/Scene/SelectionOps.hpp:17-31`; `ArcaneTests/src/SelectionOpsTest.cpp:39`).

**Design.**
- **`Edit::LiveSceneRoot(const Astra::Registry&) -> std::optional<Astra::Entity>`** returns the root when the `SceneRoot` resource is present AND `reg.IsValid(root)`. It is the one live-root check. `ReparentInScene`, `DoSaveScene`, `CreateEntityInScene` and `InstantiateSubtrees` all call it.
- **`Edit::ReparentInScene(Astra::Registry&, std::span<const Astra::Entity> set, Astra::Entity parent) -> std::size_t`** (`EntityOps.hpp/.cpp`) mirrors the `CreateEntity`/`CreateEntityInScene` split.
  - A valid `parent` forwards to `Reparent`. An invalid one resolves to `LiveSceneRoot`.
  - It returns 0 and moves nothing when `LiveSceneRoot` is nullopt.
  - The strip calls it inside `ApplyStructural`, so a move is one undo step and a refusal pushes none. The strip's label becomes "(drop here to move to scene root)".
- **`Edit::IsSceneRoot(const Astra::Registry&, Astra::Entity) -> bool`**: the entity is valid and equals `SceneRoot::entity`.
- **Root guard on structural verbs** (Delete, Cut, Duplicate, Copy, Drag). It applies at every entry point: Del/Ctrl+X/C/D, the row context menu (`EditorPanels.cpp:2145-2177`) and the Edit menu (`:203-214`).
  - One helper beside `CollectSceneEntities` (`SelectionOps.hpp`) returns the selection without the root, and every verb calls it.
  - A mixed selection drops the root and the verb proceeds.
  - A root-only selection disables the item with a reason tooltip (default). The wording, e.g. "The scene root can't be deleted", is a drafting pick (9.28).
  - The root row is never a drag source (`:2185`), and a multi-selection drag excludes it from `moving`.
  - The root row stays selectable and inspectable, and dropping ONTO it (`:2192-2207`) is unchanged.
- **Dead-root backstop.** `DoSaveScene` (`EditorAppScene.cpp:256`), `CreateEntityInScene` (`EntityOps.cpp:103-106`) and `InstantiateSubtrees` (`:439-441`) require `LiveSceneRoot`. The save refusal reuses the existing modal ("There is no scene to save...").
- **Unchanged:** raw `Reparent(Invalid)` semantics, pinned by `EntityOpsTest.cpp:125-127` and `RegistryStateCommandTest.cpp:133-138`.

**Does NOT:** migrate existing registry roots (runtime and game-module entities may live there), add sibling reordering (s10), or change dropping onto the root row.

**Tests** (`ArcaneTests`).
- `EntityOpsTest`:
  - `ReparentInScene(Invalid)` -> `SaveJson` -> `LoadJson` finds the entity by name under the root (the shape of `:497-535`).
  - `ReparentInScene` refuses with no SceneRoot and with a dead root.
  - `CreateEntityInScene` and `InstantiateSubtrees` refuse with a dead root.
  - `LiveSceneRoot` is nullopt with no resource and with a dead root.
- `RegistryStateCommandTest` (the `:130-138` shape): a root-only Delete through `ApplyStructural` pushes NO step and destroys nothing. A mixed Delete destroys the others and keeps the root.
- `SelectionOpsTest`: the root filter drops the root and keeps order. A root-only selection yields empty.
- **Desk:**
  - Drag an entity to the strip, save, reopen: it is under Scene.
  - With the root deleted, Save refuses and leaves the scene file byte-identical.

### 3.2 Preview wiring (late-bound seam)

**Problem** (critique Shader #1, Mesh & Sprite #1; top-10 #2). Documents opened during boot never reach a ready preview.
- **The factories copy the device seam at construction,** under `if (ChromeGraph())`:
  - mesh: `ArcaneEditor/src/App/EditorApp.cpp:870-879`;
  - shader: `MakeDocServices`, `EditorAppProject.cpp:86-122`.
- **The chrome graph does not exist yet.** `ChromeGraph()` (`EditorApp.cpp:2171-2174`) is null until `CreateGraphVehicles`, which `Main()` runs after every boot stage (`:2583-2588`). `--open-asset` opens inside `StageFinalize` (`:1227`; open `:1290-1318`), so its document copies a null seam.
- **Nothing rebinds it.**
  - `MeshDocument` calls `EnsurePreviewContext()` only from its constructor (`MeshDocument.cpp:118`).
  - The shader document's bind-time retry (`ShaderEditorDocument.cpp:1476`) reads the same null `m_services`.
- **Result:** "no GPU device" in a DX12 editor, and a permanent "compiling..." after a successful compile.
- **Working precedent:** the material-preview harvester resolves the chrome graph live with `hs.chromeGraph = [this] { return ChromeGraph(); }` (`EditorApp.cpp:1024-1026`) and has a `givenUp` latch.

**Design.**
- **Late-bound seam.**
  - `DocServices` (`ShaderEditorDocument.hpp:90-164`, seam fields `:136-138`) and `MeshDocument::Services` drop the copied `nriDevice`/`chromeHud`. They gain `std::function<Arcane::NriGraphContext*()> chromeGraph`, resolved at each use: `chromeGraph()->Device()` and `chromeGraph()->ImGuiHud()`.
  - The factories set `chromeGraph`, `hostConfig` and `retireGraphPreview` unconditionally, and the `if (ChromeGraph())` blocks go.
  - The lifetime argument at `EditorAppProject.cpp:86-122` still holds. `ShutdownGraphPath`/`TeardownGraphForSwitch` close every document while the chrome context lives, and the context survives project switches (`EditorApp.cpp:2684`).
- **Destructor safety.** A document records the `ImGuiNriNode*` it bound its preview image through, at vehicle creation. Its destructor invalidates that pointer (today `MeshDocument.cpp:403`, `ShaderEditorDocument.cpp:1665`) rather than re-resolving `chromeGraph()->ImGuiHud()`, because `ChromeGraph()` is null after `ShutdownGraphPath` (`EditorApp.cpp:2168-2174`).
- **Retry from `Tick`.** While a document has no vehicle and is not latched, `Tick` calls `EnsurePreviewContext()` / `EnsureGraphPreviewContext()`.
  - A null `chromeGraph()` costs a null check and never latches.
  - The first non-null attempt either builds the vehicle or sets the give-up latch (`VehicleFailed`). Tick then stops retrying.
  - A later frame failure keeps today's drop-and-rebuild-on-bind (`ShaderEditorDocument.cpp:1614-1619`) and sets `FrameFailed`.
  - The latch has no count, so it needs no tunable.
- **The `PreviewStatus` model.** This section is its only definition. T3 (s5.2) adds only `ToolbarStatusText`, `PreviewBoxText` and the UI. The new pure header is `Documents/PreviewStatus.hpp` (no ImGui):
  ```cpp
  enum class CompileStatus : std::uint8_t { NotCompiledHere, CompilerUnavailable, Compiling, Errors, Ok };
  enum class PreviewAvailability : std::uint8_t { Ready, NoDevice, VehicleFailed, FrameFailed };
  struct PreviewStatusInputs {
      bool notCompiledHere = false;    // shader: mesh surface; mesh: imported
      bool compilerAvailable = true;   // services.compiler && sources && compiler->IsAvailable()
      std::uint32_t jobsInFlight = 0;
      bool hasErrors = false;
      bool deviceSeam = false;         // chromeGraph() returned non-null
      bool vehicleFailed = false;      // creation latch
      bool frameFailed = false;        // frame latch
      bool imageBound = false;         // PreviewReady() (shader) / a rendered image (mesh)
  };
  struct PreviewStatus { CompileStatus compile; PreviewAvailability preview; bool image; };
  [[nodiscard]] PreviewStatus ComputePreviewStatus(const PreviewStatusInputs&) noexcept;
  [[nodiscard]] const char* CompileStatusId(CompileStatus) noexcept;
  [[nodiscard]] const char* PreviewAvailabilityId(PreviewAvailability) noexcept;
  ```
  - **Compile precedence:** `notCompiledHere` -> NotCompiledHere; `!compilerAvailable` -> CompilerUnavailable; `hasErrors` -> Errors; `jobsInFlight > 0` -> Compiling; else Ok.
  - **Preview precedence:** `!deviceSeam` -> NoDevice; `vehicleFailed` -> VehicleFailed; `frameFailed` -> FrameFailed; else Ready. `image = preview == Ready && imageBound`.
  - **Ids:** compile `not-compiled-here`, `compiler-unavailable`, `compiling`, `errors`, `ok`; preview `ready`, `no-device`, `vehicle-failed`, `frame-failed`.
  - **Shader inputs** come from one `PreviewStatus ComputeStatus() const`:
    - `jobsInFlight` is a new counter. It increments on each non-zero `Submit` (cpp `:1161`, `:1202`, `:1206`) and decrements when a result matches a live job id (`:1243-1246`).
    - It is zeroed wherever job ids are invalidated (`:1083`, `:1107`, `m_passJobs.clear()`).
    - A zero `Submit` return reads as CompilerUnavailable.
    - `vehicleFailed` latches where `CreateOffscreen` returns null (`:1428-1435`) and clears on a later successful creation.
    - `frameFailed` latches at the frame-failed drop (`:1614-1619`) and clears on the next good frame.
  - **Mesh inputs:** a validation reason -> Errors; imported -> NotCompiledHere; no seam -> NoDevice; vehicle unavailable -> VehicleFailed; a rendered image -> Ready.
- **The report record (schema 13, the T1 bump, s2.3).** `VerifyReport` gains `struct DocumentPreview { std::string guid, kind, name, compile, preview; bool image = false; }` and `SetDocumentPreviews(std::vector<DocumentPreview>)`.
  - It is emitted as a top-level `documents` array only when called.
  - The editor sets it for every open shader and mesh document, snapshotted beside `SetInspector` (`EditorApp.cpp:3291`) before `m_documents.CloseAll()`. The runtime never sets it.
  - `kSchemaVersion` 12 -> 13 (`VerifyReport.hpp:230`) and `$script:ReportSchemaMax` (`golden-gate.ps1:424`) move in the same commit. T3 reads the field unchanged, and in T1 it has no UI consumer.

**Does NOT:** change the toolbar or preview-box copy (T3, s5.2); add an imported-mesh or per-node preview; or defer the scripted open to MainLoop. The deferral is rejected because it fixes only the scripted path.

**Tests.**
- **`PreviewStatusTest.cpp`** (new, pure), covering the precedence tables:
  - a completed compile with no seam reads `Ok`/`NoDevice`, never `Compiling`;
  - no compiler reads `CompilerUnavailable`;
  - errors with a bound image keep `image` true;
  - a frame failure, then recovery.
- **Headless documents** (`MeshDocumentTest.cpp:95` pattern; `ShaderEditorDocumentTest.cpp` device-less cases, `:1036` pattern):
  - a null `chromeGraph` reads `NoDevice` and does not latch;
  - flipping it non-null makes the next `Tick` attempt the vehicle exactly once;
  - no compiler service reads `CompilerUnavailable`;
  - a mesh-surface shader document reads `NotCompiledHere`.
- **`VerifyReportTest`:** `documents` absent unless set; schema 13; the ids.
- **Witnesses** (`ArcaneTests/src/EditorWitnessTest.cpp`, `[witness][gpu]`). E6 (`:379`) and E7 (`:464`) already exist.
  - **E8** (new, the E5 shape at `:303-340`) runs `--headless --backend dx12 --open-asset 7e5a0011-0011-4011-8011-000000000011` (`meshes/reference_cube.arcmesh`, generated) and asserts `documents[0].preview == "ready"`.
  - **E5** additionally asserts that its `ReferenceCubeMaterial` document (`reference_mesh.arcmat`) reads compile `not-compiled-here`.
  - The T3 graph fixture's `ready` check belongs to E9 (s5.1.11).
- **Desk:** an interactive open and an `--open-asset` open both preview.

### 3.3 Undo stack correctness

**Problem.** Six defects in `Arcane::CommandStack` (`ArcaneClient/src/Arcane/Edit`) and its editor wiring, and one behaviour that stays.
- **(a) Document edits mark the SCENE unsaved.**
  - Every document command lands on the shared stack, and each Push stamps a new id (`CommandStack.cpp:104-111`). `SceneSession::IsDirty` compares only `StateId()` (`ArcaneEditor/src/Scene/SceneSession.hpp:54-57`).
  - So a material, sprite, mesh or input-actions edit dirties the title and parks Exit/New/Open/Open Project behind the confirm modal.
  - It also makes Save All re-save the scene (`EditorAppFrame.cpp:2668-2672`), makes Play Standalone refuse, and skips the post-build reload (`EditorAppProject.cpp:3206`).
  - No test covers it; `EditorSceneSessionTest.cpp:55-58` edits components only.
- **(b) Menu Undo in Play.**
  - Ctrl+Z is gated on Play (`EditorAppFrame.cpp:874-879`) and on `!m_undo->InTransaction()` (`:913-915`, reasoning `:906-912`). Edit > Undo/Redo checks only `CanUndo`/`CanRedo` (`EditorPanels.cpp:189-196`). A menu Undo in Play restores an edit-mode registry memento against the Play registry, which breaks `RegistryStateCommand`'s Edit-only contract.
  - Documents receive `&*m_undo` once, at open (`EditorAppProject.cpp:83`). The "null = Play mode" comments (`SpriteDocument.cpp:127`, `MeshDocument.cpp:157`) describe wiring that does not exist.
  - `InputActionsEditorModel` stores a raw `CommandStack* commands_` set in its constructor (`InputActionsEditorModel.hpp:26`, `:123`) and calls `Undo()`/`Push` directly (`.cpp:359-369`).
- **(c) Inert steps.** A closed document's steps stay on the stack with their labels (`DocumentHost.cpp:144-159`), and Ctrl+Z on one visibly does nothing.
- **(d) Silent history loss.** `Clear()` (`CommandStack.cpp:147-155`, called only from `ClearSceneReferences`, `EditorAppScene.cpp:162`) leaves no trace in the UI.
- **(e) No memory bound.** A structural step holds up to two whole-registry blobs (`RegistryStateCommand.hpp:66-67`), and only the 100-step cap limits them. T5's deletes add file bytes on top.
- **(f) Hot reload.** `ComponentEditCommand::m_descriptor` (`ComponentEditCommand.hpp:56`) may go stale after a module reload, because the stack is not cleared on unload.
- **(g) Stays:** the stack is cleared on New/Open scene and on project switch, as UE resets on map load.

**Design.**
- **`ICommand` gains three virtuals** (ABI, s2.3), each defaulting to the safe direction:
  ```cpp
  virtual bool        AffectsScene() const { return true; }   // false: document/asset/file steps
  virtual bool        IsExpired()    const { return false; }  // true: nothing left to act on
  virtual std::size_t PayloadBytes() const { return 0; }      // bytes held (memory or spilled)
  ```
  - `AffectsScene() == false` for the seven document commands: `ParamEditCommand`, `MeshMaterialMetadataCommand`, `GraphEditCommand`, `PassListCommand`, `SpriteDataEditCommand`, `MeshDataEditCommand` and `DraftEditCommand`. T5's file operations are also false.
  - It stays true for `ComponentEditCommand`, `RegistryStateCommand` and every `Begin`/`Commit` transaction.
  - A `Transaction` records `affectsScene` = any part affects the scene. Its `expired` = it has no component snapshots and every command in it is expired.
- **Scene dirty tracks scene steps only.**
  - `CommandStack::SceneStateId()` returns the id of the topmost undo entry with `affectsScene`, or 0. `SceneSession::IsDirty`/`MarkSaved` use it instead of `StateId()`.
  - Undo back to the save point reads clean, and undoing a document step above a saved scene stays clean.
  - `TouchedSinceState` is unchanged, because document steps carry an empty `touched`.
- **Play barrier.**
  - One predicate, `[[nodiscard]] constexpr bool UndoBarred(bool playing) noexcept`, is called by `UndoMenuState`, the Ctrl+Z/Y path (`EditorAppFrame.cpp:874-879`) and the document resolver.
  - The pure `UndoMenuState(canUndo, playing, inTransaction, clearedReason) -> {enabled, label, tooltip}` drives the menu. Undo/Redo are disabled in Play ("Stop play mode to undo") and while `InTransaction()` ("Finish the current edit first"), using the tooltip pattern at `EditorPanels.cpp:124-132`.
  - Every document `Services::undo` becomes `std::function<Arcane::CommandStack*()>`, which returns `nullptr` when `UndoBarred(InPlayMode())`. That makes the "null = Play mode" contract true, matching the Inspector (`EditorPanels.cpp:2677`).
  - `InputActionsEditorModel` takes the same resolver in place of its constructor pointer. `ApplyEdit` resolves once per edit and `Undo`/`Redo` once per call; null means no push and no undo.
  - Entering Play first flushes every open document gesture (`DocumentHost::FlushGestures`, calling `EditGesture::ClosePending` per document; drafting pick, 9.28), so a step lands in Edit mode. During Play nothing is pushed.
- **Expired steps are skipped** (UE skips expired transactions).
  - A document command's `IsExpired()` is its weak anchor having expired.
  - Before acting, `Undo()`/`Redo()` discard expired entries from the top of their deque, releasing their payloads, then act on the first live entry. Discarding, rather than skipping in place, keeps the order sound.
  - `CanUndo`, `CanRedo`, `UndoLabel` and `RedoLabel` look past expired entries.
- **"Can't undo after: <reason>".**
  - `Clear()` becomes `Clear(std::string reason)` and drops `noexcept` (`CommandStack.cpp:147`; the string move can allocate). Every caller is updated, and `ClearedReason()` returns the last reason.
  - While `CanUndo()` is false and the reason is non-empty, the disabled item reads "Can't undo after: <reason>". This lasts until the next `Clear`.
  - `ClearSceneReferences` takes the reason from its callers:
    - `DoNewScene`: "New scene";
    - `DoOpenScene`: "Opened scene <stem>";
    - `ResetPerProjectState`: "Switched project";
    - the reload below: "Game module reloaded".
- **Byte budget and disk spill, generic over commands.**
  - **Limits.** `struct UndoLimits { std::size_t maxSteps; std::uint64_t byteBudget; std::uint64_t spillThreshold; };` goes through `CommandStack::SetLimits(UndoLimits)`, replacing the constructor depth. `maxSteps` 0 still clamps to 1 (`CommandStack.cpp:11`). The editor calls `SetLimits` whenever any `editor.undo.*` cvar changes.
  - **Eviction.** After every push or commit, the stack evicts from the oldest end while the step count exceeds `maxSteps` or the summed `PayloadBytes()` exceeds `byteBudget`. The top step is never evicted. `PayloadBytes()` counts a payload whether it is held in memory or spilled, so `byteBudgetMB` bounds RAM plus `Saved/UndoCache`.
  - **Payloads.** `Arcane::UndoPayload` (`ArcaneClient/src/Arcane/Edit/UndoPayload.hpp`) is a move-only, spillable byte blob. There are two ways to make one:
    - `CommandStack::MakePayload(std::vector<std::byte>&&)`;
    - `MakePayloadFromFile(const std::filesystem::path&)`, which streams in 1 MB chunks straight to the spill file when the file exceeds the threshold, and otherwise reads it into memory.
  - **Spill layout** (drafting pick, 9.28). A payload above `spillThreshold` moves to `<spill dir>/<step>.bin` and leaves memory. All spilled payloads of one step share that file at recorded offsets, and `Load()` reads one back on Undo/Redo.
    - The file is deleted with its last payload: on eviction, on `Clear`, or when redo is truncated.
  - **Spill directory.** The editor calls `SetSpillDirectory(<project>/Saved/UndoCache)` and wipes that directory at project open and close, which removes crash leftovers. With no project open it passes an empty path, meaning memory-only.
  - **Failures.** A failed spill write keeps the payload in memory and logs one WARN. A failed read no-ops the step and logs an ERROR naming the label and path.
  - **Adopters.** T1 moves `RegistryStateCommand`'s before/after blobs onto `UndoPayload`. `ComponentEditCommand` keeps its blobs in memory but reports `PayloadBytes()`. T5's deletes adopt `MakePayloadFromFile`.
- **Hot reload: investigated, with the fix decided in advance.**
  - `m_descriptor` points at `&m_components[id]`, which is address-stable (`Astra/Component/ComponentRegistry.hpp:227-229`). The slot's CONTENTS can go stale: `ReleaseModule` clears or restores it on unload (`:547-582`), and the reloaded image re-registers it. `Restore` uses the unversioned `deserialize` (`ComponentEditCommand.cpp:42-49`).
  - An `ArcaneTests` case on a `ComponentModule` fixture pushes a `ComponentEditCommand` for a module component, runs release + re-register, and checks three things:
    1. the same type returns to the same id;
    2. a type the rebuild removed leaves `Restore` a guarded no-op;
    3. a type whose layout changed does not deserialize the old blob.
  - If any of the three fails, the finding is confirmed. The editor then calls `Clear("Game module reloaded")` before the swap in `EditorApp::EndFrame` (`EditorAppFrame.cpp:4235`, the only place the editor reaches `PluginHost::Poll`) and on the post-build reload path. If all three pass, the test stays as the regression pin and no clear is added.

**Does NOT:** replace whole-registry mementos, key steps by GUID, or restore selection after undo (all entity-record arc, s10); stop clearing on scene open or project switch; split the stack per document (one global history, as UE); or add Play-mode redo of edit steps.

**Tests** (`ArcaneTests`).
- **`CommandStackTest`:**
  - `AffectsScene` defaults to true; a `Begin`/`Commit` transaction is a scene step.
  - `SceneStateId` ignores non-scene pushes and follows undo/redo.
  - Expired steps: Undo past one undoes the next live step and removes the expired entry; `CanUndo` is false when only expired entries remain; labels skip them.
  - `Clear(reason)` / `ClearedReason`.
  - `SetLimits`: step eviction; byte-budget eviction never takes the top step; spilled bytes count toward the budget.
  - Spill:
    - an above-threshold payload lands in `<step>.bin` and round-trips through `Load`;
    - two spilled payloads of one step share the file;
    - the file is gone after eviction, `Clear` and redo truncation;
    - `MakePayloadFromFile` spills above the threshold and stays in memory below it;
    - an empty directory means memory-only;
    - a failed write keeps the payload in memory.
- **`EditorSceneSessionTest`:** a document push leaves the scene clean; save, document step, undo stays clean; undoing a scene step past the save point reads dirty.
- **Play:**
  - `UndoBarred` gates the menu, the Ctrl+Z path and the document resolver.
  - `UndoMenuState` is disabled with its reason in Play and during an open transaction, and reads "Can't undo after: ..." after a clear.
  - The resolver returns null in Play, and a drag open at Play entry commits one step before `InPlayMode()` turns true (`EditorPlayModeTest`).
- **Input actions:** with the resolver returning null, `InputActionsEditorModel` pushes nothing and undoes nothing.
- **Hot reload:** the case above.
- **Desk:** a material edit leaves the scene title clean, and Save All does not re-save the scene. After closing a material document, Ctrl+Z reaches the previous scene step.

### 3.4 Astra "now" batch

**Problem.** Stale handles can revalidate, and child order is unstable.
- **Versions wrap 255 -> 1** (`Astra/include/Astra/Entity/Entity.hpp:114-121`, `NextEntityVersion`). All three destroy paths use it (`Entity/EntityManager.hpp:123`, `:151`, `:197`).
  - Recycling is LIFO (`Entity/EntityIDStack.hpp:44-51`), so one slot takes the churn. A stale handle revalidates after 255 reuses, about 4 s at one short-lived entity per frame.
  - `BasicEntity::NextVersion` already saturates (`Entity.hpp:85-96`), so there are two rules.
- **`Clear()` resets.** It empties the free list and sets `m_nextID = 0` (`EntityIDStack.hpp:140-144`). `EntityManager::Clear()` then clears the table (`EntityManager.hpp:236-240`; via `Registry.hpp:1276-1281`). Every pre-Clear handle revalidates at once.
- **Order is unstable.** `RelationshipGraph::RemoveParent` swap-and-pops (`Registry/RelationshipGraph.hpp:185`, `:194-203`), so an ordinary delete or reparent reorders Outliner siblings. Its `m_children[parent]` inserts a missing key (`:194`). `SetParent` always appends (`:179`).
- **Silent rejects.** `SetParent` rejects silently (`:166-172`), yet `Registry::SetParent` emits `ParentChanged` unconditionally (`Registry.hpp:1508-1516`).
- **Load gap.** Load accepts a recycled `nextVersion == 0`. `Allocate` would then hand out a version-0 handle, which is the CommandBuffer placeholder encoding (`Commands/CommandBuffer.hpp:1192-1203`).

**Design** (Astra `dev` first, s2.2).
- **Retire, don't wrap.**
  - `NextEntityVersion` returns `NULL_VERSION` ("retire") at `VERSION_MASK` for every width. `BasicEntity::NextVersion`'s saturating rule becomes the only rule.
  - In all three destroy paths a retiring id is destroyed in the table and NOT recycled. `EntityManager::GetRetiredCount()` counts them.
  - Save/Load need no change: a retired id is below `nextID`, not alive, and on no free list, and Load reproduces that (`EntityManager.hpp:565-566`).
  - When fresh ids run out (`EntityIDStack.hpp:55-58`), a one-time diagnostic is logged through Astra's log seam.
- **`Clear()` recycles.** It keeps the existing free list and `m_nextID`, appends every live id at its next version (retiring at the limit), then clears the table. Keeping the free list means ids freed before `Clear` are still reused.
- **Comment.** `CommandBuffer.hpp:1195-1196` ("recycling wraps 255->1") becomes "recycling never reaches 0; exhausted slots retire".
- **Ordered children.**
  - `RemoveParent` erases while preserving order and looks the parent up with `Find` (no insert).
  - `bool RelationshipGraph::SetParent(Entity child, Entity parent, std::size_t index = npos)`. `index` is a position in the parent's child list AFTER `child` is removed from it, clamped to that count.
    - Same parent with `npos`: a no-op returning `false`, with no version bump.
    - Same parent with an index: a reorder returning `true`, with a version bump.
    - Every rejection returns `false`.
  - `std::optional<std::size_t> GetChildIndex(Entity child) const`. There is no `MoveChild`.
- **Signal.** `bool Registry::SetParent(child, parent, index = npos)` forwards to the graph and returns its `bool`. It is not `[[nodiscard]]`, so existing callers need no churn. It emits `ParentChanged` only when the graph returned `true` AND the parent changed, so a reorder fires nothing.
- **`Registry::GetInstanceId() -> std::uint64_t`.** A process-unique id from a static atomic starting at 1, following the `ParallelCommandBuffer` idiom (`CommandBuffer.hpp:1784`, `:2116`).
  - It is assigned in the three constructors (`Registry.hpp:50`, `:60`, `:70`). Load (`:1840`) builds through `:70`.
  - The type is non-copyable (`:84-85`) with no move constructor, so an id is never duplicated.
  - It does not seed ticks or `StructureVersion`. Its only consumer in this phase is tests; the entity-record arc stores `(instanceId, Entity)`.
- **Load rejects a recycled `nextVersion == 0`** with `SerializationError::CorruptedData`, beside the existing recycled-id checks (`EntityManager.hpp:516-535`).
- **Arcane callers are safe.** None relies on "reparent to the same parent moves it last". The re-vendor needs no Arcane source change beyond the ABI bump.
  - `Edit::Reparent` skips the same parent (`EntityOps.cpp:189`).
  - `DeleteEntities`' heir is always a different parent (`:141-148`).
  - The loaders parent fresh entities (`SceneSerializer.hpp:622`, `EntityOps.cpp:649`, `:661`, `SceneAsset.hpp:253`).

**Deferred to the entity-record arc:** `IEntityVisitor`, `RemapEntities`, and the CommandBuffer component-payload remap. **Dropped:** the Load generation fence, `CloneEntities`, same-id resurrection, and GUIDs in Astra. LIFO order and the ignored `preferLocal` flag are unchanged.

**Tests.**
- **Astra `tests/`:**
  - Retirement:
    - One slot destroyed and created 255 times retires on the last destroy. The next create returns a fresh id, the old `(id, 1)` handle is never valid again, and `GetRetiredCount() == 1`.
    - The same holds on the batch destroy path (32 or more entities).
    - Save/Load keeps a retired id retired.
  - `Clear()`:
    - After `Clear()` and recreate, no pre-Clear handle is valid.
    - Ids freed before `Clear` are reused afterwards.
  - Load with a recycled `nextVersion == 0` returns `CorruptedData`.
  - Relationships:
    - `RemoveParent` of a middle child keeps sibling order, and on a parent with no children entry it inserts nothing.
    - `SetParent` with an index inserts at the post-removal position and clamps an out-of-range index.
    - Same-parent `npos` returns false with no version bump; a reorder returns true and fires no `ParentChanged`.
    - `GetChildIndex` reports the position.
    - A refused `Registry::SetParent` (self, cycle, dead) returns false and emits nothing.
  - Two registries, and a registry and its Load, have distinct instance ids.
  - Existing tests:
    - `EntityVersionWrap` (`tests/Entity/EntityTest.cpp:455`, which pins 255 -> 1 and 4095 -> 1) is rewritten to the retire rule in the same Astra commit: `NULL_VERSION` for both widths.
    - `EntityTest` `NextVersion` (`:104`), `VersionOverflow` (`:114`) and `VersionTracking` (`:392`) stay green, as does `EntityManagerTest` `VersionWraparound` (`:176`, 10 same-id recycles).
    - `tests/Registry/RelationshipGraphTest.cpp` and `RelationshipGraphSerializationTest.cpp` are grepped for swap-and-pop order expectations and updated.
- **Arcane:** the full ArcaneTests suite after the re-vendor. `EntityOpsTest`: deleting a middle child keeps its siblings' Outliner order. `VENDORED.txt` names the new Astra commit.

## 4. T2 -- shared foundations

T2 builds the pieces that T3-T6 draw with. It is **editor-only**: ArcaneEditor, ArcaneTests and one premake post-build block (4.8). It touches no Core or Client header, so there is **no ABI bump and no Aphelyon restamp**. Most items land with few callers or none. Each subsection says which call sites move in T2 and which move later.

**Build order inside T2** (drafting pick, 9.28): 4.4 goes first, because 4.2 and 4.3 open popups through it. Then 4.1, 4.2 and 4.3, then 4.5-4.9 in any order.

**Goldens.**
- `editor-material-page` changes because the material title line is deleted (4.3).
- Any slot that shows an entity page changes in three ways:
  - the entity name line is deleted (4.3);
  - the Color4 swatch moves to the left (4.1(c));
  - asset fields show a thumb plus the file name (4.2).
- Label ellipsis (4.1(e)) changes any slot that has a clipped label.
- The breadcrumb layout (4.3) may move `editor-input-doc`.
- Each slot the gate reports is diffed and attributed to exactly one T2 change, then re-blessed on its own. There is no batched bless before T4.

**New tunable:** `editor.graph.fitMaxZoom` (4.5). It is defined in 4.5 and registered as s2.4 describes.

### 4.1 PropertyGrid rows

**Problem.** Critique Inspector #1, #6, #8 and #9, Shader #8, and Mesh & Sprite #10. Also the cheap fixes "ellipsize clipped labels", "shader x reset" and "integer pixel fields".
- The row set has no VecN, colour, slider, override or reset row (`Widgets/PropertyGrid.hpp:105-123`).
- `FloatRow` hard-codes `"%.2f"` and takes no range (`PropertyGrid.cpp:223-232`).
- `IntRow` can only draw `InputInt` (`:213-221`).
- `FieldLabelCell` clips at the 40% split (`EditorWidgets.cpp:416-442`).
- `ReadOnlyRow` has neither ellipsis nor tooltip (`PropertyGrid.cpp:252-259`).
- The colour field (swatch, popup and four linear boxes) exists twice: `Panels/InspectorView.cpp:824-904` and `Documents/ShaderEditorDocument.cpp:5846-5901`.

**(a) One numeric draft for N components.**
- `PropertyGridState::NumericDraft` (`PropertyGrid.hpp:73`) becomes `{ double value[4]; double seed[4]; int count = 1; bool active = false; }`.
- The `NumericRow` template (`PropertyGrid.cpp:176-210`) is generalised to `count` components. Seeding, write-through and dropping the draft when inactive work as they do today.
- Commit rule: the row deactivates after an edit **and** at least one component differs from its seed.
- The key stays `GetID("##value")` under `PushID(label)`, so every existing row id and test id is unchanged.

**(b) Ranged and formatted numeric rows.**
```cpp
bool IntRow(const char* label, int& value,
            const std::optional<Astra::Range>& range = std::nullopt, const char* format = "%d");
bool FloatRow(const char* label, float& value, float speed = 0.01f,
              const std::optional<Astra::Range>& range = std::nullopt, const char* format = "%.2f");
bool SliderRow(const char* label, float& value, float min, float max, const char* format = "%.3f");
bool VecRow(const char* label, float* v, int n, float speed = 0.01f,
            const std::optional<Astra::Range>& range = std::nullopt, const char* format = "%.3f");
```
- **`IntRow`.** With no range it draws `InputInt` as today, so the input page's Priority row (`InputActionsInspectorPage.cpp:202`) does not move. With a range it draws `DragInt` with `ClampOnInput`.
- **`FloatRow`.** The `"%.2f"` default is kept. A range routes through `RangedDragFloat`, using `DragSpeedFor` and `ClampOnInput`.
- **`RangedDragFloat` / `RangedDragInt`** (`EditorWidgets.cpp:386-405`). Both gain a trailing `const char* format`, defaulting to `"%.3f"` / `"%d"`. The mesh page's existing calls are unaffected.
- **`SliderRow`** (drafting pick, 9.28, chosen over a ranged FloatRow). It is `SliderFloat(min, max, format)`, the widget material Float params use today (`ShaderEditorDocument.cpp:5838-5841`), with no clamp flags.
- **`VecRow`.** `IM_ASSERT(n >= 2 && n <= 4)`.
  - It draws through `AxisDragFloatN`, which gains `const std::optional<Astra::Range>& range = std::nullopt, const char* format = "%.3f"`. A ranged component passes min, max, format and `ClampOnInput` to its `DragFloat`.
  - The defaults reproduce today's call (`EditorWidgets.cpp:517`), so the entity page's Vec arms are unchanged.
- **Esc.** Every numeric row keeps today's mid-drag cancel: restore the seed, `ClearActiveID`, no commit (`PropertyGrid.cpp:193-199`). It also raises `cancelled` (see (d)).

**(c) The colour field, shared.**
```cpp
// Widgets/ColorPickerPopup.hpp -- the VALUE cell: no label, no undo, no draft
struct ColorValueResult { bool changed = false; ImGuiID popupId = 0; };
[[nodiscard]] ColorValueResult ColorValue(const char* id, float linear[4], float original[4], bool hdr = false);
// PropertyGrid
bool ColorRow(const char* label, float linear[4], ImGuiID* popupIdOut = nullptr, bool hdr = false);
```
- **`ColorValue` submits three items, in this order:**
  1. An sRGB-encoded `ColorSwatchButton`, square at frame height. A click latches `original` and calls `OpenPopup(ColorPopupId(id))`.
  2. If the popup is open, `ColorPopupBody(linear, original, hdr)`.
  3. Four LINEAR boxes, **last**: `ColorEdit4` with `Float|NoSmallPreview|NoPicker|NoOptions|DisplayRGB|InputRGB`, filling the rest of the cell.
- The boxes are therefore `LastItemData` when `ColorValue` returns. The popup's `End()` restores the parent's `LastItemData`, as `ShaderEditorDocument.cpp:5856-5866` documents.
- `original` is caller storage. One slot is enough, because only one colour popup is ever open.
- **`ColorRow`** is `FieldLabelCell` + `PushID(label)` + a 4-channel draft + `ColorValue("##value", ...)`.
  - The original value is stored in the new `PropertyGridState::colorOriginal[4]`.
  - The boxes behave like `VecRow`.
  - The popup writes through every frame. On the frame it closes, it commits if the value differs from `original`.
  - `*popupIdOut` feeds the caller's EditGesture popup pair.
  - `hdr` is passed to `ColorPopupBody` (T3 needs it for ConstColor).
- **Moves in T2:** the entity page's single-selection Color4 arm (`InspectorView.cpp:824-904`) calls `ColorValue`.
  - It keeps `BeginGestureIfActivated` right after the call, its popup pair on `result.popupId`, and its `InspectorState` latch.
  - Visible change: the swatch moves from the right of the boxes to the left (drafting pick, 9.28).
- **Moves in T3:** the material Color param.

**(d) Override cell, reset, and cancel reporting.** These are one-shot decorations for the next row, in the style of ImGui's `SetNextItem*`:
```cpp
struct RowDecor
{
    bool* overridden = nullptr;   // instance override cell (UE shape)
    bool  reset = false;          // base/default reset slot
    bool  resetActive = false;    // value differs from its default: button drawn; else the slot is empty
};
void SetNextRowDecor(const RowDecor& decor);
struct RowEvents { bool overrideToggled = false; bool resetClicked = false; bool cancelled = false; };
[[nodiscard]] RowEvents LastRowEvents() const;   // what the LAST row reported this frame
void ProbeItem(const char* label);               // probe seam, public for model-aware wrappers (4.2); no-op in production
```
- **Override.** The label cell draws `Checkbox("##override")` followed by the ellipsized label.
  - Tooltips (drafting pick, 9.28): "Overridden -- untick to inherit the parent's value" and "Inherited -- tick to override".
  - While `*overridden == false`, the value widget sits inside `BeginDisabled`. Inherited rows are therefore dimmed and read-only (Inspector #9).
  - A toggle writes `*overridden` and raises `overrideToggled`. The page routes the undo step, as `ShaderEditorDocument.cpp:5802-5827` does today.
- **Reset.** `ICON_LC_ROTATE_CCW "##reset"` with `SetItemTooltip("Reset to default")`, right-aligned in the value cell.
  - When `resetActive` is false, the slot is reserved but empty, so values stay aligned (drafting pick, 9.28).
  - `overridden` and `reset` are mutually exclusive (`IM_ASSERT`). On an instance the checkbox is the only override control, with no "x" (Shader #8).
- **Submission order (binding for every decoration):** decorations are submitted **before** the value widget.
  - The reset button is placed at the cell's right edge first.
  - The cursor then returns to the start of the cell, and the value is sized `-(resetW + ItemSpacing.x)`.
  - So the value widget is always `LastItemData` when the row returns. Tab order visiting reset before the value is accepted.

**(e) Label and ReadOnly ellipsis.**
- `bool FieldLabelCell(const std::string& label, bool dimmed, bool* truncated = nullptr)`.
  - It draws `EllipsisToWidth(label, GetContentRegionAvail().x)`, using the ASCII `"..."` the goldens already carry.
  - When the label is cut and hovered (`ForTooltip`), it shows the full label as a tooltip.
- The entity row's tail tooltip is last-wins (`InspectorView.cpp:1571-1592`). It reads `truncated` and puts the full label first, so the order becomes label, value, prose, identifier.
- `ReadOnlyRow` draws `EllipsisToWidth(text, avail)`, with `SetItemTooltip(full)` when cut.

**(f) The EditGesture-after-row contract.** This is written down in `PropertyGrid.hpp`.
1. **Activation.** After a numeric, vec, slider or colour row, the value widget is `LastItemData`.
   - So `EditGesture::BeginOnActivate(stack, st, label, onOpened)`, called right after the row, fires on the activation frame.
   - This includes grouped rows: `EndGroup` forwards the active id (`imgui.cpp:12477-12482`).
   - ColorRow's popup is bracketed separately, with `BeginOnPopupOpen` / `EndOnPopupClose` on `*popupIdOut`.
2. **Live write-through.** A live-preview page writes `value` to its live target whenever the two differ. That includes the seed that Esc restored.
3. **Close.** New helper in `Scene/EditGesture.hpp`: `void EndAfterRow(Arcane::CommandStack* stack, GestureState& st, bool cancelled)` (drafting pick, 9.28).
   - With `cancelled && stack` it calls `ClosePending(*stack, st)` (`EditGesture.hpp:261`); otherwise it calls `EndOnDeactivate(stack, st)`.
   - Pages pass `grid.LastRowEvents().cancelled`.

**Why (3) is needed (verified in the vendored ImGui).**
- Esc calls `ClearActiveID` *after* the widget has run.
- For a scalar widget, `SetActiveID` stamps `DeactivatedItemData.ElapseFrame = FrameCount` when `LastItemData` is the active item (`imgui.cpp:4805-4808`). `IsItemDeactivated` (`:6554-6560`) then reports the deactivation on that same frame.
- For a **grouped** row (VecRow's `AxisDragFloatN`, ColorRow's `ColorEdit4`), it does not:
  - `EndGroup` has already set `ImGuiItemStatusFlags_HasDeactivated` (`:12494-12497`);
  - `IsItemDeactivated` answers from those group flags;
  - the late record is dropped at the next `NewFrame` (`:5819-5820`).
- Consequences for each closing path:
  - `EndOnDeactivate` alone does not close the gesture at the row.
  - A scope that has an `EditGesture::ScopeGuard` closes it at scope end on the same frame, because `ShouldCloseAbandoned` sees ActiveId 0 (`EditGesture.cpp:145`).
  - `EndAfterRow` closes it at the row, before any later row on the page can activate. It is **required** for any adopter without a `ScopeGuard`.
- After the restore, `ClosePending` commits an unchanged value. The page's before == after guard (`ShaderEditorDocument.cpp:5954-5957`) or CommandStack's unchanged-snapshot drop then pushes nothing.

**Call sites that move in T2:**
- The entity Color4 arm moves onto `ColorValue`.
- Every `FieldLabelCell` caller (entity rows and all PropertyGrid rows) gets the ellipsis.
- In T3 the sprite, mesh, material and node pages adopt these rows. The `bracket` lambdas on the sprite and mesh pages then close through `EndAfterRow`.

**Not in scope:**
- Rows stay undo-agnostic (`PropertyGrid.hpp:11-13`).
- No `validate` hook.
- The entity Vec2/3/4 arms keep `AxisDragFloatN` / `MultiScalarRow`.
- No restyle of the colour boxes (Inspector #11).
- No reset shortcut.

**Tests.** These extend `ArcaneTests/src/PropertyGridTest.cpp` (harness at `:15-80`).
- **VecRow.**
  - A 3-frame drag commits once, with the final value.
  - `value` follows the drag every frame.
  - Releasing on the seed commits nothing.
  - Esc mid-drag restores every component, returns false and reports `cancelled`.
- **Ranged rows.**
  - `FloatRow` with range 0..1: a Ctrl+click entry of `5` commits `1`.
  - `"%.0f"`: a drag commits an integral value.
  - An unranged `IntRow` keeps its step buttons.
- **SliderRow:** one commit per gesture.
- **ColorRow.**
  - A box drag commits once.
  - Popup open, edit, close: one commit, `*popupIdOut != 0`, and `original` latched at open.
- **Decorations.**
  - With the override off, the value item carries `ImGuiItemFlags_Disabled` and a drag changes nothing.
  - A toggle raises `overrideToggled` exactly once; a reset click raises `resetClicked`.
  - For every row type, `ImGui::GetItemID()` after a decorated row equals the value widget's id.
- **Ellipsis.** A 60-character label in a 392 px window reports `truncated` and shows a tooltip on hover. The same holds for `ReadOnlyRow`.
- **Contract.** For every row type, using a real `Arcane::CommandStack`, a `GestureState` and a counting builder:
  - one step per drag, and none for a pure click;
  - Esc with a live target restores the seed and adds no step, and `EndAfterRow` empties the gesture slots on the Esc frame;
  - companion case: on VecRow and ColorRow, `EndOnDeactivate` alone leaves the gesture parked until the `ScopeGuard` runs.
- **Entity page** (`EditorInspectorVectorTest.cpp` harness shape). After the migration, Color4 still records one undo step per box drag and one per popup session.

### 4.2 The shared asset-reference field

**Problem.** Critique Inspector #5, Mesh & Sprite #4 and top-10 #4. Asset references behave four different ways today:
- **Sprite:** GUID text (`SpriteDocument.cpp:337-346`).
- **Mesh:** a drop target on dim text only, slot 0 only, plus an "x" (`MeshDocument.cpp:689-727`).
- **Material:** text, a drop target and an unsearchable picker (`ShaderEditorDocument.cpp:5598-5655`).
- **Entity:** a mount-path button, red "(missing)", a searchable picker, a read-only drop guard and texture-to-sprite minting (`InspectorView.cpp:1026-1330`).

**Design.** A new `Panels/AssetReferenceField.{hpp,cpp}`. It is model-aware, so it lives in `Panels/`; `Widgets/` stays model-free (`EditorWidgets.hpp:9-13`).
```cpp
struct AssetRefServices
{
    const AssetPanelModel* model = nullptr;                                          // EditorApp::m_assetModel
    std::function<const Arcane::Project*()> project;                                 // read at call time (project switch)
    std::function<std::uint64_t(const Arcane::Guid&)> resolveThumb;                  // AssetServices::resolveAssetThumb
    std::function<bool()> canReveal;                                                 // Asset Browser visible
    std::function<void(const Arcane::Guid&)> reveal;                                 // QUEUES a reveal (next frame)
    std::function<void(const Arcane::Guid&)> open;                                   // QUEUES an open (next frame)
    std::function<Arcane::Guid(const Arcane::Guid&)> mintSpriteForTexture;
    std::function<std::optional<std::string>(const Arcane::Guid&)> tombstoneName;   // filled in T5 (s7.12)
};
struct AssetRefArgs
{
    Arcane::Guid guid;
    int  kindFilter = -1;           // AssetKind, -1 = any
    int  surfaceFilter = -1;        // MaterialSurface, -1 = any
    bool readOnly = false;
    bool mixed = false;             // multi-selection disagrees: "--"
    bool allowTextureMint = false;  // a Texture DROPPED on a Sprite field mints the wrapping .arcsprite
    bool identityGuid = false;      // an Identity guid, not a reference
    bool ownTooltip = true;         // false: the caller composes the tooltip from the result
};
struct AssetRefEdit
{
    enum class Op : std::uint8_t { None, Set, Clear } op = Op::None;
    Arcane::Guid guid;              // Op::Set: the new target
    bool hovered = false;           // the name part, ForTooltip
    bool truncated = false;
    std::string fullText;           // untruncated display / its mount path
};
[[nodiscard]] AssetRefEdit AssetReferenceValue(const char* id, const AssetRefArgs&, const AssetRefServices&);
[[nodiscard]] AssetRefEdit AssetRefRow(PropertyGrid&, const char* label, const AssetRefArgs&, const AssetRefServices&);
```
`AssetRefRow` is `FieldLabelCell` + `PushID(label)` + `AssetReferenceValue("##value", ...)` + `grid.ProbeItem(label)`. The caller routes undo on `Set` and `Clear`.

**Pure halves** (tested without ImGui):
```cpp
struct AssetRefDisplay { std::string text, tooltip; bool dangling = false; bool browsable = false; AssetKind kind = AssetKind::Other; };
[[nodiscard]] AssetRefDisplay DescribeAssetRef(const AssetRefArgs&, const AssetRefServices&);
enum class AssetRefDropVerdict : std::uint8_t { Refuse, Set, MintSprite };
[[nodiscard]] AssetRefDropVerdict DecideAssetRefDrop(const AssetDragPayload&, const AssetRefArgs&);
[[nodiscard]] std::vector<const AssetPanelEntry*> AssetRefCandidates(const AssetPanelModel&, int kindFilter,
                                                                     int surfaceFilter, std::string_view search);
```

**`DescribeAssetRef`**

| Case | Display | Tooltip / notes |
|---|---|---|
| `identityGuid` | raw guid | Never resolved and not browsable (`IsIdentityGuidFieldName`, `InspectorView.cpp:1083-1086`). |
| `mixed` | `"--"` | |
| nil | `"(none)"` | |
| resolved via `project()->Registry().Resolve` | `model->Find(guid)->fileName`, or the last segment of the mount path if the model does not know the guid yet | Mount path; browsable. |
| unresolved (dangling) | `tombstoneName(guid)`, else the raw guid, plus `" (missing)"` in `Theme::kError` | Same #e65959 the entity arm spells as a literal today. |

**`DecideAssetRefDrop`**
- `readOnly` or `identityGuid` gives `Refuse`. This keeps the fix for an Identity::id drop that once wrote an asset guid into every selected entity (`InspectorView.cpp:1142-1155`).
- Kind match, or `kindFilter == -1`, gives `Set`.
- `kindFilter == Sprite` with a `Texture` payload and `allowTextureMint` gives `MintSprite`.
- Anything else gives `Refuse`.

**`AssetRefCandidates`**
- Reads `model.Entries()` (`AssetPanelModel.hpp:700`). It does not rebuild `BuildAssetEntries` every popup frame (`:140-160`).
- Filters by kind. Search matches name and mount path, case-insensitively (the `MatchesFilter` rule).
- Excludes a material only when its surface is confirmed to differ. An unknown surface stays in the list, with no pill (`InspectorView.cpp:1241-1283`).
- Sorted by name, then by mount path.
- Cached per popup session, keyed by `(entriesStamp, kind, surface, search)`.

**The cell, left to right** (drafting pick, 9.28: chevron picker, double-click opens).
- **Thumb.** 20 px, from `resolveThumb`. Falls back to the `KindIcon(kind)` glyph (`AssetPanelModel.hpp:278`).
- **Name.** An ellipsized `AllowDoubleClick` Selectable. A double-click calls `open(guid)` when the target is browsable; a single click does nothing.
- **Picker.** `ICON_LC_CHEVRON_DOWN`, tooltip "Pick an asset". Hidden when `readOnly`.
- **Browse-to.** `ICON_LC_LOCATE`, tooltip "Show in Asset Browser". Shown when the target is browsable. If `canReveal()` returns false, the button is disabled with tooltip "Show in Asset Browser (the Asset Browser is closed)".
- **Clear.** `ICON_LC_X`, tooltip "Clear reference". Shown when `!readOnly && (guid valid || mixed)`.
- **Tooltip.** With `ownTooltip`, the cell draws `DescribeAssetRef`'s tooltip. Otherwise it fills `hovered`, `truncated` and `fullText` for the caller.
- **Drop target.** The whole cell, via `BeginDragDropTargetCustom(cellRect, GetID("##drop"))` accepting `kAssetDragType` (`AssetPanelModel.hpp:81-86`).
  - A read-only cell opens no drop target.
  - `MintSprite` calls `mintSpriteForTexture` outside the caller's undo, as today (`InspectorView.cpp:1163-1169`). An invalid result does nothing.

**Picker popup.** `"##assetpick"`, opened through `BeginPopupBelow` (4.4) and at least as wide as the cell. It contains:
- a "Search..." box, focused and cleared when the popup appears (one file-local buffer, since only one picker is open at a time);
- a `"(none)"` row, which returns `Clear`;
- the candidates as `RowWithThumb` rows, with `MaterialSurfacePillText` pills on materials; a click returns `Set`.

With no project open, the popup reads "No project open".

**Ownership and wiring.**
- `EditorApp` owns one `m_assetRefServices`, filled next to `m_inspectorServices` (`EditorApp.cpp:991-1001`).
- `reveal` and `open` do not act immediately. They write `m_assetPageActions.revealInBrowse` and a new `AssetPanelActions::openAsset` guid.
- `ConsumeAssetPanelActions` acts on them the next frame (`EditorAppFrame.cpp:2415-2419`):
  - a reveal goes through `RevealAssetInBrowser` + `FocusDockTab` (`:2909-2914`);
  - an open goes through `OpenAssetRow(*m_assetModel.Find(g), proj, m_documents, actions)`.
- The delay is needed because opening mid-draw would mutate DocumentHost's document list while a page from that list is being drawn.
- `InspectorServices` (`EditorPanels.hpp:449-466`) gains `const AssetRefServices* assetRefs`. It loses `mintSpriteForTexture` and `assetModel`, whose only readers are the replaced arm (`InspectorView.cpp:1173-1175`, `:1237-1239`).
- `DocServices` (`ShaderEditorDocument.hpp:90`), `MeshDocument::Services` and `SpriteDocument::Services` gain `const AssetRefServices* assetRefs = nullptr`, set in their factories.
  - The pointer names an app-lifetime member whose callables read state at call time.
  - A document created during a boot stage (T1's `--open-asset` case) is therefore not stale.
- **Null services** (headless tests):
  - the kind glyph instead of a thumb;
  - the raw guid as text, never flagged as dangling;
  - the picker reads "No project open";
  - no browse-to, open or mint;
  - `Set` drops still work.

**Entity-page arm migration** (default, Q4a). `InspectorView.cpp:1026-1330` becomes a single `AssetReferenceValue("##assetref", args, *services->assetRefs)` call with these args:
- `kindFilter = AssetKindFilterForFieldName(rawName)`;
- `surfaceFilter = MaterialSurfaceFilterForComponent(typeName)` for material fields, else -1;
- `readOnly`, and `mixed = Multi() && MixedFor(f).Any()`;
- `allowTextureMint = true`, `identityGuid = IsIdentityGuidFieldName(rawName)`;
- `ownTooltip = false`.

The surrounding row code changes as follows:
- `Set` and `Clear` go through `ApplyGuidImmediate`, as today.
- `hovered` feeds the row's `hovered`. When the name is truncated, `fullText` feeds `tooltipValue`.
- The row's `BeginDisabled` wrap stays.

Visible changes on the entity page:
- The row shows thumb + file name; the mount path moves into the tooltip.
- The picker opens from the chevron.
- Picker rows have thumbnails.

**Not in scope:**
- No surface check on drop; drops stay kind-only (`InspectorView.cpp:1160`).
- No multi-guid payloads.
- No preview tooltip on candidates.
- No file operations from the cell (T5 only adds `tombstoneName`).
- `CreateAssetDialog`'s parent picker and the input page's control picker are not migrated.
- The sprite, mesh and material pages adopt `AssetRefRow` in T3.

**Tests.** New file `ArcaneTests/src/AssetReferenceFieldTest.cpp`, in the PropertyGridTest harness shape, with a stub model of three entries.
- **`DecideAssetRefDrop`:** match, mismatch, any-kind, read-only, identity, texture-on-sprite with and without mint, texture-on-material.
- **`AssetRefCandidates`:** kind filter; confirmed-surface exclusion; unknown surface kept; name-then-path order; search on name and on path.
- **`DescribeAssetRef`:** each row of the table.
- **UI.**
  - The chevron opens the popup below the cell (popup `Pos.y >= cell max.y`).
  - `ActivateItemByID` on a candidate returns `Set` once; the clear button returns `Clear`.
  - A read-only cell submits neither chevron nor clear.
  - A harness drag (`BeginDragDropSource` in the same frame) onto a read-only cell is refused.
  - Double-clicking the name calls `open` once; browse-to calls `reveal`.
- **Entity page** (`EditorInspectorVectorTest.cpp` shape).
  - Setting a `MeshRenderer` material through the picker is one undo step, and undo restores it.
  - Identity::id refuses a texture drop.

### 4.3 Breadcrumb clipping; no repeated titles

**Problem.** Critique Inspector #4 and Shell #10.
- The crumbs are drawn in a `##crumbs` child that is scrolled to its end, with no ellipsis (`Panels/InspectorWindows.cpp:265`, `:287`). The head gets cut mid-word ("ene > MeshCube").
- Row 1 reserves only 120 px for the crumbs (`kInspectorHeaderMinCrumbWidth`, `InspectorWindows.hpp:42`). "Scene > MeshCube" (~145 px) stays on row 1 and loses its head.
- The subject is named up to three times:
  - the entity name line (`Panels/EditorPanels.cpp:2378-2384`);
  - the material title + "(Instance)" (`ShaderEditorDocument.cpp:2069-2082`);
  - the crumb.

**Design.**
- **Natural width.** `InspectorHeaderMetrics` gains `float crumbsNatural = 0.0f;` (0 = unknown).
  - `DrawHeader` measures it before layout as Σ(`CalcTextSize(label).x + 2·FramePadding.x`) + (n-1)·(chevron + 2·`ItemSpacing.x`).
  - The same measure is used when drawing.
- **Row 1.** `LayoutInspectorHeader` (`InspectorWindows.cpp:299-313`) reserves `crumbsNatural > 0 ? crumbsNatural : kInspectorHeaderMinCrumbWidth` for the crumbs.
  - Everything else is unchanged: the combo gives way before the pin, and the pin is never clipped.
  - The existing cases (`EditorInspectorHostTest.cpp:935-967`) leave the field at 0 and still pass.
- **Own-row fit.** A new pure function: `CrumbFit FitCrumbs(std::span<const float> widths, float chevron, float overflowButton, float avail)` -> `{ std::size_t firstShown; bool overflow; float leafMax; }`.
  - If everything fits: `{0, false, widths.back()}`.
  - Otherwise head crumbs are hidden behind an overflow button until button + chevron + the remaining crumbs fit. **The leaf is never hidden.**
  - If the leaf alone still does not fit, `leafMax` is whatever remains after the button and chevron, floored at the width of `"..."`.
- **Drawing.**
  - The child stays as a clip safety net. The `SetScrollX` hack (`:287`) is deleted.
  - When the crumbs fit on row 1, they draw as today.
  - On their own row:
    - An `ICON_LC_ELLIPSIS` SmallButton (tooltip "Show the hidden levels") opens `"##crumbmore"` through `BeginPopupBelow` (4.4).
    - The popup lists the hidden crumbs head-first. Each Selectable performs its crumb's action: `select` when unpinned, `repinKey` when pinned. Crumbs with no key are disabled while pinned, the same branch the buttons use (`:270-282`).
    - The leaf is drawn as `EllipsisToWidth(label, leafMax - 2·FramePadding.x)`, with the full label as `SetItemTooltip` when cut.
- **The model is unchanged.** `InspectorCrumbText` (`InspectorSource.hpp:72`) still feeds the full trail to history labels and to the verify report's `inspector.breadcrumb`. E5's check (`EditorWitnessTest.cpp:332`) is unaffected.
- **No repeated titles.** Rule for every page: the header names the subject, and the body starts at its first Section.
  - Delete `EditorPanels.cpp:2378-2384` (the name line and Separator). The leaf crumb already carries the name and `" (+N)"` (`Panels/SceneInspectorSource.cpp:59-62`).
  - Delete `ShaderEditorDocument.cpp:2069-2082`. This section owns that deletion; s5.3 points here. For an instance, the material crumb label becomes `m_title + " (Instance)"`, matching the window label (`:909`) (drafting pick, 9.28).
  - T3's node-page crumbs follow the same rule.

**Not in scope:** the filter face and the "Inspector - All but Assets" title (the rest of Shell #10); crumb icons (Mesh & Sprite #11); the pinned header (T3).

**Tests** (`EditorInspectorHostTest.cpp`).
- **Layout.** With 1080p main-Inspector metrics (avail 380, arrows 56, combo 130, pin 24): `crumbsNatural` 145 goes to its own row; 60 stays on row 1.
- **`FitCrumbs` table:** everything fits; one head hidden; two heads hidden; leaf ellipsized; a single crumb never shows the overflow button.
- **UI.** An instance window at 392×330, drawn through `DrawInspectorWindows` with crumbs `{"Scene", "ReferenceCubeMaterialWithALongName"}`:
  - every crumb rect lies inside the header row;
  - the overflow Selectable fires the head crumb's `select`;
  - the leaf tooltip carries the full text.

### 4.4 `BeginPopupBelow`

**Problem.** Critique Shell #7 and Assets #7, plus the cheap fix "popups anchored". A bare `OpenPopup` + `BeginPopup` opens at the mouse and can cover its own button. Affected sites:
- Create (`Panels/AssetBrowserPanel.cpp:901-903` -> `AssetPanelCommon.cpp:90-95`);
- the Play caret (`EditorPanels.cpp:843-844`, `:855`);
- Input Actions "+ Add" (`Documents/InputActionsDocumentWidgets.cpp:197-198`);
- the entity asset picker (`InspectorView.cpp:1136`, `:1201`);
- input "Pick..." (`InputActionsInspectorPage.cpp:302`, `:385`).

The only anchored popup today is the viewport gear (`EditorPanels.cpp:1337-1339`). It sets an explicit position and so bypasses ImGui's placement logic.

**Design** (`Widgets/EditorWidgets.{hpp,cpp}`):
```cpp
struct PopupAnchor { ImVec2 min, max; };
[[nodiscard]] PopupAnchor LastItemAnchor();                                    // rect of the item just submitted
[[nodiscard]] bool BeginPopupBelow(const char* id, const PopupAnchor& anchor,
                                   float minWidth = 0.0f, ImGuiWindowFlags flags = 0);   // EndPopup() as usual
```
The body mirrors `BeginComboPopup` (`imgui_widgets.cpp:2059-2069`):
1. `popupId = GetID(id)`. If the popup is not open, return `BeginPopup(id, flags)`; this also consumes NextWindowData.
2. Find the popup window by its name. The name is formatted exactly as `BeginPopupEx` does: `ImFormatString(name, IM_COUNTOF(name), "##Popup_%08x", popupId)` (`imgui.cpp:13155`). If the window exists and `WasActive`:
   - set its `AutoPosLastDirection = ImGuiDir_Down`;
   - `pos = FindBestWindowPosForPopupEx(anchor bottom-left, CalcWindowNextAutoFitSize(w), &dir, GetPopupAllowedExtentRect(w), anchorRect, ImGuiPopupPositionPolicy_ComboBox)` (`imgui.cpp:13377`, `:7129`, `:13452`);
   - `SetNextWindowPos(pos)`.
3. On the first frame, before the size is measured (auto-fit hides that frame): `SetNextWindowPos(anchor bottom-left)`.
4. If `minWidth > 0`: `SetNextWindowSizeConstraints({minWidth, 0}, {FLT_MAX, FLT_MAX})`.
5. `BeginPopup(id, flags)`.

`FindWindowByName`, `FindBestWindowPosForPopupEx`, `CalcWindowNextAutoFitSize`, `GetPopupAllowedExtentRect` and `ImGuiPopupPositionPolicy_ComboBox` are `imgui_internal.h` APIs. `EditorWidgets.cpp` already includes that header (`:7`), and `EditorWidgets.hpp` stays free of internals.

The resulting placement matches a combo: below the anchor and left-aligned with it, flipped above when there is no room below, clamped to the work area.

**Sites that move in T2.** Each anchor is captured right after the item that opens the popup.

| Site | Anchor | Signature change |
|---|---|---|
| Create | Create button | `DrawCreateMenu(actions)` -> `DrawCreateMenu(actions, const PopupAnchor&)` (`AssetPanelCommon.hpp:180`, one caller) |
| Play caret | caret button | none |
| Input Actions "+ Add" | Add button | none |
| input "Pick..." | ButtonRow's last item (Pick) | `DrawPicker(id)` -> `DrawPicker(id, const PopupAnchor&)` |
| asset picker, crumb overflow | the cell / the overflow button | internal to 4.2 and 4.3 |

**Not in scope:** the viewport gear keeps its right-aligned pivot; context menus still open at the mouse; combos are unchanged.

**Tests.** New file `ArcaneTests/src/EditorWidgetsTest.cpp`: a bare ImGui context at 800×600 (the `ImGuiTest.cpp` shape), running three frames after the open.
- A button at y≈20: popup `Pos.y >= anchor.max.y`, and `Pos.x` within 1 px of `anchor.min.x`.
- A button at y≈570 with a 200 px popup: `Pos.y + Size.y <= anchor.min.y` (flipped above).
- `minWidth` is honoured.
- The popup stack is balanced.

### 4.5 `GraphFit`

**Problem.** Critique Shader #6 and Assets #5, plus the cheap fix "frame graphs on open".
- The shader editor seeds node positions (`ShaderEditorDocument.cpp:3800-3810`; pass canvas `m_passCanvasSeeded`, `:2576-2612`) but never frames them.
- `ed::NavigateToContent` (`imgui_node_editor_api.cpp:569-572`) adds a margin but has no zoom cap. A small graph can magnify into the blurred-glyph range described at `AssetGraphPanel.cpp:1320-1328`.

**Design.** `Widgets/GraphFit.{hpp,cpp}`. Only the `.cpp` includes `imgui_node_editor_internal.h`.
```cpp
struct GraphRect { ImVec2 min, max; };
// PURE: resize `content` about its centre so fitting it into `viewPx` lands in [kZoomLevels[0], maxZoom].
[[nodiscard]] GraphRect ComputeGraphFitRect(const GraphRect& content, ImVec2 viewPx, float maxZoom);
// Between ed::Begin/ed::End of the CURRENT editor: fit every node. false = nothing to fit (no nodes,
// empty bounds, zero-size view) and nothing changed.
bool GraphFitToContent(float maxZoom, float durationSeconds = 0.0f);
```
- **Maths.** `fit = min(viewW / cw, viewH / ch)`.
  - If `fit > maxZoom`, each axis grows to `max(content, view / maxZoom)` about the content centre.
  - If `fit < 0.1` (the zoom table's first stop, `GraphZoomLevels.hpp`), each axis shrinks to `min(content, view / 0.1)` about the centre. A huge graph is then framed at its centre instead of zooming below the wheel's range.
  - Zero-extent axes are handled without dividing by zero.
- **Inputs.**
  - `content` is `EditorContext::GetContentBounds()` (`imgui_node_editor_internal.h:1427`), via `reinterpret_cast<ax::NodeEditor::Detail::EditorContext*>(ed::GetCurrentEditor())`.
  - `view` is `ed::GetScreenSize()` (`imgui_node_editor.h:427`).
- **Navigation.** `ctx->NavigateTo(rect, /*zoomIn*/ true, duration)` (`internal.h:1434`).
  - `WithMargin` adds `c_NavigationZoomMargin` (`imgui_node_editor.cpp:144`, applied at `:3543`). The margin only lowers zoom, so the cap holds. At the floor the fit can land slightly under 0.1.
  - The result is not snapped to the zoom table: `NavigateAction::SetViewRect` takes `CalcCenterView`'s scale as-is (`:3635-3640`). The next wheel step snaps to the nearest stop through `MatchZoom` (`:3673`).
  - Duration 0 finishes immediately.
- **Selection is never touched.** The fit works from content bounds and never does a select-all. T3's node-page selection mirror depends on this.
- **Cvar.** `editor.graph.fitMaxZoom`, registered with `ARC_CVAR_RANGED` (module `"editor"`).
  - Archive, Float32, default 1.0, min 0.1, max 2.0 (the zoom table's range; with the 0.1 fit floor and no snapping, drafting pick, 9.28).
  - Help text: "Largest zoom a graph's frame-to-fit may pick (1.0 = never magnify)."

**Applied in T2: the shader editor.**
- `m_fitPending`, and a twin flag for the pass canvas, are set on any frame that seeds positions.
- They are consumed on the next canvas draw, once nodes have measured sizes (`NodeCulled` exempts unmeasured nodes, `:5194-5213`), with `GraphFitToContent(cvar, 0.0f)`.
- **F keeps its meaning** (`:3895-3903`, `:3042-3049`): with a selection, `NavigateToSelection(true)`; otherwise `NavigateToContent()`. F is uncapped.

**Applied in T4: the Asset Graph** (drafting pick, 9.28: it lands with T4's canvas shrink). See s6.9. The fit is armed in the lazy-create branch (`AssetGraphPanel.cpp:1312`) and in the rebuild caused by a focus or kind-filter change. It is never armed by a rebuild caused only by `entriesStamp`. Its tests live in s11 T4.

**Not in scope:** no persisted view; no toolbar Frame button or zoom readout (Shader #5); no change to the Asset Graph's centre-on-external-selection (`:2106-2119`).

**Tests.**
- **`ComputeGraphFitRect`** (view 1000×500):
  - 200×60 content with cap 1.0 gives a rect of at least 1000×500 (zoom 1.0);
  - 4000×1000 gives the content unchanged (zoom 0.25);
  - 20000×1000 gives 10000×1000 (zoom 0.1);
  - the centre is preserved;
  - zero-width content gives a finite rect.
- **Headless** (`GraphCanvasHeadlessTest.cpp` shape): open a shader document and run three frames.
  - Every node's screen rect lies inside the canvas.
  - `ed::GetCurrentZoom() <= cap`.
  - `ed::GetSelectedObjectCount()` is unchanged.

### 4.6 One OS-shell path helper

**Problem.**
- `AssetPathAction` is file-local (`App/EditorAppFrame.cpp:174-214`). As a result, `CrashReportDocument` duplicates its show-in-explorer branch, and says so in a comment (`Documents/CrashReportDocument.cpp:26-43`).
- Crash #5 (the Show .txt/.dmp buttons open Explorer instead of the file) and T6's File locators both need an "open this path" call.
- Raw Win32 calls sit in two translation units.

**Design** (drafting pick, 9.28 for the location). `ArcaneEditor/src/Project/OsShell.{hpp,cpp}`, next to IdeLaunch and RuntimeLaunch and following their split between pure logic and OS calls:
```cpp
namespace Arcane::Editor::OsShell
{
    enum class ShellResult : std::uint8_t { Ok, NotFound, NoHandler, Failed, Unsupported };
    ShellResult ShellOpen(const std::filesystem::path& path);        // OS default handler ("open")
    ShellResult ShowInExplorer(const std::filesystem::path& path);   // explorer /select: folder with the item selected
    ShellResult OpenAsText(const std::filesystem::path& path);       // "open"; no association -> Open With ("openas")
    // pure halves, [editor]-tested
    [[nodiscard]] std::wstring ExplorerSelectArgs(const std::filesystem::path& path);   // /select,"<native path>"
    [[nodiscard]] ShellResult  ClassifyShellExecute(std::intptr_t code);                 // >32 Ok; 2,3 NotFound; 31 NoHandler; else Failed
    [[nodiscard]] std::string_view Describe(ShellResult r);                              // WARN / tooltip wording
}
```
- Each call checks `std::filesystem::exists` first and returns `NotFound` without touching the shell.
- All Win32 (`ShellExecuteW`) sits behind one `#ifdef _WIN32` in `OsShell.cpp`. Other platforms return `Unsupported`.
- Behaviour matches today's code:
  - `ShowInExplorer` runs `explorer.exe /select,"..."` for both files and directories;
  - `OpenAsText` is open followed by openas (`EditorAppFrame.cpp:196-213`);
  - callers keep their own WARN text and get the wording from `Describe`.

**Moves in T2.**
- `AssetPathAction`'s two shell branches call `OsShell`. `AssetPathAction` remains the guid-to-path resolver plus Copy Path.
- `CrashReportDocument`'s own `ShowInExplorer` is deleted. `siblingButton` (`:255-278`) calls `OsShell::ShowInExplorer`.
- Both translation units drop `<shellapi.h>`, and `<windows.h>` wherever nothing else needs it.

**Later consumers:**
- T5 adds the Recycle Bin call (`IFileOperation`) to this unit.
- T6: the crash viewer's Open buttons use `ShellOpen`, and Problems routes use `ShowInExplorer` / `OpenAsText`.

**Not in scope:** IDE launching (stays in IdeLaunch); the standalone reporter's own Win32 calls (`ArcaneCrashReporter/src/ReporterMain.cpp`, a Core-only exe); URL opening; non-Windows implementations.

**Tests.**
- New file `ArcaneTests/src/OsShellTest.cpp` [editor]:
  - `ExplorerSelectArgs` quoting, with spaces and with forward slashes converted to native;
  - the `ClassifyShellExecute` table;
  - a missing path gives `NotFound` from all three calls.
- **Desk:** Show in Explorer from the asset page and from a crash document; Open as text on `Player.arcinput`.

### 4.7 Link-row widget

**Problem.** Critique Console & Problems #3 and Crash #4.
- Every Problems row highlights on hover, but only rows with a locator respond to a click (`Panels/ProblemsPanel.cpp:86-93`).
- Crash frames cannot be clicked.
- `ImGui::TextLink` (`imgui.h:653`, `imgui_widgets.cpp:1525-1575`) is unused, although `ImGuiCol_TextLink` is already themed (`EditorTheme.hpp:228`, = `kCheck`).

**Design** (`Widgets/EditorWidgets.{hpp,cpp}`):
```cpp
// Inline link. live: ImGui::TextLink (hand cursor, underline, TextLink colour).
// Not live: same text in TextDisabled, no underline, arrow cursor, never returns true.
[[nodiscard]] bool LinkText(const char* label, bool live = true);
struct LinkRowResult { bool clicked = false; bool hovered = false; };
// Full-width row. live: Selectable hover highlight, hand cursor, text in ImGuiCol_TextLink, underlined while hovered.
// Not live: NO hover highlight (Header/HeaderHovered/HeaderActive pushed to Theme::kNone), ambient ImGuiCol_Text,
// arrow cursor, clicked never true.
[[nodiscard]] LinkRowResult LinkRow(const char* id, std::string_view text, bool live,
                                    const char* leadIcon = nullptr, ImU32 leadColor = 0);
```
- In both functions the hit item remains `LastItemData`. T6 can then attach `SetItemTooltip` and `BeginPopupContextItem`:
  - Problems: Open / Show in Explorer / Copy path;
  - crash viewer: Copy line.
- A non-live `LinkText` is an `InvisibleButton` sized to the text, with a stable id derived from the label, and the text drawn through the draw list. Tooltips (e.g. "File not found on this machine") and context menus still attach to it.
- `leadIcon` is drawn in `leadColor` before the text, so Problems keeps its severity colour on the icon.

**Consumers:** T6 only (Problems rows, crash-frame `[file:line]` links).

**Not in scope:** routing (`IsRoutable` and `RouteLocator` belong to T6); wrapped or multi-line rows.

**Tests** (`EditorWidgetsTest.cpp`).
- Live `LinkText`: a click returns true once.
- Non-live `LinkText`: returns false, and `ImGui::GetMouseCursor() != ImGuiMouseCursor_Hand` after a hover frame.
- Live `LinkRow`: hovering sets the hand cursor, and a click is reported.
- Non-live `LinkRow`: no `ImGuiCol_HeaderHovered` vertices in the draw list (the `errorDrawn` scan idiom from `PropertyGridTest.cpp`), and `clicked` is false.
- `BeginPopupContextItem` right after either widget opens on right-click.

### 4.8 Monospace font

**Problem.**
- The editor has no monospace face (`Widgets/EditorFonts.hpp:11-16`).
- The reporter borrows system Consolas (`ArcaneCrashReporter/src/ReporterWindow.cpp:551-562`).
- The input page notes the face as owed (`InputActionsInspectorPage.cpp:295-296`).
- T6's frame rows and the later in-editor text editor both need a mono face.

**Design** (default: ship one).
- Ship **JetBrains Mono Regular** (SIL OFL 1.1) as `data/font/jetbrainsmono/JetBrainsMono-Regular.ttf` plus `OFL.txt`, laid out like `inter/` and `roboto/` (drafting pick, 9.28; Cascadia Mono is the OFL alternative).
- `EditorFontSet` gains `ImFont* mono = nullptr;`.
  - `InstallEditorFonts` loads it after Roboto, so `Fonts[0]` stays Inter.
  - It loads through `AddFaceWithIcons` at the UI size, so `ICON_LC_*` glyphs work in mono rows. `GlyphExcludeRanges` protects the icon block, as it does for Inter.
- The `ArcaneEditor` post-build (`premake5.lua:1100-1107`) gains the matching `{MKDIR}` + `{COPYFILE}`. The solution must be regenerated.
- `struct MonoFont { MonoFont(); ~MonoFont(); };` in `EditorFonts.hpp`:
  - when `GetEditorFonts().mono` is non-null, it pushes it at the current size via `PushFont(font, 0.0f)` (`imgui.h:531`);
  - otherwise it pushes nothing, since headless tests install no fonts.

**Not in scope:** moving existing surfaces onto the face (the Console columns of Console #10 and the input page's Path row stay proportional); bold or italic cuts; reporter changes.

**Tests.**
- Device-less: `MonoFont` with no fonts installed leaves `g.FontStack` unchanged.
- Gate: the editor lane logs contain no `Arcane Editor: failed to load font` line.
- Desk: checked through T6's first consumer.

### 4.9 `IconToggle` helper

This section is the single definition of the helper. s6.2 only adopts it.

**Problem.** Critique Shell #1, plus a code bug.
- Three copies of the "lit" lambda push only `ImGuiCol_Button = ButtonActive`:
  - transport (`EditorPanels.cpp:667-676`);
  - Play caret (`:841-842`);
  - viewport overlay (`:1252-1260`).
- Hovering a lit toggle paints `ButtonHovered` #3d3d3d, which is darker than the lit `kButtonActive` #4b4b4b (`EditorTheme.hpp:80-81`). The "on" state vanishes on hover.
- Pressing an unlit toggle looks the same as "on".
- `SegmentedStrip` (`EditorWidgets.cpp:711`) is dead code.

**Design** (`Widgets/EditorWidgets.{hpp,cpp}` + `EditorTheme.hpp`):
```cpp
void PushToggleOnColors();   // pushes Button, ButtonHovered AND ButtonActive = kToggleOn / kToggleOnHovered / kToggleOnActive
void PopToggleOnColors();    // pops all three
[[nodiscard]] bool IconToggle(const char* label, bool on);   // Button(label), inside the push when on; draws NO tooltip
```
- **Tokens** (drafting pick, 9.28). Three new tokens: `Theme::kToggleOn`, `kToggleOnHovered`, `kToggleOnActive`.
  - In T2 all three alias `kButtonActive`. The only pixel change is that a lit toggle no longer darkens on hover.
  - T4 (s6.1) re-points them to `kAccent` / `kAccentHovered` / `kAccentActive`.
- **`IconToggle`.** `label` carries the icon and the `##` id. When `on` is false it pushes nothing.
  - The button stays `LastItemData`, so callers attach their own tooltips.
  - For example, the viewport's `AllowWhenDisabled` tooltip lambda (`EditorPanels.cpp:1235-1243`) draws at full alpha inside a disabled scope through `g.DisabledAlphaBackup` (`imgui_internal.h:2709`).

**Call sites:** none in T2. T4 (s6.2) replaces the three lambdas, deletes `SegmentedStrip`, and moves the Input Actions Preview's amber "on" to the accent. s8.2's `SeverityToggle` builds on `PushToggleOnColors`.

**Not in scope:** the accent colour, the Play frame and the theme contrast test (all T4).

**Tests** (`EditorWidgetsTest.cpp`; there is no separate test file).
- Between `PushToggleOnColors()` and `PopToggleOnColors()`, `GetStyleColorVec4` returns the `kToggleOn*` tokens for `Button`, `ButtonHovered` and `ButtonActive`, and the pop restores the previous colours.
- `IconToggle(label, false)` draws with the ambient Button colours.
- `IconToggle` leaves the colour and style-var stacks balanced, lit or not, disabled or not.
- In T4 the same cases run against the re-pointed tokens (s11 T4).

## 5. T3 -- Inspector pages

T3 adds the shader **node page** that the filters spec promised (`docs/superpowers/specs/2026-09-29-inspector-filters-design.md` s6a: "node pages are the next phase"). It also makes preview status honest, ports the material, sprite and mesh pages onto `PropertyGrid`, fits the asset page into the Assets-only Inspector at 1080p, and pins the Inspector header.

T3 uses T2 as it stands and redefines none of it. The pieces it uses are `VecRow`, `ColorRow`, `SliderRow`, ranged and formatted `IntRow`/`FloatRow`, `SetNextRowDecor` (the override cell and the reset slot), `LastRowEvents`, label and `ReadOnlyRow` ellipsis, `AssetRefRow`, the breadcrumb wrap, `EditGesture::EndAfterRow` (s4.1(f)) and "pages never repeat their title" (s4.3). It also relies on T1's late-bound `chromeGraph` seam and its `PreviewStatus` model (s3.2). Document stacks are T1's resolver `m_services.undo()`, which is null during Play (s3.3).

Paths are relative to `ArcaneEditor/src/` unless rooted. `cpp`/`hpp` mean `Documents/ShaderEditorDocument.{cpp,hpp}`. Line numbers are at main `57af0cab`.

**Order inside T3 (drafting pick, 9.28):**
1. 5.1.10 fixture. The tests and goldens that count ReferenceProject assets are re-counted in the same commit.
2. 5.7 pinned header.
3. 5.2.
4. 5.3.
5. 5.1.
6. 5.4.
7. 5.5.
8. 5.6.

Each step ends with a green build and ArcaneTests. The golden gate and the desk check run at the end of the tranche (s2.1).

### 5.1 The node page

**Problem.**
- Clicking a node does not change the Inspector. Every content click bumps the epoch of the single `"material"` key (`Documents/DocumentPageSelection.hpp:35-51`, called at cpp `:2049`). The seam comment says the node page "lands here as a second key" (hpp `:193-196`).
- All node editing happens inline on the canvas (`DrawGraphNode`, cpp `:4315-5192`), and it disappears at `LowDetail` zoom (`showPinText`, cpp `:4411`).
- A Custom pin's name cannot be edited anywhere. The pin controls at cpp `:4724-4760` offer only width and remove.
- Every node header is the same grey (`kNodeTitleColor`, cpp `:288`, painted by `DrawNodeTitleBand` `:451-466`). This is critique Shader #7.
- No node type has a description (`GraphNodeTypeInfo`, `ArcaneCore/src/Arcane/Material/MaterialGraph.hpp:190-201`).

#### 5.1.1 Selection mirror and key grammar

- **Key grammar.** A node key is `node:<pass>:<id>`.
  - `<pass>` is `GraphOptAt`'s chain index: 0 is the base, and k is `m_data.passes[k-1]` (cpp `:3391-3396`).
  - `<id>` is `GraphNode::id`, which is "unique within the graph, > 0, NEVER reused". It is unique only per graph, so the key carries the pass.
  - Both numbers are canonical unsigned decimals: no sign, no leading zeros (except `0` itself), no whitespace, and `id > 0`.
- **Pure header `Documents/ShaderNodeKey.hpp`:**
  - `struct NodeKey { std::size_t pass; std::uint32_t id; };`
  - `std::optional<NodeKey> ParseNodeKey(std::string_view)` refuses every non-canonical spelling, so each node has exactly one history key.
  - `std::string FormatNodeKey(NodeKey)`.
  - `std::optional<NodeKey> ParseNodeSelectPath(std::string_view, std::size_t activePass)` accepts `"<id>"` (the active pass) or `"<pass>/<id>"`.
  - The existing `CHECK_FALSE(doc.Resolves("node:7"))` (`ArcaneTests/src/ShaderEditorDocumentTest.cpp:121`) still holds, because that key is malformed.
- **Document state:**
  - `std::optional<NodeKey> m_nodeSel` is the mirror.
  - `std::optional<NodeSelRequest> m_nodeSelRequest` holds a pending canvas write: `NodeSelRequest { enum { Select, Clear } op; std::uint32_t id; }`.
  - `bool m_nodeSelApplying` is set on the frame a request is applied.
  - `m_pageSel` (hpp `:795`) keeps the epoch and the `"material"` key.
  - The new predicate `bool ChainViewShowing() const { return m_surface == 0 && m_inChainView; }` is the test Draw uses at cpp `:2004`. The raw `m_inChainView` is seeded `true` for every document (cpp `:924`), and Draw clears it only once a non-chain surface draws (`:2001-2002`). Reading the raw flag before the first draw would therefore report a chain view that is not there.
- **Canvas writes go only through the request.** The request is applied inside `ed::Begin/End`, right after `HandleGraphEdits()` (cpp `:3881`), in the same place as `m_focusNode` (`:3886-3892`):
  - `Select` calls `ed::SelectNode(id)` with no `NavigateToSelection`;
  - `Clear` calls `ed::ClearSelection()`;
  - then `m_nodeSelApplying = true` is set and the request is reset.
- **The read happens after `ed::End()`** (cpp `:4311`), before `ed::SetCurrentEditor(nullptr)` (drafting pick, 9.28; the brief says "before ed::End").
  - The library runs selection actions inside `End` (`imgui_node_editor.cpp:1353`) and snapshots `m_LastSelectedObjects` in `Begin` (`:1266-1269`). `HasSelectionChanged` compares the two (`:1850-1853`). A read taken before `End` would see last frame's selection and would never see a click or a marquee.
  - The read is `ed::NodeId buf[2]; const int n = ed::GetSelectedNodes(buf, 2)`. The count is capped at the buffer size (`ThirdParty/imgui-node-editor/imgui_node_editor_api.cpp:22-36, 462-468`), so 2 means "two or more".
  - When `n == 1`, `m_nodeSel = {m_activePass, id}`. Otherwise `m_nodeSel` is reset.
  - `if (ed::HasSelectionChanged() && !m_nodeSelApplying) ++m_pageSel.epoch;`, then `m_nodeSelApplying = false`.
- **What counts as an event:**
  - A click, Ctrl-click, marquee, background clear and paste-select are events.
  - A restore is not an event, because the applying flag suppresses it.
  - A pass switch is not an event: the rebuilt context starts empty, and cpp `:3797-3798` changes nothing that `Begin` snapshotted.
  - `NoteContentClick` still bumps on any content click, and history swallows the echo (`Panels/InspectorHost.hpp:28-45`).
- **When the canvas does not draw** (HLSL view, background tab, collapsed window), the mirror keeps its value.
- **`SelectionKey()`** returns `FormatNodeKey(*m_nodeSel)` when `m_nodeSel` is set, `!ChainViewShowing()`, and `Resolves` holds. Otherwise it returns `"material"`.
  - Chain-view pass nodes keep the material page.
  - The mirror survives the chain view. Leaving the chain view restores the node page with no event (`SelectionEdge`, `InspectorHost.hpp:28-45`).
- **`Resolves(key)`** is pure. It is true for `"material"`. For a node key it is true when `ParseNodeKey` succeeds, `pass <= m_data.passes.size()` (checked *before* `GraphOptAt`, which silently falls back to the base when out of range), the graph at `pass` exists, and `FindNode(id)` finds the node.
  - Instances and graphless (repair-mode) bases resolve only `"material"`.
- **`RestoreSelection(key)`** returns false when `!Resolves(key)`. It never bumps the epoch.
  - `"material"`: reset `m_nodeSel` and arm `Clear`.
  - A node key:
    1. If `pass != m_activePass || ChainViewShowing()`, call `EnterPass(static_cast<int>(pass))`. `m_activePass` and `EnterPass` are `int` (hpp `:704`, `:482`; cpp `:2418-2424`), and `Resolves` has already range-checked `pass`. `EnterPass` leaves the chain view and records the document's own navigation history.
    2. Set `m_nodeSel` **immediately**, because `InspectorHost::TryLand` re-reads `SelectionKey()` straight after the restore (`Panels/InspectorHost.cpp:240-245`).
    3. Arm `Select{id}`.
- **`SelectByPath(path)`** (`--select-in-document`, `Documents/EditorDocument.hpp:79-80`) runs `ParseNodeSelectPath(path, m_activePass)`, then `RestoreSelection(FormatNodeKey(k))`, then `++m_pageSel.epoch` (a scripted select is a selection).
  - It runs in `StageFinalize`, before the first draw. The key is valid at once because it is checked against the data, and the request lands on the first canvas frame.
- **Undo that deletes the selected node.** `Resolves` fails, so the key falls back to `"material"` with no event. A pinned instance reads "Pinned selection is gone" (`Panels/InspectorWindows.cpp:367-377`). Redo restores the same id, and the pinned page returns on the next `PageFor`.
- **Problems GraphNode locator.** `RequestFocusGraphNode` (hpp `:345`) keeps its `ed::SelectNode + NavigateToSelection` path (cpp `:3886-3892`).
  - That change is visible after `End` and is not an applied request, so it raises a node-selection event and the node page shows.
  - It is the only path that frames the canvas on a selection.
  - It still acts on the active pass. s8.2 calls only this function.

#### 5.1.2 Routing and the page object

- **The page class.** `class NodeInspectorPage final : public InspectorPage` is nested beside `MaterialInspectorPage` (hpp `:409-422`).
  - Members: `ShaderEditorDocument& m_doc; std::size_t m_pass = 0; std::uint32_t m_id = 0;`.
  - `SetTarget(pass, id) noexcept`.
  - `Draw(PropertyGrid& g)` calls `m_doc.DrawNodePageBody(g, m_pass, m_id)`.
  - The member is `NodeInspectorPage m_nodePage{ *this };`, declared beside `m_page` (hpp `:796`).
- **`PageFor(key)`:**
  - `"material"` returns `&m_page`;
  - a node key that resolves calls `m_nodePage.SetTarget(pass, id)` and returns `&m_nodePage`;
  - anything else returns `nullptr`.
  - `Page()` is `PageFor(SelectionKey())`.
- **Retargeting on every call is legal.** A returned page is valid until the next `Page()`/`PageFor()` (`Panels/InspectorSource.hpp:48-55`). Two instances, one pinned to A and one following, each resolve right before they draw. The precedent is `InputActionsDocument.cpp:66-77`.
- **Never hold a `GraphNode*` across frames.** `DrawNodePageBody` re-resolves `(pass, id)` on every call, because create and paste reallocate `nodes`. When the node is gone (a transient frame after an undo), the body draws one `ReadOnlyRow`, "This node no longer exists", and returns.
- **The kind stays `"material"`.** Filters, stamps, the host and `EditorInspectorHostTest` are unchanged.

#### 5.1.3 Crumbs

| Crumb | `label` | `select` (unpinned) | `key` (pinned repin) |
|---|---|---|---|
| Material | `m_title` (the instance suffix per s4.3) | reset `m_nodeSel`, arm `Clear`, `++m_pageSel.epoch` (a crumb click is a selection; precedent `SelectMap({})`, `InputActionsInspectorPage.cpp:78`, bump at `InputActionsEditorModel.cpp:238`) | `"material"` |
| Pass (only when `ChainMode()`, hpp `:433-437`, for every pass including `base`) | `PassLabel(pass)` (cpp `:1849-1859`) | `EnterPass(pass)`, then the material crumb's `select` | `nullopt` (inert while pinned) |
| Node | `GraphNodeInfo(type).display` | no-op | `FormatNodeKey({pass, id})` |

- The three crumbs rely on T2's wrap and collapse (s4.3).
- `InspectorCrumbText` reads, for example, "NodePageGraph > Multiply" (`InspectorSource.hpp:72-78`).
- **Navigation rule.** History back/forward and crumb landings on a node select it with the `Select` request and **never** call `NavigateToSelection`.

#### 5.1.4 Page body

`DrawNodePageBody(PropertyGrid& grid, std::size_t pass, std::uint32_t id)` runs these steps in order:
1. `const EditGesture::ScopeGuard gestureGuard{ m_services.undo(), m_gesture };` is the FIRST local. The page draws on collapsed and background frames too, like `DrawMaterialPageBody` (cpp `:2055-2064`).
2. `DrawSaveWithErrorsConfirm()` (cpp `:2237`), then `DrawGraphModals()` (5.1.7).
3. Resolve the node. Push the RAII id scope `PushID("node") / PushID(pass) / PushID(id)` before any `Rows`, as `TargetIdScope` does (`InputActionsInspectorPage.cpp:51-61`). This keeps text and numeric drafts per node.
4. Draw the sections.
5. Run the queued discrete edits. Combos, checkboxes, buttons and in-draw text commits go into a `std::vector<std::function<void()>>` that runs after the last section, following the `edit_`/`Defer` precedent in `Documents/InputActionsInspectorPage.hpp`. Removing a Custom pin mid-loop would otherwise invalidate the loop. Numeric write-through stays inline (5.1.5).

**Header** (not a Section). One row holds the **category chip**, `GraphNodeCategoryName(category)` (`MaterialGraph.cpp:221-236`) in `kNodeTitleText` on a rounded `GraphCategoryHeaderColor(category)` fill, followed by the type display in `kTextDim`. The description follows, wrapped, in `kTextDim`.
- This is the one sanctioned repeat of the crumb leaf, and it costs no extra line.
- **Colour table** (drafting pick, 9.28): new `Documents/ShaderGraphCategoryColors.hpp`, `constexpr ImVec4 GraphCategoryHeaderColor(Arcane::GraphNodeCategory) noexcept`.

  | Category | Colour |
  |---|---|
  | Uncategorized | #232326 (today's `kNodeTitleColor`) |
  | Input | #24384a |
  | Math | #26402f |
  | Vector | #3a2a4a |
  | Procedural | #4a3a22 |
  | Output | #5a2626 (the sink stands out, Shader #7) |
  | Utility | #2e2e33 |

- **Canvas.** The signature becomes `DrawNodeTitleBand(std::uint32_t id, float headerMaxY, ImVec4 color = kNodeTitleColor)`.
  - The three pass-canvas calls (cpp `:2777`, `:2814`, `:2841`) keep the default.
  - Only `DrawGraphNode` (`:5189`) passes `GraphCategoryHeaderColor`. At `LowestDetail` the band is the whole node, so the category stays visible when zoomed out.
  - Comment boxes (`ed::Group`, cpp `:4317-4351`) are unaffected.

**Inputs** (`grid.Section("Inputs")`). Omitted when `GraphNodeInputCount(n) == 0` (UV, Time, the Const nodes, Param, Comment, Vertex Color). There is one row per pin under `PushID(pin)`, labelled `GraphNodeInputPin(n, pin).name`.

| Pin state | Row |
|---|---|
| Wired | `ReadOnlyRow` `"<- " + srcDisplay + "." + srcPinName`. For a Param or Texture Sample source, `srcDisplay = display + " '" + paramName + "'"`. |
| Unwired, `GraphPinAcceptsLiteral(n, pin)` (`MaterialGraph.hpp:394`), `GraphPinLiteralLanes(width)` = 1 | `FloatRow` (speed 0.01, `"%.3f"`), live (5.1.5) |
| Same, lanes 2 or 4 | `VecRow(n = lanes, speed 0.01, no range, "%.3f")` |
| Literal present (`FindPinLiteral(pin)`) | Also gets the T2 reset decoration (`ICON_LC_ROTATE_CCW`, "Reset to default"). Reset erases the entry so codegen reads the neutral, as one discrete step, "Reset Pin Value". |
| Unwired, no literal, neutral is `Expression` (Panner `uv`) | The same editable row, with the neutral's `hlsl` ("v.uv") as its format string. ImGui prints a format that has no conversion verbatim, the canvas trick at cpp `:4789-4794`. |
| Unwired, pin refuses literals | `ReadOnlyRow` `"default: " + FormatPinNeutral(neutral)` |

- `FormatPinNeutral` prints:
  - Constant: `%g` for one lane, `(a, b)` or `(a, b, c, d)` for more;
  - Expression: the `hlsl` text;
  - Passthrough: "unchanged (only a wire contributes)".
- Values come from `GraphPinNeutralDefault` (5.1.8).
- **No `ColorRow` among the pin rows** (drafting pick, 9.28; the brief says "FloatRow/VecRow/ColorRow"). No literal-accepting pin is a colour: `Output.color` and `Vertex Output.color` refuse literals (`MaterialGraph.cpp:1296-1320`), and Custom width-4 pins are plain vectors.

**Settings** (`grid.Section("Settings")`). Omitted for types that have none (data model `MaterialGraph.hpp:240-309`). Undo labels match the canvas's.

| Node | Rows | Commit |
|---|---|---|
| Float | `FloatRow "Value"` → `value[0]` | live, "Edit Value" |
| Float2 / Float4 | `VecRow(2\|4) "Value"` → `value` | live, "Edit Value" |
| Color | `ColorRow "Color"`, **hdr = true** (a ConstColor feeds raw maths, cpp `:4880-4887`) | live + popup pair, "Edit Value" / "Edit Color" |
| Param | `TextRow "Name"` → `paramName` | "Rename Param", then `BeginParamRename(old, new)` with copies (cpp `:4921-4948`) |
| | `ComboRow "Type"` {float, float2, float4, color} | queued; sets `paramType` and `paramDefault.type`; "Param Type" |
| | `"Default"`: `FloatRow` / `VecRow(2)` / `VecRow(4)` / `ColorRow` with **hdr = false** | live, "Param Default" |
| | `CheckboxRow "Range"`; when on, `FloatRow "Min"` → `rangeMin` and `"Max"` → `rangeMax` | checkbox queued, drags live, both "Param Range" |
| Texture Sample | `TextRow "Texture Param"` → `paramName` | "Rename Param" + `BeginParamRename` |
| Swizzle | `TextRow "Mask"` → `swizzleMask` | "Edit Swizzle" |
| Pass Input | `ComboRow "Slot"` {in0..in3} (`kMaxPassInputs = 4`, `ArcaneCore/src/Arcane/Material/MaterialSource.hpp:88`) | queued, "Input Slot" |
| Panner | `CheckboxRow "Fractional"` → `pannerFractional` | queued, "Panner Fraction" |
| Custom | `SubSection "Pins"`. For each pin `k` (`PushID(k)`): `TextRow "Name"` (**new: pin rename**), `ComboRow "Width"` {float, float2, float4}, `ButtonRow` {"Remove"}. Then `ButtonRow` {"Add Pin"}. | "Rename Pin"; "Pin Width"; "Remove Pin" through the new member `RemoveCustomPin(pass, id, pin)`, lifted from cpp `:4735-4760` (link and literal re-index) and called by both surfaces; "Add Pin" through `AddCustomPin(pass, id)` (the `p<k>` rule, cpp `:5073-5088`) |
| | `ComboRow "Output"` {float, float2, float4} → `customOutWidth` | queued, "Output Width" |
| | `ReadOnlyRow "Body"`: the first body line, ellipsized, with the full body in the tooltip. `ButtonRow` {"Edit HLSL..."} → `RequestBodyEdit(pass, id)` (5.1.7). | "Edit HLSL Body" on Apply |
| Comment | `TextRow "Text"` → `paramName` (hpp `:240`); size not exposed | "Edit Comment", dirty only, no recompile (cpp `:4336-4341`) |
| All other types | none (pin literals only) | -- |

- **Pin rename** (drafting pick, 9.28) changes only `customPins[k].name`. Links address pins by index, so nothing re-wires.
  - The page never rewrites the user's HLSL body. A body that still uses the old name fails to compile, and the failure shows in Errors.
  - The value tooltip says "Renaming does not edit the HLSL body".
- **No name validation on the page** (drafting pick, 9.28). Codegen is the single validator for param names, reserved names, custom pins and masks (`MaterialGraph.cpp:503-515, 566-585, 588-598`). Its verdict appears in Errors.

**Outputs** (`grid.Section("Outputs")`). Omitted for Output, Vertex Output and Comment. There is one `ReadOnlyRow` per output pin, labelled with the pin name. The text is the declared width (`float`/`float2`/`float4`, or `dynamic` for 0; Custom reads `customOutWidth` through `GraphNodeOutputPin`), followed by `" -> "` and every target as `display.pinName` joined by ", ", or by `" (unused)"`.

**Errors** (`grid.Section("Errors (N)")`). Drawn only when N > 0. Each line is drawn wrapped in `Theme::kError`. Graph-level errors (`nodeId == 0`) stay on the material page and in Problems. The lines come from two sources:
- `m_passGraphErrors[pass]` filtered to `nodeId == id`;
- compile diagnostics mapped to the node by the new `void ForEachNodeDiagnostic(std::size_t pass, std::uint32_t nodeId, const std::function<void(std::string_view)>&) const`. This is the per-pass generalisation of `RebuildDiagBadges`' line map (cpp `:3691-3717`), which serves only the active pass, while a pinned page may show another pass.

#### 5.1.5 The live editing contract

- **Live while dragging (user decision).** For every numeric, vector and colour row, each frame:
  1. Read the node's value into a local.
  2. Draw the row on the local. The row writes its draft through every frame (`Widgets/PropertyGrid.cpp:176-210`).
  3. Call `EditGesture::BeginOnActivate(m_services.undo(), m_gesture, label, [&]{ return GraphEditBuilder(label, pass); })`.
  4. If the local differs from the node, write it into the re-resolved node and call `NoteGraphValueEdited()`.
  5. Call `EditGesture::EndAfterRow(m_services.undo(), m_gesture, grid.LastRowEvents().cancelled)` (s4.1(f)).

  Because the snapshot comes before the write, the snapshot on the activation frame is pre-edit. This is the canvas pin-literal ordering (cpp `:4812-4815`). T2 pins that the value widget is still `LastItemData` after the row.
- **`GraphEditBuilder(const char* label, std::size_t pass)`** is a member lifted from the canvas lambda `buildGraphEdit` (cpp `:4549-4582`).
  - It captures `pass` and `before = GraphOptAt(pass)`.
  - Its close step calls `PushGraphUndo(label, before, pass)` (the pass-pinned overload, cpp `:3458-3475`) unless `GraphOptEqual(before, GraphOptAt(pass))`.
  - The canvas passes `m_activePass` and the page passes its target, so a pinned page that edits a non-active pass is correct.
  - The weakness when passes are reordered within range (cpp `:3462-3470`) is pre-existing and unchanged.
- **One undo step per gesture** through `GraphEditCommand` (cpp `:641-670`). Undo goes through `ApplyGraphState` (cpp `:3439-3449`), which re-seeds canvas positions for the active pass only.
- **`NoteGraphValueEdited()`** is lifted from `valueEdited` (cpp `:4602-4607`): `m_dirty = true` always, and `RegenerateFromGraph()` only when `m_live`. Both surfaces call it.
- **Colour rows** add `ColorRow`'s popup pair with the same builder, as ConstColor does (cpp `:4914-4933`).
- **Esc mid-drag.**
  - The row restores its seed and clears the active id (`PropertyGrid.cpp:193-199`).
  - The page writes the restored value back, and `EndAfterRow` closes the gesture on that row. The graph compares equal, so nothing is pushed.
  - **Pin literals:** `literalExisted` is recorded at activation. When the written value equals the neutral and `!literalExisted`, the entry is erased. A cancelled gesture therefore never leaves a new literal behind, and dragging back onto the neutral leaves no entry.
  - Otherwise the literal is updated in place, one entry per pin (invariant `MaterialGraph.hpp:295-301`; canvas cpp `:4816-4835`).
- **Text rows are commit-only** (`TextRow`, deactivate-after-edit).
  - A stored commit captures `std::weak_ptr<ShaderEditorDocument*> anchor = m_anchor` (hpp `:664`) and `pass` and `id` **by value**.
  - When it fires, it locks the anchor and re-resolves the node. It drops the edit if either is gone. Otherwise it takes `before = GraphOptAt(pass)`, writes the field, calls `NoteGraphValueEdited()` and calls `PushGraphUndo(label, before, pass)`.
  - `CommitOrphans` runs before an Inspector window begins (`InspectorWindows.cpp:337-340`), possibly after the selection has moved, so a commit never reads the page's current target. The precedent is `alive_` in `InputActionsInspectorPage.hpp`.
- **Param and Texture Sample renames** push "Rename Param", then call `BeginParamRename(oldName, newName)` with copies (cpp `:3532-3617`). That call scans every pass and arms the propagation modal.
- **Canvas and page on one field.** Only one item can be active at a time, so the two surfaces never race. The page draws after the documents, so a page edit in frame N shows on the canvas in frame N+1.
- **The canvas keeps its inline widgets.** The page is a second surface over the same data and the same commands.

#### 5.1.6 Descriptions

- `const char* description;` is appended to `GraphNodeTypeInfo` after `category` (`MaterialGraph.hpp:190-201`; the argument for appending is the same as the category comment at `:197-199`).
- All 49 rows of `kNodeInfos` are filled (`MaterialGraph.cpp:61-171`; coverage `static_assert` at `:177`). Each description is one plain sentence that says what the output is in terms of the inputs and names the HLSL intrinsic when there is one. Examples:
  - Multiply: "A times B, per component (a * b); a scalar input splats to the other's width."
  - Panner: "Scrolls UV by Time x Speed; Fractional wraps the offset to [0, 1) to keep precision."
  - Comment: "A labelled box; nodes inside it move with it. It has no effect on the shader."
- **Test:** the loop at `MaterialGraphTest.cpp:107-122` gains `CHECK(info.description != nullptr && info.description[0] != '\0')` with `INFO(info.token)`.
- **ABI:** `GraphNodeTypeInfo` is an exported Core struct returned by reference, so this is a bump. It is T3's single Core bump, shared with 5.1.8, and is followed by the Aphelyon restamp and module rebuild.

#### 5.1.7 Modal hoist

- **`void DrawGraphModals()`** takes over the HLSL body editor and the param-rename propagation modal from `DrawGraphPanel` (cpp `:3998-4090`). It takes no graph reference.
  - `DrawGraphPanel` calls it where they sit today, inside `ed::Suspend`.
  - `DrawNodePageBody` calls it in normal ImGui space.
  - The precedent is `DrawSaveWithErrorsConfirm`, which the toolbar (cpp `:2234`) and the page (cpp `:2068`) both draw. Whichever draws first consumes the request flag, and the other's `BeginPopupModal` returns false. So "Edit HLSL..." and rename propagation work while the canvas is hidden.
- **The body editor is bound to a pass.**
  - `RequestBodyEdit(std::size_t pass, std::uint32_t id)` replaces `m_bodyEditRequest = n.id` (cpp `:5129`). The canvas passes `m_activePass`.
  - The request branch (`:3998-4006`) resolves `GraphOptAt(m_bodyEditPass)->FindNode(m_bodyEditRequest)`.
  - Apply resolves `GraphOptAt(m_bodyEditPass)->FindNode(m_bodyEditNode)`, takes `before = GraphOptAt(m_bodyEditPass)`, and calls `PushGraphUndo("Edit HLSL Body", before, m_bodyEditPass)`. This replaces today's closure over the canvas's `g` and `ActiveGraphOpt()` (`:4017-4028`).
- **The rename modal needs no pass.** `BeginParamRename` already walks every pass (cpp `:3538-3549`).

#### 5.1.8 `PinNeutralDefault` moves to core

The new declaration sits beside `GraphPinAcceptsLiteral` (`MaterialGraph.hpp:394`):

```cpp
enum class GraphPinNeutralKind : std::uint8_t { Constant, Expression, Passthrough };
struct GraphPinNeutral
{
    GraphPinNeutralKind kind  = GraphPinNeutralKind::Constant;
    int                 lanes = 1;               // width of the neutral itself (Adapt's defWidth)
    float               v[4]  = {};              // Constant only
    const char*         hlsl  = "0.0";           // EXACT text codegen emits; static storage; nullptr for Passthrough
};
[[nodiscard]] ARCANE_CORE_API GraphPinNeutral GraphPinNeutralDefault(const GraphNode& n, std::uint32_t pin) noexcept;
```

- **It covers every input pin.** It is one row table in `MaterialGraph.cpp` beside the emission switch.
  - Literal-pin neutrals (today's editor copy, cpp `:571-609`):
    - Combine `a` = 1;
    - Clamp `max` = 1;
    - Smoothstep `edge1` = 1;
    - Power exponent = 1;
    - Tiling & Offset `tiling` = 1 (splat);
    - Simple Noise `scale` = 10;
    - Scale & Offset `scale` = 1;
    - Panner `uv` = Expression "v.uv", 2 lanes;
    - everything else 0.
  - Direct-read neutrals (shown as "default: ..."):
    - Output `color` = `float4(0.0, 0.0, 0.0, 1.0)` (`:748-751`);
    - Texture Sample, Sprite Texture and Pass Input `uv` = "v.uv" (`:771-786, :1003-1017`);
    - Tiling & Offset and Simple Noise `uv` = "v.uv";
    - Split and Swizzle source = `0.0` (`:866-887, :942-985`);
    - Remap ranges = `float2(0.0, 1.0)` (`:906-921`);
    - the three Vertex Output pins = Passthrough (`:993-1001`).
- **Single truth.**
  - Codegen's `argOr` sites (`:722-739`) and the direct-read sites above take `hlsl` and `lanes` from this function.
  - The editor reads `v`, `kind` and `hlsl`.
  - The canvas's `PinNeutralDefault` (used at cpp `:4787`) is deleted, and the canvas calls core.
- **`hlsl` is codegen's current text verbatim**, for example "1.0", "10.0" or "v.uv". It is not formatted from `v`, because `FormatF` uses `%g` (`:278-283`) and would print "1". Every snippet expectation in `MaterialGraphTest` therefore stays byte-identical, and that is the tripwire.
- **ABI:** this export rides 5.1.6's bump.

#### 5.1.9 What the node page does not do

- **No per-node preview in v1.** It is size L, it is blocked on previews working, and its editor machinery was deleted in `ed540e39`. The page shows no copy of the material preview either.
- **No pages for post-chain pass-canvas nodes.** They keep the material page.
- **No multi-select editing (default).** 0 or 2+ selected nodes show the material page, with no "N nodes selected" page.
- **No canvas framing** on history, crumb or restore landings.
- **No create, delete, copy or paste** of nodes from the page.
- **No Comment size row and no in-page body editing.**
- **No validation that duplicates codegen, and no rewriting of the body on pin rename.**
- **No stable pass identity in keys.** A stored key into a pass list that has since been re-sorted can resolve to the same id in whichever pass now holds that index. This is the same pre-existing weakness as `GraphEditCommand` (cpp `:3462-3470`). Live selection is rebuilt from the canvas every drawn frame.
- **History grows by one entry per node click** (depth 32, shared). This is accepted, as it is for the input-actions document (R22).

#### 5.1.10 Fixture

- **The asset.** New `ReferenceProject/Content/materials/node_page_graph.arcmat`, named `"NodePageGraph"`.
  - Its id is `7e5a0012-0012-4012-8012-000000000012`, which is unused in ReferenceProject, ArcaneTests and scripts at `57af0cab`.
  - It is a graph-owned **sprite**-surface base material, so it compiles and previews, and it has no chain.
- **Nodes** (ids fixed in the file so tests and witnesses can name them):
  - Output;
  - Sprite Texture;
  - Param `tint` (color);
  - Multiply, with Sprite Texture `rgba` → `a` and `tint` → `b`, wired to Output `color`;
  - Power, with an `exponent` literal of 2 and `x` unwired;
  - Swizzle, mask `xy`, unwired;
  - Custom, with one pin `p1` (float4), body `return p1;`, out float4;
  - Panner, `Fractional` on;
  - one Comment, "Node page fixture".
- **Asset counts shift** in every golden and witness that counts ReferenceProject assets (for example the `editor-ui` Asset Browser rows). They are re-counted in the fixture's commit, and the slots are re-blessed one at a time at the end of T3 (s2.6).

#### 5.1.11 Tests and verification

- **Pure** (`ArcaneTests/src/ShaderNodeKeyTest.cpp`, new):
  - the `ParseNodeKey`/`FormatNodeKey` round trip;
  - refusals of `node:01:7`, `node:0:0`, `node:7`, `node: 0:7` and `node:0:7x`;
  - `ParseNodeSelectPath` for `"7"` and `"2/7"` against a given active pass.
- **Document, headless** (`ShaderEditorDocumentTest.cpp`, the `:98-122` pattern):
  - **`PageFor`:** `"material"` returns the material page (this closes an untested gap); `"node:0:<id>"` returns the node page; a missing id returns null; an out-of-range pass returns null, **not** the base fallback.
  - **`RestoreSelection`** sets `SelectionKey()` immediately, with no epoch bump. On a sprite-surface document before its first draw it calls no `EnterPass` and adds no navigation-history entry.
  - **`SelectByPath`** bumps the epoch exactly once.
  - **Crumbs** are [material, node], or [material, pass, node] in a chain document, with the 5.1.3 keys.
  - **Undo/redo of a delete:** deleting the selected node and undoing falls back to `"material"` with the epoch unchanged, and redo restores the node key.
  - **One undo step each,** through a headless `PropertyGrid` frame on the node page (pattern `:1263`, `:1314`):
    - a literal commit (`GraphToJson` compare);
    - Reset (literal erased);
    - Param rename (`m_paramRenames` gains the pair);
    - Custom pin rename;
    - Remove Pin (literals and links re-indexed).
  - **No step:** an Esc-cancelled drag pushes nothing and leaves no literal.
  - **Inert commits:** a stored `TextRow` commit is inert after the document is destroyed, and after the node is deleted.
  - **Pinned pass:** a pinned-pass edit targets that pass while another pass is active.
  - **Row set:** `PropertyGridState::probe` (`Widgets/PropertyGrid.hpp:75-77`) asserts the rows per node type from the 5.1.4 tables, and that each section is omitted where those tables say.
- **Canvas, headless** (`GraphCanvasHeadlessTest.cpp:23-104` harness):
  - `RestoreSelection` followed by two frames: `ed::IsNodeSelected` agrees and the epoch does not move;
  - a programmatic multi-select gives `"material"`;
  - a simulated click-select gives one bump;
  - a pass switch gives no bump;
  - in the chain view the key is `"material"`, and leaving the view restores the node key with no bump;
  - the body modal opens from the page with the canvas not drawn, and Apply writes the pinned pass.
- **Core** (`MaterialGraphTest.cpp`):
  - every node type has a description;
  - a `GraphPinNeutralDefault` truth table over every type and input pin (kind, lanes, v, hlsl), next to the literal table at `:818-905`;
  - for every Constant row, the numbers parsed from `hlsl` equal `v`;
  - every existing snippet expectation passes unchanged.
- **Colours:**
  - every category colour has contrast ≥ 4.5 against `kNodeTitleText` (#cecfd4), measured with a pure `Theme::ContrastRatio(ImVec4, ImVec4)` added to `Widgets/EditorTheme.hpp` (T4's theme test reuses it). The listed colours measure ≥ 7.0:1.
  - Output's colour differs from every other category's.
- **Witness E9** (`EditorWitnessTest.cpp`, new, the E5 pattern at `:303-340`, `[witness][gpu]`; E6 and E7 already exist and T1 adds E8; drafting pick, 9.28). It runs `--open-asset 7e5a0012-0012-4012-8012-000000000012 --select-in-document <Multiply id>` and asserts:
  - `inspector.instances[0].breadcrumb == "NodePageGraph > Multiply"`;
  - `instances[1]` still reads "Assets";
  - the `documents[]` entry for "NodePageGraph" is {compile `ok`, preview `ready`, image `true`} (5.2).

  This spec adds no golden slot for the node page.
- **Desk** (1920x1080, the user's layout, Aphelyon `logo_showcase` scratch copy):
  - click, marquee and Ctrl-click on the canvas;
  - edit from the page at LowDetail zoom;
  - press Esc during a page drag;
  - rename a Param while instances exist on disk;
  - use "Edit HLSL..." from the page with the HLSL view showing;
  - pin a node page, then switch passes;
  - check that the category colours read on the canvas;
  - **R5:** check whether clicking a node's inline widget (for example the Sine `x` drag) selects the node in imgui-node-editor. If it does not, that click leaves the page on its previous key. The result is recorded, not fixed, in T3.

### 5.2 Honest preview status

**Problem.** The toolbar's last else-branch and the preview box both print "compiling..." whenever `PreviewReady()` is false (cpp `:2222-2232`, `:3383-3386`). That covers five different states: a compile in flight, no compiler, compiled but no device, vehicle creation failed, and preview frame failed (critique Shader #1, #3; Top-10 #2). "Thumbs" stays enabled when there is nothing to show (cpp `:2141`).

**Design.**
- **T3 extends T1's `Documents/PreviewStatus.hpp` (s3.2).** The model, the inputs, the precedence rules, the ids and the shader/mesh input wiring all belong to s3.2. T3 adds the text and the UI:

  ```cpp
  [[nodiscard]] std::string ToolbarStatusText(const PreviewStatus&);   // compile + preview, one line
  [[nodiscard]] std::string PreviewBoxText(const PreviewStatus&);      // the box's text when there is no image
  ```

  The schema-13 `documents[]` record is T1's and is not changed here.
- **The toolbar shows two statuses:**
  - Ok with an image: "ok";
  - Ok without an image: "compiled, no preview (<why>)", where why is "no GPU device", "preview context failed -- see the log" or "preview frame failed -- see the log";
  - Compiling: "compiling...";
  - Errors: "errors" in `Theme::kError`, plus ", no preview (<why>)" when the preview is not Ready (the last good image stays bound through errors, cpp `:3436`);
  - CompilerUnavailable: "not compiled -- shader compiler unavailable (see the log)" in `Theme::kAmber`;
  - NotCompiledHere: "not compiled here".
- **The preview box** draws the image when `image` is set, and otherwise draws `PreviewBoxText` centred. 5.3 owns its layout.
- **`MeshDocument`.** Its branch at `MeshDocument.cpp:456-500` is replaced by `PreviewBoxText`. An imported mesh reads "Imported mesh -- preview it on a mesh in the viewport".
- **"Thumbs" becomes "Output preview".**
  - When `!image` it is disabled (`BeginDisabled` + `SetItemTooltip` with `AllowWhenDisabled`), with the tooltip "Shows the material preview on the Output node. Unavailable: <why>".
  - It still gates only the Output node's copy (cpp `:3737-3755`).

**Does not.** It does not render imported-mesh or mesh-material previews in their documents (owed: "mesh document full-pane preview"). It adds no Problems row for "see the log", and it changes no compile behaviour.

**Tests.**
- **`PreviewStatusTest.cpp`** (the s3.2 file) gains text cases:
  - every (compile, preview) pair maps to the toolbar string above;
  - Errors with a bound image keeps the image and appends the preview reason;
  - `PreviewBoxText` covers each non-Ready state and the imported-mesh text.
- **`ShaderEditorDocumentTest.cpp`**, device-less (`:1036` pattern):
  - with no compiler service the toolbar text is the CompilerUnavailable string;
  - a mesh-surface document reads "not compiled here";
  - "Output preview" is disabled.
- **`MeshDocumentTest.cpp:95`:** a device-less document's box reads "no GPU device".
- **Witness:** E9 (5.1.11) asserts `documents[]` for the fixture.
- **Desk:** the fixture's toolbar reads "ok" with an image, and `reference_mesh` reads "not compiled here".

### 5.3 Material page port

**Problem.**
- The page ignores the grid it is given (hpp `:418`).
- About half its height goes to a 0.55 preview split (hpp `:224`, cpp `:2096-2104`).
- The params are value-left raw ImGui and clip ("baseColo", cpp `:5838`, `:5898`).
- Inherited instance rows are not dimmed (`:5829-5903`).
- The override checkboxes have no labels (`:5802-5827`).
- The "x" reset has no tooltip (`:5993-6008`).
- The texture param is a text line with a "pick" popup that has no search (`:5598-5655`).

Critique Inspector #1, #3, #4, #5, #6, #8, #9, #10; Shader #3, #4, #8.

**Design** (`DrawMaterialPageBody(PropertyGrid&)`; `MaterialInspectorPage::Draw` passes the grid through).
- **The title is removed in T2 (s4.3).** The page body begins at `Section("Preview")`.
- **`Section("Preview")`** is collapsible and open by default.
  - A mesh surface shows one dim line, the NotCompiledHere status from 5.2, and no box.
  - Every other surface shows a centred **square** of side `min(availX, f × pageHeight)`. `pageHeight` is the height of the page child (5.7) at the page's start.
  - `f` is the Archive cvar `editor.inspector.materialPreviewFraction` (drafting pick, 9.28): Float, default 0.45, range 0.2..0.8, help "Largest share of the Inspector's height the material page's preview square may take". It is registered with T1's `ARC_CVAR_RANGED` (s2.4), module `"editor"`.
  - The square shows the image scaled to fit, or `PreviewBoxText`.
  - Amendment (2026-10-02, user decision A): for a material instance the document tab owns the preview; the page omits Section("Preview").
- **The split is retired.** These are removed:
  - `LayoutPrefs`, `kPreviewSplitDefault`, the `ClampSplit`/`SanitizeSplit` users and the `PaneSplitter` call (hpp `:201-230`, cpp `:2096-2104`);
  - the "MaterialPanel" settings handler and its registration (cpp `:731-772`, `:1876-1906`).

  ImGui skips a saved section that has no handler and never writes it back, so a stale `PreviewSplit=` line drops at the next ini save.
- **`Section("Rendering")`** appears for mesh materials only (`CaptureMeshMaterialMetadata`, cpp `:5692`):
  - `ComboRow "Blend"` {Opaque, Masked, Transparent} → `SetMeshMaterialMetadataWithUndo`;
  - `FloatRow "Alpha cutoff"` (0..1, `"%.2f"`, speed 0.01), with an EditGesture bracket after the row and live `ApplyMeshMaterialMetadata` while dragging (cpp `:5747-5768`), one step at the close;
  - `CheckboxRow "Two sided"` → `SetMeshMaterialMetadataWithUndo`.

  On instances each row uses T2's override cell. It replaces the unlabelled `##blend_override` / `##cutoff_override` / `##twosided_override` (cpp `:5714-5781`), and inherited values draw dimmed and disabled.
- **`Section("Parameters")`.**
  - On instances, "Only overridden" is a trailing checkbox on the section's header row (replacing cpp `:5690`). For it, `PropertyGrid::Section` gains `Section(const char* label, bool defaultOpen, const std::function<void()>& trailing)` (drafting pick, 9.28), which draws `trailing` right-aligned on the band with `ImGuiTreeNodeFlags_AllowOverlap`.
  - "params appear after the first successful compile" becomes a `ReadOnlyRow`.
  - Rows go under `PushID(d.name)` with the label `d.name`. The label tooltip is `ParamMeta::tooltip` when it is not empty (`ArcaneCore/src/Arcane/Material/MaterialTypes.hpp:133-139`).

  | Param type | Row |
  |---|---|
  | Float | `SliderRow` over `[meta.sliderMin, meta.sliderMax]` (today's `SliderFloat`; drafting pick, 9.28) |
  | Float2 / Float4 | `VecRow(2\|4, speed 0.01, no range, "%.3f")` |
  | Color | `ColorRow`, hdr = false (a Color param is a colour, cpp `:5850-5856`) |
  | Texture | `AssetRefRow` (kind Texture, `allowTextureMint` false). Set → `SetParamWithUndo(d, value)` with the guid. Clear → `SetParamWithUndo` with the nil guid, the never-assigned state. This retires `DrawTextureParam` and its popup. |

  - **Live preview is kept.** The `BeginOnActivate(buildParamEdit)` bracket sits right after each row, around the live `m_instance->Set` (cpp `:5908-5957`), and closes through `EndAfterRow`. Colour rows add the popup pair (`:5964-5972`). Esc with live preview restores the value and pushes nothing, because of `buildParamEdit`'s no-op guard (`:5954-5957`; T2 pins it).
  - **Instances.** The override cell replaces `##ov_<name>` and keeps the same materialise and clear commands (`:5805-5825`). Inherited rows are dimmed and disabled. There is **no** reset on instances, because the checkbox is the only override control.
  - **Bases.** When `HasOverride`, the T2 reset decoration (`ICON_LC_ROTATE_CCW`, "Reset to default") replaces `x##reset_` and pushes the same `ParamEditCommand` "Reset <name>" (`:5993-6008`).
- **Surface combo.** It stays in the toolbar and gains a leading `TextDisabled("Surface")` + `SameLine` before `Combo("##surface")` (cpp `:2163-2195`). It does not move, so a re-kind still pushes no step (cpp `:2208-2219`). The brief makes it undoable only if it moves.

**Does not.** It does not move the surface combo, change param semantics, mint textures, or preview mesh materials.

**Tests.**
- **`ShaderEditorDocumentTest.cpp`:**
  - the body's first item is the Preview section (probe);
  - Float, Float2, Float4, Color and Texture each map to their row (probe);
  - an instance override toggle pushes "Edit <name>" / "Reset <name>", and undo restores the value;
  - a base reset pushes "Reset <name>";
  - texture `AssetRefEdit::Set` and `Clear` are one step each;
  - the Alpha cutoff drag is one step with live apply;
  - Esc on a Float slider pushes nothing.
- **Re-keyed cases.** The existing page cases (`:1210`, `:1278`) move to the `PushID(label)` + `"##value"` ids.
- **Golden.** `editor-material-page` (dx12 + vulkan, `scripts/golden-gate.ps1:362-363`; witness E5) is re-blessed in T3. It is a mesh material, so it shows the one-line status and full-height params.
- **Desk:** an instance of a material with Color and Texture params, and the preview square at 1080p.

### 5.4 Sprite page port

**Problem.** `SpriteDocument::DrawFormBody` (`Documents/SpriteDocument.cpp:237-348`) has three faults:
- its rows are value-left `DragFloat`/`DragFloat2` with `"%.3f"` pixel fields (`:315-322`);
- it shows a grey "(0, 0) = whole texture" hint (`:324`);
- it shows the texture as a raw guid, while the document window resolves its name (`:337-346` vs `:200-201`).

Critique Inspector #1, #5; Mesh & Sprite #3, #4, #6, #10.

**Design** (`Section("Sprite")` + `Rows`).
- The existing `bracket(label)` lambda (`:299-310`) stays after each numeric row, and its close becomes `EndAfterRow` (s4.1). `m_data` still mutates live, so the document's crop follows the drag.
- **`AssetRefRow "Texture"`** is **read-only**: thumb, resolved name and browse-to, with no picker, clear or drop. The v1 ruling (`:339-343`) stands. `SpriteDocument::Services` gains the shared `AssetRefServices*` (T2).
- **`FloatRow "Pixels Per Meter"`:** 1..4096, `"%g"`, speed 0.5, ClampOnInput.
- **`CheckboxRow "Whole texture"`** is a UI-only view, checked iff `sourceSize == (0,0)` (`ArcaneCore/src/Arcane/Sprite/SpriteAsset.hpp:37`). The file format is unchanged.
  - Ticking it writes `sourcePos = sourceSize = (0,0)`.
  - Unticking it writes `sourcePos = (0,0)` and `sourceSize = (texW, texH)` from `Services::assets->TextureInfoFor(texture)` (`:211-214`).
  - When the dimensions are unknown, the ticked box is disabled with the tooltip "Texture size unknown -- type a Source Size to use a sub-rect".
  - Each flip is one `PushDataEdit("Whole Texture", before)` with `m_dirty = true`.
- **`VecRow "Source Pos"` / `"Source Size"`:** 0..FLT_MAX, `"%.0f"` (the integer pixel fix; drags round to the format), speed 1, ClampOnInput. Typing a non-zero Source Size unticks "Whole texture" by construction.
- **`VecRow "Pivot"`:** 0..1, `"%.3f"`, speed 0.005. The +Y-up tooltip (`:327-330`) moves onto the value.

**Does not.** It does not make the texture editable, add a rect or pivot overlay, or change `SpriteAssetData`.

**Tests.**
- **`SpriteDocumentUndoTest.cpp`** (page harness):
  - each drag is one step;
  - each "Whole texture" tick or untick is one step and round-trips (0,0);
  - with unknown dimensions the box stays disabled;
  - the Texture row has no picker or clear (probe).
- **Re-keyed:** the `GetID("Pixels Per Meter")` expectation (`:277`) moves to the `PushID(label)` + `"##value"` id.
- **Desk:** the pixel fields read as integers ("64", "0 0").

### 5.5 Mesh page port

**Problem.** `MeshDocument::DrawFormBody` (`Documents/MeshDocument.cpp:524-737`) has four faults:
- a value-left `BeginCombo("Source")` (`:594-605`);
- grey sentences in place of rows (`:584-592`, `:643`, `:661`);
- the material is dim, drag-only text on slot 0 only, with a drop rect only as wide as the text (`:689-735`);
- no facts about the mesh.

Critique Inspector #1, #5; Mesh & Sprite #3, #4, #6, #7.

**Design.** The `bracket` (`:545-557`) and `commit` (`:564-571`) lambdas are kept, and the bracket's close becomes `EndAfterRow` (s4.1).
- **`Section("Mesh")`.**
  - A generated mesh gets `ComboRow "Source"` over `kSources`, committing with `commit("Change Source", before)`.
  - An imported mesh gets `AssetRefRow "Source model"`: read-only, `importedSource`, browse-to.
  - **Topology** uses ranged rows, each followed by `bracket(label)`:
    - Plane: `IntRow "Subdivisions"` 1..64;
    - UV Sphere: `IntRow "Rings"` 3..128 and `"Segments"` 3..128;
    - Cylinder: `"Segments"` 3..128;
    - Capsule: `"Rings"` 2..64, `"Segments"` 3..128, and `FloatRow "Length Ratio"` 1..20 (`"%.2f"`, speed 0.02).

    The ranges are `ValidateMeshAsset`'s floors and today's ceilings (`:620-661`). Cube and Imported draw no topology rows, and the grey sentences are deleted.
- **`Section("Material")`.**
  - **Generated:** one `AssetRefRow "Material"` (kind Material, surface filter Mesh, picker, clear, drop).
    - Set creates slot 0 if it is absent, otherwise writes `slots[0].material`, as `commit("Assign Material")`.
    - Clear calls `ClearPrimarySlotMaterial`, as `commit("Clear Material")` (`:165`, `:727-735`).
  - **Imported:** one `AssetRefRow` per `slots[]` entry (default), labelled with the slot name or `"Slot <k>"`.
    - Set writes `slots[k].material`.
    - Clear nils it and keeps the slot, through a new pure static `ClearSlotMaterial(MeshAssetData&, std::size_t slot)`. `ClearPrimarySlotMaterial(d)` becomes `ClearSlotMaterial(d, 0)` and keeps its generated-mesh erase rule. This fills the gap the comment at `:676-684` names.
  - An imported mesh with no slots shows `ReadOnlyRow "Material"`: "(the model defines no material slots)".
- **`Section("Info")`** for generated meshes only (drafting pick, 9.28; `m_previewMesh` is null for imported meshes, `:198-200`). It has three `ReadOnlyRow`s:
  - "Vertices" = `vertices.size()`;
  - "Triangles" = `indices.size() / 3`;
  - "Bounds" = `ComputeMeshBounds(*m_previewMesh)` (as at `:313`), printed as `x × y × z` m, with min and max in the tooltip.

**Does not.** It shows no facts for imported meshes, whose geometry lives in the cooked artifact. It adds no Reimport or Open-source action and changes no slot semantics.

**Tests.**
- **`MeshDocumentTest.cpp`** (page harness), one step each:
  - a source change;
  - `AssetRefEdit` Set and Clear on slot 0 of a generated mesh (Clear erases);
  - `AssetRefEdit` Set and Clear on slot k of an imported mesh (Clear nils and keeps the slot).
- **Info:** the counts match `BuildMeshData`.
- **Re-keyed:** the `GetID("Source")` expectation (`:580`) moves to the `PushID(label)` + `"##value"` id.
- **Pure:** `ClearSlotMaterial` gets its own unit tests.
- **Desk:** ReferenceCube, and one imported `.glb` with more than one slot.

### 5.6 Asset page fit (critique Inspector #2, revised: fit the Assets-only Inspector at 1080p; do NOT reroute selections)

**Problem.** At 1920x1080, Inspector 2 is about 392×347 with a page body of about 376×290.
- A texture with one derived child needs about 455 px, so the full-width 24 px action buttons sit about 165 px below the fold (`Panels/AssetInspectorSource.cpp:325-349`).
- The guid clips at "b9d7-9" (`:240-246`).
- The name is not clamped (`:199-219`).
- The page ignores the grid (`:137-142`).

**Design.** `DrawAssetPage` gains `PropertyGrid&`, and `AssetInspectorSource::Draw` passes it through. Routing (`Panels/InspectorHost.cpp:176-183`) is untouched.
- **Thumbnail.**
  - Compact form: `thumbSize = min(140, availX - spacing - kPreviewCompactTextColumnMin, clamp(h × innerHeight, floor, 140))`.
  - Stacked form: `min(140, availX, clamp(h × innerHeight, floor, 140))`.
  - `innerHeight` is the page child's height (5.7).
  - `floor` is the Archive cvar `editor.inspector.assetThumbMinPx`: Int, default 64, range 32..140, help "Smallest the asset page's thumbnail shrinks to in a short Inspector".
  - `h` is the Archive cvar `editor.inspector.assetThumbHeightFraction` (drafting pick, 9.28): Float, default 0.30, range 0.1..0.6, help "Share of the Inspector's height the asset page's thumbnail may take".
  - Both are registered with `ARC_CVAR_RANGED` (s2.4), module `"editor"`.
  - The `##previewMeta` child sizes from its content (`ImGuiChildFlags_AutoResizeY`) instead of `thumbSize` (`:292`), so a shrunken thumb never clips the cook row.
- **Ellipsis.**
  - The name's budget is `availX` minus the measured pill run (kind, subkind, "inst" and their spacing), measured before the name is drawn. A cut name shows the full name in its tooltip.
  - The guid uses `EllipsisToWidth`, with the full guid and "click to copy" in its tooltip. Click-to-copy is unchanged.
- **One icon row.** `DrawActionRow(std::span<const PageAction>)`, with `PageAction { const char* icon; const char* tooltip; const char* id; bool enabled; std::function<void()> run; }`.
  - The stable ids are `ICON_LC_EXTERNAL_LINK "##asset_open"`, `ICON_LC_FOLDER_OPEN "##asset_explorer"`, `ICON_LC_FILE_TEXT "##asset_astext"`, `ICON_LC_COPY "##asset_copypath"`, and one kind action: `ICON_LC_LAYERS "##asset_newinstance"`, `ICON_LC_FLAG "##asset_bootscene"` or `ICON_LC_STICKER "##asset_createsprite"`.
  - T5 adds `ICON_LC_PENCIL "##asset_rename"`, `ICON_LC_COPY_PLUS "##asset_duplicate"` and `ICON_LC_TRASH_2 "##asset_delete"`.
  - Each button's `SetItemTooltip` carries today's label text.
  - Buttons that do not fit move into an `ICON_LC_ELLIPSIS "##asset_more"` popup, opened with `BeginPopupBelow` (s4.4), as `MenuItem(icon + label)`. The row is designed for 8+ actions.
  - Actions stay *reported*, never performed here (`AssetPanelActions`).
- **`grid.Section("Derived (N)")`**, open by default, holds the existing `DrawDerivedRow` selectables (`:73-86`).
- **`grid.Section("Import")`** (textures only), open by default. `DrawTextureImportSettings` takes the grid (`Panels/TextureImportSettings.cpp:39-60`):
  - `ComboRow "Format"` {Auto, Bc7, Rgba8};
  - `CheckboxRow "sRGB"`;
  - `CheckboxRow "Generate Mips"`;
  - `IntRow "Max Size"`, 0..16384, with the value tooltip "0 = unlimited".

  It still writes to the `.meta` on commit, with no undo.
- **Separators** are deleted, because the section bands replace them (critique Inspector #10).

**Does not.** It does not change routing or instance filters, perform actions, add undo to the import knobs, or add the T5 verbs, which T5 adds to this row.

**Tests.**
- **`AssetInspectorSourceTest.cpp`**, fit case: `DrawAssetPage` in a **392×330** window for a texture with one derived child.
  - `ScrollMax.y == 0`.
  - Every action is reachable through `ActivateItemByID`, on the row or through `##asset_more`. Actions that don't fit the row move into `##asset_more`.
  - The name and guid ellipsize at 110 px.
  - The ids are re-keyed: `ICON_LC_COPY " Copy Path"` becomes `##asset_copypath`, and `"sRGB##texmeta"` becomes the row id (`:235-307`).
- **Stacked form:** at 229×350 the page uses the stacked form and `thumbSize == min(140, availX, clamp(0.30 × innerHeight, assetThumbMinPx, 140))`.
- **Golden.** `editor-asset-page` (dx12 + vulkan, `golden-gate.ps1:360-361`; witness E4) is re-blessed in T3.
- **Desk:** both sections open and everything visible at 1080p on the user's layout.

### 5.7 Pinned Inspector header

**Problem.** The header and the page body share one window (`Panels/InspectorWindows.cpp:355-381`). Scrolling a tall page scrolls the breadcrumb, the filter and the pin out of view. This affects every page.

**Design.** This is its own small step, and it affects every page.
- In `DrawInspectorWindows`, after `DrawHeader(...)` (`:366`), everything below the header goes into `ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_NavFlattened)`. That includes the "Pinned selection is gone" / "closed" selectable (`:367-377`), `page->Draw(grid)` (`:381`) and the no-source lines.
- **`page->Draw(grid)` is called whether or not `BeginChild` returns true, and `EndChild()` is always called.** The pages' `EditGesture::ScopeGuard` must still run on collapsed, refused-`Begin` and background frames (comment `:350-355`). Widgets bail on `SkipItems` as they do today.
- **Parent-window behaviour.** `Shortcut(Ctrl+S)` (`:361`) and the focus test (`:357`, `RootAndChildWindows`) stay on the parent. The child is in the parent's focus route, so a focused page still saves.
- **Scroll.** Scroll position stays per instance, and the child id is constant per window.
- **Readers of the child's size:** 5.3's `pageHeight` and 5.6's `innerHeight`.

**Does not.** It does not change the header layout, `LayoutInspectorHeader` or routing, and it does not reset scroll on a selection change.

**Tests.**
- **`EditorInspectorHostTest.cpp`**, headless `DrawInspectorWindows` harness:
  - a fake 60-row page scrolled to the maximum keeps the crumb and pin item rects inside the window's visible rect;
  - a collapsed window still calls the fake page's `Draw` once per frame (counter);
  - Ctrl+S with focus inside `##page` reports `saveRequested`.
- **Unchanged:** the existing layout cases (~`:1022`) pass as they are.
- **Goldens.** Slots whose page overflows change pixels, because the scrollbar now starts under the header. They are re-blessed one slot at a time at the end of T3 when 5.3 or 5.6 has not already re-blessed them, identified by the gate going red.
- **Desk:** scroll a long entity page and the asset page at 1080p, and check that the pin stays reachable.

## 6. T4 -- shell state

Answers critique top-10 #5 (Shell #1, #3, #6; Outliner #11) and the shell cheap fixes (Shell #4, #8, #12; Assets #4, #5). **Editor only:** no Core or Runtime change, no ABI bump, no Aphelyon restamp. The runtime never applies the theme (`ArcaneClient/src/Arcane/Host/ProjectBoot.cpp:466` only names it in a comment).

**Gate policy inside T4.** Every item changes pixels in the five editor golden slots. Intermediate steps require the build and ArcaneTests (s2.1). A golden run before 6.10 is expected to fail on the ten editor lanes and only those. 6.10 is the tranche's last commit: it re-blesses all five slots, and T4 goes green there. A failing runtime lane (runtime-scene, f3-cull-blend) at any point in T4 is a regression and is never re-blessed. Order: 6.1 -> 6.2 -> 6.3 -> 6.4 + 6.5 -> 6.6 -> 6.7 -> 6.8 -> 6.9 -> 6.10.

### 6.1 Accent tokens and the tab overline

**Problem (Shell #1).**
- The selected-tab overline is kSelection #2e4053 (`ArcaneEditor/src/Widgets/EditorTheme.hpp:201`), 1.65:1 on the #191919 strip.
- It is 1 px, ImGui's default (`ThirdParty/imgui/imgui.cpp:1555`).
- Unfocused dock nodes draw no overline: `TabDimmedSelectedOverline = kNone` (`:204`).
- There is no accent token. "On" reuses `ImGuiCol_ButtonActive`, which is also the colour of a held button.

**Design.**
- **Tokens (default).** A new `-- ACCENT --` block after SELECTION in `EditorTheme.hpp`:
  - `kAccent` #5b7fa6 = (0.357, 0.498, 0.651)
  - `kAccentHovered` #6386ad = (0.388, 0.525, 0.678) and `kAccentActive` #52769c = (0.322, 0.463, 0.612) (drafting pick, 9.28)
  - The block comment states the split. kAccent means "on / active / playing": toggle-on fills, the tab overline and Play presence. kSelection keeps `Header` (selected rows), `TextSelectedBg` and `DockingPreview`, and gives up the overline. Nothing else adopts kAccent in this phase.
  - s4.9's `kToggleOn` / `kToggleOnHovered` / `kToggleOnActive` (= `kButtonActive` in T2) are re-pointed to `kAccent` / `kAccentHovered` / `kAccentActive`.
- **`ApplyEditorTheme` (`:133`):**
  - `c[ImGuiCol_TabSelectedOverline] = Theme::kAccent`. The existing "selected == accent" comment becomes true.
  - `c[ImGuiCol_TabDimmedSelectedOverline] = Theme::WithAlpha(Theme::kAccent, 0.45f)` (default; `WithAlpha` `:54`). Every dock node now marks its active tab.
  - `style.TabBarOverlineSize = 2.0f`.
  - The metrics comment ("The first of TWO metrics this theme changes") is rewritten to name all four: FrameBorderSize, DockingNodeHasCloseButton, TabBarOverlineSize, DisabledAlpha (6.6).
- **Measured contrast.** The overline is drawn over the tab fill (`imgui_widgets.cpp:10883-10898`). Focused: 4.21:1 on kChrome, 3.99:1 on the kPanel selected tab. Unfocused: the 45% composite is #374758, 1.85:1 on its #191919 tab. That is quieter than focused, but brighter than today's focused overline.
- **Reporter.** No code of its own: `ArcaneCrashReporter/src/ReporterWindow.cpp:535` applies the same theme, and `:536` `ScaleAllSizes` DPI-scales the 2 px overline (`imgui.cpp:1638`).

**Not in scope.** The tab fill ramp (TabSelected kPanel vs Tab kChrome) is unchanged, so the overline carries focus. No recolour of selection, DockingPreview or text selection. No typography change (Shell #5).

**Tests.** The contrast test (6.6) asserts accent/button, overline/chrome, `TabBarOverlineSize == 2` and the 0.45 dimmed-overline alpha. Pixels are covered by 6.10.

### 6.2 Toggles: IconToggle adoption and the hover bug

**Problem (Shell #1, Outliner #3 "Active is one grey step lighter").**
- Three "lit" lambdas each push only `ImGuiCol_Button = ButtonActive` #4b4b4b: transport (`ArcaneEditor/src/Panels/EditorPanels.cpp:667-676`), Play caret (`:841-844`) and viewport overlay (`:1252-1260`). Lit vs unlit is 1.54:1.
- **Found in code:** hovering a lit toggle paints ButtonHovered #3d3d3d, darker than lit, so "on" vanishes under the cursor. Pressing an unlit button paints #4b4b4b, exactly the "on" look.
- A fourth "on" language: Input Actions Preview uses amber at 35% (`Documents/InputActionsDocumentWidgets.cpp:234-236`).
- `SegmentedStrip` (`Widgets/EditorWidgets.cpp:711`, declared `EditorWidgets.hpp:221`) has no callers.

**Design.** The helper is s4.9's single definition: `PushToggleOnColors()` / `PopToggleOnColors()` / `IconToggle(const char* label, bool on)`, with no tooltip. `PushToggleOnColors` (s4.9) now resolves to the accent trio through the re-pointed `kToggleOn*` tokens (6.1). Adoption:
- **Transport.** Play/Stop and Pause call `IconToggle`, and the lambda at `:667` is deleted. Tooltips stay each site's own `IsItemHovered` code. Step keeps `iconBtn`.
- **Caret.** `:841-844` becomes `IconToggle(ICON_LC_CHEVRON_DOWN "##sim_playmode", playing)` inside its existing FramePadding push, so it stays welded to a lit Play.
- **Viewport overlay.** 2D/Persp and Select/Move/Rotate/Scale call `IconToggle`, and the lambda at `:1252` is deleted. The panel's `tooltip` lambda (AllowWhenDisabled plus the alpha restore) stays as it is.
- **Input Actions Preview.** While `state.previewArmed`, its `Button` is wrapped in `PushToggleOnColors` / `PopToggleOnColors`. Label and glyph are unchanged.
- **Deletions.** `SegmentedStrip`, its declaration and comment, and its mention in the widget list at `EditorWidgets.cpp:284`.
- **Result.** A lit toggle is 3.21:1 against unlit, and its icon is 3.16:1 on the fill. Hover lightens (kAccentHovered, 3.53:1 vs kButton) and holding darkens (kAccentActive). Pressing an unlit toggle stays grey #4b4b4b, 2.09:1 from lit.

**Not in scope.** Local/World stays a glyph swap with no lit state. The overlay's ambiguous glyphs are untouched (the cube means both Perspective and Local; Move vs Scale differ only in line ends, Outliner #3). Preview keeps its Play glyph (Input #9). The shader editor's Live checkbox stays a checkbox. No tooltip text or behaviour changes.

**Tests** (`EditorWidgetsTest.cpp`, s4.9's cases on the re-pointed tokens):
- Inside `PushToggleOnColors` the three entries equal `kAccent` / `kAccentHovered` / `kAccentActive`.
- Hover-bug pin: a hovered `on` toggle's frame-fill vertices carry `GetColorU32(kAccentHovered)` and never `kButtonHovered`; an unhovered `on` toggle carries `kAccent`; a hovered `off` toggle carries `kButtonHovered`.
- Grep gate: no `GetStyleColorVec4(ImGuiCol_ButtonActive)` pushed as `ImGuiCol_Button` remains in `ArcaneEditor/src`, and no `SegmentedStrip` remains.

### 6.3 Play presence

**Problem (Outliner #11, Shell #1).** The only Play cue is the hidden tool overlay (`/*showToolOverlay=*/!InPlayMode()`, `App/EditorAppFrame.cpp:3477`). Nothing tints the viewport or the strip.

**Design (default: 2 px frame + accent Stop, no strip tint).**
- **Parameter struct (drafting pick, 9.28).** `DrawViewportPanel`'s `bool showToolOverlay` (`Panels/EditorPanels.hpp:326`) becomes `const ViewportChrome& chrome`:
  ```cpp
  struct ViewportChrome
  {
      bool showToolOverlay = true;
      bool playing         = false;   // 6.3: the accent frame
      bool sceneDirty      = false;   // 6.4: the Viewport tab's unsaved dot
  };
  ```
  The one caller (`EditorAppFrame.cpp:3477`) passes `{ !InPlayMode(), InPlayMode(), CurrentTitleParts().sceneDirty }`.
- **Frame.** When `chrome.playing` is set and an image was drawn (`textureId != 0 && texW > 0 && texH > 0`), four 2 px `AddRectFilled` bands in kAccent are drawn INSIDE `r.imageRect` (`EditorPanels.cpp:1270-1271`). They go on the window draw list after `imageOverlay` and before the tool overlay (hidden in Play anyway). Drawing inside the rect avoids stroke half-pixel arithmetic and bleed into window padding. The frame is draw-only: `r.hovered`, click capture and game input are unchanged.
- **Stop.** Play/Stop is `IconToggle(on = playing)` (6.2), so while playing it is an accent-filled Stop square with a lit caret. There is no separate Stop control (drafting pick, 9.28). Pause lights while paused, and the frame stays while paused.
- **Topologies.** `playing` = `InPlayMode()` = `PlaySession::IsPlaying()` (`App/EditorApp.hpp:853`). That covers in viewport, listen server, client + embedded server, and client + separate server process. A Separate-window launch never frames the viewport, for the same reason it never lights the button (`EditorPanels.cpp:750-756`).
- **Same frame.** The viewport draws after the toolbar (`EditorAppFrame.cpp:465-467`), so a Stop clicked this frame removes the frame this frame.

**Not in scope.** No whole-strip tint. No frame on documents docked over the viewport. No cvar: the colour is a theme token, not a tunable.

**Tests.** `EditorPanels.cpp` is not compiled into ArcaneTests (`premake5.lua:1230,1354`), and goldens never enter Play, so 6.3 alone changes no slot. Desk at 1920x1080:
- in viewport: frame, accent Stop, lit caret;
- Pause: Pause lit, frame stays;
- Stop: frame gone on the same frame;
- Separate window: no frame;
- client + separate server process: frame.

### 6.4 Top-strip status, EditorTitle, and the scene-dirty tab dot

**Problem (Shell #3).**
- Project, scene and unsaved state show only in the OS title (`App/EditorApp.cpp:1792-1808`), composed by an anonymous-namespace `EditorTitle` (`:97-135`).
- The strip's right side is empty.
- The Viewport window opens with no flags (`EditorPanels.cpp:1263`), while every document raises `ImGuiWindowFlags_UnsavedDocument` when dirty (`ShaderEditorDocument.cpp:1918`, `MeshDocument.cpp:421`, `SpriteDocument.cpp:163`, `InputActionsDocument.cpp:211`).

**Design.**
- **New `App/EditorTitle.hpp`.** Header-only and pure: std only, no Project or SceneSession includes.
  ```cpp
  struct TitleParts
  {
      std::string project;          // Manifest().name; empty = no project open
      std::string scene;            // SceneSession::DisplayName(): "Untitled" or the file stem
      bool        sceneDirty = false;
  };
  // Today's OS title, byte for byte ("<project> - <scene>* - <buildInfo> <<backend>>").
  [[nodiscard]] std::string FormatOsTitle(const TitleParts&, std::string_view buildInfo,
                                          std::string_view backend);
  // The strip's plain text: "<project> > <scene>", plus " *" when dirty;
  // "No project" alone when `project` is empty.
  [[nodiscard]] std::string FormatStripStatus(const TitleParts&);
  ```
  - `EditorTitle` and its stale comment (`EditorApp.cpp:90-96`, "never from a bare launch") are deleted. The new header's doc comment does not repeat that claim; the start page (s8.4) depends on the project-less state being reachable.
  - `UpdateWindowTitle` calls `FormatOsTitle(CurrentTitleParts(), Arcane::BuildInfo(), backend)`. Passing `BuildInfo` in keeps the header DLL-free and testable.
- **One composition.** `TitleParts EditorApp::CurrentTitleParts() const` is the only place the parts are assembled: the project from `m_runtime->CurrentProject()`, `m_scene.DisplayName()`, and `m_undo && m_scene.IsDirty(*m_undo)` (the guard at `:1796`). The OS title, the strip and `ViewportChrome` (6.3) all read it.
- **Dirty** is whatever `SceneSession::IsDirty` says. After T1 (s3.3) that counts only scene-affecting steps, so a material edit stars neither the title nor the strip.
- **Strip drawing** (placed by the 6.5 cluster):
  - project in TextDisabled;
  - `ICON_LC_CHEVRON_RIGHT` in TextDisabled (the Inspector breadcrumb separator, `Panels/InspectorWindows.cpp:271`);
  - scene in Text, then ` *` in Text when dirty;
  - with no project, "No project" alone in TextDisabled.

  Read-only and not clickable. The hover tooltip has three lines: the `FormatStripStatus` text; "Scene file: <SceneSession::Path()>" or "Scene not saved yet"; and "Unsaved changes" when dirty.
- **Viewport tab dot.** When `chrome.sceneDirty` is set, the call is `ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_UnsavedDocument)`. The Viewport shares its dock node with documents, so the scene's dot sits beside theirs. ImGui widens a marked tab by one glyph (`imgui_widgets.cpp:10052-10053`), so tabs to its right shift while the scene is dirty.
- **Timing.** `CurrentTitleParts()` is sampled once before the toolbar, where `EditorAppFrame.cpp:2239` already refreshes the title. A change consumed later in the frame shows on the next frame, as the OS title does today.

**Not in scope.** No document dirtiness in the strip (tabs carry their own dots; `DocumentHost::AnyDirty` is not read). No click-through or scene switcher. The Outliner root label "Scene", the OS title text and the chrome height (~70 px, now carrying state) are unchanged.

**Tests.**
- New `ArcaneTests/src/EditorTitleTest.cpp`:
  - `FormatOsTitle` reproduces today's `EditorTitle` exactly. Literal strings are captured from the current function BEFORE the move, for: project + scene + dirty + backend; no project ("Untitled - <build> <dx12>"); empty backend (no " <>"); empty scene.
  - `FormatStripStatus`: "Ref > main", "Ref > main *", "Ref > Untitled", and "No project" (with a scene, and with a dirty scene).
- `EditorSceneSessionTest.cpp:65-124` stays green unchanged.
- Desk (dot): edit an entity and the dot appears; undo to the save point and it clears; edit then Save and it clears.

### 6.5 The right-side status cluster (shared with s8.2's Problems chip)

**Problem.** The strip's right side has two decided occupants, the 6.4 status and T6's Problems chip (s8.2). One layout owns both, so two right-aligners never fight over the edge.

**Design** (drafting pick, 9.28: `ToolbarResult` and the `StripChip` slot contract). In `Panels/EditorPanels.hpp`:

```cpp
struct StripChip
{
    std::string label;      // drawn text; the chip's ImGui id is "##strip_problems"
    ImVec4      color;      // s8.2 picks it from the worst severity
    std::string tooltip;
};
struct ToolbarStatus
{
    TitleParts               title;      // 6.4
    std::string              scenePath;  // tooltip only; empty = never saved
    std::optional<StripChip> problems;   // s8.2 (T6); nullopt = nothing drawn, no space reserved
};
struct ToolbarResult
{
    bool launchStandalone    = false;    // today's bool return
    bool launchServer        = false;    // today's launchServerRequested out-param
    bool problemsChipClicked = false;    // s8.2 routes it
};
[[nodiscard]] ToolbarResult DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                                               Arcane::PluginHost* host, PlayLaunchMode& mode,
                                               uint64_t logoTex, const ToolbarStatus& status);
```

- **Signature.** The bool return and the out-param (`EditorPanels.cpp:645-649`; the one call site `EditorAppFrame.cpp:2269-2272`) fold into `ToolbarResult`.
- **Placement.** Drawn after the transport and before the closing `Dummy` (`:921`), with absolute `SetCursorPos` like the left cluster. The transport's placement and the strip height never move.
- **Geometry.**
  - `rightEdge = lineStartX + fullContentW - 8` mirrors `leftPad` (`:698`).
  - Items run right to left, status rightmost then the chip, separated by `2 x ItemSpacing.x`. Both are centred vertically on the button row, as the wordmark is (`:713`).
  - `minX` = transport right edge + 12 px, mirroring the left clamp (`:745`).
- **Narrow windows.** The chip never shrinks (it is a count). The status elides the project first, then the scene, with `EllipsisToWidth` (`Widgets/EditorWidgets.hpp:91`), keeping the chevron and ` *`. If "..." + chevron + "..." does not fit, the status is not drawn. The tooltip always carries the full text.
- **Pure layout** in `EditorWidgets.hpp`:
  ```cpp
  struct StripClusterLayout { float chipX; float statusX; float statusBudget; bool drawStatus; };
  [[nodiscard]] StripClusterLayout LayoutStripCluster(float minX, float rightEdge, float chipW,
                                                      float statusNaturalW, float statusMinW, float gap);
  ```
  `chipW == 0` means no chip and no gap.
- **T4 vs T6.** T4 ships `problems == nullopt`. s8.2 fills it, and leaves it `nullopt` under `UnderVerifyHarness`, so goldens never show the chip.

**Not in scope.** No chip in T4. Not a general item bar or registry. The status is not clickable.

**Tests.**
- New `ArcaneTests/src/EditorStripLayoutTest.cpp`, `LayoutStripCluster`:
  - wide (1920 px): status ends at `rightEdge`, chip left of it at the gap;
  - no chip: no gap reserved;
  - narrow: chip keeps its width while `statusBudget` shrinks; `drawStatus` false below `statusMinW`;
  - no x is ever below `minX`.
- Desk at 1920x1080: status right-aligned; the transport's x unchanged against a pre-T4 capture.

### 6.6 Dim text, DisabledAlpha, and the contrast test

**Problem (Shell #6).**
- `kTextDim` #737373 (`EditorTheme.hpp:93`) is 3.52:1 on kPanel. It reaches the screen two ways:
  - `ImGuiCol_TextDisabled` (`:140`): ~115 `TextDisabled` calls and 12 colour reads across 21 files, plus ImGui's hint text and menu shortcuts (`imgui_widgets.cpp:5528`, `:9663`);
  - ~20 direct `Theme::kTextDim` draw-list uses.
- `DisabledAlpha` is ImGui's stock 0.6 (`imgui.cpp:1519`). Disabled kText composites to #929292 (5.36:1), so disabled text is brighter than dim text today. Raising dim text alone would make the two indistinguishable.

**Design (default).**
- `kTextDim` -> #8e8e8e (0.557, 0.557, 0.557): 5.09:1 on kPanel, 5.37:1 on kChrome, 5.72:1 on kWell.
- `style.DisabledAlpha = 0.45f` in `ApplyEditorTheme`. Disabled kText composites to #757575 (3.62:1, dimmer than dim text by 1.41:1); disabled dim text to #505050.
- One token edit reaches both routes, so no call site changes.
- The `tooltipAlpha` comment in `DrawViewportPanel` (`EditorPanels.cpp:1229-1234`, "come out at 60%") stops naming a number.

**Not in scope.** Literal greys outside the token (Asset Graph legend swatches, AssetPill's kGrab text). No per-surface re-dimming. No font-weight change (Shell #5).

**Tests.**
- New `ArcaneTests/src/EditorThemeContrastTest.cpp`. It includes `<Widgets/EditorTheme.hpp>` (as `PropertyGridTest.cpp:6` does) and computes WCAG relative luminance from the float tokens. Its `ImGuiStyle` is filled by `ApplyEditorTheme`; `StyleColorsDark` takes a destination pointer, so no context is needed. Assertions:
  - `kTextDim` / `kPanel` >= 5.0;
  - `kAccent` / `kButton` >= 3.0;
  - `kAccent` / `kChrome` >= 3.0 (overline);
  - `kText` on `kAccent` >= 3.0 (a lit toggle's icon);
  - luminance of composite(`kText`, `kPanel`, `style.DisabledAlpha`) < luminance of `kTextDim`;
  - `style.TabBarOverlineSize == 2`, `style.DisabledAlpha == 0.45f`, `TabDimmedSelectedOverline` alpha `== 0.45f`.
- Resting state only: an icon on the hovered accent is 2.87:1, under the 3:1 bar, and hover is transient (drafting pick, 9.28).
- Desk:
  - the Outliner filter's dimmed ancestors (`EditorPanels.cpp:1824,1893,2048`) still read as ancestors next to matches (#8e8e8e vs #e0e0e0);
  - the Console time and channel columns;
  - Pause and Step greyed in Edit;
  - disabled menu items still explain themselves.
- The reporter has no golden. Its dim and disabled text is desk-checked with s8.1's reporter fixes.

### 6.7 "0 refused"

**Problem (Shell #4, Assets #4).**
- `DrawAssetPanelHealthDigest` formats "<triangle> %d refused" and draws it in kAmber unconditionally (`Panels/AssetPanelCommon.cpp:565-579`). It is shared by the Browser (`AssetBrowserPanel.cpp:966`) and the Graph (`AssetGraphPanel.cpp:2633`).
- Status's refused tile always passes variant 1 (`AssetStatusPanel.cpp:668`), so its icon is amber at zero (`EditorWidgets.cpp:917`).

**Design.** A pure helper in `Panels/AssetPanelCommon.hpp`:

```cpp
struct RefusedStyle
{
    bool        alarm;        // refused > 0
    std::string text;         // ICON_LC_TRIANGLE_ALERT " N refused" when alarm, else "0 refused"
    ImVec4      color;        // Theme::kAmber when alarm, else Theme::kTextDim
    int         tileVariant;  // StatTile variant: 1 when alarm, else 0
};
[[nodiscard]] RefusedStyle DigestRefusedStyle(int refused);   // refused <= 0 reads as 0
```

- The digest draws `style.text` in `style.color`. Its width is still measured as the refused part plus the rest.
- The Status tile passes `style.tileVariant`, so at zero the icon is drawn in Text colour.
- The meter legend's amber swatch stays (`AssetStatusPanel.cpp:683`), because it is a key. Per-row badges are already conditional (`AssetBrowserPanel.cpp:575-588`, `AssetStatusPanel.cpp:240`).

**Not in scope.** The "cooking" and "unused" parts, zero-greying of other counts, and the digest click-through are unchanged.

**Tests.**
- `ArcaneTests/src/AssetPanelCommonTest.cpp`: 0 -> `alarm` false, no triangle in `text`, `color == kTextDim`, variant 0; 1 and 12 -> alarm, amber, `text` starts with `ICON_LC_TRIANGLE_ALERT`, variant 1; -3 -> as 0.
- The digest click test (`AssetsGraphCanvasTest.cpp:1106`) stays green. It clicks 20 px inside the right edge, which the shorter zero text does not move.

### 6.8 The Preferences item

**Problem (Shell #12).** `EditorPanels.cpp:226` `MenuItem("Preferences...")` is enabled and does nothing. The menu comment at `:78-85` and the inline comment at `:222-225` still call both items placeholders, but Project Settings is wired (`:227-228`).

**Design.**
- `ImGui::MenuItem("Preferences...", nullptr, false, false)`, then `if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Settings window coming in the configuration pass");`. This is the house pattern at `:127-128`.
- The item stays in place as the future settings window's home over the cvar registry (s10.2).
- Both comments are rewritten to say so and to name Preferences as the only remaining placeholder.

**Not in scope.** No settings window and no cvar enumeration. The single-item View menu and the footer wording (the rest of Shell #12) are untouched.

**Tests.** Desk only (`EditorPanels.cpp` is not in ArcaneTests): the item is greyed and the tooltip shows on hover.

### 6.9 Asset Graph: selection strip, legend, and fit

**Problem (Shell #8, Assets #5).**
- The canvas is the whole body: `canvasSize = GetContentRegionAvail()` (`Panels/AssetGraphPanel.cpp:1349-1350`), then `ed::Begin` (`:1377`).
- The 48 px selection strip is a child overlaid on the body's bottom edge (`:2538`, `:2548-2551`). It also steals hover from the bottom 48 px of canvas.
- The legend sits at canvas bottom - 12 - boxH (`:1198-1200`, drawn `:2301`), which is inside the strip.
- Rows are 90 px apart and nodes 54 px tall, so three rows need 234 px against ~207 visible.
- No framing: positions are set only on rebuild (`:1647-1649`), and there is no canvas persistence (`:1319`).

**Design.**
- **One strip constant.** The local `kSelectionStripH` (`:2538`) moves to `AssetGraphPanel.hpp` as `inline constexpr float kAssetGraphSelectionStripH = 48.0f;`. The strip and the canvas both read it.
- **Shrink the canvas.** In `DrawAssetGraphBody`, `canvasSize = GetContentRegionAvail() - ImVec2(0, kAssetGraphSelectionStripH)`, with y clamped to >= 0 before the existing `<= 0` early-out. The backdrop, the empty-graph message, `ed::Begin`'s height and `DrawGraphLegend` all take the shrunk size. The legend and nodes now sit above the strip, and the strip covers no canvas.
- **Fit triggers.** The fit is wired here, beside the shrink (drafting pick, 9.28), because a fit measured before the shrink frames the bottom row under the strip. `AssetGraphPanelState` gains `CanvasNavLatch graphFitPending;` (`Widgets/GraphFit.hpp`) and a test seam `std::uint32_t graphFitCount = 0;` beside `graphLayoutDirty` (`AssetGraphPanel.hpp:167`). (Amended at T4-A11, per the T2-C4 ruling: a latch, not a `bool`. The node editor's `Begin` discards a navigation issued the draw before a canvas resize, so a one-shot `bool` loses the creation fit to the first-frames resize. The latch re-issues the fit until a draw at the same canvas size confirms it, then disarms.)
  - **Armed** in the lazy-create branch (`AssetGraphPanel.cpp:1312`). It is also armed in the rebuild branch (`:1293-1309`) iff `!graphBuilt || graphBuiltFocus != graphFocus || graphBuiltKindFilter != graphKindFilter`, evaluated BEFORE those fields are overwritten. An `entriesStamp`-only rebuild (cook churn, a new asset) never arms it.
  - **Consumed** through the latch: `graphFitPending.Update(ed::GetScreenSize(), now, 0.0f, /*canIssue*/ !applyLayout)` runs on every canvas draw while armed, so the layout-write draw sees the canvas size but cannot issue. The fit is first issued on the first canvas frame with `!applyLayout` (`:1386`): positions were written the frame before, and node sizes are measured by then. It calls T2's `GraphFitToContent(editor.graph.fitMaxZoom, 0.0f)` (s4.5). A draw at a different canvas size re-issues it; a later draw at the issued size confirms it and disarms the latch. `graphFitCount` counts one fit per arming (the first issue, `CanvasNavLatch::Issues() == 1`), never a re-issue. An empty graph, or a fit that returns false, disarms the latch without fitting.
  - The fit runs after step 8b's select-and-centre (`:2106-2119`), so when both fire on one frame the fitted view wins. 8b's `SelectNode` mirror still happens.
  - The fit never touches selection (s4.5). A panel resize after the latch confirmed does not refit. F keeps the library's own frame (`imgui_node_editor.cpp:3355`).

**Not in scope.** Edge-label clutter (Assets #6) is still owed. The empty left quarter is not traced, because each trigger's fit replaces the view. Row pitch and node height are unchanged. Still no canvas persistence (the panel's "Ruling 2: NO canvas persistence", `AssetGraphPanel.cpp:1315`).

**Tests** (`ArcaneTests/src/AssetsGraphCanvasTest.cpp`, `GraphMouseHarness` node rects via `ed::GetNodePosition` / `GetNodeSize` / `CanvasToScreen`, `:551-645`, on a fixture with >= 3 layout rows):
- after 4 frames, every node rect and the legend box lie inside the canvas rect, and canvas bottom == strip top (body bottom - `kAssetGraphSelectionStripH`);
- creation gives `graphFitCount == 1`; an `entriesStamp` bump (`MarkAllDirty` plus a provider change) leaves it at 1; a focus change -> 2; a kind-filter change -> 3;
- a fit frame leaves `model.selected` and `ed::GetSelectedObjectCount()` unchanged.

### 6.10 The single batched golden re-bless

**Slots.** All five editor slots in `ReferenceProject/Verify/References/`. Each is ExpectedLevel `shared`: one PNG serves both backends and both configs (`scripts/golden-gate.ps1:335-363`).

| Slot | Lane ExtraArgs |
|---|---|
| editor-ui | (none) |
| editor-ui-perspective | `--view-mode perspective` |
| editor-input-doc | `--open-asset 97260310-8b35-4b29-b12f-1fd6f8e99071 --select-in-document Player/Jump` |
| editor-asset-page | `--select-asset d7f389fd-f687-407d-b9d7-9753eb6b0258` |
| editor-material-page | `--open-asset 7e5a0010-0010-4010-8010-000000000010` |

- Every other file under `Verify/References/` stays untouched and green: runtime-scene, f3-cull-blend, `vulkan/`, inprocess-lit-cube and `thumbs/`. A diff there is a regression.
- T6 runs after T4 (s2.1), so `editor-input-doc`'s Input Actions change is its own slot re-bless in T6, by the standard procedure.

**Allowed diffs:**
- the 2 px accent overline on focused tab bars and the 45% overline on the rest;
- accent-lit viewport overlay toggles, where the viewport is visible (editor-ui, -perspective, -asset-page);
- brighter dim text;
- dimmer disabled widgets (Pause, Step);
- the strip status at the right;
- the digest's "0 refused" dim, with no triangle;
- the Asset Graph canvas and legend moved, wherever the graph is visible.

The scene is clean in every lane, so no tab dot appears. Any other delta (a moved panel, a shifted transport, a changed count) stops the batch. Fix it; don't bless it.

**Procedure** (bless the STAGED slot, copy it to source IMMEDIATELY):
1. **Preconditions.** 6.1-6.9 are committed. `Arcane.slnx` is built in Debug and Release. ArcaneTests is green except the `[witness]` cases that compare editor slots. `git status --porcelain -- ReferenceProject` is empty. The exe-dir `imgui.ini` is deleted.
2. **Gate first.** `scripts\golden-gate.ps1` in Debug: exactly the 10 editor lanes Failed, and the 4 runtime lanes Passed or PassedOnFallback. Read every staged `ReferenceProject\Saved\Verify\<slot>-<backend>-diff.png` against the allowed list.
3. **Bless each slot once, dx12**, from the Debug editor's exe dir (`bin\Debug-windows-x86_64-md\ArcaneEditor\`). The relative `--project` resolves to the staged tree:
   `ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --report <scratch>\<slot>.json --compare <slot> --bless <lane ExtraArgs>`
   Read each run's log for new warnings.
4. **Copy immediately** after each bless: staged `...\ArcaneEditor\ReferenceProject\Verify\References\<slot>.png` over source `ReferenceProject\Verify\References\<slot>.png`, then view it. The gate rebuilds ReferenceProject, and its post-build restages `Verify\`, which clobbers a staged-only bless.
5. **Gate both configs, Release then Debug.** The last run decides which config's ReferenceGame.dll sits in the single-slot `ReferenceProject\Binaries\`, so end on Debug. Expect 14/14 and 0 diff px. A lane that passes in one config and fails in the other means the editors render differently: investigate it, and never bless per config.
6. **Witnesses.** Run ArcaneTests `[witness]` from its exe dir (`EditorWitnessTest.cpp`):
   - E2 `:165`, E3 `:227`, E4 `:268`, E5 `:303` and E6 `:379` (the default-layout editor-ui witness) compare these slots and must be green;
   - E8 (T1 mesh preview) and E9 (T3 node page) compare no slot, but read document and Inspector state that T4 re-lays out, and must be green too.
7. **One commit**, only the five PNGs. Its message names T4 and the surfaces changed. It is the tranche's last commit.

## 7. T5 -- asset file operations

Answers critique Assets #1 (no rename/delete/duplicate/move), #3 (single selection, count hard-coded 0/1, one-guid drag), #11 (keys stop at Up/Down/Enter) and top-10 #6. The brainstorm's asset-ops code map (cited below as "map §N", beside the Arcane undo baseline "arcane-baseline §N" and the UE survey "ue-asset-file-ops"; evidence files, not checked in; re-checked at `57af0cab`) corrects two critique claims:

- **Rename/move need no reference rewriting:** content references are by guid (`Assets.cpp:1100-1206`, `Project.hpp:95-105`); the registry maps guid → mount path only (`AssetRegistry.hpp:3-23`, `:111`).
- **The reference index is not enough for Delete:** it reads on-disk JSON only (missing the unsaved scene, dirty documents, the `.arcproj` manifest) and counts one hop.

So every operation is a filesystem transaction: planned purely, executed all-or-nothing, pushed as one undoable step on the shared stack.

### 7.1 Scope and refusals

**Problem.** The row menu has only kind verbs, Create, Show in Explorer, Open as text, Copy Path/Guid, Focus in Graph, Reveal in Browser (`AssetPanelCommon.cpp:299-359`); the action struct has no file-op fields (`AssetPanelCommon.hpp:53`). Users fall back to Explorer, which causes the damage ranked below.

**Scope v1.** Rename, Duplicate, Delete, Move, New Folder on `game://` (`<project>/Content`) only; `diag://` (`<project>/Saved/Diagnostics`, `Project.cpp:281-282`, `:494`) is delete-only.

**Gates** (UI and executor both check; a failed gate disables the item with the reason as tooltip):

| Gate | Reason |
|---|---|
| Project open | "No project open" |
| Edit mode (`CanEditStructure` precedent, `EditorPanels.cpp:1434-1438`) | "Stop Play to change asset files" |
| `!m_undo->InTransaction()` (a file step must never join a cancellable gesture, `RegistryStateCommand.cpp:63-68`) | "Finish the current edit first" |

**Refusals** (planner, 7.3; any refusal blocks the whole batch; shown as tooltip or a delete-modal line):

| Case | Message |
|---|---|
| `source://`, any verb | "C++ source: rename or move it in your IDE (a source file's identity is its path)." (guid = `Guid::FromName(path)`, `AssetRegistry.hpp:17-23`) |
| `plugin/<name>://`, `engine://` | "Plugin and engine content is read-only here." |
| `diag://` Rename/Duplicate/Move | "Crash reports can only be deleted." |
| Move outside `game://` | "Can't move across mounts." |
| Delete the open scene (`SceneSession::Id()`, `SceneSession.hpp:38-41`) | "This scene is open. Open another scene first." |
| Delete the boot scene (`BootSceneGuid`, `AssetPanelCommon.hpp:212`) | "This is the project's boot scene. Set another boot scene first." |
| Destination exists (primary, `.meta` or `.gltf` companion) | "`<name>` already exists in `<folder>`." (case-only rename excepted, 7.6) |
| `ValidateCreateName` rules 0-2 fail (`CreateAssetDialog.hpp:330-385`) | That rule's message |
| `.gltf` Move, relative URI leaves its folder (`..`, absolute) | "References `<uri>` outside its folder." |
| `.gltf` Move, buffer/image shared with a non-moving `.gltf` | "Shares `<file>` with `<other>`." |
| Guid no longer registered | "No longer exists." |
| Registered path missing on disk | "Missing on disk. Reopen the project to rescan." |

**Dangerous cases, ranked (asset-ops map §9, extended):**

| # | Case | Closed by |
|---|---|---|
| 1 | Plain-copy duplicate keeps the `"id"`/`.meta` guid: two files, one guid, registry keeps the first (`AssetRegistry.cpp:431-450`) | id rewritten before register (7.7) |
| 2 | Save after rename/delete writes the old path back (same guid) | `NoteMoved` (7.11); documents closed first (7.5) |
| 3 | Move without the sidecar, or in Explorer, mints a new guid (`AssetRegistry.cpp:97-130`) | `.meta` always moves (7.3); Explorer out of scope |
| 4 | No rebind: `AddFile` keeps the old path (`AssetRegistry.cpp:431-450`) | `Rebind`/`Remove` (7.2) |
| 5 | Referenced one hop away or only in memory | referencer union + cascade (7.5) |
| 6 | `.gltf` moved away from `.bin`/images | companions travel or refuse (7.8) |
| 7 | Companion `.arcmesh` copy makes `UniqueImportedCompanion` ambiguous (`MeshImportWave.hpp:198-203`) | `importedSource` stripped (7.7) |
| 8 | `source://`/`diag://`/plugin/engine rename; cross-mount move | refusal table |
| 9 | Ctrl+D/X/C/V reach entity ops from the Browser (`EditorAppFrame.cpp:944-956`, `:2646-2650`) | key routing (7.10) |
| 10 | Case-only rename "already exists" on Windows | `ValidateRenameName` (7.6) |
| 11 | *(new)* `.arcscene` copy repeats every `Arcane::Identity.id` (`Components.hpp:284-296`) | re-minted (7.7) |
| 12 | *(new, map §6)* path-keyed `Assets` memos remember contents and failures (`Assets.cpp:480-515`, `:372-386`): rename A→B, create A, old contents served | `Assets::EvictPath` (7.2, 7.12) |
| 13 | *(new, map §2)* deleted guid stays `selected` ("1 selected") | `PruneSelection` (7.9) |

**Out of scope v1:** folder rename/delete/move (folder rows are drop targets and New Folder parents only); detecting Explorer changes (the watcher ignores deletes/renames, `EditorAppProject.cpp:553-556`); Replace References; game code loading via the path overloads of `Assets::GetJson`/`GetBytes` (`Assets.hpp:84-94`); an asset clipboard (7.10).

**Tests.** `AssetFileOpsTest.cpp`: one fake-facts planner case per refusal row.

### 7.2 Registry Remove/Rebind + Runtime seams

**Problem.** `AssetRegistry` has only `ScanContent`, `AddContent`, `AddFile`, `Resolve`, `All`, `Count`, `Clear` (`AssetRegistry.hpp:80-108`): a moved file stays mapped to its old path even after `RegisterCreatedAsset` (`AssetRegistry.cpp:431-450`), and a deleted guid resolves until reopen.

**Design (Core).**

- **`static std::optional<Guid> AssetRegistry::PeekId(const std::filesystem::path&)`** reads the id as the scan would -- embedded `"id"` (native JSON), `.meta` `"guid"` (imported), envelope `"guid"` (`.arcdiag`) -- and **never mints or writes**; nullopt for source files, unknown kinds, unreadable ids. It is the read halves of `ResolveNativeId`/`ResolveSidecarId`/`ResolveDiagId` (`AssetRegistry.cpp:62-130`) factored out; write-back stays in the resolvers.
- **`bool AssetRegistry::Remove(const Guid&)`** erases the mapping (false if unknown), drops that id's `DiagLocator::Asset(id)` rows from `m_scanDiagnostics` and republishes `"assets"` whole-set as `ScanContent` does (`AssetRegistry.cpp:244`, `:278`).
- **`RebindResult AssetRegistry::Rebind(const Guid& id, const std::filesystem::path& newFile, const std::filesystem::path& contentDir, std::string_view scheme)`**, `enum class RebindResult : std::uint8_t { Ok, UnknownGuid, NotTrackable, OutsideContent, CrossMount, IdMismatch, PathTaken, NoProject }`: `source` → NotTrackable; mount path computed as `AddFile` does, outside `contentDir` → OutsideContent; scheme ≠ the guid's current mount → CrossMount; `PeekId(newFile) != id` → IdMismatch; path owned by another guid → PathTaken (linear scan, once per op). Ok assigns `m_byGuid[id]`; never mints, writes or publishes.
- **`Project`:** `bool UnregisterAsset(const Guid&)`; `RebindResult RebindAsset(const Guid&, const std::filesystem::path& newFile)` (finds the containing root from the list `RegisterAsset` walks, `Project.cpp:~491-494`).
- **`Runtime`** (beside `RegisterCreatedAsset`, `Runtime.hpp:209-221`): `bool UnregisterAsset(const Guid&)`; `RebindResult RebindMovedAsset(const Guid&, const std::filesystem::path&)` (NoProject without one). GUID loads see both at once (the resolver reads the live registry, `Runtime.cpp:514-515`).
- **`Assets` facade**, two virtuals appended per "NEW VIRTUALS GO AT THE END" (`Assets.hpp:175`, `:216`):
  - `virtual void EvictPath(const std::filesystem::path& resolved) = 0;` -- drops every path-keyed memo (JSON, bytes, pixels, texture info; success or failure) for that `CacheKey` (`Assets.cpp:72`).
  - `virtual void ForgetUnresolved(const Guid& id) = 0;` -- erases the id from `m_idFailures`/`m_unresolvedDiagnosticIds` and republishes `"assets.unresolved"` (today cleared only on resolver install, `Assets.cpp:334-348`).
- **One Core ABI bump** (s2.3 T5 row): Aphelyon restamp + module rebuild.

**Not in scope.** Rescan or watcher pruning; `ScanContent` still runs only at open.

**Tests.** `AssetRegistryTest.cpp` (TempDir + ScanContent, `:36-115`): `PeekId` writes nothing (id-less native file byte-identical; no `.meta` created); `Rebind` moves a native asset and an imported binary + `.meta`, `Resolve` returns the new path; `Rebind` refuses IdMismatch (`.meta` left behind), CrossMount (`plugin/…` root), PathTaken, NotTrackable, map untouched each time; `Remove` → `Resolve` nullopt and the duplicate-id row retracted. `AssetsTest.cpp`: `EvictPath` after an in-place replace serves the new JSON and clears a remembered failure; `ForgetUnresolved` retracts exactly one row.

### 7.3 AssetFileOps planner/executor

**Design.** `ArcaneEditor/src/Project/AssetFileOps.{hpp,cpp}`: one unit answers, for every verb, which files move, what refuses, who references what and which documents are touched, then executes and follows up identically.

**Planner (pure, headless)** -- the brief's `PlanAssetOp(op, guids, dest, registry, RefIndex, extraReferencers)`, grouped:

```cpp
enum class AssetOpKind : std::uint8_t { Rename, Duplicate, Delete, Move, NewFolder };

struct AssetOpRequest {
    AssetOpKind               kind;
    std::vector<Arcane::Guid> guids;           // Rename: exactly 1; NewFolder: empty
    std::string               newStem;         // Rename; NewFolder: the folder name
    std::string               destFolder;      // Move/NewFolder: relative to Content/, "" = root
    bool                      cascadeDerived = true;   // Delete (7.5)
};

struct FileMove { std::filesystem::path from, to; };            // absolute; Delete: `to` empty
struct AssetMove { Arcane::Guid guid; AssetKind kind; std::vector<FileMove> files; };
                                                                 // files[0] = the id-bearing file
enum class RefSource : std::uint8_t { AssetOnDisk, OpenScene, UnsavedDocument, BootScene, InputActions };
struct AssetReferencer { Arcane::Guid target, referencer; std::vector<RefSource> sources; std::string label; };
struct DerivedChild { Arcane::Guid parent, child; bool cascades; std::vector<Arcane::Guid> referencers; };
struct AssetRefusal { Arcane::Guid guid; std::string reason; };

struct AssetOpPlan {
    AssetOpKind                  kind;
    std::vector<AssetMove>       moves;        // Rename/Move/Duplicate(copy)/Delete(doomed, incl. cascaded)
    std::vector<Arcane::Guid>    newGuids;     // Duplicate: minted here, parallel to moves
    std::vector<AssetRefusal>    refusals;     // non-empty => nothing runs
    std::vector<AssetReferencer> referencers;  // Delete only
    std::vector<DerivedChild>    derived;      // Delete only
    std::vector<Arcane::Guid>    openDocs, dirtyDocs;
    std::string                  label;        // the undo step's label (7.4)
};

struct AssetOpFacts {                           // built by EditorApp; faked by tests
    std::filesystem::path contentDir;           // Project::Root()/"Content"
    std::filesystem::path diagDir;              // Project::Root()/"Saved"/"Diagnostics"
    std::span<const std::pair<Arcane::Guid, std::string>> registry;   // Registry().All()
    const AssetReferenceIndex* refs = nullptr;  // m_assetModel.RefIndex()
    std::span<const Arcane::Guid> openSceneAssets;                    // 7.5
    Arcane::Guid openScene, bootScene, inputActions;
    struct Doc { Arcane::Guid guid; bool dirty; std::vector<Arcane::Guid> liveRefs; };
    std::span<const Doc> docs;
    std::function<bool(const std::filesystem::path&)> exists;
    std::function<std::optional<Arcane::Guid>(const std::filesystem::path&)> peekId;   // AssetRegistry::PeekId
    std::function<std::vector<std::string>(const std::filesystem::path&)> gltfUris;   // 7.8
    std::function<std::vector<std::filesystem::path>(const std::filesystem::path&)> diagSiblings;  // 7.5
};

[[nodiscard]] AssetOpPlan PlanAssetOp(const AssetOpRequest& op, const AssetOpFacts& facts);
```

- File sets: imported binaries bring `<file>.meta`; `.gltf` its relative buffers/images (7.8); `.arcdiag` its siblings (7.5).
- Mount paths resolve against `contentDir` only (a plan never names a file outside `game://`); `diag://` Delete resolves against `diagDir`.
- Duplicate chooses and reserves copy names within the batch ("x 1", "x 2"; 7.7).

**Executor.** `class AssetFileOpExecutor`, owned by `EditorApp` with a `std::shared_ptr<AssetFileOpExecutor*>` anchor (document-command precedent, arcane-baseline §7), reaching the app only through:

```cpp
struct AssetFileOpHost {
    virtual ~AssetFileOpHost() = default;
    virtual RebindResult        Rebind(const Arcane::Guid&, const std::filesystem::path&) = 0;  // Runtime::RebindMovedAsset
    virtual bool                Unregister(const Arcane::Guid&) = 0;                          // Runtime::UnregisterAsset
    virtual std::optional<Arcane::Guid> Register(const std::filesystem::path&) = 0;           // Runtime::RegisterCreatedAsset
    virtual RecycleResult       Recycle(std::span<const std::filesystem::path>) = 0;          // OsShell::ShellRecycle (7.4)
    virtual bool                CloseDocumentFor(const Arcane::Guid&, bool discardDirty) = 0; // false = a dirty doc blocks
    virtual void                NoteMoved(const Arcane::Guid&, const std::filesystem::path& from,
                                          const std::filesystem::path& to) = 0;               // 7.11
    virtual void                AssetsChanged(std::span<const Arcane::Guid> removed,
                                              std::span<const Arcane::Guid> added) = 0;       // 7.12
    virtual void                Invalidate(const Arcane::Guid&, AssetKind) = 0;               // 7.12
    virtual void                EvictPaths(std::span<const std::filesystem::path>) = 0;       // Assets::EvictPath
    virtual void                Activity(AssetActivityEntry) = 0;
    virtual void                ReportError(std::string title, std::string message) = 0;      // ModalErrorQueue + ARC_ERROR
};

struct ExecResult { bool ok; std::string error; };   // !ok => nothing pushed
```

- **`ExecResult Execute(const AssetOpPlan&, Arcane::CommandStack&)`:** (1) re-check gates (7.1) and disk preconditions (TOCTOU: every `from` exists with `peekId == guid`, every `to` absent); (2) apply the primitive (7.4), rolling back completed file steps in reverse on the first failure and reporting once via `ReportError`; (3) follow-up: registry, `AssetsChanged`, documents, caches, feed; (4) **then** push the command (forward already applied, `Command.hpp:3-4`).
- A failed rollback step stops the executor; nothing is deleted to "clean up". The error names both paths: "`<file>` could not be moved back from `<to>`; it is safe there".
- Undo/redo reuse the same primitives and follow-up: one code path per disk effect.

**Not in scope.** Async execution (synchronous on the main thread like `CreateInstanceAt`, `EditorAppProject.cpp:1266-1296`); progress UI.

**Tests (`AssetFileOpsTest.cpp`).** Planner over fake facts and a real `AssetReferenceIndex` (pattern `AssetReferenceIndexTest.cpp`): file sets per kind, refusals, batch name reservation, labels. Executor with a fake host over a TempDir + real `AssetRegistry`: a failure at the k-th move leaves the tree byte-identical and pushes nothing; success pushes exactly one step.

### 7.4 The undoable file-op commands

**Problem.** No file operation is undoable (arcane-baseline §9). UE's are not transactional and its delete is permanent (ue-asset-file-ops §Short answer); only FChange-style expiry is borrowed (9.15).

**Design.** Abstract `AssetFileCommand : Arcane::ICommand`; subclasses `AssetMoveCommand` (Rename, Move), `AssetDeleteCommand`, `AssetDuplicateCommand`, `NewFolderCommand`. A batch is one command, one step.

- **State:** `std::weak_ptr<AssetFileOpExecutor*> m_exec` (inert when the executor dies), the plan's `moves`, `m_applied`, `m_blocked`, `m_label`.
- **`AffectsScene()` = false** (s3.3).
- **Labels:** "Rename f3_gold → f3_gold_v2", "Move 2 assets to materials/", "Duplicate f3_gold", "Delete uv_marker.png" / "Delete 3 assets", "New Folder materials/rocks/".
- **`Undo()`/`Redo()`:** (1) no-op if `m_blocked` or the anchor is dead; (2) pre-check the side about to run -- destinations free (undone delete: the original paths; undone rename: `from`), sources present with the right id; on failure set `m_blocked`, touch nothing, **refuse loudly** via `ReportError` (e.g. "Can't undo Delete uv_marker.png" / "`textures/uv_marker.png` is occupied by another file. Your deleted file is in the Recycle Bin."); the step then reads expired so the other side is skipped too; (3) run the primitive with rollback (a rolled-back failure also blocks); (4) flip `m_applied`.
- **`IsExpired()`** (T1: skipped by undo/redo) is true when blocked, anchor dead, or the side about to run lost its source: Move -- id file missing or `PeekId` ≠ guid; Delete applied -- a payload unreadable; Delete unapplied -- restored files gone or lost their ids; Duplicate mirrored; NewFolder applied -- folder missing or **not empty**. Occupied destinations are not expiry (they can be temporary; step 2 catches them). Cost: one `stat` + one small id parse per asset (`.meta`, never the binary).
- **Primitives:**
  - **Move:** `std::filesystem::rename(from, to)` per file, parent created; reversed on rollback (case-only: 7.6).
  - **Remove:** (1) capture each file as `struct FilePayload { std::filesystem::path path; std::uint64_t size; std::filesystem::file_time_type mtime; Arcane::UndoPayload bytes; }` with `CommandStack::MakePayloadFromFile(path)` (s3.3); (2) recycle all files in **one** `Recycle` call; (3) verify each is gone -- a survivor fails the step and already-removed files are rewritten from their payloads; (4) `Unregister` each guid. Redo **re-captures** (the file may have been edited since undo).
  - **Restore:** (1) pre-check occupancy; (2) write `.meta`, then primaries, then companions from the payloads (`UndoPayload::Load`), creating parents and restoring each `last_write_time` so the watcher sees no change; (3) `Register(primary)` must return the recorded guid, else remove the files this step wrote and block; (4) `ForgetUnresolved(guid)`.
  - **Copy:** Duplicate forward only (7.7).
- **Payloads.** T1's store is the only one (s3.3): `MakePayloadFromFile` streams a file over `editor.undo.spillThresholdKB` in 1 MB chunks into the step's `Saved/UndoCache/<step>.bin` (drafting pick, 9.28), never holding it whole in RAM; `PayloadBytes()` counts spilled bytes against `editor.undo.byteBudgetMB`; freed on eviction or `Clear`. T5 defines no spill format. A capture failure (including a full disk) aborts **before** anything is recycled.
- **Recycle Bin safety net** -- one platform function in `Project/OsShell` (s4.6), inside its single `#ifdef _WIN32`:
  ```cpp
  struct RecycleResult { bool ok; std::vector<std::filesystem::path> notRecycled, permanentlyDeleted; std::string message; };
  RecycleResult ShellRecycle(std::span<const std::filesystem::path> files, void* ownerHwnd);   // Window::NativeHandle(), Window.hpp:85
  ```
  - **Windows** (`IFileOperation`): COM through `IdeLaunch`'s `CoScope` (`IdeLaunch.cpp:116-122`); `SetOwnerWindow(ownerHwnd)` so the nuke prompt cannot open behind the editor; `SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE)`; `Advise` an `IFileOperationProgressSink` whose `PostDeleteItem(dwFlags, psiItem, hrDelete, psiNewlyCreated)` adds each item with `psiNewlyCreated == nullptr` (no bin item created) to `permanentlyDeleted`; `DeleteItem` per `SHCreateItemFromParsingName`, `PerformOperations`, `GetAnyOperationsAborted`. `notRecycled` = files that still exist. `FOFX_RECYCLEONDELETE` needs Windows 8+; below that `FOF_ALLOWUNDO` alone recycles.
  - `FOF_WANTNUKEWARNING` partially overrides `FOF_NOCONFIRMATION`: a file the bin cannot take (no bin on the volume, over quota) prompts first. Cancel aborts and the executor rolls back from the payloads; accept still leaves the bytes in the payload and the item in `permanentlyDeleted` (activity text, 7.12).
  - **Elsewhere:** `ok = false`, "Recycle Bin not supported on this platform"; Delete disabled with that reason.
  - The bin is **not** the undo mechanism (undo writes the bytes back); each redo recycles again, harmlessly.
- **History limits.** Scene New/Open and project switch still clear the stack (`EditorAppScene.cpp:162`) with T1's "Can't undo after: <reason>"; cap/budget eviction is silent. Then recovery is the bin plus the activity row (7.12). Mid-session bin restores: `.png`/`.gltf`/`.glb` are rediscovered by the 2 s watcher with their `.meta` and keep the guid (`EditorAppProject.cpp:466-543`); other kinds return at next open.
- Ctrl+Z undoes the top step whatever its kind; the Edit menu names it ("Undo Delete uv_marker.png").

**Not in scope.** Undo across scene open/project switch; in-editor "restore from Recycle Bin"; reopening documents closed for a delete.

**Tests (`AssetFileOpsTest.cpp`; `Recycle` faked in every default run).**
- Round trips ending byte-identical with the registry as at start: move → undo → redo; delete → undo; duplicate → undo → redo; new folder → undo.
- Occupied original path on undo of a delete: blocks, writes nothing, one `ReportError`, then `IsExpired()`. A renamed file deleted outside the editor → `IsExpired()`. Non-empty folder → NewFolder undo expired.
- Redo of a delete re-captures edited bytes; an over-threshold payload spills and undo restores from it; a capture failure recycles nothing; a fake `permanentlyDeleted` result yields the "permanently" activity text.
- `AffectsScene()` false on all four subclasses; `SceneSession::IsDirty` stays false across delete + undo (`EditorSceneSessionTest.cpp`).
- `OsShellTest.cpp`: one Windows-only `[shell]` case recycles a TempDir file and asserts it is gone; `[shell]` is excluded from default runs like `~[gpu]`.

### 7.5 Delete + referencer confirm

**Problem.** Critique Assets #1 wants Del with "referenced by N" from the index, which misses in-memory, manifest and one-hop referencers (map §4); deleting a derived source (texture, model) strands its children (map §5).

**Design.**

- **Entry points:** Del (7.10); row menu "Delete  Del"; asset page trash (`ICON_LC_TRASH_2 "##asset_delete"`, s5.6, acting on the page's asset). All open **one** modal, `DeleteConfirmState`, owning the plan.
- **Facts, gathered once at open** (the cascade checkbox re-plans against them): (1) `RefIndex().Find(g)->inbound`; (2) the live scene manifest `Arcane::SaveJson(registry)["assets"]` (`SceneSerializer.hpp:121-131`; Edit mode is gated); (3) `LiveReferences()` of each `Dirty()` open document; (4) `bootScene`/`inputActionsAsset` from the manifest.
- **`virtual std::vector<Arcane::Guid> EditorDocument::LiveReferences() const { return {}; }`** from typed data: `ShaderEditorDocument` = `parent` + texture params; `SpriteDocument` = `texture`; `MeshDocument` = `importedSource` + `slots[].material`; Input Actions and crash reports return nothing.
- **Referencer union (brief):** index inbound; inbound of each DerivesFrom child, one hop (an inbound `r` whose `Find(r)->outbound` holds `{g, DerivesFrom}`); the live manifest, tagged "(open scene, unsaved)" when the saved file lacks it; dirty documents, "(unsaved in `<title>`)"; the project manifest, "(project: boot scene)" / "(project: input actions)". Doomed referencers are dropped; each appears once with all tags.
- **Cascade** "Also delete N derived assets", **on by default**: plain sprites (`.arcsprite` DerivesFrom a doomed Texture; sliced sprites are References and don't cascade) and a doomed Model's companion `.arcmesh` (`importedSource == g`). Children are listed with their own referencers and pass the refusal table. Instance materials and a companion's import materials (References from `slots[]`, possibly shared) never cascade; instances are listed as referencers.
- **Diag set:** the report, the envelope's existing `siblingTxt`/`siblingDmp`/`siblingGpuDump` (`DiagEnvelope.hpp:69-76`), and any `<stem>.log.txt`/`<stem>.symbolized.txt` beside it (`diagSiblings`), as one asset.
- **Modal:** "Delete `<name>`?" / "Delete N assets?"; doomed rows (thumb + name); "Referenced by (K)" grouped by source; the cascade checkbox; "Unsaved changes in `<titles>` will be discarded."; footer "Files go to the Recycle Bin. Ctrl+Z restores them while this session's undo history lasts."; Cancel + confirm "Delete" / "Delete anyway" (K > 0) / "Discard changes and delete" (dirty documents). This single modal replaces the brief's separate Discard/Cancel prompt (drafting pick, 9.28). Refusals replace the confirm with their list and disable it.
- **On confirm:** (1) **close affected documents first,** unsaved (UE invariant, `OT:3648-3669`); a closing document's parked gesture commits as its own earlier step (`ShaderEditorDocument.cpp:952-971`); new public `void DocumentHost::CloseForAssetRemoval(EditorDocument*)` (`Close` is private, `DocumentHost.hpp:110`) clears `m_pendingClose` when it names that document; (2) Remove (7.4) every doomed asset incl. cascaded children in one step; (3) follow-up (7.12).
- **After:** selection pruned (7.9); a following Inspector shows "No selection", a pinned one "Pinned selection is gone" (`InspectorHost.hpp:65`), history pruned (`InspectorHost.cpp:61`); T2 asset-reference cells show red "(missing)" tombstones; the Graph keeps a tombstone node (`AssetPanelModel.cpp:211`). **Undo restores the same guid, so tombstones heal** and pins revive; history does not.
- **Undo/redo with open documents:** a clean document for a doomed or copied guid closes silently; a dirty one blocks (`CloseDocumentFor(…, false)` → false; "Close or save `<title>` first").

**Not in scope:** Replace References or rewriting referencers; rewriting `.arcproj`; walks beyond one hop; outbound edges for `.arcinput`/`.json`/`.arcdiag` (`Assets.cpp:1063-1080`); pre-v4 scenes' dropped targets (`Assets.cpp:1197-1205`); game-code path references; blocking a referenced delete.

**Tests.**
- Planner: deleting `uv_marker.png` lists the scenes referencing its sprite (one hop) and the sprite as cascaded child; the live manifest adds an unsaved reference; a dirty document's `LiveReferences` counts, the clean one doesn't; boot scene/input actions tagged; open and boot scenes refuse; doomed referencers drop; unticked cascade removes children from `moves`; diag Delete collects siblings.
- Per document kind: `LiveReferences` == `ListAssetReferences` of the same data saved to a temp file.
- `EditorDocumentHostTest`: `CloseForAssetRemoval` destroys a dirty document without saving.
- Desk: delete `uv_marker.png` on ReferenceProject (modal text, cascade, Inspector tombstones), Ctrl+Z (rows and cells heal), delete again and find it in the OS Recycle Bin.

### 7.6 Rename

**Problem.** Critique Assets #1/#11: no rename, no F2.

**Design.**

- **Entry points:** F2 with one selected asset (7.10); row menu "Rename  F2" (disabled for 2+: "Select one asset to rename"); asset page pencil (`ICON_LC_PENCIL "##asset_rename"`, s5.6), opening a small Rename modal with the same field (a page has no row; the asset may be filtered out).
- **Inline edit** (Outliner `BeginRename`, `EditorPanels.cpp:1481`): `AssetBrowserPanelState` gains `Arcane::Guid renameTarget`, `char renameBuf[128]`, `bool renameFocusPending`. The matching row in `DrawAssetRow`/`DrawChildRow` (`AssetBrowserPanel.cpp:461`, `:~622`) draws thumb + `InputText` with the **stem only** + dim extension; no drag source or context menu while editing. Commit on Enter or `IsItemDeactivatedAfterEdit`; Esc reverts. Invalid: Enter keeps the box open, deactivation cancels; the reason tooltips while typing. `renameTarget` drops whenever its row stops drawing (Outliner wedge lesson, `EditorPanels.cpp:1742-1757`).
- **`ValidateRenameName(std::string_view stem, const std::filesystem::path& currentFile)`** (`CreateAssetDialog.hpp`): `ValidateCreateName` rules 0-3, except a target `std::filesystem::equivalent` to `currentFile` is not "already exists" (case-only on NTFS). A byte-identical name is a no-op.
- **Plan:** `files[0]` = primary renamed in place, plus `<new>.<ext>.meta` for imported binaries; a `.gltf` renames only itself + `.meta` (URIs are folder-relative); the extension is fixed (it is the kind). Case-only `rename(a, A)` is a direct `MoveFileExW`; pre-flight treats an `equivalent` destination as free.
- **Execution:** Move (7.4) → `Rebind` (7.2) → `NoteMoved` for an open document and, if it is the open scene, the session (7.11) → scene recents (7.11) → follow-up (7.12). The **guid is kept**: selection, pins and history hold (guid-string keys, `AssetInspectorSource.cpp:89-103`); derived children follow by guid.
- **`InspectorHost::RefreshLabels()`** recomputes every `HistoryEntry.label` (`InspectorHost.hpp:68-73`) and `pinnedName` whose source still `Resolves(key)`; called when `m_assetModel.entriesStamp` changes.

**Not in scope.** Rewriting the internal `"name"` (default), so tabs keep their display name (titles come from `"name"`, e.g. `ShaderEditorDocument.cpp:908`); extension change; folder rename.

**Tests.** `CreateAssetDialogTest.cpp`: `a.png` → `A.png` accepted, real collision and bad characters refused. `AssetFileOpsTest.cpp`: a `.png` rename moves its `.meta`, keeps the guid, `Resolve` returns the new path; case-only rename round-trips through undo; `.gltf` rename leaves its `.bin`. `EditorInspectorHostTest.cpp`: `RefreshLabels` updates a history label and pin name; selection survives. Desk: F2 a material with its document open, save, confirm the new file is written and the old path does not reappear.

### 7.7 Duplicate

**Problem.** Critique Assets #1; a naive copy is hazard #1, and two kinds carry second identities (#7 companion link, #11 scene entity GUIDs).

**Design.**

- **Entry points:** Ctrl+D when the Browser owns the keys (7.10); row menu "Duplicate  Ctrl+D"; asset page duplicate (`ICON_LC_COPY_PLUS "##asset_duplicate"`, s5.6; `ICON_LC_COPY` stays Copy Path).
- **Name** `NextCopyName(stem, dir, ext)`: strip a trailing `" <digits>"`; try `"<base> 1"`, `"<base> 2"`, …; take the first `ValidateCreateName(candidate, dir, ext)` accepts whose `.meta` is also absent. Batch reservations count as taken. The copy lands beside the source.
- **Copy rules.** Native files are written via `<to>.arctmp` then rename (atomic-replace precedent `Project::SetBootScene`), with the new id **written before** `Register`, so `AddFile` never mints.

| Kind | Copy |
|---|---|
| `.arcmat` | New `"id"`; `"name"` = stem (as Create, `EditorAppProject.cpp:1266-1296`); graph, params, `parent` kept (an instance copy is a sibling instance) |
| `.arcsprite` | New `"id"`; `"name"` = stem; `texture` kept. Accepted: reuse-or-mint (`EditorAppProject.cpp:1298-1300`) then sees two sprites; a later texture drop mints a third |
| `.arcmesh` | New `"id"`; `"name"` = stem; non-nil `importedSource` → nil; `slots[]` kept |
| `.arcscene` | New `"id"`; **every** `entities[].components["Arcane::Identity"].id` re-minted `{hi,lo}`; positional parent/link indices unchanged (`SceneSerializer.hpp` header) |
| `.arcinput`, `.json` | New `"id"` |
| Imported binaries | `copy_file` + fresh `.meta`: all source fields, new `"guid"` |
| `.gltf` | As imported; same folder, so URIs resolve (buffers shared) |
| `.arcdiag`, `source://` | Refused (7.1) |

- **Execution:** pre-flight; write copies (a failure removes only files this op created, which never pre-existed); `Register(to)` must return `newGuids[i]` or roll back; follow-up (7.12); `selection` = the copies, primary = last; the `selectionStamp` scroll shows it (`AssetBrowserPanel.cpp:683-697`) -- the post-create path critique problem 7's "select and scroll to the new row" shares.
- **Undo** = Remove on the copy; **Redo** = Restore.
- **Companion copies** strip `importedSource` per the brief (drafting pick, 9.28): the copy is Imported with a nil model and draws nothing (`MeshAsset.hpp:108-111`), with no in-editor route to a source (T3's "Source model" row is read-only). The alternative is refusing to duplicate a companion.

**Not in scope.** Duplicating children, companions or `.gltf` buffers; opening the copy.

**Tests.** `AssetFileOpsTest.cpp`: every kind's copy has a new valid on-disk id, registry holds both; `.meta` settings copied with a new guid; companion copy has nil `importedSource` and `UniqueImportedCompanion` still finds the original (`MeshImportWave.hpp:198-203`); scene copy's Identity ids all fresh, originals untouched, copy loads (`LoadJson`); `rock` → `rock 1` → `rock 2`, and `rock 1` → `rock 2`; undo recycles the copy, redo restores it with the same new guid. `MaterialAssetTest`/`SpriteAssetTest`/`MeshAssetTest`-style load round-trips of the copies.

### 7.8 Move + Move to + New Folder

**Problem.** Critique Assets #1. No Browser drop targets (`DrawGroupRow`, `AssetBrowserPanel.cpp:327`, is a fold toggle), and groups exist only where an asset lives (map §2), so empty folders are untargetable.

**Design.**

- **Empty folders become rows.** `AssetPanelProviders` gains `std::function<std::vector<std::string>()> emptyFolders` (directories under `Content/` with no registered asset beneath; a parse-free walk, only on `MarkAllDirty` rebuilds), drawn as group rows with a dim "(empty)" and included by `BuildFolderChoices` (`CreateAssetDialog.cpp:98-127`). ReferenceProject has none.
- **Drop targets:** each group row resolving to a `game://` folder is a `BeginDragDropTarget` for `kAssetDragType` (`AssetPanelModel.hpp:81`) with `AcceptBeforeDelivery`; `RelativeDirOfFolderKey` (`CreateAssetDialog.cpp:73`) maps the key; a group spanning mounts targets the game folder. A hover dry-run plan is cached per (group, dragged guid); a refused drop tooltips its reason and does not highlight. Delivery acts on **the model selection if it contains the dragged guid** (tooltip "N assets"), else the dragged guid. The payload stays `{Guid, AssetKind}` (`AssetPanelModel.hpp:82`). No modal (undoable); a drop onto the current folder is an empty plan, nothing pushed.
- **"Move to…"** (row menu, on the selection): modal "Move N assets" with the Create dialog's Location combo, factored from `DrawNameAndLocation` (`CreateAssetDialog.cpp:178-225`) as `DrawLocationCombo(folders, index)`, a "New Folder…" button, and the first refusal inline. Move to… and New Folder exist because empty folders have no rows (default).
- **"New Folder…"** from a folder row's menu (child), the background menu (under `Content/`) or the Move to… modal (selects it). Name checked by `ValidateCreateName(name, parentDir, "")` (rule 3 catches an existing file/folder; noun "folder"). `NewFolderCommand`: `create_directory`; undo removes it only while empty (7.4).
- **`.gltf` companions:** `gltfUris` parses `buffers[].uri`/`images[].uri`, skips `data:`, percent-decodes; each relative file moves to `destDir / uri`; an image that is a registered asset moves as its **own** `AssetMove` with `.meta` and is rebound; refusals per 7.1. `.glb` is self-contained.
- **Execution:** Move → `Rebind` per asset → `NoteMoved` → recents → follow-up; selection kept and `selectionStamp` bumped to scroll to the primary.

**Not in scope.** Dragging folder rows, a folder tree, drops onto asset rows, OS file drops.

**Tests.** `AssetFileOpsTest.cpp`: a `.gltf` with `a.bin` and registered `tex.png` moves all three, `tex.png` keeps its guid; `../` URI, a buffer shared with an unmoved `.gltf`, and a destination occupied only by a `.meta` each refuse; undo returns everything. `AssetPanelModelTest.cpp`: `emptyFolders` yields an empty group row in `BuildFolderChoices`. `CreateAssetDialogTest.cpp`: folder-name validation. Desk: drag a two-asset selection onto `materials/`; New Folder then Move to…; undo both.

### 7.9 Multi-select

**Problem.** Critique Assets #3: bottom bar hard-codes 0/1 (`AssetBrowserPanel.cpp:963`), the model holds one guid (`AssetPanelModel.hpp:721`, `:744`), a deleted guid stays `selected` (cleared only by `ResetForProjectSwitch`, `AssetPanelModel.cpp:~750`).

**Design.**

- **Model:** `AssetPanelModel` gains `std::vector<Arcane::Guid> selection`; `selected` stays **the primary** (Inspector, Graph, Status), mirroring `SelectionContext` (`Scene/SelectionContext.hpp:27-60`):
  - `Select(g)` → `selection = {g}` (signature and bumps unchanged).
  - `ApplySelection(std::vector<Guid> sel, Guid clicked)`: `clicked` is primary if still selected, else the last entry or nil; bumps `selectionGesture` only for a valid `clicked`.
  - `SetPrimary(g)`, `InSelection(g)`, `SelectionCount()`.
  - `PruneSelection()` in `RebuildIfDirty` (`AssetPanelModel.cpp:147`) drops entry-less guids, moves/clears `selected`, bumps `selectionStamp`, **not** `selectionGesture` (`SelectionContext::Epoch` rule).
  - `ResetForProjectSwitch` clears `selection`.
- **Table:** `DrawTable` (`:666`) wraps clipped rows in `ImGui::BeginMultiSelect(ClearOnEscape | ClearOnClickVoid | BoxSelect1d, …)` with `ImGuiSelectionExternalStorage` (a 128-bit guid cannot be an `ImGuiID`; the Console's `ImGuiSelectionBasicStorage` keying doesn't transfer). Asset/child rows call `SetNextItemSelectionUserData(rowIndex)`. `ApplyRequests` calls `AdapterSetItemSelected` for every index of a range and of SetAll (`imgui_widgets.cpp:8762-8773`), so the adapter looks up visible row `idx` and returns without effect for group rows: Shift-ranges and Ctrl+A select only asset and child rows. `clipper.IncludeItemByIndex(ms->RangeSrcItem)` sits beside the existing scroll inclusion (`AssetBrowserPanel.cpp:768`). **The body never runs in a skipped child** (Console crash lesson, `EditorPanels.cpp:999-1010`). The `BeginMultiSelect` precedent is the Console (`EditorPanels.cpp:1082-1093`, `:1166-1167`); the Outliner's is hand-rolled (`:2070-2115`). Up/Down still collapse to one row (`:810-837`); Ctrl+A arrives as SelectAll.
- **Context menu:** right-click inside the selection keeps it and calls `SetPrimary(row)` once on the popup's first frame (`:107-126`); outside, today's `Select(row)`. Disabled reasons come from one cached dry-run plan per popup open.
- **Bottom bar:** "`%d assets · %d selected`" via `SelectionCount()`.
- **Batch ops:** one plan, one confirm (Delete), one step.
- **Asset page** shows the primary; its icons act on the page's asset (a pin may be outside the selection).

**Not in scope.** A "+N" multi-asset page, Shift+Up/Down extension, a multi-guid drag payload.

**Tests (`AssetPanelModelTest.cpp`).** Toggle/range semantics; primary fallback on removal; `PruneSelection` after `Remove` clears the guid, bumps `selectionStamp` only; `Select` bumps as today (existing tests untouched); bottom-bar count helper. Headless ImGui (`AssetInspectorSourceTest.cpp:259-286` pattern): Ctrl-click then Shift-click over a clipped list selects the expected guids; group rows are never selected (pins the adapter).

### 7.10 Key routing (fixes Ctrl+D)

**Problem.** Ctrl+X/C/V/D are raw-scancode edges folded globally into entity clipboard requests (`EditorAppFrame.cpp:944-956`, `:2646-2650`), gated only by `ShortcutsLive` (not Play, not `WantCaptureKeyboard`, not rebind capture; `ViewportInput.hpp:22-24`). `WantCaptureKeyboard` is false with the Browser focused, so **Ctrl+D in the Asset Browser duplicates the selected entity today**. The Browser's key block also lacks the Outliner's guards (`AssetBrowserPanel.cpp:810` vs `EditorPanels.cpp:1766-1783`).

**Design.**

- `DrawAssetBrowserPanel` (`AssetBrowserPanel.cpp:872`) returns new `AssetPanelActions::ownsEditKeys = IsWindowFocused(ChildWindows | NoPopupHierarchy) && !io.WantTextInput && !renameTarget.IsValid()` (`NoPopupHierarchy` keeps Del off rows behind an open context menu).
- **Browser keys** (`ImGui::IsKeyPressed(key, false)` + `io.KeyCtrl`, as the Outliner): F2 Rename (one selected); Delete → confirm; Ctrl+D Duplicate; Ctrl+X/C/V consumed, no action in v1; Up/Down/Enter unchanged but behind the same guards.
- The fold-in at `EditorAppFrame.cpp:2646-2650` becomes pure `FoldEntityClipboardShortcuts(menuReq, fs, browserActions.ownsEditKeys)`, folding `fs.scCut/scCopy/scPaste/scDuplicate` only when the Browser does not own the keys. The Browser draws at `:2400`, before the fold-in: no lag (unlike `DocumentHost::FocusedDoc`, since documents draw after the keybind phase).
- **Unchanged:** Ctrl+Z/Y global on the shared stack; the Outliner's Del/F2; Edit-menu Cut/Copy/Duplicate/Rename/Delete stay entity-scoped (`EditorPanels.cpp:203-215`; an open menu takes focus, so `ownsEditKeys` is false).
- Ships in T5 step 1, fixing Ctrl+D before asset Duplicate exists.

**Not in scope.** A general per-panel shortcut router.

**Tests.** Unit: `FoldEntityClipboardShortcuts` folds none when the Browser owns the keys, all four otherwise. Headless ImGui: Browser focused, no popup → true; open row context menu → false and Del requests nothing; active search box → false. Desk: select an entity, focus the Browser, Ctrl+D -- the asset duplicates, entity count unchanged.

### 7.11 Documents NoteMoved + SceneSession

**Problem.** Documents store their path (`ShaderEditorDocument.hpp:644`, `MeshDocument.hpp:349`, `SpriteDocument.hpp:162`, `CrashReportDocument.hpp:143`, `InputActionsDocument.hpp:105` `path_`), the session `m_path` (`SceneSession.hpp:134`). After a rename Save writes the old path, and `ShaderEditorDocument::Save` also calls `RegisterCreatedAsset(m_path)` (`ShaderEditorDocument.cpp:1736-1741`): two files, one guid (`AssetRegistry.cpp:431-450`). No retarget hook exists (`EditorDocument.hpp:27-87`).

**Design.**

- **`virtual void EditorDocument::NoteMoved(const std::filesystem::path& newPath) = 0;`** (pure, so a new kind can't keep the bug): assign the path; recompute `m_title` when the data's `name` is empty (stem fallback, e.g. `ShaderEditorDocument.cpp:908`); rebuild the visible label keeping `###<kind>doc_<guid>` (dock slot holds). `ShaderEditorDocument` also republishes its File-locator compile rows (`:3330`). `CrashReportDocument` implements it for completeness; test fakes trivially.
- **`DocumentHost::NoteAssetMoved(const Arcane::Guid&, const std::filesystem::path&)`:** `FindByGuid` (`DocumentHost.hpp:58`) → `NoteMoved`.
- **`SceneSession::NoteMoved(const std::filesystem::path& from, const std::filesystem::path& to)`** retargets `m_path` only when `Path()` is equivalent to `from`; never touches `m_id`, `m_savedStateId`, `m_pending`. The T4 title reads `DisplayName()` and follows.
- **`SceneRecents::Replace(List&, const std::filesystem::path& from, const std::filesystem::path& to)`** (pure; normalizes, keeps position, dedupes), applied to `Saved/recent_scenes.json` per moved `.arcscene` (`SceneRecents.hpp:36-60`); the shared project-recents file is untouched.
- The host's `NoteMoved` (7.3) calls all three -- forward and both undo directions -- before the step is pushed.

**Not in scope.** Reopening documents on undo of a delete; retargeting Inspector keys (guids).

**Tests.** `EditorDocumentHostTest.cpp`: `NoteAssetMoved` retargets by guid; a real `SpriteDocument` over a TempDir saved after `NoteMoved` writes the new path, old path absent. `EditorSceneSessionTest.cpp`: non-matching `from` is a no-op; matching retargets, `IsDirty` unchanged. `SceneRecentsTest.cpp`: `Replace` keeps order and dedupes.

### 7.12 Caches, diagnostics, activity invalidation

**Problem.** Guid-keyed caches keep drawing deleted assets and memoized failures keep restored ones broken; path-keyed `Assets` memos go stale (#12); `m_cookDiagnostics` rows linger; `AssetActivityKind::Deleted` has no producer (`AssetActivityLog.hpp:31-43`).

**Design.**

- **One entry point, `EditorApp::InvalidateAssetCaches(const Arcane::Guid&, AssetKind)`**, factored from today's sequences:

| Kind | Invalidation (existing precedent) |
|---|---|
| Material | `m_resolver->InvalidateMaterial` (which also drops the `PostChainCache` entry and clears the mesh-material cache, `SceneRenderResolver.cpp:344-356`) + `InvalidateMaterialThumb` (`EditorAppProject.cpp:589-594`) |
| Mesh | `m_resolver->InvalidateMesh` (`SceneRenderResolver.hpp:291`) + `m_materialThumbs->InvalidateMesh` (`EditorAppProject.cpp:646`) |
| Texture / Model | `AssetsFacade().InvalidateArtifact`/`InvalidateMeshArtifact` (`:776-784`; `Assets.hpp:225`, `:396`); Model also its companion's `m_resolver->InvalidateMeshArtifact` + harvester `InvalidateMesh` (`:805-816`) and the viewport graph step there |
| Sprite | `m_resolver->InvalidateSprite` (`EditorApp.cpp:814`; `SceneRenderResolver.hpp:252`) |

- **Per operation:**

| Operation | Guid invalidation | `EvictPath` | Other |
|---|---|---|---|
| Delete; redo delete; undo duplicate | removed guids + sprites deriving from a removed texture | removed paths | erase `m_cookDiagnostics[g]`, `PublishCookDiagnostics()` (`EditorAppProject.cpp:1224`) |
| Restore (undo delete, redo duplicate) | restored guids | restored paths | `ForgetUnresolved(g)` |
| Move/Rename, both directions | none (bytes identical) | every `from` and `to` | -- |
| Duplicate forward | none (fresh guid) | copy paths | -- |
| New Folder | -- | -- | -- |

- **Model:** every op calls `m_assetModel.MarkAllDirty()` (grouping changes; `CreateInstanceAt` reasoning, `EditorAppProject.cpp:1286-1292`); the rebuild tombstones removed guids (`AssetPanelModel.cpp:211`) and runs `PruneSelection`; `entriesStamp` drives the Graph rebuild and `RefreshLabels` (7.6).
- **Left in place:** artifacts (guid + content-hash keyed, so a restore reuses them; swept at next open by `SweepArtifactOrphans`, `EditorAppProject.cpp:1239`) and `Saved/Thumbnails/<guid>.png`.
- **Watcher:** nothing new. Removed guids leave `Registry().All()`; a moved path's first sighting is a baseline for Material/Mesh and one hash-gated no-op cook for Texture/Model (`EditorAppProject.cpp:686-707`); restores keep `last_write_time` (7.4).
- **Activity**, one entry per asset with a name snapshot: `Deleted`, detail "restore from Recycle Bin", or "permanently; not in the Recycle Bin" for a `RecycleResult::permanentlyDeleted` path (first producer; the "no producer today" comment is updated); new **`Moved`** kind for rename/move, "from `<old mount path>`"; `Created` for a duplicate ("duplicate of `<name>`") and a restore ("restored (undo)"); redo of a delete logs `Deleted` again.
- **Tombstones:** `m_assetRefServices.tombstoneName` (s4.2) returns the name snapshot of the newest `Deleted` entry for the guid, else nullopt; a later restore's `Created` entry makes it nullopt again.
- **Diagnostics:** no new persistent Problems rows. Op failures and undo/redo refusals go to `ModalErrorQueue` (`App/ModalErrorQueue.hpp:19-35`) + `ARC_ERROR` (a one-shot row would have no producer to retract it). Owners retract stale rows: `"assets"` via `Remove`, `"assets.unresolved"` via `ForgetUnresolved`, `"diagnostics:cook"` via the erase + republish.

**Not in scope.** Mid-session thumbnail/artifact GC; a `Dangling` Problems card (`docs/superpowers/specs/2026-09-06-asset-manager-redesign-design.md` ruling 8, still owed).

**Tests.** `AssetFileOpsTest.cpp` (call-recording fake host): the per-operation table as exact call sets; `tombstoneName` yields the snapshot after a delete and nullopt after its undo. `AssetActivityLogTest.cpp`: `Moved` round-trips. Desk: delete a texture the open scene draws -- gone the same frame, back on undo; its refused-cook row leaves Problems.

### 7.13 In-T5 order

Brief order: foundation + rename → duplicate → delete → multi-select → move. Each step ends with a Debug + Release build and green ArcaneTests; the golden gate and the 1920x1080 windowed desk check on the user's layout run at the tranche's end. **Goldens:** `editor-asset-page` moves (its action row gains Rename, Duplicate and Delete) and is re-blessed alone after step 3, once the last icon lands. No other golden is expected to move (menus/modals are not captured, the bottom bar is unchanged at 0/1 selected, ReferenceProject has no empty folders); any other movement is attributed and re-blessed slot by slot.

1. **Foundation + rename:** 7.2 (one Core ABI bump, Aphelyon restamp, module rebuild); 7.3 planner + executor skeleton; 7.4 command base, move primitive, expiry (needs T1's `AffectsScene`/`IsExpired`/`MakePayloadFromFile`); 7.10 routing (Ctrl+X/C/V/D off entity ops in the Browser; F2/Del wired); 7.11 `NoteMoved`/recents, `RefreshLabels`, 7.6 Rename, `##asset_rename`.
2. **Duplicate:** 7.7 -- Ctrl+D, Identity re-mint, companion strip, select + scroll, `##asset_duplicate`.
3. **Delete:** `OsShell::ShellRecycle`; remove/restore primitives with spill; `LiveReferences`, `CloseForAssetRemoval`, the 7.5 modal with cascade and refusals; 7.12 invalidation, activity producers, `tombstoneName`; `##asset_delete`; the T2 cell's tombstone verified against a real delete.
4. **Multi-select:** 7.9, batch plans and modal wording, in-selection context menu.
5. **Move:** 7.8 -- empty-folder rows, drop targets, Move to…, New Folder, `.gltf` companions.

Desk per step: (1) F2 + save-after-rename; (2) Ctrl+D with an entity selected elsewhere; (3) delete / undo / Recycle Bin, plus deleting an asset the viewport shows; (4) Ctrl/Shift/box selection with the Inspector following the primary; (5) drag a selection onto a folder, New Folder + Move to…, undo all the way back.

## 8. T6: independent surfaces

T6 runs after T4 (s2.1). Its four items are independent of each other and can land in any order. Each one ends at the tranche gate (s2.1). Pieces it uses from earlier tranches:
- **T2:** `OsShell::ShellOpen` / `ShowInExplorer` / `OpenAsText` (s4.6), `LinkText` / `LinkRow` (s4.7), `EditorFontSet::mono` with the `MonoFont` scope (s4.8), and `IconToggle(label, on)` / `PushToggleOnColors()` (s4.9).
- **T4:** the accent tokens (s6.1) and the strip's `ToolbarStatus::problems` slot (s6.5).

Paths are relative to `D:\dev\starworks\Arcane`. ABI: 8.2's Core changes take T6's one bump (s2.3). 8.1, 8.3 and 8.4 need no bump.

### 8.1 Crash viewer (and reporter fixes)

**Problem** (critique §9 #1-#6, #9, #10; top-10 #7). `CrashReportDocument` shows the least useful part of what is on disk:
- It draws the portable `cpuThreadSummary` as plain text (`ArcaneEditor/src/Documents/CrashReportDocument.cpp:232-241`).
- It never reads the `<stem>.symbolized.txt` the reporter writes (`ArcaneCrashReporter/src/ReporterMain.cpp:67-90`). Nothing parses that file back.
- It ignores `reason`, `exitCode`, `logPath` and `commandLine` (`DiagEnvelope.hpp:94-102`), and never calls `ForeignModules::Classify`.
- The header is the raw kind in body weight (`:142-144`). "GPU Queues" is `DefaultOpen` even when empty (`:190-193`). Every field is followed by a `Separator`.
- The buttons run `explorer /select` instead of opening the file (`:34-41`, `:276-278`).
- The tab title is the machine stem (`:54-55`), in local time (`Diagnostics.cpp:392-399`), while the header shows UTC (`:408-415`).

The reporter already has a pure model (`ReportView.hpp:23-53`, `SymbolizedText.hpp:20-67`), but its window has two defects:
- The details well repeats the headline, the time and the reason drawn above it (`ReporterWindow.cpp:592-603` against `ReportView.cpp:134-144`).
- Close sits in the middle of the button row (`ReporterWindow.cpp:653-658`, Win32 `:224`), and Copy gives no feedback on success (`ReporterMain.cpp:289-291`).

**Design.**

1. **Source compile, (default).** The model moves to Core only if a third consumer appears.
   - ArcaneEditor's project in `premake5.lua` compiles `ArcaneCrashReporter/src/{ReporterArgs,SymbolizedText,ReportView,LogTail}.cpp` and adds `ArcaneCrashReporter/src` as an include dir, the way ArcaneTests already does (`premake5.lua:1576-1595`).
   - ArcaneTests' list gains `LogTail.cpp` (std-only).
   - `CrashReportDocument.cpp` is already compiled into the tests (`:1328`).

2. **`ParseSymbolized`** is the pure inverse of `FormatSymbolized` (`SymbolizedText.cpp:21-67`), and lives in `SymbolizedText.{hpp,cpp}`. `FormatSymbolized` takes `buildInfo` and `portableFallback` as parameters (`SymbolizedText.hpp:58-59`) and `Symbolized` stores neither, so the parse returns both:
   ```cpp
   struct ParsedSymbolized { Symbolized sym; std::string buildInfo; std::string portableBody; };
   [[nodiscard]] std::optional<ParsedSymbolized> ParseSymbolized(std::string_view text);
   ```
   - **Header:** `nullopt` unless line 1 starts with `symbolized by ArcaneCrashReporter `. The rest of that line is `buildInfo`.
   - **Engine available.** `engine      : dbgeng` sets `engineAvailable`, and the next line's `symbol path : ` value becomes `symbolPath`.
   - **Engine unavailable.** `engine      : unavailable (<err>) -- module+offset from the portable stack`:
     - `engineError` is the text between `unavailable (` and that exact suffix, matched at the end of the line, so an error containing `)` survives.
     - Everything after the following blank line is `portableBody`, which is not parsed. The caller falls back to the envelope (`ReportView.cpp:125-128`).
   - **Threads:** `--- thread <n|<unknown>>[ (faulting)]` starts a `SymThread`. `<unknown>` maps to `systemId = 0` (R77).
   - **Frames.** A frame line is `NN <frame>`, two or more digits then one space (`%02zu`, `SymbolizedText.cpp:44`). The frame is parsed from the right:
     1. a trailing `]` and the last ` [` give the bracket body;
     2. the last `:` in it splits file from line, so `D:\...` paths survive;
     3. the last `+0x` gives `displacement`;
     4. the rest splits at the first `!` into `module` and `function` (no `!` means `function` is empty).
     
     `address` is not in the text and stays 0.
   - **Markers:**
     - `  <no frames recovered>` leaves `frames` empty.
     - `   ... (truncated at ...-frame cap)` sets `framesTruncated`.
     - `--- (truncated at ...-thread cap)` sets `threadsTruncated`.
   - Unrecognised lines are skipped. A malformed line never fails the parse.

3. **`ReportView` additions** (pure, `ReportView.hpp/.cpp`; `frames`, `DisplayProduct`, the GPU-section `Layers:` line and the action row under the header are drafting picks, 9.28):
   - `ThreadView` gains `std::vector<SymFrame> frames`. It is filled on the symbolized branch (`ReportView.cpp:112-124`) and empty on the portable fallback. `text` is unchanged.
   - `DisplayProduct(std::string_view appName)` inserts a space after a leading `Arcane` when more follows (`"ArcaneEditor"` becomes `"Arcane Editor"`). Anything else is returned unchanged. The editor passes it as `Args::product`, because the envelope carries no product name (`main.cpp:373` hands it only to the reporter).
   - `DetailsHeader(v)` is the headline, the when line and the reason. `DetailsBody(v, threadIndex)` is everything `DetailsText` prints after the reason: injected modules, the selected thread, GPU, the log tail and the folder. `DetailsText` becomes `DetailsHeader + DetailsBody`.
   - `enum class CopyState { Idle, Copied, Failed }`. `CopyButtonLabel(CopyState)` returns `"Copy Details"`, `"Copied"` or `"Copy failed"`.
   - `enum class ReporterButton`, with `ReporterWindow`'s `kBtn*` ids mapped one to one. `VisibleButtons(const ReportView&)` returns them left to right:
     - `OpenFolder`, `Copy`;
     - `KeepWaiting`, `Terminate` (only if `isHang`);
     - `Relaunch` (only if `!relaunchLine.empty()`);
     - `Close`, always last.
   - `FormatLocalStamp(std::string_view isoUtc, const std::chrono::time_zone*)` turns `2026-09-29T16:57:12Z` into `2026-09-29 11:57`, and returns `""` if the input does not parse.

4. **The document loads everything once, at construction** (rule at `CrashReportDocument.cpp:57-63`).
   - `stem = m_path.parent_path() / m_path.stem()`. Siblings `<stem>.symbolized.txt` and `<stem>.log.txt` are built with path `+=`, never by string concatenation (R64, `ReporterMain.cpp:71-83`).
   - `m_symbolized = ParseSymbolized(Slurp(sym))` when the file exists.
   - `m_view = BuildReportView(env, Args{product = DisplayProduct(env.appName), envelopePath = m_path}, m_symbolized ? &m_symbolized->sym : nullptr, ReadLogTail(stem, env.logPath, 200))`.
   - `m_logResolved` is `<stem>.log.txt` if it exists, else `env.logPath` if it exists, else empty.
   - Accessors: `View()`, `HasSymbolized()`, `SymbolizedPath()`, `LogPath()`.
   - **Late `.symbolized.txt`** (drafting pick, 9.28). A missing file is re-checked only on the rising edge of window focus in `Tick` and in `NoteReopened()`, with no timer. When the file now exists, the document re-parses and rebuilds `m_view`. This amends the non-goal at `.hpp:28-29`: a hang report's `.symbolized.txt` is written after the host has registered the report (`CrashPathTest.cpp:488-498`).

5. **`Draw()` is rewritten**, top to bottom, with no separators between fields:
   - **Header.**
     - `view.headline` in the UI face at 1.35x;
     - the `whenLine` (dim), then `reasonText`;
     - `Exit code: <dec> (0x<hex>)`, only when `exitCode != 0`.
   - **Action row**, directly under the header:
     - **Copy Details** puts `DetailsText(view, m_threadIndex)` on the clipboard. The label shows `CopyButtonLabel(Copied)` for 0.75 s, the Console's existing feedback (`EditorPanels.cpp:968-977, :1076`), with a `###` id and a fixed width.
     - **Open .txt / .log / .symbolized.txt / .dmp** through `OsShell::ShellOpen` (default app; `.dmp` opens in VS or WinDbg by association).
     - **Show in Explorer** runs `OsShell::ShowInExplorer(m_path)`.
     - A file that is recorded but does not resolve draws a disabled "(missing)" button. A file that was never recorded draws no button (today's rule, `:245-274`).
   - **Injected modules.** One row per line of `view.injectedText` (classified by `ForeignModules::Classify` in `InjectedLines`, `ReportView.cpp:91-110`), each with the warning glyph in `Theme::kWarning` (s8.2 item 1).
   - **Stack.**
     - A thread combo when `threads.size() > 1`, faulting thread first.
     - A dim source note: "Symbolized (dbgeng)"; "Portable stack (module+offset) -- `<stem>.symbolized.txt` not found"; or "No stack recorded" when there are no threads (`isAbnormalExit`).
     - A `MonoFont` child with one row per frame:
       - **Symbolized rows** read `NN module!function+0xoff`, followed by a `LinkText` `[file:line]` that calls `Services::openSourceAtLine(file, line)`. When `!exists(file)` the link is not live, with the tooltip "Not on this machine".
       - **Portable rows** are the lines of `ThreadView::text`, with no link.
       - Right-click on any row offers **Copy line**.
   - **GPU.** `CollapsingHeader("GPU", DefaultOpen)`, drawn only when `!VisibleQueues().empty() || HasVisibleFault()`. It holds:
     - the queue timeline (in-flight in `kAmber`);
     - the fault block;
     - the `Layers:` line, which is no longer top-level;
     - the `.gpudump` section inventory with its Explorer button.
     
     `IsEmptyQueueTimeline` and `IsNoiseFault` stay.
   - **Log tail.** `CollapsingHeader("Log (last 200 lines)")`, closed by default, holding a read-only `InputTextMultiline` in mono (the reporter's pattern, `ReporterWindow.cpp:637-638`).

6. **Opening source at a line.**
   - `IdeLaunch::OpenFileAtLine(devenv, solution, file, line)`:
     - **Running instance:** after `OpenFileIn` (`IdeLaunch.cpp:349-375`), calls `InvokeNamed(dte, L"ExecuteCommand", {"Edit.GoTo", "<line>"})`. On failure it logs `ARC_WARN`, and the outcome stays `OpenedInInstance`.
     - **New instance:** `ComposeLaunchArgs(solution, file, line = 0)` appends `/Command "Edit.GoTo <line>"` when `line > 0`.
   - `EditorApp::OpenInIde` gains `int line = 0` and returns its `Outcome` (it returns void today, `EditorAppProject.cpp:2894`).
   - The new `EditorApp::OpenSourceAtLine(file, line)` calls it, and falls back to `OsShell::ShellOpen(file)` on `NoDevenv`, `NoSolution` or `DetectionFailed`.
   - The `.arcdiag` factory (`EditorApp.cpp:764-773`) captures `this` and passes `CrashReportDocument::Services{ std::function<void(const std::filesystem::path&, int)> openSourceAtLine }` as an optional third constructor parameter, so the existing tests are unchanged.

7. **Title.**
   - `m_title = view.headline + " -- " + FormatLocalStamp(env.timestampUtc, std::chrono::current_zone())`, falling back to the stem when the stamp does not parse.
   - `m_windowLabel = m_title + "###crashdoc_" + guid`, so the id is unchanged.
   - The tab tooltip shows the stem.

8. **Reporter fixes, (default).**
   - **Details text.** The styled well (`RebuildDetails`, `ReporterWindow.cpp:566-575`) and both Win32 fills (`ApplyView :275`, `RefreshDetails :304`) render `DetailsBody`. `CurrentDetails` (`:180, :191`, used by Copy) keeps `DetailsText`.
   - **Button order.** Both presenters iterate `VisibleButtons(v)`, right-aligned:
     - Styled: Close is filled with the T4 accent as the primary (drafting pick, 9.28).
     - Win32: `Layout` (`:333-378`) places the buttons in that order and keeps R92's shrink-to-fit.
     - Unchanged: the ids, `BS_DEFPUSHBUTTON` and initial focus on Close, and Enter never triggering Relaunch (R83).
   - **Copy feedback.** `ReporterWindow::NoteCopy(bool ok)` is called from `OnButton`'s `kBtnCopy` case, next to the existing warning.
     - Styled: the label is `CopyButtonLabel(Copied/Failed)` until a 0.75 s `ImGui::GetTime()` deadline. It reverts on the first input-driven frame after that.
     - Win32: `SetWindowTextW` on the Copy button, reverted by the next `ApplyView`/`RefreshDetails`.

**Does not.**
- Symbolize in the editor, or refresh on a timer.
- Change the body `whenLine`, which stays UTC (the `Z` marks it). Only the tab title is local.
- Add a VS Code target, or Relaunch / Keep Waiting in the editor.
- Cover critique §9 #11 (an external kill pops an attended window) or #12 (reports counted as assets).
- Ship a golden: verify runs decline `diag://` (`ProjectBoot.hpp:171-177`).

**Verification.**
- **`SymbolizedTextTest`:**
  - Re-formatting a parse with its `buildInfo`/`portableBody` reproduces the text byte for byte, and every field except `address` round-trips.
  - Also covered: a `D:\...` file path; `<unknown>` ids; frame-cap and thread-cap markers; `<no frames recovered>`; an engine-unavailable body whose error contains `)`; a foreign file returns `nullopt`.
- **`ReportViewTest`:**
  - `frames` is filled on the symbolized branch and empty on the portable one.
  - `DisplayProduct`.
  - `DetailsText == DetailsHeader + DetailsBody`, and the body does not contain the headline.
  - `CopyButtonLabel`.
  - `VisibleButtons`: Close last, Relaunch right before it when shown, hang buttons only for a hang.
  - `FormatLocalStamp` against `locate_zone("UTC")` and a fixed zone.
- **`IdeLaunchTest`:** `ComposeLaunchArgs` with and without a line.
- **`CrashReportDocumentTest`** (fixture pattern `:44-200`: an `.arcdiag`, a `FormatSymbolized` sibling and a `.log.txt`):
  - `View().threads[0].label == "thread N (faulting)"`, and a frame's file and line are parsed;
  - the log tail is present and `LogPath()` resolves;
  - with no symbolized file: the portable fallback and `!HasSymbolized()`;
  - a symbolized file written after construction is picked up by `NoteReopened()`;
  - `Title()` begins with "Arcane Editor crashed -- ".
- **Desk:**
  - `--hang-main N` on a windowed editor. Let the reporter write `.symbolized.txt`, then open the report from its Problems row. Check: faulting thread first; mono frames; a `[file:line]` link opens VS at that line; Copy shows "Copied"; the Open buttons launch their files.
  - Reporter window: the well starts below the reason, Close is the rightmost primary, and Enter still closes.

### 8.2 Problems and Console

**Problem** (critique §5 #1-#5, #7, #8; top-10 #8).
- **Severity filter.** The two Problems checkboxes (`ProblemsPanel.hpp:16-21`) collapse into one severity floor (`ProblemsPanel.cpp:47-50`). With Info on (the default), Warnings does nothing, and Errors cannot be hidden.
- **Toolbar.** A red "0" is always drawn outside the toggles (`:35-37`). Colours are literals. Search skips `detail` (`DiagnosticStore.cpp:48`).
- **Tabs.** No counts on either tab (`Begin("Problems")` `:30`; `Begin("Console")` `EditorPanels.cpp:928`).
- **Rows.** Every row hover-highlights, but only locator rows route (`:89-93`). A File locator reaches only an already-open shader document, and does not focus its tab (`EditorAppProject.cpp:231-241`; `FindByPath` never arms `m_focusRequest`). The other six File producers are no-ops.
- **Console noise.** Residency lines at `ARC_INFO` bury the warnings (`NriTextureCache.cpp:352, :588`). A leading `[tag]` is never read as a category (`Panels/ConsoleModel.cpp:15-37`).
- **Command line.** No hint, no history, and no caller of `Arcane::ConsoleModel::Complete` (`ArcaneCore/src/Arcane/Config/ConsoleModel.cpp:17-26`, cvars only). It loses focus after Enter (`EditorPanels.cpp:1210-1221`), and the runtime has a duplicate of it (`ArcaneRuntime/src/RuntimeFrame.cpp:349-367`).

**Design.**

1. **Severity mask** (`Panels/DiagnosticStore.{hpp,cpp}`, pure, already in the tests):
   ```cpp
   enum class SeverityMask : std::uint8_t { None = 0, Error = 1, Warning = 2, Info = 4, All = 7 };
   [[nodiscard]] constexpr SeverityMask MaskOf(Arcane::DiagSeverity) noexcept;
   [[nodiscard]] bool MatchesDiagnosticFilter(const Arcane::Diagnostic&, SeverityMask, std::string_view search) noexcept;
   [[nodiscard]] std::vector<Arcane::Diagnostic> DiagnosticStore::Filtered(SeverityMask, std::string_view search) const;
   ```
   - The floor overloads are removed and their callers migrated. Search matches `message`, `code` or `detail`.
   - `ProblemsUiState` becomes `{ bool showError = true, showWarning = true, showInfo = true; char search[128]; }`.
   - The Console's `visible` lambda (`EditorPanels.cpp:1024-1034`) passes `SeverityMask::All`, and its own three bools keep gating severity.
   - **Theme.**
     - `Theme::kWarning = (0.95, 0.77, 0.30)` (#f2c44d, today's literal value; drafting pick, 9.28) joins `kError` (`EditorTheme.hpp:121`) and replaces the 4 warning literals.
     - The header-only `Panels/SeverityStyle.hpp` maps `DiagSeverity` to `{icon, colour}`: Error = `ICON_LC_CIRCLE_X`/`kError`, Warning = `ICON_LC_TRIANGLE_ALERT`/`kWarning`, Info = `ICON_LC_INFO`/`kText`.
   - **Shared toggle.** `bool SeverityToggle(const char* id, const char* icon, ImVec4 tint, std::size_t count, bool& on)` in `Widgets/EditorWidgets.{hpp,cpp}` stays model-free:
     - It is one `IconToggle` (s4.9) labelled `icon + " " + count + "##" + id`, so the count is inside the hit area.
     - The label is `kTextDim` at zero and `tint` above zero.
   - **Toolbars:**
     - Problems: `[E n][W n][I n] [search]`.
     - Console: `[Clear][Copy][Collapse][Scroll][Wrap]` + the same three toggles + search. The typed `"|"` is gone (`:1023`).

2. **Badges, chip, suppression.**
   - **Tab titles.** Pure `ProblemsTabTitle(nErr, nWarn)` / `ConsoleTabTitle(unseen)` return `"Problems  3###Problems"`, or `"Problems###Problems"` at zero, and the same shape for Console.
     - `###` hashes back to the bare name (`imgui.cpp:2539-2544`), so the ini entry, `DockBuilderDockWindow` (`EditorPanels.cpp:485-486`), `FindWindowByName` and `SelectDockTab("Problems")` (`EditorAppFrame.cpp:2965`) are unchanged.
     - The `PanelRegistry` name stays the menu label.
   - **Tint.** `PushStyleColor(ImGuiCol_Text, worst)` around the `Begin`, popped right after it, tints the tab label (`imgui.cpp:21219`, `:19657`). `worst` is `kError` with any error, else `kWarning` when the count is above 0.
   - **Problems count** = `Count(Error) + Count(Warning)`.
   - **Console count.**
     - `ConsoleUiState` gains `std::uint64_t lastSeenSeq = 0`, and the snapshot moves ahead of `Begin`.
     - Pure `UnseenAlerts(std::span<const ConsoleEntry>, lastSeenSeq)` counts Warning and Error entries with `seq > lastSeenSeq`.
     - Inside the drawn `##consolerows` branch, `lastSeenSeq = entries.back().seq`, so the badge clears the frame after the tab is shown.
   - **Chip.**
     - It fills `ToolbarStatus::problems` (`StripChip{label, color, tooltip}`, s6.5): the worst-severity icon plus the Problems count, colour `worst`, tooltip "N errors, M warnings".
     - `nullopt` at zero.
     - `ToolbarResult::problemsChipClicked` opens the panel (`PanelVisibility`) and calls `SelectDockTab("Problems")`.
   - **Suppression** (drafting pick, 9.28: the brief named only the badges).
     - Pure `UnderVerifyHarness(const HostConfig& c)` in the new `App/HarnessRules.hpp` is `c.headless || !c.screenshotPath.empty() || !c.reportPath.empty()`, a superset of `CaptureWanted` (`EditorAppFrame.cpp:1594-1597`).
     - When it holds, both titles use the zero form with no tint, and `ToolbarStatus::problems` is `nullopt`. This desk's foreign-module warnings (`EditorApp.cpp:2319`) therefore never reach `editor-ui`.

3. **Routable rows.**
   - **Classifier.** Pure, in the new `Panels/LocatorRoute.{hpp,cpp}`, compiled into ArcaneTests:
     ```cpp
     struct RouteFacts {
         std::function<bool(std::uint64_t)>                               entityAlive;
         std::function<std::optional<std::filesystem::path>(const Guid&)> resolveAsset;
         std::function<bool(const std::filesystem::path&)>                hasDocumentFactory;
         std::function<bool(const std::filesystem::path&)>                isDirectory;
         std::function<bool(const std::filesystem::path&)>                exists;
     };
     enum class RouteAction : std::uint8_t { None, SelectEntity, OpenDocument, RevealAsset,
                                             OpenGraphNode, ShowInExplorer, OpenAsText };
     [[nodiscard]] RouteAction ClassifyLocator(const DiagLocator&, const RouteFacts&);
     [[nodiscard]] inline bool IsRoutable(const DiagLocator& l, const RouteFacts& f)
     { return ClassifyLocator(l, f) != RouteAction::None; }
     ```
   - **Rules:**
     - Entity: `SelectEntity` when alive.
     - Asset: `None` when unresolved (`assets.unresolved`, `Assets.cpp:1371`, never routes); `OpenDocument` with a factory; else `RevealAsset`.
     - GraphNode: `OpenGraphNode` when the owner resolves.
     - File: `None` when missing; `ShowInExplorer` for a directory or `.dll`/`.exe`; `OpenDocument` with a factory; else `OpenAsText`.
   - `DocumentHost` gains `bool HasFactory(const std::filesystem::path&) const`, using `RegisterFactory`'s lowercase extension match.
   - **`RouteLocator`** (`EditorAppProject.cpp:209-253`) executes the action:
     - File `OpenDocument`: `m_documents.OpenPath(file)` (focuses the tab), then `RequestJumpToLine(line)` on a `ShaderEditorDocument` result.
     - The shell actions go through `OsShell`.
     - `RevealAsset` calls `RevealAssetInBrowser` (`AssetPanelCommon.hpp:194`).
     - `OpenGraphNode`: open, then `RequestFocusGraphNode(nodeId)`. That one call selects the node, frames it and raises the node-selection event, so the node page shows (s5.1.1).
     - `FindByPath` is deleted.
   - **Drawing.**
     - A routable row's message is a live `LinkRow` (s4.7) with the severity icon as `leadIcon`.
     - A non-routable row is a not-live `LinkRow`: plain text, no hover highlight.
     - `detail` stays dim below the message.
     - Rows with a path (File, Asset, GraphNode owner) get a context menu: **Open** (same as a click), **Show in Explorer**, **Copy path**.
   - `DrawProblemsPanel` builds `RouteFacts` from services the host passes in, so the panel stays free of `EditorApp`.

4. **Noise, level, categories.**
   - **Demotion.** `NriTextureCache.cpp:352` and `:588` go to `ARC_DEBUG`. The `:256` miss `ARC_WARN` stays. Nothing greps that text.
   - **`log.level`**, registered in `ArcaneCore/src/Arcane/Base/Log.cpp` through `CVarRegistry::Get().Register(CVarDesc{...})`, not `ARC_CVAR_RANGED`, because its default is `Init`'s runtime argument and the macro's default is fixed at static init.
     - Int32, min 0, max 6, flags `Archive | Dev`. Help: "Engine log level: 0 trace, 1 debug, 2 info, 3 warn, 4 error, 5 critical, 6 off. Gates stderr, the log file and the Console."
     - **Precedence.** `Log::Init(level)` registers it with `level` as the default: 2 (info) for today's no-argument hosts (`ArcaneEditor/src/main.cpp:320`, `ArcaneRuntime/src/main.cpp:50`; default at `Log.hpp:25`; the first caller wins, `Log.cpp:212`). A host that passes a non-default level is therefore never overridden by the cvar default. An Archive value from `Saved/Config` applies through the callback when config loads.
     - The callback calls the new `ARCANE_CORE_API void Log::SetLevel(spdlog::level::level_enum)` (spdlog `set_level` is runtime-safe).
     - `--set log.level=1` or the console recovers the demoted lines.
     - The stderr sink and the Console's callback sink are separate (`Log.cpp:221-231`, `EditorApp.cpp:1862`), so a Console-only level can be added later without changing this cvar.
   - **Tag categories.** `CategoryForMessage` first reads a leading `[tag]` (1-24 characters of alphanumerics, `-` or `_`, closed by `]`) and returns it verbatim (`nri-graph`, `thumbs`, `PERF`). `e.message` is unchanged, so copy fidelity holds (`FormatConsoleRow`, `ConsoleModel.cpp:85-104`).
   - **New `kPrefixRules` rows:**
     - `Arcane Editor: ` = Editor;
     - `input: ` = Input;
     - `IdeLaunch: ` and `IDE: ` = IDE;
     - `Diagnostics: ` = Diagnostics;
     - `AudioDevice: ` = Audio.
   - **Optional last step** (not gated; drafting pick, 9.28): a Console category combo, "All categories" plus the snapshot's distinct categories, in `ConsoleUiState::categoryFilter`.

5. **Command line.** This is the UX half. Module lifetime stays in cvar plan 2 (`docs/plans/2026-09-24-cvar-system-plan.md:262`).
   - **`CVarRegistry`:**
     - `[[nodiscard]] std::vector<CVarListEntry> ListCommands() const`, with `List()`'s Hidden/Dev rule.
     - The constructor, which already registers `cvarlist` and `cvar_explain` on `this` (`CVarRegistry.cpp:115-116`), also registers `console.historySize` through `CVarDesc` (drafting pick, 9.28): Int32, Archive, default 64, min 1, max 1024, help "Command-line history depth." It goes through `CVarDesc` because every registry instance needs it, test registries included, and the macro targets only `Get()`.
   - **`Arcane::ConsoleModel`** (Core layout change, part of T6's ABI bump):
     - `Complete()` returns the sorted, de-duplicated prefix matches over `List()` and `ListCommands()`.
     - `bool CompleteInput(const CVarRegistry&)`:
       - one match: the input becomes `name + " "`;
       - several: the input becomes their longest common prefix, plus one reply line listing them;
       - returns whether the input changed.
     - `bool HistoryPrev()` / `bool HistoryNext()`. The draft is stashed on the first Up and restored past the newest entry.
     - `Submit` pushes the input onto `std::deque<std::string> m_history`: non-empty only, skipped when equal to the newest entry, capped at `console.historySize` read from the registry it is given.
   - **One widget.** `ARCANE_API bool DrawConsoleInputLine(const char* id, Arcane::ConsoleModel&, Arcane::CVarRegistry&, Arcane::Permission)` in the new `ArcaneClient/src/Arcane/ImGui/ConsoleInputLine.{hpp,cpp}`:
     - `InputTextWithHint`, hint "cvar or command -- Tab completes, Up/Down history";
     - flags `EnterReturnsTrue | CallbackCompletion | CallbackHistory`, with the callback rewriting the buffer through the model;
     - `SetKeyboardFocusHere(-1)` after a submit;
     - returns true on submit.
   - **Both hosts.** The editor's model moves from the function-local `static` (`EditorPanels.cpp:1015`) into `ConsoleUiState::cvars`, and the 6-line reply cap stays. `RuntimeFrame.cpp:349-367` becomes one call with `Permission::Player`.

**Does not.**
- Acknowledge environment warnings (#6) or collapse remedies (#7; only search is fixed).
- Jump from Console rows to source (#9, refuted: `ARC_*` carries no source location, `Log.hpp:130-135`).
- Add a mono log, ms timestamps or a search syntax (#10).
- Persist history, or add a Console-only log level.

**Verification.**
- **`EditorDiagnosticStoreTest`:** the floor cases at `:92` and `:107` become mask cases (Info without Warnings; Errors hidden), plus `detail` search.
- **New `LocatorRouteTest`:** every kind and action with fake facts; an unresolved asset is `None`; a directory or dll is `ShowInExplorer`; a factory-less asset is `RevealAsset`.
- **`EditorConsoleModelTest`:** `[nri-graph] X` gives category `nri-graph` with the message unchanged; `[` without `]` falls through to the prefix table; every new prefix row; `UnseenAlerts`, `ProblemsTabTitle` and `ConsoleTabTitle` at zero and above.
- **`CVarRegistryTest` (`:197`):** `ListCommands` includes `cvarlist`; `Complete` covers commands; `CompleteInput` single-match and longest-common-prefix cases; history Up/Down with draft restore; consecutive duplicates skipped; the cap follows `console.historySize`; `log.level` exists with range 0..6.
- **`UnderVerifyHarness`** unit test.
- **Goldens:** `editor-ui` passes without a re-bless. That is the suppression witness.
- **Desk:**
  - on this desk, the Problems badge and chip show tinted amber;
  - Warnings toggles while Info is on;
  - a plugin-dll File row opens Explorer, and an asset row with no editor reveals in the Browser;
  - `log.level 1` brings back the residency lines;
  - Tab and Up work in both hosts, and focus is kept after Enter.

### 8.3 Input Actions

**Problem** (critique §7 #1, #2; top-10 #9).
- **Space placeholders.** `AddBinding`, `AddComposite` and `AddPart` default every slot to `<Keyboard>/space` with no groups (`InputActionsEditorModel.hpp:78-85`, `.cpp:502-568`). Every UI caller takes those defaults and arms no capture (`InputActionsDocumentWidgets.cpp:204, 207-208, 349-350, 457, 460-461, 473`). A new 2D Vector is four Space bindings in every scheme.
- **Hidden Rebind.** Rebind is painted only on hover or selection (`:541-547`), at a per-row x (`:529-530`). That is as specified (`docs/superpowers/specs/2026-09-28-input-actions-editor-redesign-design.md:61`).
- **No double-click.** `RowWithThumb` passes `AllowDoubleClick` (`EditorWidgets.cpp:810-815`), but `AssetRowResult` does not report it (`EditorWidgets.hpp:274-279`).

**Design.**

1. **Pending add** (new pure `Documents/InputPendingAdd.{hpp,cpp}`, compiled into ArcaneTests):
   ```cpp
   struct PendingAdd {
       enum class Kind : std::uint8_t { Binding, Composite, Part, RebindComposite };
       Kind kind; Guid map, action, binding;          // binding: Part / RebindComposite target
       std::string composite;                         // "1DAxis" | "2DVector"
       std::vector<std::string> roles;                // capture order
       std::vector<Guid> parts;                       // RebindComposite: existing part ids, in order
       std::vector<std::string> captured;             // paths so far, parallel to roles
       std::vector<std::string> groups;               // [] = ungrouped
       [[nodiscard]] bool Done() const noexcept { return captured.size() == roles.size(); }
   };
   [[nodiscard]] PendingAdd MakeAddBinding(Guid map, Guid action, std::vector<std::string> groups);
   [[nodiscard]] PendingAdd MakeAddComposite(Guid map, Guid action, std::string composite, std::vector<std::string> groups);
   [[nodiscard]] PendingAdd MakeAddPart(Guid binding, std::string role);
   [[nodiscard]] std::optional<PendingAdd> MakeRebindComposite(const nlohmann::json& draft, Guid binding);
   bool CommitPending(InputActionsEditorModel&, const PendingAdd&);   // captured.empty() -> false, no edit
   ```
   - **Role orders:** 1DAxis is `negative, positive`; 2DVector is `up, down, left, right`; Part is its one role; RebindComposite uses the existing parts in draft order.
   - **`CommitPending`** makes exactly one model call, which is one undo step:
     - Binding: `AddBinding(map, action, captured[0], groups)`.
     - Composite: `AddComposite(map, action, composite, zip(roles, captured), groups)`, with only the captured roles.
     - Part: `AddPart(binding, role, captured[0])`.
     - RebindComposite: the new `SetPartPaths(binding, zip(parts, captured))`, labelled "Rebind composite" (drafting pick, 9.28).
   - **Model signatures.** The Space defaults are removed.
     - New signatures: `AddBinding(map, action, std::string path, std::vector<std::string> groups)` and `AddComposite(map, action, std::string composite, std::vector<std::pair<std::string, std::string>> parts, std::vector<std::string> groups)`.
     - They refuse an empty path, an empty parts list, an invalid role, or a group no scheme's `bindingGroup` names (drafting pick, 9.28). The model has to guard itself because `ApplyEdit` does not validate (`.cpp:353-364`).
     - Groups are written as the binding's or composite's `"groups"` array, the loader's shape (`InputActionAsset.cpp:73-87`).
   - **Document** (`InputActionsDocument`) holds `std::optional<PendingAdd> pending_`.
     - `BeginPending(PendingAdd)` sets it and calls `capture_.Begin(Guid::Generate(), nullopt, 10.0f, previewSnapshot_)`. Any non-nil id works (`InputRebindOperation.cpp:48`).
     - In `TickCapture` (`:150-203`), while `pending_` is set:
       - **`Completed`** appends `replacementPath`. If `!Done()`, the next role begins at once; the operation ignores controls held at `Begin`, so the completing key does not count. If `Done()`, it calls `CommitPending`.
       - **`Canceled`/`TimedOut`** (Esc, focus loss, click-away or the 10 s timeout, all as today) call `CommitPending`. Esc on the first part therefore adds nothing, and a later Esc commits the parts captured so far. A partial composite is legal (`InputActionAsset.cpp:150-163`).
       - If the model refuses the commit (the action vanished), nothing is added and `ARC_WARN("input: ... nothing added")` is logged.
     - `InputSwallowed()` covers every step.
   - **Group prefill**, resolved when capture begins:
     - `state_.schemeFilter` is used when some `controlSchemes[].bindingGroup` equals it;
     - otherwise, and under "All schemes", the add is ungrouped.
     - `DrawToolbar` re-validates the filter every frame and resets one that names no scheme to All (`:219-228`) (drafting pick, 9.28). Today nothing re-checks it after `EditScheme`/`RemoveScheme`.
   - **Widgets.**
     - `Services` gains `beginAdd(PendingAdd)` and `std::function<const PendingAdd*()> pending`.
     - The Add menu, the `+ Binding` ghost, the action context menu and "Add part" all call `beginAdd` with the matching `Make*`.
     - While a pending add targets an action, its `+ Binding` ghost becomes amber ghost rows:
       - simple add: "Press a control... Esc cancels · N s";
       - composite: a header ghost ("2D Vector") plus one ghost per role. Captured roles show their display string, the live role shows the amber countdown, and later roles show a dim "<role> -- waiting".

2. **Rebind column** (amends the 2026-09-28 spec's line 61, which said hover-only).
   - `InputActionsDocumentState` gains `float rebindColumnX = 0` (content-relative) and `float rebindColumnXNext`.
   - Each drawn Binding/Part row records `max(rebindColumnXNext, ownTrailingEnd)`. At the end of `DrawActions`, the value is copied to `rebindColumnX` and `rebindColumnXNext` is reset, so the column shrinks one frame after the widest row leaves.
   - The button sits at `max(ownTrailingEnd, min(rebindColumnX, contentMaxX - buttonW))`.
   - It is always painted:
     - at rest: `ImGuiCol_Button` fill with "Rebind" in `kTextDim`;
     - row hover or selection: the text is `kText`;
     - button hover: `ImGuiCol_ButtonHovered`.
   - Unchanged: the always-submit rule (`:533-537`), and the skip on composite headers and during capture (`:531`).
   - `state.probe` records `"rebind:<id>"` (the button centre) for tests.

3. **Double-click.**
   - `AssetRowResult` gains `bool doubleClicked = clicked && IsMouseDoubleClicked(ImGuiMouseButton_Left)`, set right after the `Selectable`.
   - In `DrawRow`, after `:395`, when `!swallowed`:
     - Binding or Part: `beginRebind(row.id)`;
     - Action: `OpenRenameOn(model, state, row.id)`;
     - CompositeHeader: `beginAdd(*MakeRebindComposite(model.Draft(), row.id))`.
   - Starting while the button is still down is safe: `TickCapture` has already run this frame (`InputActionsDocument.cpp:215`), and the held control is ignored until it is released.

**Does not.**
- Make "Unbound" (an empty path) a legal asset state.
- Infer a group from the device (the name-keyed pill bug, critique §7 #5).
- Touch scheme UX (#7), column width (#3), hierarchy (#4), vocabulary (#6), or the combo preview showing the raw group (`:219`).

**Verification.**
- **`InputActionsEditorModelTest`:**
  - the cases at `:179, :228, :645` pass explicit paths;
  - `AddComposite` writes exactly the given parts and groups in one undo step;
  - it refuses an empty parts list, a bad role and an unknown group;
  - `SetPartPaths` is one step;
  - a grep check that no `<Keyboard>/space` default remains in `InputActionsEditorModel.hpp`.
- **New `InputPendingAddTest`:**
  - role orders;
  - committing with 0 captured adds nothing and leaves the undo count unchanged;
  - 2 of 4 captured gives a composite with `up` and `down`;
  - RebindComposite keeps the part ids.
- **`InputActionsDocumentUiTest`** (the `DocUi` harness at `:32`, driving capture through `SetPreviewSnapshot`, with its probe extended to binding rows):
  - `+ Binding` leaves the draft unchanged while `InputSwallowed()`, and a W press commits one binding grouped by the active scheme filter;
  - `+ Binding` then Esc leaves the draft and the undo stack untouched;
  - 2D Vector, W, then Esc gives one composite with one part;
  - on the second frame, every visible `rebind:<id>` x is equal, and clicking an unselected row's button arms capture;
  - double-click arms capture on a binding, opens rename on an action, and steps the parts on a composite header.
- **Golden:** T6 runs after T4 (s2.1), so the Input Actions change to `editor-input-doc` is its own slot re-bless in T6, done by the standard procedure (s2.6).
- **Desk:** add a binding, a 1D axis and a 2D vector under a scheme filter; press Esc mid-composite; check the Rebind column is aligned at rest.

### 8.4 Start page

**Problem** (critique §9 #7, #8, §1 #9; top-10 #10, revised: no Hub mention or launch, and no process calls to the Hub).
- A bare launch raises `ShowOpenFileDialog` with no `defaultPath`, filtered to `*.arcproj` (`EditorAppFrame.cpp:2552-2560`), although a folder is a valid project (`Project.hpp:46-62`).
- `ShowOpenFolderDialog` is never called (`Window.hpp:93-94`).
- Recents fill at boot (`EditorRecents.cpp:43-49`) but are drawn only in the File menu (`EditorPanels.cpp:134-168`).
- The project-less shell reads "No project open (data/-next-to-exe)" (`AssetPanelCommon.cpp:102`) above an enabled Create button (`AssetBrowserPanel.cpp:901`).
- The stale comment claiming a bare launch never reaches the project-less state (`EditorApp.cpp:90-96`) is removed in T4 (s6.4). T6 confirms no copy of it remains.

**Design.**
- **Model.**
  - `RecentProject` gains `std::uint64_t lastOpenedUnix = 0`.
    - `Recents::Parse` (`RecentProjects.cpp:130-153`) reads the on-disk key `lastOpenedUtc`: UNIX seconds, written as a decimal string by both the editor (`RecentProjects.cpp:277`) and the Hub (`ArcaneHub/src/lib/api.ts:9`).
    - It accepts either a numeric string or a JSON number. Anything else gives 0, and the entry is never dropped for it. The write side is unchanged.
  - New pure `Project/StartPageModel.{hpp,cpp}`, following the `RecentSelection`/`ConsoleBuffer` precedent:
    ```cpp
    struct StartPageRow   { std::string name, path, opened; };
    struct StartPageModel { std::vector<StartPageRow> rows; std::string hiddenLine; };
    [[nodiscard]] std::string RelativeOpened(std::uint64_t lastOpenedUnix, std::uint64_t nowUnix);
    [[nodiscard]] StartPageModel BuildStartPage(const RecentSelection&, std::uint64_t nowUnix);
    [[nodiscard]] std::string DialogStartDir(const RecentSelection&);   // parent of the first row's project dir, "" if none
    ```
  - **`RelativeOpened`** bands (drafting pick, 9.28):
    - `""` for 0;
    - "opened just now" for a future time or under 60 s;
    - "opened N minute(s) ago" under 60 min;
    - "opened N hour(s) ago" under 24 h;
    - "opened yesterday" under 48 h;
    - "opened N days ago" under 30 days;
    - "opened on YYYY-MM-DD" (local date) after that.
  - **`hiddenLine`** reuses the menu's "N project(s) hidden (built for another engine version)". The filter is the menu's `Select`: other ABIs are counted, current and missing paths are dropped, capped at 10.
- **Window.**
  - `EditorApp::DrawStartPage()` runs just before `m_documents.DrawAll(m_viewportDockId)` (`EditorAppFrame.cpp:2496`).
  - **Visibility** is `!m_runtime->CurrentProject()`, derived every frame and never latched. A switch that fails after teardown brings the page back, and a successful switch hides it with no teardown code.
  - `SetNextWindowDockID(m_viewportDockId, ImGuiCond_Always)`, then `Begin("Start###startpage", nullptr, ImGuiWindowFlags_NoSavedSettings)`. No Close X, (default).
  - On the rising edge of visibility: `SetNextWindowFocus()` and `m_recents.RefreshAll(nullptr)`.
  - **Body**, a centred column at most 640 px wide:
    1. the heading "No project open";
    2. **Open Project...** (`menuReq.openProject`) and **Open Folder...** (new `menuReq.openProjectFolder`);
    3. "Recent projects": one full-width row per `StartPageRow` with the name, the dim path and the right-aligned `opened` text, full path in the tooltip;
    4. the disabled `hiddenLine`, or "No recent projects" when there are no rows.
  - **Row click:** `m_dialogs.projectOpen.Stash(Arm(), path)`, the Open Recent slot (`:2567-2568`), so every guard in `ConsumeProjectDialogResult` and `SwitchProject` applies.
  - The body is drawn as ordered sections, so crash-window plan 3's "Recover" section can slot in above "Recent projects".
- **Dialogs and menu.**
  - **Deleted:** `RaiseOpenProjectOnStart`, `m_raiseOpenProjectOnStart` (`EditorApp.hpp:112-115, :2223-2226`), its consume (`:2552-2556`), and the call at `main.cpp:633`.
  - `openProject` passes `defaultPath = DialogStartDir(m_recents.projects)`, or null when that is empty.
  - `openProjectFolder` calls `m_gpu->Win().ShowOpenFolderDialog(&EditorApp::PathPickedThunk, new PathDialogRequest{&m_dialogs.projectOpen, m_dialogs.projectOpen.Arm()})`. The callback signature matches (`Window.hpp:93, 103`).
  - The File menu gains "Open Folder..." under Open Project.
- **Empty states.**
  - `DrawAssetPanelNoProjectMessage` reads "No project open".
  - Create is wrapped in `BeginDisabled(!project)`, with the tooltip "Open a project to create assets" (`AllowWhenDisabled`).
  - The default empty scene (Main Camera) is kept, (default).

**Does not.**
- Mention or launch the Hub, or make any process call to it.
- Write the shared recents file: no "remove from list", no touch.
- List missing or other-ABI projects (they appear only as a count).
- Add a Recover section.
- Change headless behaviour: the `main.cpp:559-566` refusal stays ahead of `EditorApp` construction.

**Verification.**
- **`EditorRecentProjectsTest`:** `lastOpenedUtc` as a string, a number, missing and garbage. The entry is kept every time.
- **New `StartPageModelTest` (`[editor]`):**
  - every `RelativeOpened` band, including the future and 0 cases;
  - the rows mirror the selection;
  - the `hiddenLine` plural forms;
  - `DialogStartDir` for folder-shaped and `.arcproj`-shaped entries.
- **New non-`[gpu]` witness** in `EditorWitnessTest.cpp`: `ArcaneEditor --headless --frames 1` with no project exits 2, and stderr contains "no project selected" (`ArcaneEditor/src/main.cpp:562-565`). Nothing pins this today.
- **No golden:** headless refuses a no-project run.
- **Grep:** no "never from a bare launch" claim remains in `ArcaneEditor/src`.
- **Desk:**
  - a bare windowed launch shows the Start tab in front in the Viewport node, with recents and relative times;
  - a recent opens;
  - Open Folder opens a project folder;
  - a failing open brings the page back, and a successful one hides it;
  - the Asset Browser shows the empty state and a disabled Create.

## 9. Decisions taken in the brainstorm (2026-09-30)

The user approved the whole plan on 2026-09-30 ("write what you need"); these decisions are binding. **(default)** = a recommended pick accepted as a group (9.27); 9.28 lists drafting choices the brief did not rule on. UE paths are under `D:\dev\_reference\UnrealEngine-5.8.2-release\Engine\Source\`: **ET**, **ES**, **PL**, **OT** = `EditorTransaction.cpp`, `EditorServer.cpp`, `PlayLevel.cpp`, `ObjectTools.cpp` in `Editor/UnrealEd/Private/`, **TB** `Editor/UnrealEd/Classes/Editor/TransBuffer.h`. Astra paths are under `ThirdParty/Astra/include/Astra/` (identical to Astra `dev` 8324c78 apart from line endings).

**9.1 One spec, one plan, six gated tranches** (T1 correctness, T2 shared foundations, T3 Inspector pages, T4 shell state, T5 asset file ops, T6 independent surfaces). Each ends green on s2.1 steps 1-4 (Debug+Release build, ArcaneTests, golden gate, 1920x1080 windowed desk check), preceded by the ABI restamp (step 5) when it bumped. Rejected: one plan per critique item (shared rows, cell, undo changes and slots would be re-blessed and re-argued per plan); one ungated arc (T1 regressions would hide under T4's pixel churn).

**9.2 One batched golden re-bless at T4's end.** Other tranches re-bless only slots their gate fails, one at a time, diff read and attributed. The five editor slots are 10 of 14 lanes (`scripts/golden-gate.ps1:335-363`). T6 runs after T4 (s2.1), so `editor-input-doc` is T6's own re-bless (s2.6); the brief's "folds into T4's batch if T6 lands after" cannot trigger under strict tranche order (flagged for the user).

**9.3 Asset page fits; selections are not rerouted (#3, REVISED).** Rejected: the critique's fall-through and single-following-Inspector options; the two-inspector default is the user's layout (`docs/superpowers/specs/2026-09-29-inspector-filters-design.md` s6, s8.7). The 392x330 fit test is the contract.

**9.4 Start page: no Hub mention or launch (#10, REVISED).** The editor makes no process calls to the Hub and only READS the shared recents file. Rejected: a modal; a "shown once" latch (the page is derived every frame from `!CurrentProject()`, so a failed switch brings it back). No Close X **(default)**; default empty scene kept **(default)**.

**9.5 Node page = second member page of `ShaderEditorDocument`, selection mirrored from imgui-node-editor.** Key `"node:<pass>:<id>"` (ids unique per graph only). 0 or 2+ selected → material page **(default)**; rejected: an "N nodes selected" page, common-property editing (L). No `GraphNode*` kept: re-resolve by id every Draw (undo and pass switches destroy nodes; a pass switch destroys the `ed` context, `ShaderEditorDocument.cpp:3766-3770`). Crumb/history landings select without framing; `NavigateToSelection` is reserved for the Problems GraphNode locator, which also selects. No page for post-chain pass-canvas nodes; no per-node preview in v1. Refinement: the pending request is applied before `ed::End` (brief), but the read and `HasSelectionChanged()` run after it, because selection actions are processed inside `End` (`imgui_node_editor.cpp:1353`, snapshot `:1266-1269`).

**9.6 Node-page edits are live while dragging (user decision).** Rejected: commit-only writes (code-map pick; PropertyGrid draft contract; Input Actions precedent). Cost: an EditGesture pair per row; Esc restores and pushes nothing (R2, R3).

**9.7 Descriptions are a core column, `GraphNodeTypeInfo::description`, appended after `category`** (precedent `MaterialGraph.hpp:197-199`). Rejected: an editor table keyed by `GraphNodeType` (second table). ABI bump accepted. `PinNeutralDefault` moves to core beside `GraphPinAcceptsLiteral` (one truth).

**9.8 LATE-BOUND preview seam; two toolbar statuses.** `DocServices`/`MeshDocument::Services` carry `std::function<NriGraphContext*()> chromeGraph` (harvester precedent); Tick retries with a give-up latch. Rejected: deferring `--open-asset` until the chrome graph exists (one entry point, not the class); one merged status ("ok" only with an image).

**9.9 One shared undo stack; scene dirtiness filters by `ICommand::AffectsScene()`, default true.** Rejected: per-document stacks, filtering by `touched`, per-document dirty ids. UE: one global `GEditor->Trans` (Material/Blueprint editors pop its top, `MaterialEditor.cpp:7074`, `BlueprintEditor.cpp:4006`); per-package `bWasDirty` + save fence per transaction (`ET:681-699`), undo applies `dirty = bWasDirty || fence changed` (`ET:837`). Arcane: `IsDirty` compares only the top step id (`SceneSession.hpp:54-57`), so any document Push dirties the scene (CONFIRMED). Default true errs toward visible "unsaved"; comparing against the topmost scene-affecting step keeps undo-to-save-point clean. Vtable → ABI bump.

**9.10 Undo barred during Play** (menu items and document stacks). UE: barrier at PIE start (`PL:2971-2972`), removed at `EndPlayMap` (`PL:676-677`). Today the menu is gated only by `CanUndo` (`EditorPanels.cpp:189-196`), so a Play-time menu Undo restores an edit-mode memento into the Play registry.

**9.11 Expired steps are skipped.** `ICommand::IsExpired()`, default false (closed-document steps, expired T5 file steps). UE: `ET:1631-1666`, `ET:1698-1722`, rule `ET:541-554`; `FChange::HasExpired` (`Runtime/Core/Public/Misc/Change.h:54`). Rejected: today's inert steps.

**9.12 "Can't undo after: \<reason\>"** in the Edit menu after a clear (UE `ET:1490-1496`). The stack is STILL cleared on scene open and project switch, where no handle survives (`EditorApp.hpp:1106-1117`; `EditorAppScene.cpp:162`; UE `ES:2456`).

**9.13 Byte budget + disk spill, generic, cvar-driven:** `editor.undo.maxSteps` (100), `editor.undo.byteBudgetMB`, `editor.undo.spillThresholdKB`; spill to `Saved/UndoCache/<step>.bin`, freed on eviction/`Clear`. UE caps by memory (256 MB, `ES:1257-1259`), evicting oldest (`TB:92-123`). Rejected: a step-count-only cap (T5 delete steps hold bytes).

**9.14 Module-reload dangle is investigated, not assumed.** `ComponentEditCommand::m_descriptor` (`ComponentEditCommand.hpp:56`) may outlive its module. If confirmed, clear on unload with "Game module reloaded" (UE "Reloaded Package", `PackageTools.cpp:1233`).

**9.15 Asset file ops are UNDOABLE, Recycle Bin as safety net (beyond UE).** UE opens no transaction for Content Browser delete/rename/move/duplicate (no `FScopedTransaction` in `Developer/AssetTools/Private`, `Editor/ContentBrowser`, `Editor/ContentBrowserData`); delete is permanent `IFileManager::Delete` (`OT:2653`, `:2672`, `:2709`), with no Recycle Bin call; history is wiped when undo is a deleted asset's only referencer (`OT:2548`; `SDeleteAssetsDialog.cpp:831,863`); a rename without redirector deletes the old file (`AssetRenameManager.cpp:1880`, `:2029-2036`). Rejected: (a) not undoable, bin as undo (code-map pick); (b) `Saved/Trash`; (c) permanent delete. Chosen: FChange-style apply/revert commands, `AffectsScene()` false, with expiry (the one UE borrowing). Delete captures file + `.meta` bytes (spilling) THEN recycles; undo never depends on the bin, which is the recovery after history clears ("Deleted X (restore from Recycle Bin)").

**9.16 v1 = `game://` only.** `source://` refused with an IDE pointer (rename mints a guid, breaks `#include`s and the build); plugin/engine content and cross-mount moves refused; `diag://` delete-only.

**9.17 Referenced delete: confirm, "Delete anyway" allowed.** Referencers = RefIndex inbound ∪ inbound of each DerivesFrom child (one hop) ∪ the live scene's `"assets"` manifest ∪ dirty documents' params ∪ `.arcproj` `bootScene`/`inputActionsAsset`. Dangling guids heal on restore. Rejected: blocking; UE Replace References (deferred). Open-scene and boot-scene deletes refused. Rename keeps the internal `"name"` **(default)**; empty folders via "Move to..." + "New Folder" **(default)**.

**9.18 GUID rule (binding, refined).** Anything outliving registry state keys entities by `Identity` GUID; live components hold Astra handles only if visitor-declared and remapped; copies mint fresh GUIDs; no same-id resurrection; this plan adds no handle holders. Survey evidence:
- Handles are transient: Bevy "serialized `Entity` values persisted to long term storage… will fail to deserialize" (`crates/bevy_ecs/src/entity/mod.rs` L379-387), stable ids via a user `HashMap<MyId, Entity>` (Discussion #3543); Unity `EntityGuid` is session-unique, loads re-handle via `EntityRemapUtility`/`SerializeUtility` (com.unity.entities 1.4.8).
- Live refs = handles + remap: Bevy `MapEntities` (`map_entities.rs` L21-42); EnTT `continuous_loader::map` (`snapshot.hpp` L294-295, L495-504); Unity `EntityRemapUtility`; Fyrox `NodeHandleMap` (`fyrox-graph/src/lib.rs:55-67`) + `instance_id_map` (`fyrox-impl/src/scene/graph/mod.rs:174`).
- Resurrection is fragile: Flecs `make_alive` fails on an id live at another version; EnTT `create(hint)` silently picks a new id; Flecs v4 removed snapshots (MigrationGuide), with an open take/restore repro (flecs #620; cause undocumented, timing is).
- Rejected: GUIDs/GUID index in Astra; same-id resurrection. GUID-keyed per-entity undo is s10.1.

**9.19 Astra changes = the "now" batch only** (Astra `dev` first, then re-vendor).
- **Retire exhausted slots.** Today 255→1 wrap (`Entity/Entity.hpp:114-121`) on all three destroy paths (`Entity/EntityManager.hpp:123,151,197`) with LIFO recycling (`Entity/EntityIDStack.hpp:44-51`): a stale handle revalidates after 255 reuses, ~4 s at one entity per frame. `NextVersion`'s saturating rule (`Entity/Entity.hpp:85-96`) becomes the only rule; `Clear()` recycles live ids at next versions instead of resetting to id 0 (`Entity/EntityIDStack.hpp:140-144`). Precedent: Weissflog, "Once the generation counter would 'overflow', disable that array slot" (floooh.github.io 2018-06-17). Rejected: Bitsquid FIFO queue (delays the alias).
- **Ordered children:** order-preserving `RemoveParent` (today swap-and-pop, `Registry/RelationshipGraph.hpp:193-203`); `SetParent(child, parent, index = npos)`, clamped; `GetChildIndex`; no `MoveChild`.
- **Signals:** `RelationshipGraph::SetParent` returns `bool`; `Registry::SetParent` emits `ParentChanged` only on success (today unconditionally, `Registry/Registry.hpp:1508-1516` vs `RelationshipGraph.hpp:164-172`).
- **`Registry::GetInstanceId()`** (`ParallelCommandBuffer` idiom, `Commands/CommandBuffer.hpp:1784`, `:2116`).
- **Load rejects recycled `nextVersion==0`** (deferred-placeholder encoding, `Commands/CommandBuffer.hpp:1192-1223`).
- Selection: the placement review kept slot retirement (smallest change, makes `IsValid` real for every remaining raw holder) over the necessity review. Dropped: the Load generation fence (retired slots break its "max" rule; `ResetRegistry` bypasses Load, `ArcaneCore/src/Arcane/Base/Runtime.cpp:378-387`) and `CloneEntities`. `IEntityVisitor`, `RemapEntities`, payload remap → s10.1.

**9.20 Every NEW tunable is an Archive cvar, never `constexpr`** (help, min/max, dotted category; `CVarFlags::Archive`, `ArcaneCore/src/Arcane/Config/CVarTypes.hpp:36`). This plan's (s2.4): `editor.undo.maxSteps`, `editor.undo.byteBudgetMB`, `editor.undo.spillThresholdKB`, `editor.graph.fitMaxZoom`, `editor.inspector.assetThumbMinPx` (64), `editor.inspector.assetThumbHeightFraction`, `editor.inspector.materialPreviewFraction`, `console.historySize`; `log.level` is Archive|Dev.

**9.21 Shared asset-reference field in `Panels/` (model-aware); `Widgets/` stays model-free.** One `EditorApp`-owned `AssetRefServices` feeds InspectorServices, DocServices and the Mesh/Sprite Services. Entity-page arm migrates **(default)**. Rejected: a third private picker.

**9.22 Overrides and resets.** Instances: UE-shape override checkbox in the label column, inherited value dimmed/disabled, no "x". Bases and node page: `ICON_LC_ROTATE_CCW`, tooltip "Reset to default". Rejected: both controls on instances.

**9.23 Problems/Console.** Per-severity mask in `DiagnosticStore` (rejected: the floor model, which cannot say "Info without Warnings"). Badges suppressed under the verify/screenshot harness (foreign-module warnings, `diagnostics:foreign-modules`, `EditorApp.cpp:2319`, would make `editor-ui` machine-dependent; rejected: hiding those rows). Console file:line jumps out of scope (`ARC_*` carry no source location; critique refuted). Command-line UX folded in; module lifetime stays in cvar plan 2.

**9.24 Input Actions add = PENDING ADD transaction.** Nothing enters the draft until capture completes; one undo step; Esc adds nothing; Esc on a later composite part commits the parts so far; "Unbound" never persisted. Rejected: insert-then-arm (today's Space placeholder). Rebind column always visible, AMENDING `docs/superpowers/specs/2026-09-28-input-actions-editor-redesign-design.md:61`.

**9.25 Crash viewer source-compiles the reporter's pure files** (`ReportView.cpp`, `SymbolizedText.cpp`, `ReporterArgs.cpp`, `LogTail.cpp`) into ArcaneEditor as ArcaneTests does **(default)**. Rejected: Core promotion now (reporter vocabulary + Core ABI move for two consumers; a third triggers it). Reporter display fixes land too **(default)**.

**9.26 Mechanics.** Astra: commit in `dev` FIRST, then `scripts/sync-astra.ps1`; stage real files by name + `VENDORED.txt`, never the CRLF fan-out. ABI bumps are cheap; each = Aphelyon restamp + module rebuild.

**9.27 Defaults accepted as a group (for review).**
1. Root-only selection disables Delete/Cut/Duplicate/Copy/Drag with a reason tooltip; mixed selection drops the root.
2. Entity-page AssetRef arm migrates to the shared cell (keeps "--", read-only drop guard, identity-guid rule, texture→sprite mint).
3. Shipped OFL monospace face in `data/font` (JetBrains Mono or Cascadia Mono), registered in `EditorFontSet`.
4. 0 or 2+ selected nodes show the material page.
5. Imported meshes list every `slots[]` entry as an AssetRow.
6. `kAccent` #5b7fa6 plus hovered/active.
7. Unfocused dock nodes: overline at ~45% alpha.
8. Play presence: 2 px accent viewport frame + accent Stop, no strip tint.
9. `kTextDim` ~#8e8e8e; `DisabledAlpha` 0.6 → ~0.45.
10. Rename keeps the internal `"name"`.
11. "Move to..." + "New Folder" verbs.
12. Reporter files source-compiled; Core promotion on a third consumer.
13. Reporter fixes: `DetailsBody()` without repeated header; Close (primary, right), Relaunch, secondaries; Copy feedback.
14. No Close X on the start page.
15. Default empty scene (Main Camera) kept with no project.

**9.28 Drafting picks (not in the brief; for review).** Body tag: "(drafting pick, 9.28)".
- **Mechanics.** 1. `ARC_CVAR_RANGED(name, module, type, default, min, max, flags, help)` beside `ARC_CVAR` (which hard-codes "engine"); `log.level` and `console.historySize` register via `CVarDesc` (s2.4). 2. `byteBudgetMB` 512, `spillThresholdKB` 256. 3. `PreviewStatus` and the schema-13 `DocumentPreview` record (lowercase kebab ids) move T3 → T1 (s2.3, s3.2). 4. One `<step>.bin` per step, payloads at recorded offsets, freed with the last; `UndoLimits.spillThreshold`; `MakePayloadFromFile` (1 MB chunks); `PayloadBytes()` counts spilled bytes (s3.3). 5. Play entry runs `FlushGestures`; one `UndoBarred` predicate; menu Undo also disabled while `InTransaction()`, tooltip "Finish the current edit first" (s3.3). 6. Witnesses E8 (T1 mesh preview), E9 (T3 node page); E6/E7 exist.
- **T1/T2.** 7. Root-only tooltip wording ("The scene root can't be deleted"). 8. `SliderRow` over ranged `FloatRow`. 9. Reserved empty reset slot at default. 10. Override-checkbox tooltip wording. 11. Instance crumb "<title> (Instance)". 12. JetBrains Mono over Cascadia Mono. 13. `OsShell` in `Project/`. 14. T2 order: `BeginPopupBelow` first. 15. `EditGesture::EndAfterRow` + `LastRowEvents().cancelled` (s4.1(f)). 16. Asset picker opens from a chevron, name double-click opens the asset, entity-page swatch moves left of the boxes (s4.1, s4.2). 17. `fitMaxZoom` range 0.1..2.0, 0.1 fit floor, no snapping to zoom stops (s4.5). 18. `IconToggle` draws no tooltip; `kToggleOn*` alias `kButtonActive` in T2, accent in T4 (s4.9). 19. Asset Graph fit wired in T4 with the canvas shrink (s6.9).
- **T3.** 20. Selection read after `ed::End` (departs from brief; 9.5). 21. No `ColorRow` among pin rows (departs from brief; no literal pin is a colour). 22. Custom pin rename never edits the HLSL body; no page-side name validation. 23. Category colour hex table. 24. Step order, fixture first. 25. `materialPreviewFraction`, `assetThumbHeightFraction` cvars. 26. `PropertyGrid::Section(label, open, trailing)` for "Only overridden". 27. Mesh Info section for generated meshes only.
- **T4.** 28. `kAccentHovered` #6386ad, `kAccentActive` #52769c; contrast test covers resting state only (icon on hovered accent is 2.87:1). 29. `ViewportChrome` replaces `showToolOverlay`; `ToolbarResult` replaces bool + out-param; `StripChip` slot contract. 30. "Stop" = the existing Play/Stop toggle made accent.
- **T5.** 31. One delete modal replaces the brief's separate Discard/Cancel prompt. 32. Companion `.arcmesh` duplicates strip `importedSource` (inert mesh, no in-editor source route; alternative: refuse).
- **T6.** 33. Chip also suppressed under the harness; `UnderVerifyHarness` = `headless || screenshot || report`. 34. `console.historySize` (64, 1..1024); `log.level` Int32 0-6, default = `Log::Init`'s level. 35. `Theme::kWarning` at the current literal. 36. `ThreadView::frames`; `DisplayProduct` ("Arcane Editor crashed"); `Layers:` in the GPU section; action row under the header. 37. Late `.symbolized.txt` rechecked on focus edge/reopen, not a timer. 38. Reporter Close accent-filled. 39. `RelativeOpened` bands. 40. Model refuses unknown groups; stale scheme filter resets to All. 41. Whole-composite rebind = one "Rebind composite" step. 42. Optional Console category combo specified but outside the gate.

## 10. Out of scope, and owed afterwards

### 10.1 Entity-record undo arc: RIGHT AFTER this phase, ahead of crash-window plan 3
Structural undo stays on `RegistryStateCommand` (whole-registry mementos restoring EXACT ids, `ArcaneClient/src/Arcane/Edit/RegistryStateCommand.hpp:3-8`). The arc:
- GUID-keyed per-entity records for create, delete, reparent, add/remove component, hide, paste, duplicate: component bytes (descriptor binary path), parent GUID, sibling index (9.19); undo creates fresh (9.18).
- GUID→handle index, hits re-checked (`IsValid(e)` and `Identity::id == g`), rebuilt at `Runtime::RestoreRegistry`/`ResetRegistry` (`ArcaneCore/src/Arcane/Base/Runtime.cpp:335-387`), replacing linear scans.
- Rekey holders: `ComponentEditCommand::m_entity` (`ArcaneClient/src/Arcane/Edit/ComponentEditCommand.hpp:55`), `Transaction::touched` (`CommandStack.hpp:152`), `SelectionContext` (`ArcaneEditor/src/Scene/SelectionContext.hpp:106-107`), Inspector pins/history (`Panels/SceneSelectionKey.hpp:29-34`), collapsed Outliner rows (`Panels/EditorPanels.hpp:347`), `DiagLocator::entity` (`ArcaneCore/src/Arcane/Base/Diagnostics.hpp:641`), `quatEulerViews` (`Panels/InspectorView.cpp:942`), `SceneRoot` (`ArcaneCore/src/Arcane/Scene/SceneResources.hpp:31`).
- Astra (repo first): `IEntityVisitor { virtual void Visit(Entity&) = 0; }` (`IFieldVisitor` style); `Registry::RemapEntities(IEntityVisitor&)` over components and resources; CommandBuffer payload remap (an `Entity` inside an `AddComponent` value is not translated today).
- It waits: nothing in T1-T6 needs it, and 9.18 forbids adding holders it would rekey.

### 10.2 Configuration pass
A settings window over the cvar registry; `Edit > Preferences...` is its home (T4 disables it: "Settings window coming in the configuration pass"). 9.20 means no migration.

### 10.3 Deferred product items
Per-node shader previews (blocked on previews; old machinery deleted in ed540e39); Outliner sibling reordering and type icons (9.19 is the prerequisite); viewport snapping UI; Console source locations (`ARC_*` must carry `std::source_location`); sprite texture editing (Texture row read-only in v1); Asset Graph edge-label clutter; full-pane imported-mesh preview (harvester `RequestMesh`); the critique outside its top 10 and cheap fixes.

### 10.4 Boundaries stated in the body
No pages for post-chain pass-canvas nodes; Comment page edits text, not size; no file ops on `source://`/plugin/engine content, no cross-mount moves, `diag://` delete-only, no Replace References; "Unbound" not persisted; command-line module lifetime in cvar plan 2; no Hub launch or mention; Astra items not taken (Load fence, `CloneEntities`, resurrection, GUIDs in Astra) stay dropped, visitor/remap in 10.1.

### 10.5 Queued order after this phase
1. Entity-record undo arc (10.1). 2. Crash-window plan 3, autosave (`docs/specs/2026-09-22-crash-window-design.md`); its "Recover" section slots into the T6 start page. 3. F5. 4. Editor mini-arc 3. 5. Cvar plan 2. 6. arcbuild II, hygiene, introspection, replication, mini-arc 4.

## 11. Verification

Run order and tranche checklist; the authoritative lists are the **Tests** paragraphs in sections 3-8.

**Every tranche closes with (s2.1):** `msbuild Arcane.slnx` Debug + Release (VS 18, `-nr:false`); `ArcaneTests.exe "~[gpu]"` + `[witness]` (`[shell]` excluded) FROM THE EXE DIR, seed captured, report through `scripts/check-baselines.ps1 -Invocation "~[gpu]"`, count rises committed on purpose; `scripts/golden-gate.ps1 -Configuration Release` then `Debug` (ends on Debug; single-slot `Binaries/`); Astra tranches run `Astra.slnx` in the Astra repo first, then `scripts/sync-astra.ps1` (`-DryRun` first), staging by name + `VENDORED.txt`; every ABI bump moves `kGamePluginABIVersion` (46, `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:987`) with the ReferenceProject restamp in that commit, plus `engine.abi` in `D:\dev\starworks\Aphelyon\Aphelyon.arcproj` and `arcbuild build`; no tranche closes with a restamp owed.

**Golden procedure:** gate, read `Saved\Verify\*-diff.png`, delete exe-dir `imgui.ini`, bless the STAGED slot from the host exe dir (`--headless --backend dx12 --frames 60 --settle 30 --report <p> --compare <slot> --bless`), IMMEDIATELY copy to `ReferenceProject/Verify/References/<slot>.png` (post-build restages `Verify\`), re-gate both configs.

**Desk rig:** windowed, maximized 1920x1080, the user's layout (default since 2026-09-30); `LOCALAPPDATA` at a scratch dir; shader graphs on a SCRATCH COPY of Aphelyon's `Content/logo_showcase.arcmat` (ReferenceProject has no graph-owned material until T3).

### T1: correctness
- Outliner: s3.1 (`EntityOpsTest.cpp`, `World` `:33`, round trip `:497`; `RegistryStateCommandTest.cpp:130-138`; `SelectionOpsTest`; `EntityClipboardTest.cpp:167`/`:211`/`:330` green); raw `Reparent(Invalid)` pins unchanged.
- Previews: s3.2. A null seam never latches; the first non-null attempt builds the vehicle or latches `VehicleFailed`, then Tick stops. E8 (new): `reference_cube.arcmesh` → `documents[0].preview == "ready"`; E5 (`:303-340`) also asserts `ReferenceCubeMaterial` compile `not-compiled-here`. `kSchemaVersion` 12 → 13 (`VerifyReport.hpp:230`) with `$script:ReportSchemaMax` (`golden-gate.ps1:424`), pinned by `VerifyReportTest`.
- Undo: s3.3 (`CommandStackTest.cpp`, pattern `:96`; `EditorSceneSessionTest.cpp:76`, `:91`); three `editor.undo.*` cvars via `ARC_CVAR_RANGED`; `UndoBarred` gates menu, Ctrl+Z and document stacks; the reload investigation's verdict goes in the plan (if confirmed, a test asserts the clear with "Game module reloaded").
- Astra: s3.4, including the `EntityVersionWrap` (`tests/Entity/EntityTest.cpp:455`) rewrite and `RelationshipGraphTest.cpp` order cases.
- Goldens: none expected; `editor-material-page` alone if the preview now renders.
- Desk: unparent strip drag → save → reopen keeps the entity; root-row Delete greyed with tooltip; a material edit leaves title and Save All clean; in Play, Edit > Undo/Redo greyed and document Ctrl+Z inert.

### T2: shared foundations
- PropertyGrid: s4.1 (`PropertyGridTest.cpp`, `GridHarness` `:15-80`): `g.LastItemData` is the value widget for every row type; live-preview Esc re-applies and pushes nothing.
- Asset-reference cell: s4.2: pure `DecideAssetRefDrop`/`AssetRefCandidates`/`DescribeAssetRef`; headless picker Set/Clear via `ActivateItemByID`; first drag-drop case (`BeginDragDropSource` → `AcceptDragDropPayload`); entity-page Inspector tests green on the migrated arm.
- Header: s4.3 (`EditorInspectorHostTest.cpp`): `crumbsNatural` wrap, "..." collapse, leaf ellipsis.
- `BeginPopupBelow`: s4.4's 800x600 context test, including flip-above.
- GraphFit: s4.5 (`GraphCanvasHeadlessTest.cpp:23`): nodes inside canvas, zoom ≤ cap, selection unchanged.
- Helpers: s4.6-s4.9 (`OsShell` refuses a missing path; link row; mono face in `EditorFontSet`; `IconToggle` pushes all three Button colours).
- Goldens: only slots with a diff attributed to T2.
- Desk: logo_showcase fits on the first measured frame and F still frames the selection; Create, Play caret, Input Actions "+ Add" open under their buttons.

### T3: Inspector pages
- Node page: s5.1.11. Witness E9: `--open-asset 7e5a0012-0012-4012-8012-000000000012 --select-in-document <Multiply id>` → `inspector.instances[0].breadcrumb == "NodePageGraph > Multiply"`, `documents[]` "NodePageGraph" = {ok, ready, image true}; no node-page golden.
- Preview status: s5.2.
- Ports: s5.3-s5.5. Material: override/reset undo labels, live preview per row, "Surface" label on the toolbar combo, re-kind still pushes no step. Sprite (`SpriteDocumentUndoTest.cpp:277`) and Mesh (`MeshDocumentTest.cpp:580`, one AssetRow per `slots[]` entry) re-keyed.
- Asset page: s5.6 (`AssetInspectorSourceTest.cpp`, `:235-307` re-keyed): 392x330 → `ScrollMax.y == 0`, every action reachable by `ActivateItemByID`, overflow into `##asset_more`.
- Pinned header: s5.7, including the ScopeGuard on refused-`Begin` frames (`InspectorWindows.cpp:350-355`).
- Fixture lands first; asset-counting tests and goldens re-counted in that commit.
- Goldens: `editor-material-page`, `editor-asset-page`, plus any of `editor-ui`, `editor-ui-perspective`, `editor-input-doc` showing fixture counts or the pinned header; each alone, attributed.
- Desk: s5.1.11's list (inline-widget click, R5); the asset page fits the Assets-only Inspector at 1920x1080 with no scrollbar.

### T4: shell state
- Pure tests: contrast (s6.6); `App/EditorTitle.hpp` formatter (s6.4, beside `EditorSceneSessionTest.cpp:65-124`); `LayoutStripCluster` (s6.5); `DigestRefusedStyle(int)` (s6.7); a hovered lit `IconToggle` keeps the accent (s4.9 on re-pointed tokens); Asset Graph (s6.9, `GraphMouseHarness`, `AssetsGraphCanvasTest.cpp:551-645`): nodes and legend inside canvas minus strip, `entriesStamp`-only rebuild does not refit, creation/focus/kind-filter changes fit, selection unchanged.
- Goldens: THE batched re-bless of all five editor slots (s6.10; 10 lanes); `runtime-scene`, `f3-cull-blend`, `vulkan/`, `inprocess-lit-cube` and `thumbs/` green and untouched (s6.10).
- Desk: overlines on focused/unfocused nodes; Play = 2 px frame + accent Stop only; strip "Project > scene *" / "No project"; Viewport unsaved dot; dim "0 refused"; Preferences... disabled with tooltip; Asset Graph fits on creation; logo_showcase legible under the new dim text.

### T5: asset file operations
- Registry: s7.2 (`AssetRegistryTest.cpp`, TempDir `:36`).
- Planner/executor: s7.3, s7.4 (`AssetFileOpsTest.cpp`): refusal table, 9.17 union, `.gltf` buffer/image set, cascades, `openDocs`; k-th-move rollback, undo/redo per verb, occupied-path undo refusal, expired skip, spill, scene clean throughout. Recycle faked in default runs; one `[shell]` test (excluded like `~[gpu]`) recycles a TempDir file.
- Duplicate: s7.7, with `MaterialAssetTest`/`SpriteAssetTest`/`MeshAssetTest` round trips.
- Model/routing: s7.9, s7.10; `CreateAssetDialogTest` case-only rename (s7.6).
- Survivors: s7.5, s7.6, s7.11 (pages survive rename, pruned on delete; tombstones; Save after `NoteMoved`; guid-keyed artifact lookup survives a move).
- Goldens: `editor-asset-page` alone at s7.13 step 3.
- Desk: s7.13's per-step checks (ReferenceProject scratch copy); a deleted file reaches the real Recycle Bin and restores with its guid.

### T6: independent surfaces
- Crash viewer: s8.1; no golden (`diag://` declined, `ProjectBoot.hpp:171-177`).
- Problems/Console: s8.2 (`EditorDiagnosticStoreTest.cpp:92`, `LocatorRouteTest`, `EditorConsoleModelTest`, `CVarRegistryTest.cpp:197`, `UnderVerifyHarness`); `editor-ui` green through the badges.
- Input Actions: s8.3. Start page: s8.4, including the non-gpu witness (exit 2, stderr from `ArcaneEditor/src/main.cpp:562-565`).
- Goldens: `editor-input-doc`, T6's own re-bless (s2.6).
- Desk: a real `diag://` report (frame link opens Visual Studio AT the line; Copy Details shows "Copied"); independent Problems toggles, badges, chip; Console Tab/Up/Down; add-and-listen incl. a composite; no-project launch shows the start page, a recent opens, Open Folder..., a failed open brings the page back.

## 12. Risks

**R1 Golden churn, stale blesses.** 10 of 14 lanes read the five editor slots (`scripts/golden-gate.ps1:335-363`) under dE94; post-build restaging overwrites staged-only blesses; T3's fixture shifts displayed counts. *Mitigation:* 9.2's batch, else slot-by-slot on attributed diffs; bless staged, copy IMMEDIATELY; delete `imgui.ini`; gate both configs ending on Debug; fixture first in T3, re-counted in that commit.

**R2 Esc during a live drag.** Esc restores the seed and calls `ClearActiveID` (`ArcaneEditor/src/Widgets/PropertyGrid.cpp:193-199`), so ImGui reports `IsItemDeactivatedAfterEdit` and `EditGesture::EvaluateEnd` returns **Commit** (`Scene/EditGesture.cpp:5-12`); `CommandStack::Cancel` discards without reverting. *Mitigation:* a before==after guard in every live builder (param path exists, `ShaderEditorDocument.cpp:5954-5957`; graph-literal, sprite, mesh, material-rendering via `GraphToJson` or value compare) and the restored value re-applied to the preview; T2/T3 pin Esc-pushes-nothing per row type; `EndAfterRow` closes grouped-row cancels at the row (s4.1(f)).

**R3 The gesture reads `g.LastItemData`.** A trailing reset/browse/clear/override control becomes the last item and the gesture never opens ("SUBMISSION ORDER IS LOAD-BEARING", `ShaderEditorDocument.cpp:5858-5866`). *Mitigation:* decorations submit before the value widget (s4.1(d), binding); a T2 test per row type.

**R4 Two sources of node selection.** `ed` and the mirror can disagree; `ed` may list an undone id; pass switches destroy the context (`ShaderEditorDocument.cpp:3766-3770`); writes outside `Begin`/`End` are ignored. *Mitigation:* write `ed` only via the pending request between `HandleGraphEdits` and `ed::End`; rebuild the mirror after `ed::End` each drawn frame, validated by `Resolves`; a pass switch's empty context resets the mirror next frame unless a pending `Select` is applied (`RestoreSelection`); key restores arm the request with no epoch bump.

**R5 Inline canvas widget clicks may not select the node** (e.g. Sine `x`). *Mitigation:* named T3 desk item; `NoteContentClick` still re-asserts the document; finding recorded in the plan.

**R6 GraphFit zoom and internals.** `NavigateToContent` has no clamp (small graphs zoom past 2.0 into blurred glyphs, `Panels/AssetGraphPanel.cpp:1320-1328`); the helper needs `imgui_node_editor_internal.h` (precedent `Widgets/GraphZoomLevels.hpp`). *Mitigation:* one helper with cap (≤1.0) and 0.1 floor; never on cook-driven rebuilds; never touches selection; T2/T4 tests.

**R7 ABI bumps fan out (s2.3).** T1 `ICommand` vtable, `CommandStack` layout, `Registry::SetParent` return; T3 `GraphNodeTypeInfo` column, `GraphPinNeutralDefault` export; T5 `AssetRegistry`/`Project`/`Runtime` seams, `Assets` appended virtuals; T6 `ConsoleModel` layout, `CVarRegistry::ListCommands`, `Log::SetLevel`, `DrawConsoleInputLine`. A stale `engine.abi` makes the host refuse the module. *Mitigation:* bump + ReferenceProject restamp in the boundary commit; Aphelyon restamp + rebuild before close; no `[[nodiscard]]` on `SetParent`'s `bool` (seven Arcane, ~20 test call sites). The logo_showcase check needs no module.

**R8 VerifyReport schema.** `kSchemaVersion` (`VerifyReport.hpp:230`) and `ReportSchemaMax` (`golden-gate.ps1:424`) move in one commit or every report is rejected. *Mitigation:* one T1 bump, pinned by `VerifyReportTest` (s3.2).

**R9 Shared recents file.** Hub-owned; the editor never writes an unparseable or newer file (`ArcaneEditor/src/Project/RecentProjects.hpp:21-28`); `lastOpenedUtc` (→ `RecentProject::lastOpenedUnix`) may be string, number or absent. *Mitigation:* read-only; tolerant never-failing parse; tests for all three.

**R10 Headless no-project refusal must stay first** (`main.cpp:559-566`, before `EditorApp` at `:631`); moving it would open a window in a scripted run and hang CI. *Mitigation:* T6's non-gpu witness (exit 2 + stderr); the page derives from project state, never CLI state.

**R11 Recycle Bin API.** `IFileOperation` needs COM (`IdeLaunch`'s `CoScope`, `ArcaneEditor/src/Project/IdeLaunch.cpp:116-122`). Without `FOF_WANTNUKEWARNING` an unbinnable file (no bin, over quota) is deleted permanently with no prompt; s7.4 sets it, so the OS asks first and the progress sink still reports nuked items. Without `FOF_SILENT | FOF_NOERRORUI` the shell raises UI; without an owner window the prompt can open behind the editor. `FOFX_RECYCLEONDELETE` needs Windows 8+ (s7.4). *Mitigation:* undo never depends on the bin (9.15); `ShellRecycle` sets the owner window and records per item whether a bin item was created (`IFileOperationProgressSink::PostDeleteItem`, `psiNewlyCreated` non-null), else the activity reads "Deleted X (permanently; not in the Recycle Bin)" (s7.12); tests inject a fake.

**R12 UndoCache disk use.** Unbudgeted spill would grow without bound; crashes leave orphans. *Mitigation:* `PayloadBytes()` counts spilled bytes toward `editor.undo.byteBudgetMB` (s3.3), so disk ≤ budget; the directory is wiped at project open and close, safe because the stack is empty at open and the per-project `editor.lock` (rival check `main.cpp:583-595`, written `EditorAppProject.cpp:2557`) keeps one live editor per project.

**R13 Stale state after file ops.** The `Assets` JSON cache is path-keyed, memoizes failures, has no evict (`ArcaneCore/src/Arcane/Assets/Assets.cpp:480-486`); `AddFile` keeps the OLD path when a guid moves (`AssetRegistry.cpp:431-436`); the mtime watcher misses deletes/renames (`EditorApp::PollAssetWatch`, `EditorAppProject.cpp:419`). *Mitigation:* the executor is the only mutator (`Rebind`/`Remove`, never `AddFile`), evicts source and destination via `Assets::EvictPath` (s7.2), marks every model dirty; one hash-gated no-op cook per moved texture is accepted.

**R14 Save after a move writes the old path** (two files, one guid; the registry keeps the first). *Mitigation:* `EditorDocument::NoteMoved(path)` and the `SceneSession` equivalent run before the step is pushed (s7.11); T5 tests.

**R15 Misclassified `AffectsScene()`.** A false scene command drops unsaved warnings; a document command left at true keeps today's (safe) false dirtiness. *Mitigation:* explicit T1 list (seven document commands + T5 ops); each document test asserts the scene stays clean.

**R16 Play leaks into document undo.** Documents get `&*m_undo` once at open (`EditorAppProject.cpp:83`), the input-actions model holds its own pointer (`InputActionsEditorModel.hpp:123`), `DrawAll` is ungated in Play (`EditorAppFrame.cpp:2496`). *Mitigation:* `UndoBarred` gates menu, shortcut and every document resolver (input-actions included); Play entry flushes gestures; T1 tests (s3.3).

**R17 Slot-retire lifetime budget.** 24/8 bits ≈ 16.7M ids × 255 ≈ 4.28×10⁹ creates per registry (fine for editor/client); `EntityVersionWrap` (`tests/Entity/EntityTest.cpp:455`) pins the wrap. *Mitigation:* one-time diagnostic on exhaustion; long-running shards use `ASTRA_ENTITY_BITS=64`; the test is rewritten in the same Astra commit (s3.4).

**R18 Ordered children change visible order** after deletes/reparents. At 57af0cab no caller relies on same-parent "moves last" (`Reparent` skips same-parent moves, `ArcaneClient/src/Arcane/Edit/EntityOps.cpp:189`; other sites parent fresh entities). *Mitigation:* re-grep Arcane and Aphelyon at implementation; update order assertions found.

**R19 CRLF fan-out on re-vendor.** `sync-astra.ps1` reports CRLF-only "modified" files. *Mitigation:* stage by name + `VENDORED.txt`; never `git add -A` the vendored tree.

**R20 Machine-dependent badge and chip.** Foreign-module rows vary per machine (`EditorApp.cpp:2319`); `"Problems  3###Problems"` keeps ini/dock identity (`ImHashStr` resets at `###`, `ThirdParty/imgui/imgui.cpp:2539-2545`) but the text still varies. *Mitigation:* harness suppression covers the T4-cluster chip and tab badges; the Console's unseen count is never drawn under the harness.

**R21 Test id churn from ports** (`AssetInspectorSourceTest.cpp:235-307` Copy Path and `sRGB##texmeta`, `SpriteDocumentUndoTest.cpp:277`, `MeshDocumentTest.cpp:580`). *Mitigation:* stable `##` ids; re-key in each port's commit.

**R22 Node clicks fill the history** (depth 32); accepted, as in the Input Actions document. Back/forward onto a node selects without navigating (9.5).
