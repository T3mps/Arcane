# Project Input Actions Authoring and Rebinding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a project-wide gameplay Input Actions asset, its editor, a game-facing `LocalInputUser` with saved rebinding profiles, and a ReferenceProject controller driven by authored actions.

**Architecture:** Keep the existing snapshot evaluator in `ArcaneClient` and extend it to compile versioned `.arcinput` definitions with stable IDs and schemes. Own the gameplay session in `ClientRuntime`, load its asset by the project manifest GUID, and keep host/editor actions in their existing config evaluator. Register `.arcinput` with the asset system and edit it through an `EditorDocument` whose primary view follows the supplied maps/actions/bindings hierarchy.

**Tech Stack:** C++20, ArcaneCore/ArcaneClient DLLs, nlohmann JSON, SDL3 snapshots, ImGui editor, Catch2, Premake/MSBuild.

**Spec:** [Project Input Actions Authoring and Rebinding Design](../specs/2026-09-28-project-input-actions-authoring-design.md)

## Global Constraints

- Keep `InputSnapshot` fixed-size POD.
- Keep engine/editor input configuration separate from project gameplay actions; do not register `data/EngineConfig/input.json` as an asset.
- Use stable GUIDs for maps, actions, bindings, and composite parts; use GUIDs, not names or array indexes, for runtime references and saved overrides.
- Keep one active `LocalInputUser` per running game instance; the first device sampler remains keyboard, mouse, and first connected gamepad.
- Store gameplay binding overrides per project and profile in writable user data, never in project `Content/`.
- Keep external module memory ownership compatible with `/MD`; bump `kGamePluginABIVersion` for any `EngineContext` layout or plugin entry-point change.
- Keep controller movement in MKS units and do not add game-specific automated tests for the ReferenceProject controller.
- Regenerate Premake projects when source file lists change; build ArcaneCore before ArcaneClient/editor/runtime and build ReferenceProject through the SDK.

## Review Focus

- Legacy host `input.json` has no stable IDs and must continue to load through the existing host evaluator path.
- Same-named actions in different maps must resolve by GUID; ambiguous mapless name lookup must fail explicitly.
- A scheme switch must filter bindings without losing authored paths or applying overrides to unrelated bindings.
- A press and release before the next fixed tick must remain ordered; the first fixed tick receives transitions once and later ticks do not repeat them.
- A stale or malformed profile entry must not erase a valid asset or the last good in-memory profile.

---

### Task 1: Add the versioned gameplay action asset model

**Files:**
- Create: `ArcaneClient/src/Arcane/Input/InputActionAsset.hpp`
- Create: `ArcaneClient/src/Arcane/Input/InputActionAsset.cpp`
- Test: `ArcaneTests/src/InputActionAssetTest.cpp`
- Modify: `premake5.lua` only if ArcaneClient or ArcaneTests source globs do not include the new files.

**Interfaces:**
- Produces `Arcane::InputActionAsset`, `InputActionMapDefinition`, `InputActionDefinition`, `InputBindingDefinition`, and `InputControlSchemeDefinition` value types. The asset retains its source JSON so unknown metadata fields round-trip without entering the evaluator's compiled representation.
- `InputActionAsset::FromJson(const nlohmann::json&, std::string* error = nullptr) -> std::optional<InputActionAsset>` validates native version 1 documents.
- `InputActionAsset::ToJson() const -> nlohmann::json` serializes the same IDs and hierarchy.
- `InputActionAsset::CreateDefault() -> InputActionAsset` creates a valid empty document and fresh asset GUID.
- Definition value types carry GUID identity, display name, action type, binding path, processors/interactions, composite parts, scheme groups, and map blocking behavior.

