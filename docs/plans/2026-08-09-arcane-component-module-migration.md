# Arcane ComponentModule Migration (Movement 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Vendor-sync Astra to `f8a75a9` (ComponentModule; `ReRegisterComponent` deleted) and migrate Arcane to the RAII module-ownership model — engine module handle, plugin handles, teardown-step-7 deletion, secondary coverage, ABI v10.

**Architecture:** The sync and the migration are one atomic change (Arcane cannot compile against the new headers until the migration lands). Engine roster ownership moves from anonymous registration to a `ComponentModule` held by `Runtime::Impl`; plugins register ONLY their own types via heap-held handles reset in Shutdown; `PluginHost` keeps the registry-reset-while-mapped step and the `UnregisterModuleRange` net (now applied to secondaries too) and DELETES the roster re-registration step. Authoritative spec: `D:\dev\starworks\Astra\docs\superpowers\specs\2026-08-09-astra-component-module-raii-design.md` §3.5/§4/§5 — every binding requirement is restated inline here; workers do not need the Astra repo.

**Tech Stack:** C++23 Arcane workspace (Gacha monorepo, "Aphelyon"), MSVC via `Arcane.slnx`, premake5, Catch2 tests, vendored header-only Astra.

## Global Constraints

- Repo: `D:\dev\starworks\Gacha`. Branch: `feature/component-module-migration` off `main` (current HEAD). Work in-place (no worktree — vcpkg + build caches are repo-local).
- **The working tree carries the user's unrelated uncommitted WIP in exactly these 6 files: `Arcane/ArcaneEditor/src/AssetBrowser.hpp`, `EditorAppFrame.cpp`, `EditorPanels.cpp`, `EditorPanels.hpp`, `InspectorView.cpp`, `Arcane/Tests/src/AssetBrowserTest.cpp`. NEVER stage, modify, revert, or commit them. Every commit uses explicit `git add <path> <path> ...` — never `git add -A`, `-u`, or `.`.**
- Build (from `D:\dev\starworks\Gacha\Arcane`): `GenerateProjects.bat` after the vendor sync (header set changed) and after any premake/file-list change, then `msbuild Arcane.slnx /p:Configuration=Debug /m` (also Release/Dist for the final gate). Output: `bin\<Config>-windows-x86_64-md\`.
- Tests: `bin\<Config>-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "~[gpu]"` is the standard gate; this machine HAS a GPU, so the final task also runs the full suite (including `[gpu]`) plus the scripted verify `bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --backend vulkan --frames 180` (exit 0 = pass; asserts `RenderErrorCount()==0`).
- Baseline-first: Task 1 records the pre-change test count on the dirty-but-user-owned tree. A red baseline = BLOCKED; report, do not proceed.
- House rules (repo CLAUDE.md): UTF-8 without BOM, ASCII comments; /MD everywhere in the Arcane workspace; no `/fp:fast`; three configs Debug/Release/Dist.
- ComponentModule contract (from the Astra spec — restate in comments where you touch plugin code): handles are HEAP-HELD in plugins (`std::optional<Astra::ComponentModule>` at file scope), reset explicitly in `GamePlugin_Shutdown`, NEVER a DLL static object (its destructor would run under the loader lock during FreeLibrary); `Open` requires `Astra::SetTypeContext` to have run in that module; `Register<T>` must be instantiated in the module whose code the descriptors should point into; register ONLY types the module owns.
- Commit style: match repo history (`feat(...)`, `chore(vendor): ...`, `test(...)`). The sync+code migration is ONE commit (a sync-only commit cannot build).

---

### Task 1: Branch, baseline, vendor sync

**Files:**
- Modify: `ThirdParty/Astra/**` (via `scripts/sync-astra.ps1` — never hand-edit)
- No other files.

**Interfaces:**
- Produces: vendored Astra @ `f8a75a9` with `include/Astra/Component/ComponentModule.hpp` present and `ReRegisterComponent` absent; a recorded Debug baseline test count; a recorded list of expected compile breaks.

- [ ] **Step 1: Branch and baseline**

```bat
cd D:\dev\starworks\Gacha
git checkout -b feature/component-module-migration
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "~[gpu]"
```

Record the exact pass count. If ANY test fails: status BLOCKED, report the failures (the tree carries user WIP; a red baseline is not ours to fix).

- [ ] **Step 2: Vendor sync**

```bat
cd D:\dev\starworks\Gacha
powershell -ExecutionPolicy Bypass -File scripts\sync-astra.ps1
```

Verify: `ThirdParty/Astra/VENDORED.txt` stamps commit `f8a75a920ad07a7e06af2936948f1ffa19463804` (dev, "fix(component): hoist alignment refusal above the meta phase..."); `ThirdParty/Astra/include/Astra/Component/ComponentModule.hpp` exists; `grep -rn ReRegisterComponent ThirdParty/Astra/include/` returns zero hits.

- [ ] **Step 3: Prove the breakage surface is exactly the migration surface**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
```

Expected: compile FAILURES only in files that call `ReRegisterComponent` — `Sandbox/src/Sandbox.cpp`, `PlaygroundGame/src/PlaygroundGame.cpp`, `Tests/plugins/HotReloadPlugin.cpp`. (`Runtime.cpp`/`PluginHost.cpp`/tests reference it only in comments.) Any OTHER failure is an unplanned API break — record it and report BLOCKED rather than improvising. Do NOT commit — Task 2 completes the atomic change.

---

### Task 2: Code migration (engine + host + plugins)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/SceneModule.hpp` (:19-39 region)
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsComponents.hpp` (:243-257 region)
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.cpp` (Impl members ~:74-112; ctor ~:114-188; comment block :124-162)
- Modify: `Arcane/Arcane/src/Arcane/Plugin/PluginHost.cpp` (TeardownImage :227-285; LoadInitPlugins :298-323; Unload :550-576)
- Modify: `Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp` (:69-84)
- Modify: `Arcane/Sandbox/src/Sandbox.cpp` (:109-122)
- Modify: `Arcane/PlaygroundGame/src/PlaygroundGame.cpp` (:117-123)
- Modify: `Arcane/Tests/plugins/HotReloadPlugin.cpp` (:32-66)
- Modify: `Arcane/Tests/src/PluginHostTest.cpp` (:181-230 — the disown test's semantics flip, so THIS task's commit is green; Task 3 adds the new coverage)

**Interfaces:**
- Consumes: `Astra::ComponentModule::Open(std::shared_ptr<Astra::ComponentRegistry>, std::string_view) -> ComponentModule` (empty/falsy on refusal), `template<Component...> void Register()`, `void Reset()`, `explicit operator bool()`. `ComponentRegistry::UnregisterModuleRange(const void*, size_t)` unchanged signature.
- Produces: `RegisterSceneComponents(Astra::ComponentModule&)` and `RegisterPhysicsComponents(Astra::ComponentModule&)` overloads (Task 3's tests may use them).

- [ ] **Step 1: Roster overloads**

`SceneModule.hpp` — add `#include <Astra/Component/ComponentModule.hpp>` and, below the existing two overloads (keep both — tests use them):

```cpp
    // Module-owned form: the engine Runtime registers its roster through its
    // ComponentModule handle so plugin overrides restore to THESE descriptors
    // on unload. Same append-only ordering rule as above.
    inline void RegisterSceneComponents(Astra::ComponentModule& mod)
    {
        mod.Register<Transform, WorldTransform, PreviousTransform, SpriteRenderer,
                     PostProcess, Identity, Hidden, Camera>();
    }
```

`PhysicsComponents.hpp` — same pattern:

```cpp
    inline void RegisterPhysicsComponents(Astra::ComponentModule& mod)
    {
        mod.Register<RigidBody2D, Collider2D, PhysicsBodyRef>();
    }
```

- [ ] **Step 2: Runtime engine module**

`Runtime.cpp`: add `#include <Astra/Component/ComponentModule.hpp>`. In `Runtime::Impl`, DIRECTLY BELOW the `components` member (:80), add:

```cpp
        // The engine's ownership handle over its component roster. Declared
        // AFTER `components` (destroyed first, and it holds the shared_ptr
        // anyway). Plugins override these descriptors via their own handles;
        // their destruction restores to the entries registered here -- the
        // old "re-register the roster after unload" host ritual is gone.
        std::optional<Astra::ComponentModule>       engineModule;
```

In the ctor, REPLACE the two lines `RegisterSceneComponents(*components); RegisterPhysicsComponents(*components);` (:163-164) with:

```cpp
            engineModule = Astra::ComponentModule::Open(components, "Arcane.Engine");
            ARC_ASSERT(static_cast<bool>(*engineModule),
                       "engine ComponentModule refused -- SetTypeContext must precede Runtime construction");
            RegisterSceneComponents(*engineModule);
            RegisterPhysicsComponents(*engineModule);
```

(`Astra::SetTypeContext(context)` at :120 already precedes this — `Open`'s tripwire is satisfied. If `ARC_ASSERT` is not the local assert macro name, use the one `Runtime.cpp` already uses.)

REWRITE the comment block at :124-162: keep the ComponentID-numbering paragraph (ids are still first-touch registration order; the append-only rule stands) but (a) replace the "plugin happened to ReRegisterComponent" history with one sentence — the roster is now module-OWNED, plugins register only their own types, and unload restores engine descriptors automatically; (b) DELETE the stale CR-4 paragraph (:152-162) — the hash-resolved archetype rebuild landed in the vendored Astra, so cross-process Save/Load is no longer categorically unsupported; do not claim support either, just remove the stale caveat.

- [ ] **Step 3: PluginHost — delete step 7, cover secondaries**

`TeardownImage` (:227-285): DELETE the trailing engine-roster re-registration block (:278-284, "Rebind the engine's own types to THIS module...") and the now-unneeded includes if unused (`Scene/PhysicsComponents.hpp`, `Scene/SceneModule.hpp` at :8-9 — check for other uses first). Rewrite the big comment (:246-265): the hot-reload contract is now handle-based — a plugin's `ComponentModule` cleans up in its own `Shutdown`; the range purge below stays as the fallback net for plugins that forget, and the engine roster survives because it was never displaced (its module never dies).

Add a private helper on `Impl` and call it at BOTH secondary-unmap sites, while the modules are still mapped:

```cpp
        // Fallback net for secondaries, mirroring TeardownImage's purge for the
        // primary: a well-behaved plugin's ComponentModule already cleaned up in
        // its Shutdown; this catches one that forgot, BEFORE FreeLibrary.
        void DisownPluginImages()
        {
            const auto& creg = runtime.Components();
            if (!creg)
                return;
            for (auto& img : plugins)
            {
                if (!img.plugin)
                    continue;
                if (const Module::ImageSpan image = img.plugin->LoadedModule().Image(); image.size != 0)
                {
                    const std::size_t dropped = creg->UnregisterModuleRange(image.base, image.size);
                    if (dropped != 0)
                        ARC_TRACE("PluginHost: disowned {} descriptor(s) from a secondary that skipped handle cleanup", dropped);
                }
            }
        }
```

Call `DisownPluginImages();` immediately before `plugins.clear();` in `LoadInitPlugins`'s failure unwind (:312) and in `Unload` (:575).

- [ ] **Step 4: ABI v10**

`PluginABI.hpp`: below the v9 entry (:69-83), add and bump:

```cpp
    // v10 (2026-08-09): Astra re-vendored to dev f8a75a9 -- the ComponentModule
    //     program. ReRegisterComponent NO LONGER EXISTS (plugins own their types
    //     via RAII ComponentModule handles, heap-held + reset in Shutdown), and
    //     ComponentRegistry grew owner/shadow/meta-thunk state, so a v9 plugin's
    //     inlined template code manipulates a registry whose layout changed.
    //     Layout mismatch plus a removed API: reject the pairing.
    inline constexpr uint32_t kGamePluginABIVersion = 10;
```

(Replace the existing `= 9` line; keep the whole history comment chain above it.)

- [ ] **Step 5: Plugins register only what they own**

`PlaygroundGame.cpp` (:117-123): DELETE the `auto creg = ...` line and all six `ReRegisterComponent` lines. Replace with a two-line comment: engine components are engine-owned now (Runtime's ComponentModule); this plugin owns no component types and registers nothing.

`Sandbox.cpp` (:109-122): DELETE the comment block ("2. Re-register every component descriptor...") plus the `auto creg = ...` line and all ten `ReRegisterComponent` lines; renumber/reword the step comments (the systems block becomes step 2, scene build step 3). Same replacement comment as PlaygroundGame.

`HotReloadPlugin.cpp`: add `#include <Astra/Component/ComponentModule.hpp>` and `#include <optional>`. In the anonymous namespace (:32-42), add:

```cpp
    // Heap-held ownership handle for this module's component types. Reset in
    // Shutdown (which the host calls BEFORE unmapping) -- never a DLL static:
    // a static's destructor would run under the loader lock during FreeLibrary.
    std::optional<Astra::ComponentModule> g_module;
```

In `GamePlugin_Init` (:55), replace `ctx->engine->Components()->ReRegisterComponent<Pulse>();` with:

```cpp
        g_module = Astra::ComponentModule::Open(ctx->engine->Components(), "HotReloadPlugin");
        if (!*g_module)
            return false;                                  // SetTypeContext above makes this unreachable; fail loudly if not
        g_module->Register<Pulse>();                       // descriptors + meta -> THIS image
```

In `GamePlugin_Shutdown` (:66): `GAME_API void GamePlugin_Shutdown() { g_module.reset(); g_pulse = {}; }`

- [ ] **Step 6: Rewrite the flipped regression test (restore-not-hole semantics)**

The owner stack changes one existing test's correct expectation: the test binary's anonymous `RegisterComponent<Pulse>` (PluginHostTest.cpp:195) is now the shadowed BASE, and plugin unload RESTORES it instead of leaving a purged hole — so `== nullptr` at :212 is no longer the correct assertion. Replace the whole test at :181-230:

```cpp
TEST_CASE("Unloading a plugin restores the descriptors it overrode", "[hotreload]")
{
    // REGRESSION lineage: diagnosed 2026-07-31 as a permanent dangling-descriptor
    // AV; first fixed by UnregisterModuleRange (purge to a hole); now the
    // ComponentModule owner stack RESTORES the previous owner instead. This
    // test binary registers Pulse anonymously (below), the plugin's handle
    // overrides it, and unload must pop back to the test binary's entry --
    // non-null AND callable (a dangling restore faults right here).
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    const Astra::ComponentID pulseId = Astra::TypeID<Pulse>::Value();
    const Astra::ComponentDescriptor* base = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(base != nullptr);

    {
        Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
        REQUIRE(host.Load());
        REQUIRE(rt.Components()->GetComponentDescriptor(pulseId) != nullptr);
        host.Unload();
    }

    // Restored to the test binary's own registration: same slot address
    // (m_components[id] is pointer-stable), callable code in THIS image.
    const Astra::ComponentDescriptor* restored = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(restored == base);
    REQUIRE(restored->defaultConstruct != nullptr);
    alignas(Pulse) std::byte pbuf[sizeof(Pulse)];
    restored->DefaultConstruct(pbuf);
    restored->Destruct(pbuf);

    // The engine roster half is unchanged in spirit: Transform survives, callable.
    const Astra::ComponentID transformId = Astra::TypeID<Arcane::Transform>::Value();
    const Astra::ComponentDescriptor* tf = rt.Components()->GetComponentDescriptor(transformId);
    REQUIRE(tf != nullptr);
    alignas(Arcane::Transform) std::byte buf[sizeof(Arcane::Transform)];
    tf->DefaultConstruct(buf);
    tf->Destruct(buf);

    CHECK(Arcane::RenderErrorCount() == 0);
}
```

- [ ] **Step 7: Build + full non-GPU suite**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "~[gpu]"
```

Expected: build green, ALL tests green (count = Task 1 baseline). Any failure is a regression to fix before committing.

- [ ] **Step 8: Commit (sync + migration atomically, green)**

```bat
cd D:\dev\starworks\Gacha
git add ThirdParty/Astra Arcane/Arcane/src/Arcane/Scene/SceneModule.hpp Arcane/Arcane/src/Arcane/Scene/PhysicsComponents.hpp Arcane/Arcane/src/Arcane/Base/Runtime.cpp Arcane/Arcane/src/Arcane/Plugin/PluginHost.cpp Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp Arcane/Sandbox/src/Sandbox.cpp Arcane/PlaygroundGame/src/PlaygroundGame.cpp Arcane/Tests/plugins/HotReloadPlugin.cpp Arcane/Tests/src/PluginHostTest.cpp
git commit -m "feat(plugin)!: Astra f8a75a9 + ComponentModule migration -- module-owned rosters, ABI v10"
```

---

### Task 3: Test migration + new acceptance coverage

**Files:**
- Modify: `Arcane/Tests/src/PluginHostTest.cpp` (:181-230 rewrite; new test after :230; comment at :185)
- Modify: `Arcane/Tests/src/PlaygroundGamePluginTest.cpp` (:55 ABI assert)
- Modify: `Arcane/Tests/src/EditorComponentCatalogTest.cpp` (:297 comment only)

**Interfaces:**
- Consumes: the migrated HotReloadPlugin fixtures (V1/V2 both register Pulse via handles) and the Task 2 engine module.

- [ ] **Step 1: (moved to Task 2 Step 6 — the flipped test is rewritten there so Task 2's commit is green; this task ADDS coverage only)**

- [ ] **Step 2: New secondary-coverage regression test** (the 2026-08-09 audit's live bug — secondaries were never disowned; now their handles clean up, and the new host net catches a forgetter). Add after the rewritten test:

```cpp
TEST_CASE("Unloading secondaries leaves no descriptor aimed at their images", "[hotreload]")
{
    // The audited live bug: plugins.clear() used to FreeLibrary secondaries with
    // no purge, leaving Pulse's descriptor aimed at the unmapped V2 image
    // permanently. Now V2's own ComponentModule resets in its Shutdown (and the
    // host's DisownPluginImages nets a forgetter). After Unload, the descriptor
    // must be the test binary's restored base -- calling through it faults if
    // any dangling entry survived.
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();
    const Astra::ComponentID pulseId = Astra::TypeID<Pulse>::Value();
    const Astra::ComponentDescriptor* base = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(base != nullptr);

    {
        Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
        host.AddPlugin(std::filesystem::path("HotReloadPluginV2.dll"));   // secondary
        REQUIRE(host.Load());
        StepAllK(rt, host, 1);
        // Primary AND secondary both pushed handles over the base: depth-2 stack.
        host.Unload();
    }

    const Astra::ComponentDescriptor* restored = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(restored == base);
    alignas(Pulse) std::byte buf[sizeof(Pulse)];
    restored->DefaultConstruct(buf);      // faults if a dead image's entry was restored
    restored->Destruct(buf);
    CHECK(Arcane::RenderErrorCount() == 0);
}
```

(Reuse the file's existing `StepAllK` helper; match its ReadPulse/fixture-copy conventions. If `StepAllK` requires the vtable differently, mirror the secondary test at :116-149.)

- [ ] **Step 3: Meta-transience acceptance assertion** (spec §4: no consumer retains a `TypeMeta*` across erasure). Extend the test Task 2 Step 6 rewrote (`"Unloading a plugin restores the descriptors it overrode"`) — after its restore assertions, add:

```cpp
    // Meta acceptance (spec 2026-08-09 §4): Pulse is REFLECTED, and its meta was
    // rebound to the plugin image while loaded. After unload+restore, consumers
    // that look meta up fresh must get a live entry -- and the restored
    // descriptor's cached meta pointer must AGREE with the registry (the Astra
    // side rebinds in place at a stable address on pop).
    const Astra::TypeMeta* liveMeta = Astra::MetaRegistry::Instance().Get(Astra::TypeID<Pulse>::Hash());
    REQUIRE(liveMeta != nullptr);
    CHECK(restored->meta == liveMeta);
```

(Add `#include <Astra/Reflection/MetaRegistry.hpp>` if the file lacks it.)

- [ ] **Step 4: ABI + comment touch-ups**

`PlaygroundGamePluginTest.cpp:55`: update the assert to `CHECK(Arcane::kGamePluginABIVersion == 10u);` and its comment ("v10: Astra re-vendored to f8a75a9; ComponentModule replaces ReRegisterComponent; registry layout changed").
`PluginHostTest.cpp:185` region and `EditorComponentCatalogTest.cpp:297`: update the two comments that describe the old ReRegisterComponent contract to name the handle model instead (comment-only; grep the two files for `ReRegisterComponent` to catch stragglers).

- [ ] **Step 5: Build, run, commit**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "~[gpu]"
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[hotreload]"
```

Expected: all green (the Task-2 flip is resolved; count = baseline + 1 new test). Commit:

```bat
cd D:\dev\starworks\Gacha
git add Arcane/Tests/src/PluginHostTest.cpp Arcane/Tests/src/PlaygroundGamePluginTest.cpp Arcane/Tests/src/EditorComponentCatalogTest.cpp
git commit -m "test(plugin): restore-not-hole semantics, secondary-coverage regression, meta transience, ABI v10 assert"
```

---

### Task 4: 3-config gate, GPU verify, zero-legacy sweep

**Files:** none modified (verification + possible comment stragglers only).

- [ ] **Step 1: 3-config gate**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m   && bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "~[gpu]"
msbuild Arcane.slnx /p:Configuration=Release /m && bin\Release-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "~[gpu]"
msbuild Arcane.slnx /p:Configuration=Dist /m    && bin\Dist-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "~[gpu]"
```

All green with the intended new tests (record all three counts).

- [ ] **Step 2: GPU + scripted verify (this box has the GPU)**

```bat
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[gpu]"
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --backend vulkan --frames 180
```

Both must exit 0. The runtime run exercises the REAL hot-reload host (Sandbox.dll) on the migrated registration path.

- [ ] **Step 3: Zero-legacy sweep (exit gate)**

```bash
cd /d/dev/starworks && grep -riwl ReRegisterComponent --exclude-dir=.git --exclude-dir=bin --exclude-dir=bin-int --exclude-dir=site --exclude-dir=graphify-out .
```

Gate: ZERO hits under `Gacha/Arcane` source and `Gacha/ThirdParty/Astra`. Permitted, documented remainders: `Astra/docs/**` + `Astra/README-history` mentions (spec/plan/review records), `Gacha/docs/**` review records, and `Aphelyon/Source/Aphelyon.cpp` (**Movement 3 — expected; do not touch it in this plan**). Any hit outside those: fix (comments) or report.

- [ ] **Step 4: Commit stragglers (if any) and report**

If Step 3 fixed comment stragglers, commit them (`docs: retire ReRegisterComponent mentions`). Final report: baseline vs final counts ×3 configs, GPU verify result, sweep result, the flipped-test explanation, and the note that Aphelyon (Movement 3) is now the only remaining consumer — it correctly FAILS the v10 ABI pairing until migrated.

---

## After the plan

Whole-branch review, then the finishing flow (the Gacha repo's convention is push-branch → CI green → merge to main, but local-only is the user's call at finish time). Movement 3 (Aphelyon: delete `Source/Aphelyon.cpp:81-83`, re-stamp manifest `engineAbi 10`, rebuild against the v10 SDK) follows as its own small task.
