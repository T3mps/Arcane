# F2b — Asset Cook (Texture Slice) + Bindless Material Table Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Textures become compiled assets — sources cook to BC7 `.arcart` artifacts the runtime
refuses to live without — and a textured cube reaches the screen through the arc's own spine:
source → artifact → BC upload → bindless slot → `mesh.hlsl`, authored end-to-end in the editor.

**Architecture:** New `ArcaneAssetPipeline` static lib + `arccook.exe` (stb + bc7enc_rdo live
there; ArcaneClient gains only a reader for our own binary format). Full sprite migration onto
artifacts. `BindlessTable` per NRI Phase 4 Task 8, fed by cooked albedo. Editor cooks in-process
on a new JobSystem `Submit` seam.

**Tech Stack:** C++23 / premake5 / stb_image (moves to pipeline lib) / bc7enc_rdo (new vendor,
MIT) / nlohmann::json / Catch2 3.15.0 / NRI.

**Spec:** `docs/specs/2026-09-04-f2b-asset-cook-and-bindless-design.md` — read it first;
conflicts resolve against it. It carries the UE and Source 2 citations; they are not repeated
here.

**Plan-depth convention (Phase 4 precedent, deliberate):** Task 1 and the format/interface
blocks carry literal code. Later tasks pin exact INTERFACES and exact ASSERTIONS, not literal
test bodies — implementers write bodies against real code, and clauses marked "adjust to
source" mean the cited neighbouring pattern governs. Plan-supplied code is unrun code; the
cross-task contracts are what this plan must pin.

## Global Constraints

- **Build with PowerShell, never Git Bash** (Bash mangles `/p:`):
  `$env:ARCANE_SDK='D:\dev\starworks\Arcane'; & 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Arcane.slnx /p:Configuration=<Debug|Release|Dist> /m /v:m /nologo`
  from the repo root. Never a bare `.vcxproj`. Regenerate after premake edits:
  `ThirdParty\premake5\premake5.exe vs2026` with `ARCANE_SDK` set.
- **Zero `warning C`** in all three configs — a gate, not an aspiration.
- **Tests run FROM the exe dir**: `cd bin\<Config>-windows-x86_64-md\ArcaneTests` then
  `.\ArcaneTests.exe "<filter>"`. `"[mesh]"` pulls REAL GPU cases — use `"[mesh]~[gpu]"` for
  CPU-only runs. This desk's GPU is safe; `[gpu]` runs take seconds.
- **Never mutate live staged trees under `bin/`** outside the staging changes Tasks 5/8
  deliberately make. Fresh-copy hygiene for anything adversarial.