- [ ] **Step 1: Add failing codec tests** named `input asset: parses and round-trips a version 1 document`, `input asset: rejects duplicate or missing stable IDs`, `input asset: validates the default map reference`, `input asset: rejects invalid composite references and scheme groups`, `input asset: rejects unsupported versions`, and `input asset: preserves unknown fields`. Assert stable asset/map/action/binding/part GUIDs, hierarchy order, binding paths, processor/interaction arrays, default map, scheme groups, and unknown fields survive a round trip.
- [ ] **Step 2: Run the targeted test** from `bin\Debug-windows-x86_64-md\ArcaneTests`: `ArcaneTests.exe "[input][asset]"`. Confirm the new cases fail because the codec/types do not exist.
- [ ] **Step 3: Implement the version 1 value model and strict codec** in the two `InputActionAsset` files. Require top-level `id`, `version: 1`, `actionMaps`, and `controlSchemes`; include optional `defaultMap` as a map GUID (required when a nonempty asset is ready for runtime use); require unique non-nil GUIDs at every identity level; reject unsupported versions. Preserve unknown JSON fields verbatim in the source document and ignore them during evaluation so editor saves do not discard metadata.
- [ ] **Step 4: Re-run the targeted test** and confirm all codec assertions pass, including invalid-document error text identifying the offending map/action/binding.
- [ ] **Step 5: Commit** as `feat(input): add versioned gameplay action asset model`.

### Task 2: Compile stable IDs, control schemes, and overrides in the evaluator

**Files:**
- Modify: `ArcaneClient/src/Arcane/Input/InputActions.hpp`
- Modify: `ArcaneClient/src/Arcane/Input/InputActions.cpp`
- Modify: `ArcaneTests/src/InputActionsTest.cpp`

**Interfaces:**
- Consumes `InputActionAsset` from Task 1.
- Add `bool LoadAsset(const InputActionAsset&)`, `bool SetControlScheme(std::string_view)`, `std::optional<Guid> FindAction(std::string_view map, std::string_view action) const`, `std::optional<Guid> FindAction(std::string_view action) const`, `InputActionValue Value(const Guid&) const`, `std::optional<bool> ButtonDown(const Guid&) const`, `std::optional<float> ScalarValue(const Guid&) const`, `std::optional<glm::vec2> VectorValue(const Guid&) const`, `std::optional<InputActionPhase> Phase(const Guid&) const`, and `bool SetBindingPath(const Guid& binding, std::string_view path)` to `InputActions`.
- Add value-returning `Maps()`, `Actions(mapId)`, `Bindings(actionId)`, and `BindingDisplayString(bindingId)` queries so game-built controls screens can list authored and effective bindings safely.
- `InputActionValue` carries `InputActionType { Button, Axis1D, Axis2D }`, `bool down`, `float scalar`, `glm::vec2 vector`, and `InputActionPhase { Waiting, Started, Performed, Canceled }`.
- Keep existing name-based queries and `LoadJson` behavior for host config compatibility.

- [ ] **Step 1: Add failing evaluator tests** named `input: native asset queries by stable action ID`, `input: ambiguous unqualified action names do not resolve`, `input: typed queries reject action type mismatches`, `input: scheme groups filter bindings`, and `input: binding path override affects evaluation without mutating asset data`. Include same-named actions in two maps and keyboard/gamepad alternatives; with no active scheme, grouped and ungrouped bindings are all eligible.
- [ ] **Step 2: Run** `ArcaneTests.exe "[input]"` from the test executable directory and confirm these cases fail.
- [ ] **Step 3: Compile native definitions into ID-indexed maps/actions/bindings** while preserving existing string lookups. When a scheme is selected, evaluate matching-group and ungrouped bindings; when no scheme is selected, evaluate all bindings. `SetBindingPath` changes only the compiled/effective path for the given binding ID and resets that binding's transient interaction state.
- [ ] **Step 4: Re-run** `ArcaneTests.exe "[input]"`; also confirm existing legacy `LoadJson` cases still pass.
- [ ] **Step 5: Commit** as `feat(input): resolve gameplay actions by ID and control scheme`.

### Task 3: Preserve action transitions for fixed simulation

**Files:**
- Modify: `ArcaneClient/src/Arcane/Input/InputActions.hpp`
- Modify: `ArcaneClient/src/Arcane/Input/InputActions.cpp`
- Test: `ArcaneTests/src/InputFixedTransitionsTest.cpp`

**Interfaces:**
- Add `InputActionTransition { Guid action; InputActionPhase phase; uint64_t sampleIndex; }`.
- Add `void BeginFixedStep()`, `bool PressedThisFixedStep(const Guid&) const`, `bool ReleasedThisFixedStep(const Guid&) const`, and `std::span<const InputActionTransition> TransitionsThisFixedStep() const` to `InputActions`.
- `Update(dt, snapshot)` records every action phase transition in sample order. The next `BeginFixedStep()` publishes all unconsumed transitions and consumes them; later fixed steps see an empty transition list until another sample arrives.

