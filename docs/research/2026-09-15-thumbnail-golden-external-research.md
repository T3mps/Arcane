# Thumbnail/preview golden-testing — external prior art

**Date:** 2026-09-15
**Context:** proposal for an in-process `[gpu][golden]` test driving `MaterialPreviewHarvester`
(`ArcaneEditor/src/Project/MaterialPreviewHarvester.hpp`/`.cpp`) against a reference project,
comparing each harvested PNG to a committed reference image — alongside the existing whole-frame
golden gate (`ArcaneTests/src/GoldenImageTest.cpp`, `scripts/golden-gate.ps1`,
`scripts/automation-baselines.json`). The renderer will change substantially over coming arcs
(Deadlock/Source-2-class look — see `docs/research/2026-09-14-engine-ceiling-deadlock-and-box3d.md`),
so every reference image gets re-blessed repeatedly; the question is what shape makes that cheap
and safe.

**Companion, do not duplicate:** `docs/research/2026-08-26-unreal-screenshot-comparison-research.md`
already covers Unreal's whole-frame screenshot-comparison system in depth (tolerance knob
structure, reference-variant axes, approval workflow, nondeterminism policy) from public
documentation only. This note is scoped to the part that companion note explicitly did not
cover — **per-asset thumbnail/preview rendering**, as distinct from whole-frame screenshots — and
draws on the local Unreal Engine source dump at `.example/UnrealEngine-release/` (present under
`D:\dev\starworks\Arcane\.example\`), plus public research on Godot and Unity. Per the sourcing
boundary that governs this repo, UE claims below are cited to specific file paths in that local
dump (source, license-bound, read but not reproduced beyond short identifiers); Godot claims are
cited to public GitHub URLs (MIT-licensed, freely quotable); Unity claims are cited to public docs
and GitHub mirrors of its packages.

---

## Per-engine: what is pinned, where, how keyed, how approved, tolerance model

### Unreal Engine — two *separate* systems, and thumbnails are in neither's golden path

**System A: package-embedded object thumbnails (`FObjectThumbnail`).** This is UE's actual
thumbnail mechanism — Content Browser icons, asset picker previews. It is **not covered by any
image-comparison/golden-test machinery at all.**

- What's pinned: a `TArray<uint8>` of BGRA8 pixel data (PNG-compressed for storage), embedded
  **inside the `.uasset` package file itself** in a `TMap<FName, FObjectThumbnail>` thumbnail map
  on the `UPackage` (`FObjectThumbnail::AccessCompressedImageData`,
  `Engine/Source/Runtime/Core/Public/Misc/ObjectThumbnail.h:60-330`; cache/store call site
  `ObjectTools::CacheThumbnail`, `Engine/Source/Editor/UnrealEd/Private/ObjectTools.cpp:5623-5653`).
  There is no `Saved/Thumbnails/<guid>.png`-style loose-file cache — the thumbnail travels with
  the asset file, checked in with it.
- Keying: by object full name (`ObjectFullName`) within one package's thumbnail map — not
  content-hashed, not GUID-keyed as a separate artifact.
- Invalidation: a per-render-info policy enum, not a hash —
  `EThumbnailRenderFrequency { Realtime, OnPropertyChange, OnAssetSave, Once }`
  (`Engine/Source/Editor/UnrealEd/Classes/ThumbnailRendering/ThumbnailRenderer.h:14-24`).
  Materials/particle systems use `Realtime` (re-rendered live, every time the thumbnail is drawn,
  because they can be cheaply re-rendered and users expect them animated); most static assets use
  `OnAssetSave` — `ObjectTools::GenerateThumbnailForObjectToSaveToDisk`
  (`ObjectTools.cpp:5561-5612`) is the entry point called at save time, and it explicitly blocks on
  outstanding texture streaming and shader compilation first so the saved thumbnail doesn't capture
  a mid-load/mid-compile frame. There is also a dirty flag (`FObjectThumbnail::MarkAsDirty`/
  `IsDirty`, `ObjectThumbnail.h:122-132`) but it gates *when to regenerate*, not a content hash of
  *what changed*.
- **No golden-image test targets this system.** A repo-wide search of UE's own test suites
  (`Engine/Source/Editor/UnrealEd/Private/Tests/`, `Engine/Source/Runtime/Engine/Private/Tests/`)
  turned up zero files matching `Thumbnail`, and zero `IMPLEMENT_*_AUTOMATION_TEST` macros in any
  file whose path contains `Thumbnail`. Thumbnail correctness is exercised only by hand (the
  Content Browser reflects live renders) or transitively by whatever tests exercise the
  `UThumbnailRenderer` subclasses' non-visual logic.

**System B: `AScreenshotFunctionalTest` + `ImageComparer` — the whole-frame golden system**
(already the subject of the companion research note; summarized here only for the axes this note
needs). This is a *scene/level* screenshot mechanism, driven from a functional test actor in a
level, not an asset-preview mechanism:

- What's pinned: a full render-target capture of the game viewport (or, via
  `AutomationCommon::SaveWindowAsScreenshot`, an arbitrary **Slate widget** — see "widget capture"
  below) — never a 64px per-asset icon.
- Reference path keying, confirmed directly from source
  (`Engine/Source/Runtime/Engine/Private/Tests/AutomationCommon.cpp:457-468`):
  `GetScreenshotPath()` builds `<TestName>/<IniPlatformName>/<RenderDetailsString>/<uuid>.png`,
  where `RenderDetailsString` is RHI name + shader-feature-level string
  (`AutomationCommon.cpp:395-455`, `GetRenderDetailsString`). So the on-disk key is literally
  `Platform / RHI_FeatureLevel / <run-uuid>.png` for the *incoming* capture; the companion note
  already established (from public docs, since ground-truth folder internals aren't in the local
  dump under the same file) that *approved* ground truths live under
  `<Project>/Saved/Automation/Comparisons` bucketed the same way, chosen by closest-match with a
  general fallback — not a hard requirement of an exact key.
- Tolerance model, confirmed directly from source
  (`Engine/Source/Developer/FunctionalTesting/Public/AutomationScreenshotOptions.h:67-238` and
  `Engine/Source/Developer/ScreenShotComparisonTools/Public/ImageComparer.h:27-97`):
  - `FComparisonToleranceAmount { Red, Green, Blue, Alpha, MinBrightness, MaxBrightness }` (all
    `uint8`, i.e. per-channel dead zones) with **four canonical presets** baked into
    `SetToleranceAmounts()`:
    - `Zero`: `(0,0,0,0, minBright=0, maxBright=255)` — exact match required
    - `Low` (the *default* preset — `Tolerance(EComparisonTolerance::Zero)` is the struct default
      but `Low` is what the docs call out as the practical default because of TAA noise):
      `(16,16,16,16, 16,240)`
    - `Medium`: `(24,24,24,24, 24,220)`
    - `High`: `(32,32,32,32, 64,96)`
  - `MaximumLocalError` (default **0.10**) and `MaximumGlobalError` (default **0.02**) — a
    region-budget/whole-image-budget pair (already analyzed in depth by the companion note's Q1).
  - `bIgnoreAntiAliasing` (default **true**) and `bIgnoreColors` (default **false**).
  - `Delay` (default 0.2s) + `FrameDelay` (default 5 frames) — **both** must elapse before capture
    (`AutomationScreenshotOptions.h:73-90`), and `bDisableNoisyRenderingFeatures`/
    `bDisableTonemapping` (both default **true**) strip AA/motion-blur/SSR/eye-adaptation/contact
    shadows before capturing specifically because "those features contribute a lot to the noise."
  - `FImageComparisonResult::AreSimilar()` (`ImageComparer.h:417-435`) is the actual gate: fails if
    `MaxLocalDifference > Tolerance.MaximumLocalError` **or**
    `GlobalDifference > Tolerance.MaximumGlobalError`.
- Approval workflow: already fully documented by the companion note (Q3) — Add / Replace / Add As
  Alternative, one-click bless, first-run is a warning not an error, diff stored as a reviewable
  artifact. Not re-derived here.

**Widget/panel capture — the alternative to "scroll the browser into frame."** UE's Slate
automation layer can screenshot an arbitrary widget subtree, not just a top-level window:
`FSlateApplication::TakeScreenshot(const TSharedRef<SWidget>& Widget, TArray<FColor>&, FIntVector&)`
and an overload taking an `InnerWidgetArea` sub-rect
(`Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h:1030-1062`).
`AutomationCommon::SaveWindowAsScreenshot` (`AutomationCommon.cpp:689-703`) is the call site used by
`FTakeActiveEditorScreenshotCommand`/`FTakeEditorScreenshotCommand` — it takes any `SWidget`
(a window is one), so an editor-automation test *can* target one panel instead of the whole shell.
This is architecturally the same escape hatch a per-asset-thumbnail golden test needs — capture the
small surface directly, don't derive it by cropping a whole-window screenshot.

### Godot — path-hash-keyed PNG cache, mtime-first + content-hash-tiebreak invalidation, no golden tests, no rendering-regression CI

Source: `editor/inspector/editor_resource_preview.cpp`/`.h` and
`editor/inspector/editor_preview_plugins.cpp` (godotengine/godot, MIT, commit `8898c2b3d`),
`.github/workflows/*.yml`.

- **Queue/generation**: `EditorResourcePreview` is a singleton `Node` holding a mutex-guarded
  `List<QueueItem> queue`. If the active renderer supports async resource creation, a dedicated
  background `Thread` blocks on a semaphore and drains one queue item per wake
  (`_thread_func`→`_thread`→`_iterate`, one preview per wake, not one per engine tick). If the
  driver can't create resources off the main thread (e.g. the GL compatibility renderer), there's
  no thread at all — an idle callback registered on `SceneTree` instead drains the queue every
  frame under a **100ms/frame time budget**.
- **On-disk cache and its key**: PNGs live under `EditorPaths::get_cache_dir()` (Godot's
  per-project cache directory), named `resthumb-<md5>.png` / `_small.png`, with a `<md5>` sidecar
  `.txt`. **The key is `md5(globalized_absolute_path)` — a path hash, not a content hash and not a
  GUID.**
- **Invalidation is a two-stage check, not a single hash comparison**: the sidecar stores
  `modified_time` and a content MD5 (`FileAccess::get_md5`) plus a `CURRENT_METADATA_VERSION`
  escape hatch that invalidates everything at once (engine-version bump). On load, Godot first
  compares the file's current mtime (also considering a sibling `.import` file's mtime) against the
  stored one; **only if the mtime differs** does it re-hash the content and invalidate — a bare
  mtime bump with unchanged bytes rewrites the sidecar but keeps the cached PNG. A separate
  cheap path, `check_for_invalidation(path)`, does an mtime-only check against the *in-memory*
  cache (used by the filesystem dock on file-change notifications) and emits a
  `preview_invalidated` signal.
- **No golden-image test of previews anywhere in the repo.** `tests/` contains only doctest-style
  unit tests over `core/`, `scene/`, `servers/`; no PNG-comparison harness exists.
- **No rendering-regression CI at all, honestly.** `.github/workflows/linux_builds.yml` and the
  `.github/actions/godot-project-test` composite action run the editor headless
  (`--headless --test --force-colors`) as a crash/smoke test over the demo project — no screenshot
  capture, no pixel diff, anywhere in the pipeline.
- **Framing convention is fixed-camera-and-light, scale-the-object** — the inverse of UE/Arcane's
  move-the-camera approach. `EditorMeshPreviewPlugin::generate()`
  (`editor_preview_plugins.cpp`, ~line 737) creates one camera at a **constant**
  `Transform3D(Basis(), Vector3(0,0,3))` with an **orthogonal** projection
  (`camera_set_orthogonal(1.0, 0.01, 1000.0)`) that never moves. Per mesh: take the AABB, center
  it, apply a **fixed** tilt (~22.5° yaw then ~22.5° pitch), compute
  `m = 1 / (max(rotated_aabb.size.x, rotated_aabb.size.y) * 0.5) * 0.5` and **scale the mesh** by
  `m` so its largest rotated extent fills the fixed ortho frame, then push it back along Z to clear
  the near plane. Two static `DirectionalLight3D`s (fixed directions/colors, set up once in the
  constructor) light every preview identically. `EditorMaterialPreviewPlugin` similarly uses a
  fixed perspective camera against a constant-radius sphere/torus — because the preview geometry
  itself never varies, no bounds-fitting is needed at all.

### Unity — size-bounded cache with no documented invalidation contract; a graphics test framework built for whole-frame captures, not thumbnails

- **`AssetPreview.GetAssetPreview`**: renders on demand, returns `null` until ready (poll
  `IsLoadingAssetPreview()`), and caches into a texture cache whose *capacity* is the only
  documented knob (`AssetPreview.SetPreviewTextureCacheSize`) — [docs.unity3d.com/ScriptReference/AssetPreview.html](https://docs.unity3d.com/ScriptReference/AssetPreview.html).
  **No documented invalidation rule** — nothing states "the preview updates on reimport"; it's an
  LRU-shaped cache by capacity, not a staleness contract, and forum reports of stale/evicted
  previews are consistent with that.
- **`com.unity.testframework.graphics`'s `ImageComparisonSettings`**
  (`Runtime/ImageComparison/ImageComparisonSettings.cs`,
  [needle-mirror/com.unity.testframework.graphics](https://github.com/needle-mirror/com.unity.testframework.graphics/blob/master/Runtime/ImageComparison/ImageComparisonSettings.cs)):
  `TargetWidth`/`TargetHeight` default **512×512**; `PerPixelGammaThreshold` and
  `PerPixelAlphaThreshold` default **1/255**; `IncorrectPixelsThreshold` defaults to
  **1/(512×512)** — literally "allow one bad pixel" at the default resolution;
  `AverageCorrectnessThreshold`, `PerPixelCorrectnessThreshold`, `RMSEThreshold` all default to
  **0** (strict-until-loosened, not pre-tuned permissive defaults);
  `ActiveImageTests = AverageDeltaE` by default, with `IncorrectPixelsCount`/`RMSE` available as
  opt-in flags.
- **Reference images are keyed `ColorSpace/Platform/GraphicsAPI`**, with a documented fallback:
  the runner checks `ReferenceImages/<ColorSpace>/<Platform>/<GraphicsAPI>` first, then falls back
  to a consolidated `ReferenceImagesBase` folder when one image is valid across multiple
  APIs/platforms (a "Start Optimization" tool merges identical images into that base folder) —
  [Graphics Test Framework manual](https://docs.unity3d.com/Packages/com.unity.testframework.graphics@8.3/manual/index.html).
  Three keying axes, one more than UE's two (Platform/RHI) — Unity separates color space out
  explicitly because linear-vs-gamma rendering genuinely changes reference pixels independent of
  platform or API.
- **No test golden-compares a per-asset preview thumbnail specifically.** `ImageAssert.AreEqual`
  overloads take a `Camera` or a whole `Texture2D`, at the framework's fixed target resolution —
  nothing in the public API or in `Unity-Technologies/Graphics` is scoped to inspector/
  Project-window-thumbnail-sized images. Thumbnails are outside this framework's design center
  entirely.

---

## Framing/lighting conventions — comparing the formulas directly

Arcane's `FrameMeshBounds` (`ArcaneEditor/src/Project/MeshImportWave.hpp:235-236`,
implementation in `MeshImportWave.cpp`): `distance = radius / sin(fov/2) × 1.15`, eye direction
`normalize(1, 0.6, 1)` — an exact sphere-to-camera distance (the sphere's silhouette exactly
touches the FOV cone) times a 15% pad, viewed from a fixed diagonal-ish angle.

Unreal's material/static-mesh thumbnail cameras (`FMaterialThumbnailScene::GetViewMatrixParameters`,
`FStaticMeshThumbnailScene::GetViewMatrixParameters`, both in
`ThumbnailHelpers.cpp:408-441` and `:586-615`): `TargetDistance = (SphereRadius × 1.15) / tan(FOV/2)`,
default FOV **30°**, with a per-orbit yaw/pitch/zoom stored on a `USceneThumbnailInfo` the artist
can hand-tune per asset, and a **minimum camera distance clamp of 48** units
(`ThumbnailHelpers.cpp:127-132`) so degenerate near-zero-size assets don't divide-by-near-zero into
a nonsensical view. Skeletal meshes use the same formula *without* the 1.15 pad
(`ThumbnailHelpers.cpp:503-507`, comment: "skeletal meshes already buffer bounds").

Three things worth naming explicitly:

1. **The `1.15` bounds-padding constant is not a Godot/Unity thing at all — it's independently
   present in both Arcane and Unreal**, for the same stated reason (Unreal's comment: "add extra
   size to view slightly outside of the bounds to compensate for perspective";
   `ThumbnailHelpers.cpp:417-418`). That two engines converged on the same 15% pad independently is
   corroboration Arcane's constant is well-chosen, not a coincidence to second-guess.
2. **The trig differs slightly and Arcane's is the more correct one for a *sphere* bound.**
   Unreal's `radius / tan(halfFOV)` is exact only for framing a flat object of half-width `radius`
   at the near plane's normal — the formula that actually places a *sphere's silhouette* tangent to
   the FOV cone is `radius / sin(halfFOV)` (Arcane's), which converges to the same value as
   `tan` only as `halfFOV → 0`. At Unreal's default 30° FOV (`halfFOV = 15°`) the two formulas
   differ by roughly 3.5%. Not a bug in either engine (Unreal's target is "fits inside," not "is
   exactly tangent"), but worth knowing Arcane isn't accidentally doing the less-precise version.
3. **Unreal artist-tunable per-asset override (`USceneThumbnailInfo::OrbitPitch/OrbitYaw/OrbitZoom`)
   has no Arcane analog and is deliberately worth not building yet.** It exists because UE
   thumbnails are hand-curated, checked-in metadata per asset — a legitimate feature for a shipping
   game's art-directed content browser, but it is exactly the kind of per-asset authored state that
   would make a *golden* test of thumbnails harder to reason about (two knobs producing the pixels
   instead of one deterministic function of geometry). Arcane's pure `bounds → camera` function
   with no per-asset override is the more test-friendly shape, and the report's design implication
   below leans on that directly.

Godot's fixed-camera/scale-the-object convention (above) achieves the same goal (fit any bounds
into a stable frame) through the opposite mechanism, and is arguably *more* golden-test-friendly
than either bounds→distance formula: because the camera transform is a hardcoded constant and the
object is the only thing that moves/scales, there is zero floating-point disagreement possible in
the *camera* term across engine versions — only the object's transform can drift, and that drift is
visible and attributable to one number (the scale factor) rather than folded into a distance
computation that also depends on FOV and radius. **Not a recommendation to switch** (Arcane's
approach is fine and already measured stable), but worth flagging as the shape to reach for if
FOV or the trig ever becomes a source of re-bless churn.

---

## Invalidation conventions, and hash-keying for a golden test

| Engine | Invalidation trigger | Keyed by |
|---|---|---|
| Unreal | Per-class policy enum (`Realtime`/`OnPropertyChange`/`OnAssetSave`/`Once`) + explicit dirty flag set at save time | Object full name, inside the owning package |
| Godot | File mtime changed → re-hash content → invalidate only if content MD5 also changed; explicit engine-version escape hatch | `md5(absolute path)` |
| Unity | Undocumented (capacity-bounded cache eviction, not staleness) | Not documented publicly |
| Arcane (current) | `.arcmat`/`.arcmesh` source mtime newer than the `.png`'s mtime (`MaterialPreviewHarvester.hpp:44-48`, `PrimeFromDisk`) | Asset GUID (filename `<guid>.png`) |

Godot is the only one of the three with a genuinely **hash-keyed** cache, and it's a two-part hash:
a **path hash** for the cache filename (so the file *name* never needs the content), and a
**content hash as an invalidation tiebreaker** (so a touch/checkout that bumps mtime without
changing bytes doesn't force a re-render). That second half is the piece Arcane's current
mtime-only check (`MaterialPreviewHarvester`'s `PrimeFromDisk`) doesn't have — a `git checkout`,
CI clone, or editor re-save-without-changes will bump mtime and trigger an unnecessary re-harvest
under Arcane's current rule, where Godot's would no-op after the content-hash check.

**For a *golden test* specifically** (not the harvester's own runtime cache, a separate concern),
the natural key is neither path nor mtime — both are unstable across machines/checkouts/CI clones
in a way a test fixture should not be sensitive to. The transferable shape from Godot is: **key the
golden reference by a content hash of the *inputs that determine the pixels*** — for a material
thumbnail that's the compiled shader bytecode/snippet hash plus texture content hashes plus the
camera-framing inputs (bounds, FOV); for a mesh thumbnail it's the mesh's vertex/index bytes plus
bounds. That decouples "did this reference need re-blessing" from "did a file's mtime change for
an unrelated reason," which matters more here than in Godot's case precisely *because* the renderer
is expected to change substantially and repeatedly — a hash tied to shader/mesh content survives
engine-internal refactors that don't change output, and only invalidates when the actual visual
inputs move.

---

## What no one does

**Nobody golden-tests thumbnails/previews as their own artifact class, in any of the three engines
surveyed.** This is not a gap unique to one engine — it holds across all three, and for different
structural reasons in each:

- **Unreal** has the machinery (`ImageComparer`, `FAutomationScreenshotOptions`, an entire
  Screenshot Browser UI) but built it exclusively for whole-level/whole-viewport functional-test
  screenshots. `FObjectThumbnail` generation is old, orthogonal infrastructure with its own
  invalidation policy and zero contact with the comparison system — the two were never unified,
  and after two decades of UE shipping, still aren't.
- **Godot** doesn't have a golden-image framework for *anything* — not thumbnails, not the
  renderer's own output. Its CI is a crash/smoke test only. Rendering regressions in Godot are
  caught by human eyes, not by a pixel diff, at any granularity.
- **Unity** built a real graphics test framework with real tolerance parameters and per-API/
  platform/color-space reference organization — but scoped it entirely to whole-camera captures at
  a fixed resolution (default 512×512). `AssetPreview` previews live in a completely separate,
  informally-tested code path with no documented staleness contract, let alone a golden comparison.

The honest reading: **per-asset preview/thumbnail rendering is universally trusted to "the
renderer's own tests will catch real bugs, and a human will eventually notice a wrong icon,"** never
to a dedicated small-image golden suite. That is not evidence the idea is unsound — it's evidence
it's simply unusual. Nothing about a 64px image makes it harder to golden-test than a full frame;
if anything a smaller image is cheaper to store, cheaper to diff, and (per the Godot/UE framing
survey above) rendered through a *simpler*, more deterministic camera function than a full scene
(fixed FOV, fixed light rig, one asset, no gameplay state). The absence of prior art here is best
read as "nobody needed it because nobody's thumbnail renderer was volatile enough to be worth
pinning" — which is exactly the condition Arcane's stated Deadlock/Source-2 renderer-churn premise
inverts. Small per-asset references being cheap and unusual, not wrong, is the correct takeaway.

---

## Shape decisions that carry over to Arcane's proposal

1. **Reference layout keyed by backend (RHI), matching the existing whole-frame golden gate's
   scheme, not a new scheme for thumbnails.** Unreal keys ground truths by
   `Platform_RHI_ShaderModel` (companion note, Q2) and Unity by `ColorSpace/Platform/GraphicsAPI`
   (this note, Unity §3) — every engine surveyed that does golden image comparison keys by
   graphics backend, never leaves it unkeyed. Arcane's existing gate already resolves
   `References/<backend>/<name>.png` falling back to `References/<name>.png`
   (`ArcaneClient/src/Arcane/Host/ReferenceImages.hpp`, cited in the companion note). A thumbnail
   golden test should reuse that exact resolution rule for its own reference tree
   (`References/Thumbnails/<backend>/<guid-or-name>.png` → `References/Thumbnails/<guid-or-name>.png`)
   rather than inventing a parallel scheme — two independent engines converging on "closest match
   with a general fallback" (companion note Q2) is strong evidence this generalizes to a second
   asset class inside the same repo, not just across engines.

2. **Tolerance should stay near zero, the same way the existing whole-frame gate does, and for the
   same measured reason** — Unreal's own `Low` preset exists because of TAA noise the whole-frame
   gate has to tolerate; a fixed-camera, fixed-light, single-asset 64px thumbnail render has *less*
   noise surface than a whole scene, not more (no TAA history buffer warming up, no gameplay-driven
   variance, no partial-frame timing). The companion note already established Arcane's whole-frame
   gate runs at **zero** diff budget on all four lanes today (companion note Q1). A thumbnail
   golden test should default to the same zero-tolerance-with-antialiasing-classification model
   Arcane already ships (`ImageCompare.hpp`'s existing cascade), not import Unreal's three-knob
   structure or Unity's `IncorrectPixelsThreshold`/`AverageCorrectnessThreshold` pair — there is no
   evidence from any engine surveyed that per-asset previews need a *looser* tolerance than
   whole-frame captures; if anything the framing survey above (fixed FOV/light/camera, no gameplay
   state) argues they need less slack, not more.

3. **Approve/re-bless workflow: reuse `--bless` verbatim, keyed the same way as the whole-frame
   gate, never add a bulk "replace all thumbnails" hammer.** The companion note (Q3) already
   established Unreal's dangerous "Replace" affordance (deletes all ground truth for an image) is
   a hazard Arcane correctly lacks by design (`--bless` writes exactly one image at the level it
   resolved from). That reasoning applies unchanged to thumbnails, and matters *more* here because
   the premise of this proposal is "every reference gets re-blessed repeatedly" — a bulk-replace
   tool is exactly the shortcut someone reaches for when re-blessing dozens of thumbnails after a
   renderer arc, and exactly the tool that would silently blow away a manually-curated alternative
   reference if one ever existed. Keep re-blessing to one-image-at-a-time, scripted in a loop over
   the reference project's asset list if bulk re-blessing after a renderer change is needed — the
   loop can be automated; the single-image write primitive should not become bulk.

4. **Per-widget/per-surface capture as the mechanism, not "crop a bigger screenshot."** Unreal's
   `FSlateApplication::TakeScreenshot(TSharedRef<SWidget>&, ...)` (this note, UE §"widget/panel
   capture") captures an arbitrary widget subtree directly rather than deriving a sub-image from a
   full-window screenshot. Arcane's proposal is architecturally already ahead of this — the
   harvester renders each thumbnail into its own dedicated 64×64 offscreen `NriGraphContext`
   (`MaterialPreviewHarvester.hpp:9-22`), so there is no "crop out of a bigger frame" step to get
   wrong in the first place. Worth stating explicitly as a design invariant to preserve: a
   thumbnail golden test must diff the harvester's own offscreen capture, never a screenshot of the
   Assets panel's ImGui image widget (which would reintroduce exactly the DPI/window-chrome/
   panel-scroll-position noise the dedicated offscreen context was built to avoid).

5. **Content-hash-keyed invalidation for the *reference*, distinct from the harvester's own
   runtime mtime cache.** Godot's two-part key (path-hash filename, content-hash invalidation
   tiebreaker — this note, Godot §"invalidation") is the one genuinely new idea none of Arcane's
   existing golden infrastructure has, because the existing whole-frame gate's inputs (a fixed
   scene file, fixed camera, fixed lighting) don't churn the way per-asset content does across a
   whole reference project. Recommendation: key each thumbnail golden test case not by the asset's
   GUID/filename alone but by a hash of the inputs that actually determine its pixels (compiled
   shader bytes + referenced texture bytes for a material; vertex/index bytes + bounds for a mesh).
   That makes "does this reference need re-blessing" a function of content, immune to the
   mtime-noise Arcane's harvester cache is currently exposed to (checkout/CI-clone bumping mtimes
   without changing bytes) and — more importantly for the stated renderer-churn premise — immune to
   internal engine refactors that don't change the rendered pixels at all, so a Deadlock-arc
   refactor that leaves a given material's *output* unchanged doesn't force a spurious re-bless.

---

## Sources

**Local (read, not reproduced beyond short identifiers — license-bound per the sourcing boundary
this repo already observes for UE source, see the companion note's own boundary statement):**
- `.example/UnrealEngine-release/Engine/Source/Editor/UnrealEd/Public/ThumbnailHelpers.h`
- `.example/UnrealEngine-release/Engine/Source/Editor/UnrealEd/Private/ThumbnailHelpers.cpp`
- `.example/UnrealEngine-release/Engine/Source/Runtime/Core/Public/Misc/ObjectThumbnail.h`
- `.example/UnrealEngine-release/Engine/Source/Editor/UnrealEd/Private/ObjectTools.cpp` (lines ~5555-5670)
- `.example/UnrealEngine-release/Engine/Source/Editor/UnrealEd/Classes/ThumbnailRendering/ThumbnailRenderer.h`
- `.example/UnrealEngine-release/Engine/Source/Editor/UnrealEd/Private/ThumbnailManager.cpp`
- `.example/UnrealEngine-release/Engine/Source/Developer/ScreenShotComparisonTools/Public/ImageComparer.h`
- `.example/UnrealEngine-release/Engine/Source/Developer/FunctionalTesting/Public/AutomationScreenshotOptions.h`
- `.example/UnrealEngine-release/Engine/Source/Developer/FunctionalTesting/Private/ScreenshotFunctionalTest.cpp`
- `.example/UnrealEngine-release/Engine/Source/Runtime/Engine/Private/Tests/AutomationCommon.cpp` (lines ~395-540, ~670-820)
- `.example/UnrealEngine-release/Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h` (lines ~1030-1062)
- `D:\dev\starworks\Arcane\ArcaneEditor\src\Project\MaterialPreviewHarvester.hpp`
- `D:\dev\starworks\Arcane\ArcaneEditor\src\Project\MeshImportWave.hpp` (`FrameMeshBounds` declaration)
- `D:\dev\starworks\Arcane\ArcaneTests\src\MeshThumbnailHarvestTest.cpp`
- `D:\dev\starworks\Arcane\docs\research\2026-08-26-unreal-screenshot-comparison-research.md` (companion)

**Public (Godot, MIT-licensed):**
- https://github.com/godotengine/godot/blob/master/editor/inspector/editor_resource_preview.cpp
- https://github.com/godotengine/godot/blob/master/editor/inspector/editor_resource_preview.h
- https://github.com/godotengine/godot/blob/master/editor/inspector/editor_preview_plugins.cpp
- `.github/workflows/linux_builds.yml`, `.github/actions/godot-project-test/action.yml` (godotengine/godot)

**Public (Unity docs/packages):**
- https://docs.unity3d.com/ScriptReference/AssetPreview.html
- https://docs.unity3d.com/ScriptReference/AssetPreview.GetAssetPreview.html
- https://github.com/needle-mirror/com.unity.testframework.graphics/blob/master/Runtime/ImageComparison/ImageComparisonSettings.cs
- https://docs.unity3d.com/Packages/com.unity.testframework.graphics@8.3/manual/index.html
- https://docs.unity3d.com/Packages/com.unity.testframework.graphics@7.8/api/UnityEngine.TestTools.Graphics.ImageComparisonSettings.html