- **Never commit `out.txt`** (repo root, the user's). `git add` files BY NAME.
- **Commit after every task. Never squash tasks together.**
- **Derive counts, never recall them** — every gate/baseline figure comes from the run's own
  final line.
- **Every new refusal or failure path is WATCHED FIRING ONCE before its task closes** (Arc B
  standard).
- The verify/compare oracle (`LoadPngRgba` at `RuntimeApp.cpp:592`, `EditorAppFrame.cpp:252`,
  bless at `ReferenceImages.cpp:85`, diff write at `RuntimeApp.cpp:1084`) and editor chrome
  (`LoadDisplayPixels`, `Window.cpp:188`) KEEP stb. Only content-texture decoding migrates.
- Baselines at plan time (Arc B close): `~[gpu]` Debug/Release **52462/1316**, Dist
  **52394/1310**. Task 14 re-derives; figures MUST RISE.
- ABI at plan time: **20**. Task 6 takes it to **21**.

---

## File Structure

**New:**

| Path | Responsibility |
|---|---|
| `ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactFormat.{hpp,cpp}` | `.arcart` header/mip-table/sections/thumb — write, read, validate. One codebase owns the bytes. |
| `ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactStore.{hpp,cpp}` | Hash-flat store, atomic writes, rebuildable index, orphan sweep. |
| `ArcaneAssetPipeline/src/Arcane/AssetPipeline/CookKey.{hpp,cpp}` | The triple hash + `kTextureImporterVersion` composite constant. |
| `ArcaneAssetPipeline/src/Arcane/AssetPipeline/TextureImporter.{hpp,cpp}` | decode → linearize → box mips → encode → thumbnail → artifact. |
| `ArcaneAssetPipeline/src/Arcane/AssetPipeline/TextureMetaSettings.{hpp,cpp}` | Typed `.meta` `texture` block: format/srgb/generateMips/maxSize + defaults. |
| `ArcaneAssetPipeline/src/Arcane/AssetPipeline/CookSession.{hpp,cpp}` | Enumerate sources, key-check, cook stale; shared by CLI and editor. |
| `ArcaneAssetPipeline/src/Arcane/AssetPipeline/DdsDump.{hpp,cpp}` | `--dump-dds` debug export (BC7/RGBA8, 2D). Never a load path. |
| `arccook/src/main.cpp` | CLI: `--project`, `--check`, `--verbose`, `--dump-dds`. |
| `ThirdParty/bc7enc_rdo/` (+ `premake5.lua`) | Vendored BC7 encoder, static lib. |
| `ArcaneClient/src/Arcane/Assets/ArtifactReader.{hpp,cpp}` | Reads/validates `.arcart` only. No interchange parsing. |
| `ArcaneClient/src/Arcane/Render/Nri/BindlessTable.{hpp,cpp}` | Phase 4 Task 8 interface verbatim. |
| `ArcaneTests/src/AssetPipeline*Test.cpp` (several) | Pipeline unit tests (CPU, `~[gpu]`). |

**Modified (seams verified in source 2026-09-04):**

| Path | Change |
|---|---|
| `premake5.lua` | `ArcaneAssetPipeline` static lib (pattern :110-112), `arccook` console exe (pattern :292-294), ArcaneTests links += `ArcaneAssetPipeline` (:674), postbuild cook + `{COPYDIR}` Intermediate/Artifacts (Runtime :347+, Editor :421+, Tests :692+). |
| `ArcaneClient/src/Arcane/Assets/Assets.{hpp,cpp}` | `TextureInfoFor` (new), `PixelsFor` → thumbnail semantics, artifact-refusal memoization beside `:259-272`'s pattern. Content `LoadPngRgba` route retired (oracle/chrome kept). |
| `ArcaneClient/src/Arcane/Render/SpriteCache.cpp:102` | Dims from `TextureInfoFor`, not `PixelsFor`. |
| `ArcaneClient/src/Arcane/Render/Nri/NriTextureCache.{hpp,cpp}` | BC7_SRGB/UNORM + RGBA8 artifact upload with mips (:190-231 region), artifact supply seam, raw-RGBA supply kept for `ColorSpace::Display` (:90), checkerboard pending-placeholder. |
| `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp:447` region | 20 → 21 + comment entry. Both `.arcproj` restamped; Gacha commit unpushed. |
| `data/shaders/mesh.hlsl` | t0 → bindless array; material slot packed in `normalMatrixCol0.w` (:29-36 block stays 128 bytes). |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.{hpp,cpp}` | `MeshInstance` gains `materialSlot` (the reserved block `MeshNode.hpp:186-195`), bindless wiring, `SupportsBindless()` gate (`NriDeviceCaps.hpp:26` — first production call site), immutable trilinear sampler. |
| `ArcaneClient/src/Arcane/Material/*`, `Scene/SceneResources.hpp:197-200`, `Render/MeshMaterialCache.cpp:144-151`, `Scene/MeshSubmissionSystem.hpp:102-128`, `Host/SceneRenderResolver.cpp:357-378` | `albedo` Guid end-to-end: parse → `ResolvedMeshMaterial` → resolve → slot → instance. |
| `ArcaneClient/src/Arcane/Jobs/JobSystem.hpp:24-48` (+cpp) | `Submit(std::function<void()>)` beside `WorkerCount()` (:43); enkiTS impl stays in pimpl. |
| `ArcaneEditor/src/App/EditorAppProject.cpp` | Watcher extends past `AssetKind::Material` filter (:215-216) to texture sources → cook queue; `CreateMaterialAt` kind param (:391-434, "fullscreen" at :402); cook diagnostics via `Arcane::Diagnostics::Publish` (pattern :1180-1192); orphan sweep at project open. |
| `ArcaneEditor/src/App/EditorAppFrame.cpp:207-273` | Verify/capture/bless waits on pending cooks (gate before `m_compareRequested` at :273). |
| `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp` | `meshSurface` picker enabled (:2180-2182), mesh decl path incl. albedo via `DrawTextureParam` (:5676). |
| `ArcaneEditor/src/Panels/InspectorView.cpp:287-293` | Texture preview from `PixelsFor` thumbnail — the reserved spot. |
| `scripts/golden-gate.ps1:568-611, :1136-1144` | Stage `Intermediate/Artifacts` beside the Content mirror block, and in the restore path. |
| `Jenkinsfile:40-47` | `arccook --check` stage between Build and Tests. |
| `NOTICE.md`, `ThirdParty/README.md` | bc7enc_rdo rows + the AgilitySDK rider row. |

---

### Task 1: `ArcaneAssetPipeline` lib + the `.arcart` format

**Files:**
- Create: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactFormat.{hpp,cpp}`
- Modify: `premake5.lua` (static lib after :110-112 pattern; ArcaneTests `links` :674 + includedirs)
- Test: `ArcaneTests/src/AssetPipelineFormatTest.cpp` (new; ArcaneTests globs `src/**` — no premake file-list edit)

**Interfaces — Produces (later tasks rely on these exact names):**

```cpp
namespace Arcane::AssetPipeline
{
    enum class ArtifactPixelFormat : std::uint8_t { RGBA8, BC7, BC5_Reserved, BC6H_Reserved };
    enum class ArtifactDimension  : std::uint8_t { Tex2D, Tex2DArray_Reserved, TexCube_Reserved,
                                                   TexCubeArray_Reserved, Tex3D_Reserved };
    enum class SectionTag : std::uint32_t { MipTable = 1, Payload = 2, Thumbnail = 3 };

    struct MipDesc { std::uint64_t offset; std::uint64_t size; std::uint32_t width, height; };

    struct TextureArtifactDesc {                    // header, serialized little-endian
        Guid sourceGuid;  std::uint64_t sourceHash;  std::uint32_t importerVersion;
        ArtifactPixelFormat format;  ArtifactDimension dimension;   // Tex2D this slice
        std::uint32_t arrayOrDepth;                                  // 1 this slice
        std::uint32_t width, height, mipCount;  bool srgb;
        std::vector<MipDesc> mips;               // slice-major, mip-minor when arrayOrDepth > 1
        std::uint32_t thumbWidth, thumbHeight;
    };

    // Write returns false on IO failure. Read validates magic/version/section table and
    // REFUSES (nullopt) on bad magic, unknown artifactVersion, or truncation.
    [[nodiscard]] bool WriteTextureArtifact(const std::filesystem::path&, const TextureArtifactDesc&,
                                            std::span<const std::byte> payload,
                                            std::span<const std::byte> thumbRgba);
    struct LoadedArtifact { TextureArtifactDesc desc; std::vector<std::byte> payload, thumbRgba; };
    [[nodiscard]] std::optional<LoadedArtifact> ReadTextureArtifact(const std::filesystem::path&);
}
```

- [ ] **Step 1: Premake.** Add the static-lib project (copy the ArcaneCore shape at
  `premake5.lua:110-112`; server-style warnings/flags), add `ArcaneAssetPipeline` to
  ArcaneTests `links` (:674) and includedirs. Regenerate, build Debug — empty lib compiles.
- [ ] **Step 2: Write the failing tests.** Assertions to pin (write real bodies against the
  interface above): round-trip every `TextureArtifactDesc` field byte-exactly, including
  `dimension == Tex2D` and `arrayOrDepth == 1`; mip table order preserved; payload and thumb
  bytes identical after round-trip; `ReadTextureArtifact` returns nullopt on (a) wrong magic,
  (b) `artifactVersion + 1`, (c) file truncated mid-payload — each refusal watched firing;
  tagged-section reader skips an UNKNOWN section tag without failing (forward-compat pin).
- [ ] **Step 3: RED** — compile failure naming `ArtifactFormat.hpp`. Record it.
- [ ] **Step 4: Implement.** Fixed field order, little-endian, explicit-field writes (never
  `memcpy` of structs — the spec's struct-padding determinism trap). Section table:
  `sectionCount + {tag, offset, size}` after the fixed header.
- [ ] **Step 5: Build Debug; run `ArcaneTests.exe "[pipeline]"` from the exe dir.** All pass,
  pristine output.
- [ ] **Step 6: Commit** — `feat(pipeline): ArcaneAssetPipeline lib -- .arcart texture artifact format`

---

### Task 2: Hash-flat store, rebuildable index, triple cook key

**Files:**
- Create: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactStore.{hpp,cpp}`, `CookKey.{hpp,cpp}`
- Test: `ArcaneTests/src/AssetPipelineStoreTest.cpp`

**Interfaces — Produces:**

```cpp
// CookKey.hpp
// COMPOSITE version: bump when importer logic, bc7enc_rdo, or stb change. Constituents
// named here so no one forgets the third term of the triple.
inline constexpr std::uint32_t kTextureImporterVersion = 1;   // {importer v1, bc7enc_rdo <pin>, stb <pin>}
[[nodiscard]] std::uint64_t ComputeCookKey(std::span<const std::byte> sourceBytes,
                                           const TextureMetaSettings&, std::uint32_t importerVersion);

// ArtifactStore.hpp — rooted at <project>/Intermediate/
class ArtifactStore {
public:
    explicit ArtifactStore(std::filesystem::path intermediateDir);
    [[nodiscard]] std::filesystem::path PathFor(std::uint64_t cookKey) const; // Artifacts/<hh>/<hex>.arcart
    // Writer receives the .tmp path; Commit renames into place only when it returns true.
    [[nodiscard]] bool Commit(std::uint64_t cookKey,
                              const std::function<bool(const std::filesystem::path& tmpPath)>& writer);
    [[nodiscard]] std::optional<std::uint64_t> Lookup(const Guid&) const;      // via index
    void PutIndex(const Guid&, std::uint64_t cookKey);
    void RebuildIndexFromScan();                                               // headers are self-describing
    std::size_t SweepOrphans(/*live guid set*/);
};
```

(`TextureMetaSettings` arrives in Task 3 — for this task's key tests, a two-field stub with
the real name is correct and Task 3 grows it in place.)

- [ ] **Step 1: Failing tests.** Pin: key CHANGES when any triple constituent changes (source
  byte, one setting, `importerVersion`) and is stable otherwise; `Commit` is atomic — a
  writer callback that throws leaves NO file at the final name and no stray `.tmp` (watched);
  `RebuildIndexFromScan` over a directory of artifacts written by Task 1's writer reproduces
  the index exactly; `SweepOrphans` removes artifacts whose guid is not live and touches
  nothing live.
- [ ] **Step 2: RED, Step 3: implement, Step 4: green** (`"[pipeline]"`), same discipline.
- [ ] **Step 5: Commit** — `feat(pipeline): hash-flat artifact store, rebuildable index, triple cook key`

---

### Task 3: Texture importer v1 — the RGBA8 path

**Files:**
- Create: `TextureImporter.{hpp,cpp}`, `TextureMetaSettings.{hpp,cpp}` (both under
  `ArcaneAssetPipeline/src/Arcane/AssetPipeline/`)
- Test: `ArcaneTests/src/AssetPipelineImporterTest.cpp`

**Interfaces — Produces:**

```cpp
struct TextureMetaSettings {          // the .meta "texture" block; absent block == defaults
    enum class Format : std::uint8_t { Auto, Bc7, Rgba8 };
    Format format   = Format::Auto;   // Auto == Bc7 this slice
    bool   srgb     = true;
    bool   generateMips = true;
    std::uint32_t maxSize = 0;        // 0 == unlimited
    static TextureMetaSettings FromMetaJson(const nlohmann::json&);  // tolerant, defaulted
    nlohmann::json ToMetaJson() const;
};
// ImportTexture: decode (stb, THIS lib) -> linearize sRGB -> 2x2 box mips in linear float ->
// re-encode -> thumbnail (<=64 long edge) -> TextureArtifactDesc + payload + thumb.
struct ImportedTexture { TextureArtifactDesc desc; std::vector<std::byte> payload, thumbRgba; };
[[nodiscard]] std::optional<ImportedTexture> ImportTexture(std::span<const std::byte> pngBytes,
                                                           const Guid& sourceGuid,
                                                           const TextureMetaSettings&);