- [ ] **Step 1: Add failing tests** named `input fixed: press survives a render frame with no fixed step`, `input fixed: press and release before one fixed step retain order`, and `input fixed: edge is delivered only to the first fixed step`. Assert both convenience queries and ordered phase records.
- [ ] **Step 2: Run** `ArcaneTests.exe "[input][fixed]"` and confirm the tests fail.
- [ ] **Step 3: Implement a bounded 256-transition queue** in the evaluator. When full, discard the oldest transition before appending a new one, retain the latest continuous value, and emit one diagnostic per overflow episode. Reset the queue on asset replacement.
- [ ] **Step 4: Re-run** `ArcaneTests.exe "[input][fixed]"` and confirm all three timing cases pass.
- [ ] **Step 5: Commit** as `fix(input): retain action transitions until fixed simulation consumes them`.

### Task 4: Add per-project binding profiles and interactive rebind state

**Files:**
- Create: `ArcaneClient/src/Arcane/Input/InputBindingProfile.hpp`
- Create: `ArcaneClient/src/Arcane/Input/InputBindingProfile.cpp`
- Create: `ArcaneClient/src/Arcane/Input/InputRebindOperation.hpp`
- Create: `ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp`
- Test: `ArcaneTests/src/InputBindingProfileTest.cpp`
- Test: `ArcaneTests/src/InputRebindOperationTest.cpp`

**Interfaces:**
- `InputBindingProfile` stores schema version 1 and `unordered_map<Guid, std::string>` replacement paths.
- `ProfileLoadResult` reports `Loaded`, `Missing`, or `Invalid`, plus diagnostics for ignored stale entries.
- `InputBindingProfile::Load(path, asset) -> ProfileLoadResult`, `Save(path) -> bool`, `Export(path) -> bool`, `Import(path, asset) -> ProfileLoadResult`, `SetOverride(bindingId, path, asset)`, `RemoveOverride(bindingId)`, `Reset()`, and `Dirty()` are the profile operations.
- `InputRebindOperation::Begin(bindingId, eligibleDevice, timeoutSeconds, currentSnapshot)`, `Observe(snapshot, dt)`, `Cancel()`, and `Result()` form a nonblocking capture operation with states Waiting, Completed, Canceled, TimedOut, and Invalid. `Result()` returns `{state, bindingId, replacementPath}`; `eligibleDevice` is an optional `InputDevice` filter. The current snapshot lets capture ignore controls already held when listening begins.

- [ ] **Step 1: Add failing tests** for valid save/reload without asset mutation, reset-one/reset-all, stale IDs, malformed JSON, atomic-write failure preserving the prior file, initiating-input suppression, cancel/timeout, and path validation against action/composite role.
- [ ] **Step 2: Run** `ArcaneTests.exe "[input][profile]"` from the executable directory and confirm the new cases fail.
- [ ] **Step 3: Implement profile serialization and atomic replacement** at a caller-supplied path. Ignore stale IDs individually with diagnostics. Implement capture from snapshots with explicit device filter and do not accept any control held in the current snapshot passed to `Begin`.
- [ ] **Step 4: Re-run** `ArcaneTests.exe "[input][profile]"`; assert a malformed profile leaves both the asset and last valid in-memory overrides intact.
- [ ] **Step 5: Commit** as `feat(input): add saved binding profiles and rebind capture`.

### Task 5: Expose `LocalInputUser` through `ClientRuntime`

**Files:**
- Create: `ArcaneClient/src/Arcane/Input/LocalInputUser.hpp`
- Create: `ArcaneClient/src/Arcane/Input/LocalInputUser.cpp`
- Modify: `ArcaneClient/src/Arcane/Client/RuntimePresentation.hpp`
- Modify: `ArcaneClient/src/Arcane/Client/ClientRuntime.hpp`
- Modify: `ArcaneClient/src/Arcane/Client/ClientRuntime.cpp`
- Modify: `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` only if the `EngineContext` layout or plugin entry points change.
- Test: `ArcaneTests/src/ClientRuntimeTest.cpp`

**Interfaces:**
- `LocalInputUser` owns one `InputActions`, one active profile, project identity, active maps/scheme, and rebind operation.
- `LocalInputUser` exposes `FindAction(map, action)`, `Value(Guid) -> InputActionValue`, typed `ButtonDown/ScalarValue/VectorValue/Phase(Guid)` queries, `Down/Pressed/Released/Started/Performed/Canceled(Guid)`, `PressedThisFixedStep(Guid)`, `ReleasedThisFixedStep(Guid)`, `TransitionsThisFixedStep()`, `SetBaseMap(Guid)`, `PushMap(Guid)`, `PopMap()`, `SetControlScheme(std::string_view)`, `Maps()`, `Actions(mapId)`, `Bindings(actionId)`, `BindingDisplayString(bindingId)`, `BeginRebind(Guid, std::optional<InputDevice>, float timeoutSeconds)`, `CancelRebind()`, `RebindResult()`, `LoadProfile(std::string_view)`, `SaveProfile()`, `ImportProfile(const std::filesystem::path&)`, `ExportProfile(const std::filesystem::path&)`, `SetOverride(Guid, std::string_view)`, `RemoveOverride(Guid)`, `ResetMapOverrides(Guid)`, `ResetOverrides()`, and `ProfileDirty()`. Typed getters return `std::nullopt` and publish a rate-limited diagnostic for an invalid ID or wrong action type.
- Add `ClientRuntime::GameInput() noexcept`, `ConfigureGameInput(const InputActionAsset&, const Guid& projectId)`, `UpdateGameInput(double dt, const InputSnapshot&)`, and `BeginGameInputFixedStep()`; keep `Input()` as the raw snapshot bridge.

- [ ] **Step 1: Add failing `ClientRuntimeTest` cases** for independent host/raw snapshot and gameplay evaluator state, profile isolation across project GUIDs, action queries through `GameInput()`, and fixed-step transition forwarding.
- [ ] **Step 2: Run** `ArcaneTests.exe "[client][input]"` and confirm the new integration cases fail.
- [ ] **Step 3: Implement the owned session** and a writable profile root based on `SDL_GetPrefPath("Arcane", "Arcane") / "InputProfiles" / <project-guid>`. Profile names are sanitized to a single filename component. Load the `Default` profile after a valid asset when a saved profile exists; otherwise begin with asset defaults. Forward snapshot updates and fixed-step boundaries without merging host configuration.
- [ ] **Step 4: Bump `kGamePluginABIVersion` only if `EngineContext` changes**, then re-run the targeted test and build the public SDK headers against an existing module fixture.
- [ ] **Step 5: Commit** as `feat(client): expose per-instance LocalInputUser to game modules`.

### Task 6: Select and load the project gameplay asset