```

- [ ] **Step 1: Failing tests.** Pin: mip chain dims halve to 1×1 (incl. non-square, NPOT
  stays NPOT); **linear-space downsample correctness** — a 2×2 sRGB image of pure black and
  pure white quads averages to the value whose LINEAR average re-encodes to sRGB ≈188, not
  128 (the assertion that catches gamma-space averaging); `maxSize` drops top mips; settings
  JSON round-trip incl. absent-block defaults; thumbnail ≤64 long edge with aspect kept;
  `generateMips=false` → mipCount 1; RGBA8 path cook-twice byte-identity.
- [ ] **Step 2: RED → implement → green.** stb decode lives HERE (move/include stb in the
  pipeline lib; `StbImpl.cpp` in ArcaneClient stays — the oracle/chrome need it there).
- [ ] **Step 3: Commit** — `feat(pipeline): texture importer v1 -- decode, linear mips, thumbnail, RGBA8 artifacts`

---

### Task 4: Vendor bc7enc_rdo — BC7 artifacts, byte-deterministic

**Files:**
- Create: `ThirdParty/bc7enc_rdo/` (sources + `LICENSE` + `premake5.lua` static-lib per house
  pattern), rows in `NOTICE.md` + `ThirdParty/README.md` (pinned version + upstream URL)
  **and the AgilitySDK rider row** (the tree's only non-OSS dep, twice-flagged, still absent).
- Modify: `TextureImporter.cpp` (BC7 encode path), `CookKey.hpp` (version comment gains the
  bc7enc pin), `premake5.lua` (workspace include + pipeline links)
- Test: extend `AssetPipelineImporterTest.cpp`

- [ ] **Step 1: Vendor.** bc7enc_rdo is a drop-in set of .cpp/.h — static-lib project like
  Catch2/rapidcheck. Pin the upstream commit in `ThirdParty/README.md`.
- [ ] **Step 2: Failing tests.** Pin: BC7 payload size == ceil(w/4)*ceil(h/4)*16 per mip;
  decoded-block sanity (encode a flat-colour image, decode one block with the vendored
  decoder, channels within encoder tolerance of source); **cook-twice byte-identity now
  covering BC7** — per the spec's determinism ruling, RDO runs single-threaded per asset
  unless a 10-run identity check on this desk proves the threaded path deterministic; record
  which in the task report.
- [ ] **Step 3: RED → implement → green.** sRGB flag rides the artifact header; the encoder
  gets pinned settings only (no time-based or thread-count-dependent parameters).
- [ ] **Step 4: Full three-config build** (new vendored TU set — Dist hides behind
  `ARCANE_DIST` only if someone adds guards; there are none to add). Zero warnings each.
- [ ] **Step 5: Commit** — `feat(pipeline): vendor bc7enc_rdo -- BC7 artifacts, byte-deterministic`

---

### Task 5: `arccook.exe`, `CookSession`, post-build cook + staging

**Files:**
- Create: `arccook/src/main.cpp`, `CookSession.{hpp,cpp}`, `DdsDump.{hpp,cpp}`
- Modify: `premake5.lua` (console exe per :292-294; postbuild arccook run + `{COPYDIR}`
  `Intermediate/Artifacts` for Runtime :347+, Editor :421+, Tests :692+),
  `scripts/golden-gate.ps1` (:568-611 gains an Artifacts mirror block beside Content's;
  restore path :1136-1144 restages it)
- Test: `ArcaneTests/src/AssetPipelineSessionTest.cpp`

**Interfaces — Produces:** `CookSession::CookProject(projectDir) -> {cooked, upToDate, failed}`
counts; `CookSession::CheckProject(projectDir) -> bool anyStale` (the `--check` core); both
shared verbatim by CLI and editor (Task 12).

- [ ] **Step 1: Failing tests** (session level, temp project fixture): first cook cooks N,
  second cook cooks 0 (idempotence); a `.meta` settings change recooks exactly that one;
  `CheckProject` true-then-false around a cook; a corrupt source memoizes a failure (no
  artifact, no retry-storm — assert the importer runs ONCE across two sessions for the same
  failing key), failure watched firing.
- [ ] **Step 2: RED → implement → green.** CLI exit codes: cook path 0 on success, 1 on any
  failure; `--check` 0 clean / 2 stale (distinct from failure). `--dump-dds` writes a
  standard DDS (BC7 fourCC 'DX10' header or RGBA8) for eyeballing only.
- [ ] **Step 3: Premake postbuild + gate staging.** ReferenceProject cooks ONCE at its
  post-build; `{COPYDIR}` stages `Intermediate/Artifacts` into both hosts' + tests' trees.
  Mirror-not-merge in golden-gate.ps1, copying the Content block's own shape (:568-611
  comments state the rationale — follow it).
- [ ] **Step 4: Build Debug; verify staged trees contain `Intermediate/Artifacts`; run
  `arccook --check` against the source ReferenceProject → exit 0.** Hosts are NOT yet
  consuming artifacts — nothing else changes behaviour this task.
- [ ] **Step 5: Commit** — `feat(pipeline): arccook CLI, cook session, post-build cook and artifact staging`

---

### Task 6: Runtime artifact route in ArcaneClient — `TextureInfoFor`, refusals, ABI 21

**Files:**
- Create: `ArcaneClient/src/Arcane/Assets/ArtifactReader.{hpp,cpp}` (reads `.arcart` ONLY —
  a sibling of Task 1's reader, ArcaneClient-local, sharing no pipeline code by design: the
  format doc comment in both names the other as the byte-contract peer)
- Modify: `Assets.hpp:95` region + `Assets.cpp` (`TextureInfoFor`, thumbnail `PixelsFor`,
  refusal memoization per the `:259-272` pattern), `SpriteCache.cpp:102`,
  `PluginABI.hpp` (20 → 21 + comment entry), `ReferenceProject/ReferenceProject.arcproj:6`,
  Gacha `Game/Aphelyon.arcproj:6` (separate commit there, subject
  `chore(game): restamp Aphelyon.arcproj to engine ABI 21`, do not push)
- Test: `ArcaneTests/src/ArtifactReaderTest.cpp`, additions to existing Assets tests

**Interfaces — Produces:**

```cpp
struct TextureInfo { std::uint32_t width, height, mipCount; bool srgb; /*true source dims*/ };
virtual const TextureInfo* TextureInfoFor(const Guid&) = 0;   // Assets facade — vtable MOVES
// ArtifactReader's load type — Task 7 consumes this exact name:
struct LoadedClientArtifact {
    TextureInfo info;  ArtifactPixelFormatValue format;        // mirrors the pipeline enum values
    std::vector<MipView> mips;                                  // {offset,size,width,height} views
    std::vector<std::byte> payload; std::vector<std::byte> thumbRgba;
    std::uint32_t thumbWidth, thumbHeight;
};
// PixelsFor contract narrows: PREVIEW pixels (artifact thumbnail) for cooked content.
// Refusal states (each memoized, each watched firing): ArtifactMissing, HashMismatch,
// VersionNewerThanEngine. An artifact older than the engine is a key miss == Missing —
// the spec's subsumption clause, restated in the reader's header comment.
```

- [ ] **Step 1: Failing tests.** `ArtifactReader` round-trips Task 1 writer output (fixture
  artifacts checked into the test's temp dir at runtime by calling the pipeline lib — the
  cross-lib byte contract test); the three refusals; `TextureInfoFor` serves HEADER dims
  while `PixelsFor` serves THUMB dims for the same guid (the SpriteCache-wrong-dims bug
  class, pinned).
- [ ] **Step 2: RED → implement → green** (`"[pipeline]"` + Assets/sprite suites + full
  `"~[gpu]"`).
- [ ] **Step 3: ABI.** Bump to 21 with a comment-block entry in the established voice (the
  cause: `Assets` — an ARCANE_API pure-virtual facade — gained `TextureInfoFor`, vtable
  layout moved; MEASURED: grep both game modules for `Assets`-facade calls, state the
  result). Restamp both arcprojs; rebuild `ReferenceProject.slnx` so `ReferenceGame.dll`
  is not stranded (F2a lesson); Gacha restamp committed unpushed.
- [ ] **Step 4: Commit** — `feat(host)!: runtime artifact route -- TextureInfoFor, thumbnail PixelsFor, plugin ABI 21`

---

### Task 7: `NriTextureCache` uploads compiled textures — mips, placeholder, chrome kept

**Files:**
- Modify: `NriTextureCache.{hpp,cpp}` (:190-231 upload region; :90 ColorSpace), both host
  wiring sites (`EditorApp.cpp:1739-1743`, `RuntimeApp.cpp:542`)
- Test: `ArcaneTests/src/NriTextureCacheArtifactTest.cpp` (`[gpu]` case + CPU logic cases)

**Interfaces — Produces:** the cache's supply seam becomes artifact-shaped
(`ArtifactSupplyFn: Guid -> const LoadedClientArtifact*`) for content; the raw-RGBA
`PixelSupplyFn` STAYS and serves `ColorSpace::Display`/chrome exactly as today. New states
per key: `Resident`, `PendingCook` (serves the lazily-created 8×8 checkerboard — cache-owned,
one per colour space), `Refused` (memoized, diagnostic). `PendingCook` and `Refused` must
never render alike — the spec's placeholder rule.

- [ ] **Step 1: Failing tests.** CPU: state machine transitions (pending → resident on
  supply-ready; refused sticky; checkerboard identity stable). GPU (`[gpu][pixel]`
  conventions per `NriGraphPixelTest.cpp:99-186` fixture): upload a 3-mip BC7 artifact
  (cooked in-test via the pipeline lib), sample at a scaled draw, assert the sampled colour
  matches the artifact's known flat colour on both backends (the multi-mip BC upload proof).
- [ ] **Step 2: RED → implement → green.** BC7_SRGB/BC7_UNORM/RGBA8 from
  `ArtifactPixelFormat` + srgb flag; per-mip `UploadData` rides the mip table.
- [ ] **Step 3: Wire both hosts** at the two cited sites; boot each host against the staged
  cooked tree (desk smoke: censuses unchanged, sprites render).
- [ ] **Step 4: Commit** — `feat(render): NriTextureCache uploads compiled artifacts -- mips, placeholder, chrome path kept`

---

### Task 8: The sprite cutover + the one re-bless  *(gate-touching — execution mode per controller ruling)*

**Files:**
- Modify: `Assets.cpp` (content `LoadPngRgba` route retired — the ORACLE and CHROME call
  sites stay, listed in Global Constraints), `ReferenceProject/Verify/References/*` (re-bless)

- [ ] **Step 1: Retire the content decode route.** Content texture resolution goes
  artifact-only; a `.png` with no artifact is now the `ArtifactMissing` refusal (loud), never
  a silent stb fallback. Run `"~[gpu]"` full — expected: sprite-dependent suites still green
  (they ride the staged cooked artifacts from Task 5).
- [ ] **Step 2: Re-bless, deliberately.** BC7 moves reference pixels by design. Run the
  Debug gate → expect compare failures on the four lanes; inspect diffs (compression-shaped
  deltas only — anything structural is a DEFECT, stop); bless via the gate's documented
  staged-bless procedure; **restage to BOTH hosts** (the arc-2 trap); re-run gate →
  `gatePassed: true` from the summary JSON (never the exit code), diffCount=0.
- [ ] **Step 3: Run `-SelfTest`** (clean tree required) → `SELF-TEST PASSED`. Run
  `"[witness]"` → both scenarios still pass (the oracle's stb path must be untouched — this
  run is the proof).
- [ ] **Step 4: Commit** — `feat(host)!: sprites ride cooked artifacts -- content stb route retired` (+ the blessed references in the same commit; they are the product of this change).

---

### Task 9: `BindlessTable`

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/Nri/BindlessTable.{hpp,cpp}`
- Test: `ArcaneTests/src/BindlessTableTest.cpp`

**Interfaces — Produces (Phase 4 Task 8, verbatim):**

```cpp
class BindlessTable {
public:
    static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;
    [[nodiscard]] static std::unique_ptr<BindlessTable> Create(NriDevice&, std::uint32_t capacity);
    [[nodiscard]] std::uint32_t Add(nri::Descriptor* srv);   // kInvalidSlot when full, warns ONCE
    void Release(Graveyard& g, std::uint64_t fence);          // Bury(fence, ...) — Graveyard.hpp:76
};
```

- [ ] **Step 1: Failing unit tests** (NONE backend): slots dense from 0; `Add` past capacity
  → `kInvalidSlot` + exactly one warning (watched); `Create(capacity 0)` refused;
  `Release` buries via `Graveyard::Bury` (assert with a counting destroyer, per the
  `Batch2DNode.cpp:582` call-site shape).
- [ ] **Step 2: RED → implement → green** (`"[bindless]~[gpu]"` tag the cases `[bindless]`).
- [ ] **Step 3: Commit** — `feat(nri): BindlessTable -- descriptor array + slot allocator`

---

### Task 10: `mesh.hlsl` goes bindless — gate, sampler, four-cube proof

**Files:**
- Modify: `data/shaders/mesh.hlsl` (:29-36, :39-46, :118-119), `MeshNode.{hpp,cpp}`
  (`MeshInstance` at :160-196; ranges :339-351; push :860-872)
- Test: `ArcaneTests/src/NriGraphPixelTest.cpp` (append, following :764-825 / :844-908 shapes)

**Interfaces — Produces:** `MeshInstance` gains `std::uint32_t materialSlot =
BindlessTable::kInvalidSlot;` in the reserved block (`MeshNode.hpp:186-195` names this exact
arrival). **The 128-byte root-constant block DOES NOT GROW** (`MeshNode.cpp:92` static_assert
stays): the slot is packed into `normalMatrixCol0.w` — unused today, all three normal-matrix
columns carry only xyz — via `asuint`/`asfloat`. `kInvalidSlot` in the shader selects the
flat `baseColor` path (F2a behaviour, bit-for-bit); any other slot samples the bindless
array. The s0 sampler becomes the spec's immutable trilinear sampler (aniso as the one knob).

- [ ] **Step 1: Gate first.** `MeshNode` creation checks `Caps().SupportsBindless()`
  (`NriDeviceCaps.hpp:26` — its FIRST production call site) and refuses loudly at node
  creation on tier 0. Watch the refusal fire once (force it: a test seam or a temporary
  local hack run, evidence in the report, hack reverted).
- [ ] **Step 2: Failing pixel test — four cubes.** Per the plan of record: four generated
  solid-colour textures (created directly through the NRI device in-test, the pixel-suite
  way), four `MeshInstance`s each with a distinct `materialSlot`; assert each cube's centre
  pixel is dominated by its own texture's channel (both backends, paired TEST_CASEs, tags
  `[gpu][pixel][mesh][bindless]`). A binding bug that ignores the index makes all four alike
  — the case that makes bindless proven rather than present.
- [ ] **Step 3: RED honestly** (new case against old shader fails), implement shader + node
  wiring, green on BOTH backends. Also re-run the F2a mesh cases — `kInvalidSlot` path must
  keep them green untouched (the flat-colour regression guard).
- [ ] **Step 4: Full `"~[gpu]"` + `"[gpu]"` sweeps.** State deltas.
- [ ] **Step 5: Commit** — `feat(nri): mesh materials index a bindless table`

---

### Task 11: Cooked albedo, end to end

**Files:**
- Modify: `MaterialAsset.cpp` (mesh-kind `albedo` Guid param), `SceneResources.hpp:197-200`
  (`ResolvedMeshMaterial` gains `Guid albedo;`), `MeshMaterialCache.cpp:144-151` (resolve it),
  `SceneRenderResolver.cpp:357-378` (albedo → `NriTextureCache` resolve → `BindlessTable`
  slot), `MeshSubmissionSystem.hpp:102-128` (instance carries the slot),
  F2a spec `:242-244` (annotate: diagnostic retired by implementation)
- Test: `ArcaneTests/src/NriGraphPixelTest.cpp` (the integration case), mesh-material CPU tests

- [ ] **Step 1: Failing CPU tests.** `.arcmat` mesh material with `albedo` round-trips;
  `ResolvedMeshMaterial.albedo` populated; nil albedo resolves to `kInvalidSlot` (flat path).
- [ ] **Step 2: The integration pixel case** — THE arc's proof: cook a small solid-colour
  PNG through the pipeline lib in-test → read via `ArtifactReader` → upload via
  `NriTextureCache` → `Add` to the table → a cube with that `materialSlot` renders the
  artifact's colour, both backends. Spine touched end-to-end in one assertion.
- [ ] **Step 3: RED → implement → green.** Slot lifetime: resolver owns the
  texture→slot map; slot released (buried) when the texture leaves residency.
- [ ] **Step 4: Commit** — `feat(render): mesh albedo -- cooked artifact to bindless slot, end to end`

---

### Task 12: JobSystem `Submit` + the editor's background cook

**Files:**
- Modify: `JobSystem.hpp:24-48` + `JobSystem.cpp` (`Submit` beside `WorkerCount()` at :43,
  enkiTS pinned-task/TaskSet inside the pimpl), `EditorAppProject.cpp` (watcher :202-258
  extends past the `AssetKind::Material` filter at :215-216 to texture sources — exact
  AssetKind spelling adjusted to source; on change: hash-check via `CookSession`, cook on
  `Submit`, on completion invalidate the texture cache entry + refresh; self-save
  re-baseline pattern :96-111 respected), cook diagnostics via
  `Arcane::Diagnostics::Publish("diagnostics:cook", …)` (call-site shape :1180-1192,
  clickable locator to the source file), orphan sweep at project open,
  `EditorAppFrame.cpp:207-273` (verify/capture/bless refuses-or-waits while
  `CookSession`-reported cooks are pending, gated BEFORE `m_compareRequested` at :273 —
  watched firing once with a synthetic pending cook)
- Test: `ArcaneTests/src/JobSystemSubmitTest.cpp` + cook-queue CPU tests

- [ ] **Step 1: Failing tests.** `Submit` runs the callable on a worker and completes
  (latch); destruction drains; cook-queue logic (hash-gate, one cook per change, completion
  callback) tested CPU-side with a fake importer.
- [ ] **Step 2: RED → implement → green.**
- [ ] **Step 3: Desk-adjacent smoke** (agent-runnable): launch the editor headless-verify
  path against a project with a pending-cook flag → watch the wait/refusal fire.
- [ ] **Step 4: Commit** — `feat(editor): background texture cook -- watcher-triggered, hash-decided, never blocks`

---

### Task 13: Mesh material authoring + inspector preview + the last F2a diagnostic

**Files:**
- Modify: `EditorAppProject.cpp:391-434` (`CreateMaterialAt` gains a surface argument;
  "fullscreen" hardcode at :402 and the `MaterialSurface::Fullscreen` snippet call at :422
  route by it; its caller offers Mesh), `ShaderEditorDocument.cpp:2180-2182` (`meshSurface`
  true where mesh materials are edited), the mesh decl path (albedo as a texture
  `ParamDecl` through the existing `DrawTextureParam` :5676 machinery),
  `MaterialSource.cpp:324,:347` + `MaterialGraph.cpp:389` (the snippet/graph-on-mesh guards
  become ONE memoized diagnostic instead of bare refusals — the F2a "ignores snippet/graph
  with one diagnostic" completion), `InspectorView.cpp:287-293` (texture preview from
  `PixelsFor` thumbnail — the reserved spot, comment honoured)
- Test: material-asset CPU tests (mesh kind + albedo authoring round-trip; the diagnostic
  fires once, watched); UI behaviour lands on the desk checklist

- [ ] **Step 1: Failing CPU tests → RED → implement → green.**
- [ ] **Step 2: Agent smoke:** create-mesh-material through the editor's command path
  headlessly if reachable; else defer to desk, stated in the report.
- [ ] **Step 3: Commit** — `feat(editor): author mesh materials -- surface picker, albedo param, inspector preview`

---

### Task 14: CI, full verification, closeout

**Files:**
- Modify: `Jenkinsfile` (`arccook --check` stage between Build :40 and Tests :47),
  `scripts/automation-baselines.json` (re-derived), spec status line + landed addendum

- [ ] **Step 1: Jenkins stage** — runs `bin/<Config>.../arccook.exe --project ReferenceProject --check`,
  fails the build on exit 2 (stale) or 1 (broken).
- [ ] **Step 2: Rebuild all three configs** — zero `warning C`, paste counts.
- [ ] **Step 3: Re-derive the six baselines** from each config's own `"~[gpu]"` final line
  (MUST RISE from 52462/1316 · 52394/1310); `"[gpu]"` sweep incl. the new pixel cases;
  `"[witness]"` both pass.
- [ ] **Step 4: Release gate + `-SelfTest`** — `gatePassed: true` from the summary JSON;
  `SELF-TEST PASSED`.
- [ ] **Step 5: Spec status → LANDED** with measured figures, which levers held, the
  re-bless record, and the Gacha follow-up note (Aphelyon needs a cook — one-line
  `scripts/setup.ps1` change in the Gacha repo, deliberately not this arc).
  Commit — `docs(spec): record F2b as landed` (+ baselines commit in the b076eb5c voice).
- [ ] **Step 6: DESK CHECKPOINT (USER — do not attempt, simulate, or mark done):** the
  contract's ergonomics list by hand (drop a png; change a setting; break a source; confirm
  no Reimport All exists), mesh-material authoring UX, the no-`tint` validation (two
  entities, one template, two instances differing in colour), placeholder-vs-refusal
  visual distinction. Hand off with both repos' HEADs named.

---

## Self-review (spec → plan coverage)

Run before Task 1 (the F2a lesson — the executor's pre-flight repeats it): every spec section
maps — §3→T1/T5, §4→T1/T3/T4, §5→T6/T7/T8, §6→T9/T10/T11, §7→T12/T13, §8→T5/T6/T8/T14,
§9→distributed+T14, §10 out-of-scope guarded (no browser grid, no Batcher2D bindless, no
BC5/BC6H encode, no streaming, no F2c), §11→T4, §12 rulings embedded in their tasks.