**Files:**
- Modify: `ArcaneCore/src/Arcane/Project/ProjectManifest.hpp`
- Modify: `ArcaneCore/src/Arcane/Project/ProjectManifest.cpp`
- Modify: `ArcaneCore/src/Arcane/Project/Project.hpp`
- Modify: `ArcaneCore/src/Arcane/Project/Project.cpp`
- Modify: `ArcaneClient/src/Arcane/Host/ProjectBoot.hpp`
- Modify: `ArcaneClient/src/Arcane/Host/ProjectBoot.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp`
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp`
- Test: `ArcaneTests/src/ProjectManifestTest.cpp`
- Test: `ArcaneTests/src/ClientRuntimeTest.cpp`

**Interfaces:**
- Add optional `ProjectManifest::inputActions` string containing a GUID; absent/empty means no gameplay asset.
- Add `Project::SetInputActionsAsset(const Guid&)`; nil clears the selection and edits the manifest through the existing rewrite path.
- Add `GameplayInputLoadResult { Status status; std::string diagnostic; }` with `Status { Unconfigured, Loaded, Invalid }` and `HostBoot::LoadGameplayInput(ClientRuntime&, const Project&) -> GameplayInputLoadResult`. It resolves the selected GUID through the project registry, parses the JSON as an `InputActionAsset`, and calls `ClientRuntime::ConfigureGameInput(asset, projectGuid)`.
- Keep `HostBoot::LoadInputConfig(InputActions&, Config&)` for host/editor actions and remove its use as the game-facing evaluator.

- [ ] **Step 1: Add failing manifest tests** for optional round-trip of `inputActions`, empty selection, and `SetInputActionsAsset` persistence across project reopen. Add boot tests for missing selection, missing selected GUID, valid asset, and invalid asset.
- [ ] **Step 2: Run** `ArcaneTests.exe "[project],[client][input]"` from the executable directory and confirm the new cases fail.
- [ ] **Step 3: Implement manifest read/write and project boot resolution.** An unconfigured project returns `Unconfigured` with an empty game input session; a configured-but-missing or invalid asset returns `Invalid` with a project-input diagnostic. Load the asset's `defaultMap` as the initial base map. Editor project open logs/displays `Invalid` and remains editable; standalone Runtime treats `Invalid` as boot failure. Keep engine config loading separate.
- [ ] **Step 4: Wire editor Play and standalone Runtime to configure the session before module update; update the gameplay snapshot once per rendered frame and call `BeginGameInputFixedStep()` before every fixed scheduler tick. In Editor Play, build a gameplay snapshot from viewport focus plus game-UI capture: suppress keyboard/mouse when an unrelated editor panel owns focus or the game UI captures that device, while leaving gamepad available; preserve the existing pointer ownership filtering. In standalone Runtime, use the sampled snapshot with runtime UI capture flags. Project switch tears down the prior session before loading the next project.
- [ ] **Step 5: Re-run targeted tests and commit** as `feat(project): load gameplay input from the selected project asset`.

### Task 7: Register `.arcinput` with project assets and creation flows

**Files:**
- Modify: `ArcaneCore/src/Arcane/Project/AssetRegistry.cpp`
- Modify: `ArcaneEditor/src/Panels/AssetPanelModel.hpp`
- Modify: `ArcaneEditor/src/Panels/CreateAssetDialog.hpp`
- Modify: `ArcaneEditor/src/Panels/CreateAssetDialog.cpp`
- Modify: `ArcaneEditor/src/Panels/AssetPanelCommon.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp`
- Modify: `ArcaneTests/src/AssetRegistryTest.cpp`
- Modify: `ArcaneTests/src/AssetPanelModelTest.cpp`
- Modify: `ArcaneTests/src/CreateAssetDialogTest.cpp`

**Interfaces:**
- Add `AssetKind::InputActions` and `CreateAssetKind::InputActions`; extend all kind counts and the sanctioned asset-kind/create-kind bridge.
- Classify `.arcinput` as a native JSON asset with embedded top-level `id`; browser create writes `InputActionAsset::CreateDefault().ToJson()` under `Content/input/` by default.
- Creation dispatch returns the new GUID and opens its editor document through the normal `DocumentHost` route.

- [ ] **Step 1: Add failing registry/classification/create tests** asserting that `.arcinput` receives its embedded GUID, preserves that GUID after rename/move, appears as Input Actions in the Asset Browser, gets a valid default document, and name collisions are rejected.
- [ ] **Step 2: Run** `ArcaneTests.exe "[project],[editor]"` and confirm the `.arcinput` cases fail.
- [ ] **Step 3: Implement native registry recognition, asset kind labels/icons, create-dialog vocabulary, default folder, extension, and empty-document mint through centralized create dispatch.** Return the new GUID; Task 8 adds document routing/open. Do not include engine `input.json` in the project registry.
- [ ] **Step 4: Re-run targeted tests** and confirm ordinary `.json` assets remain classified as Data.
- [ ] **Step 5: Commit** as `feat(editor): create and browse gameplay input assets`.

### Task 8: Add the input asset document model and save lifecycle

**Files:**
- Create: `ArcaneEditor/src/Documents/InputActionsDocument.hpp`
- Create: `ArcaneEditor/src/Documents/InputActionsDocument.cpp`
- Create: `ArcaneEditor/src/Documents/InputActionsEditorModel.hpp`
- Create: `ArcaneEditor/src/Documents/InputActionsEditorModel.cpp`
- Modify: `ArcaneEditor/src/App/EditorApp.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp`
- Modify: `ArcaneEditor/src/Documents/DocumentHost.cpp` only if an existing open route cannot handle focus-not-reopen behavior.
- Test: `ArcaneTests/src/InputActionsEditorModelTest.cpp`

**Interfaces:**
- `InputActionsEditorModel` owns editable draft JSON, the last valid `InputActionAsset` preview, selection `(mapId, actionId, bindingId, partId)`, validation diagnostics, and `Dirty()` state. Invalid in-progress drafts remain editable.
- `ApplyEdit(std::string label, nlohmann::json before, nlohmann::json after)`, `Undo()`, and `Redo()` integrate with the editor's shared `Arcane::CommandStack`; each UI gesture is one undo step.
- `InputActionsDocument` implements `EditorDocument`, loads/saves `.arcinput` atomically by GUID, and retains the last valid compiled preview when a draft is invalid.
- Register `.arcinput` in `DocumentHost` with a GUID peek so reopening focuses the existing document.

- [ ] **Step 1: Add failing model tests** for stable selection after rename/reorder, duplicate subtree fresh IDs, one-step gesture undo/redo, malformed schema opened as a repairable draft, invalid draft diagnostics without losing last valid preview, and atomic save/reload.
- [ ] **Step 2: Run** `ArcaneTests.exe "[editor][input]"` and confirm failures.
- [ ] **Step 3: Implement the pure edit/model layer and document lifecycle.** Keep draft JSON separate from the last compiled asset; validate before Save and refuse to publish invalid drafts. On valid save, write atomically, update the open preview at a frame boundary, and clear dirty state only after successful replacement.
- [ ] **Step 4: Re-run targeted tests** and verify document routing uses focus-not-reopen by asset GUID.
- [ ] **Step 5: Commit** as `feat(editor): add editable gameplay input action documents`.

### Task 9: Build the maps/actions hierarchy, inspector, picker, and preview

**Files:**
- Modify: `ArcaneEditor/src/Documents/InputActionsDocument.cpp`
- Create: `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp`
- Create: `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp`
- Modify: `premake5.lua` if the editor-only source glob needs an update.
- Modify: `ArcaneTests/src/InputActionsEditorModelTest.cpp` only for pure selection/edit logic; keep ImGui rendering out of the test executable.

**Interfaces:**
- The document renders Action Maps on the left, an expandable actions/bindings/composite-parts tree on the right, and a selected-row properties inspector beside or below that tree.
- Widget helpers receive `InputActionsEditorModel&`, an asset preview evaluator, and `InputSnapshot`; they do not own document state.
- Inspector fields cover action type, control path, composite and part, processors, interactions, scheme groups, and blocking behavior.

- [ ] **Step 1: Add pure model tests** for adding/removing/reordering maps, actions, bindings, and composite parts; binding/action type changes; scheme membership; conflicts among same-scheme bindings; and default-map selection after add/delete.
- [ ] **Step 2: Run** `ArcaneTests.exe "[editor][input]"` and confirm new model cases fail.
- [ ] **Step 3: Implement the reference hierarchy** with row-level add/remove/duplicate/rename/reorder; show device labels and human-readable paths. Implement inspector editing as one undoable gesture per user edit, with an asset-level selector for `defaultMap`. The first map becomes default automatically; deleting it selects the first remaining map, and deleting the last map clears the default.
- [ ] **Step 4: Implement keyboard/mouse/gamepad path picker and capture**, cancellation, timeout, device filtering, conflict and unreachable-binding warnings, control-scheme create/edit/remove, live value/phase/device/supplying-binding preview, and invalid-path diagnostics. Keep warning conflicts nonblocking; schema errors disable Save.
- [ ] **Step 5: Re-run pure model tests, build ArcaneEditor, and manually inspect** keyboard/mouse/gamepad authoring, composite nesting, selection, preview, conflicts, and save/reopen against `Player+Action+Map.webp`.
- [ ] **Step 6: Commit** as `feat(editor): author and preview action maps and bindings`.

### Task 10: Add Project Settings selection for the gameplay asset

**Files:**
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp`
- Modify: `ArcaneEditor/src/Panels/EditorPanels.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp`
- Test: `ArcaneTests/src/ProjectManifestTest.cpp`

**Interfaces:**
- Replace the inert Project Settings menu item with a settings view that lists registered `.arcinput` assets and the current selection.
- Selection calls `Project::SetInputActionsAsset(Guid)`; clearing calls the nil form. The view offers open asset and create asset actions through existing centralized flows.

- [ ] **Step 1: Add a test** that selecting and clearing the input asset changes the manifest and survives reopening the project.
- [ ] **Step 2: Run** `ArcaneTests.exe "[project]"` and confirm the new selection case fails.
- [ ] **Step 3: Implement the Project Settings view** with empty, selected, deleted/missing-GUID, and multiple-asset states. Show a clear missing-selection diagnostic and do not silently select another asset.
- [ ] **Step 4: Re-run the targeted test and build ArcaneEditor.** Verify Editor Preferences remains the existing placeholder and editor shortcut settings are not added here.
- [ ] **Step 5: Commit** as `feat(editor): select gameplay input asset in project settings`.

### Task 11: Migrate ReferenceProject platformer to `Move` and `Jump`

**Files:**
- Create: `ReferenceProject/Content/input/Player.arcinput`
- Modify: `ReferenceProject/ReferenceProject.arcproj`
- Modify: `ReferenceProject/Source/Game/ReferenceGame.cpp`
- Modify: `ReferenceProject/Source/Game/PlayerController2DSystem.hpp` only if the controller component needs explicit action-fed input fields.
- Modify: `ReferenceProject/Source/Game/PlayerController2D.hpp` only if removing the raw snapshot helper requires it.
- Do not add game-specific automated tests for this controller.

**Interfaces:**
- Project asset has a `Player` map with `Move` (Axis1D; A/D and gamepad left-stick horizontal axis) and `Jump` (Button; W, Space, and gamepad south button).
- In `ReferenceGame::Module::OnInit`, resolve and cache the `Player.Move` and `Player.Jump` GUIDs through `Client()->GameInput().FindAction(...)`, reporting a clear error if either is absent. In `OnFixedUpdate`, publish `GameInput().Value(moveId).scalar`, `GameInput().PressedThisFixedStep(jumpId)`, and `GameInput().Down(jumpId)` into each `PlayerController2D` component as the system currently expects.
- Remove hard-coded controller scancodes; keep physics tuning and movement behavior unchanged.

- [ ] **Step 1: Create the authored asset** with stable IDs and point `ReferenceProject.arcproj` to its GUID.
- [ ] **Step 2: Migrate `ReferenceGame.cpp` from `InputSnapshot` sampling** to the `LocalInputUser` `Move`/`Jump` API. Remove `PlatformerInputState` sampling and the redundant `pendingJumpSeconds` latch; the engine fixed-step transition queue preserves edges and the controller component already owns the jump buffer. Read `Down` for variable jump height.
- [ ] **Step 3: Build ReferenceProject through `arcbuild`** and launch ArcaneRuntime with the project. Manually verify A/D and horizontal gamepad movement, W/Space and gamepad jump, and that changing bindings in the asset changes controls without editing game code.
- [ ] **Step 4: Commit** as `feat(reference): drive platformer controls from gameplay actions`.

### Task 12: Run integration verification and finalize SDK wiring

**Files:**
- Modify any integration call sites found by the build; do not fold unrelated dirty-worktree changes into the feature.
- Update `docs/specs/2026-06-12-arcane-input-actions-design.md` with a short note that its rebinding/persistence deferrals are resolved by this design; do not rewrite the evaluator semantics section.

**Interfaces:**
- Editor Play and standalone Runtime use the same selected project asset and profile root behavior.
- Engine config input remains host/editor-only in `ProjectBoot::LoadInputConfig` and keeps its current `demo` context behavior.

- [ ] **Step 1: Regenerate Premake projects** with `GenerateProjects.bat` if source lists changed.
- [ ] **Step 2: Build Debug and Release** in dependency order: ArcaneCore, ArcaneClient, ArcaneEditor, ArcaneRuntime, then the SDK-built ReferenceProject.
- [ ] **Step 3: Run targeted CPU suites** from the ArcaneTests executable directory: `ArcaneTests.exe "[input]"`, `ArcaneTests.exe "[project]"`, and `ArcaneTests.exe "[editor]"`.
- [ ] **Step 4: Manually verify** new project with no configured input, valid selected asset, missing/invalid selected asset, user override save/reload, editor Play, standalone Runtime, project switching, editor camera WASD independence, and the ReferenceProject controller.
- [ ] **Step 5: Review the final diff** for ABI version changes, accidental engine-config asset registration, stale profile handling, and unrelated user changes. Commit any integration-only corrections as `fix(input): complete gameplay input integration`; make no empty commit when all task commits are already clean.

## Execution Notes

- Keep the working tree's existing unrelated edits intact; stage only files belonging to the task being committed.
- Run Catch2 from `bin\<configuration>\ArcaneTests` because test data and engine assets resolve relative to the executable directory.
- Do not claim the ReferenceProject controller has automated coverage; its acceptance is a manual runtime check as requested.
