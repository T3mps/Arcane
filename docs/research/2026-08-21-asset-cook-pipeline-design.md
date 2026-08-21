# Asset cook pipeline — design contract

**Decided 2026-08-21.** This is the design contract for Arcane's missing third
asset layer. It is pre-spec: read it before writing the cook arc's plan, the
same way `2026-08-12-deadlock-render-target.md` is read before any renderer-arc
spec.

It exists because the renderer's target contract (T2/T3) is unreachable without
it: a 144-entry box-projected cubemap array at RGBA16F is ~2.7 GB uncompressed
and ~340 MB as BC6H, and BC compression is an offline operation by nature.

## Where Arcane already sits

Every mature engine splits assets into three layers. Arcane has the first two.

| Layer | Purpose | Arcane today |
|---|---|---|
| 1 — **source** | DCC output (`.png`, `.fbx`, `.psd`). Never read by the runtime. | present — `Content/textures/uv_marker.png` + `.png.meta` sidecar |
| 2 — **authored** | Text, hand-editable, version-controlled; references sources + settings. | present — `.arcmat` / `.arcscene` / `.arcsprite`, JSON with an embedded `"id"` |
| 3 — **compiled** | Binary, load-by-memcpy, regenerable, gitignored. | **MISSING** |

Today the runtime reads layer 2 directly and stb-decodes layer 1 at load. That
holds for 2D sprites and breaks in 3D at four places, all of them inherently
offline: BC compression, mip chains, tangent generation, meshlet building.

**This arc is not a new architecture. It completes the one already here** —
`Intermediate/` exists in the project layout and is already gitignored.

## Precedent — the three pillars

| Engine | source → authored → compiled | Runtime reads |
|---|---|---|
| **Source 2** | `.fbx`/`.tga` → `.vmdl`/`.vtex` (KV3 text) → `.vmdl_c`/`.vtex_c` via `resourcecompiler.exe` | `_c` only |
| **Unreal** | `.fbx`/`.gltf` → `.uasset` (import, editor-only) → cooked `.pak`/`.uexp` | cooked only |
| **Unity** | `foo.png` + `foo.png.meta` → `Library/Artifacts/<hash>` | artifacts only |

The universal rule, and the one that matters most:

> **The interchange parser lives in the tooling. Never in the runtime.**

Arcane's project layout is already a hybrid of these — UE's directory names
(`Content`, `Config`, `Binaries`, `Intermediate`, `Saved`, `Plugins`, `Source`)
with Unity's identity model (`.meta` sidecars carrying guids). The cook step
follows Unity's artifact model because the front half already does.

## The decisions

### 1. Watcher triggers; hash decides

The existing `last_write_time` watcher (`EditorAppProject.cpp`,
`PluginHost.cpp`) is correct for *"something moved, go look"* and **wrong as a
cache key** — mtimes change on `git checkout`, differ across machines, and skew.
Timestamps wake the importer; a content hash decides whether work happens.

### 2. The cache key is a triple

```
hash( source bytes + import settings + IMPORTER VERSION )
```

The third term is the one that gets forgotten. Bump `kTextureImporterVersion`
and everything recooks — which is exactly what must happen the day the BC
encoder changes. Without it, stale artifacts ship silently.

### 3. Hash-flat, content-addressed storage

```
Intermediate/Artifacts/<hh>/<full-hash>.arcart
Intermediate/artifacts.index                      # Guid -> hash (a CACHE, see below)
```

Chosen over path-mirroring (Source 2 and UE both mirror content paths, and it is
more human-debuggable) because of **staleness**: a mirrored tree accumulates
orphans on every rename, move and delete, with no cheap way to tell an orphan
from a live artifact. Unity moved *to* hash-addressing for that reason. Dedup
and write atomicity come free.

### 4. Artifacts are SELF-DESCRIBING; the index is a rebuildable cache

Each artifact header carries its own source Guid, source hash and importer
version. The index can therefore be reconstructed by scanning the artifact
directory. This is what stops hash-flat storage from turning `artifacts.index`
into a single point of failure — losing it costs a slow startup, not a recook.

### 5. Atomic writes

Write `<hash>.tmp`, flush, `rename()` into place. A killed or crashed cook must
never leave a truncated file sitting at a name that claims to be a valid hash.
Cheapest robustness win available; non-negotiable.

### 6. Deterministic output

Same input, same bytes. No timestamps in artifacts, no unordered-container
iteration during serialization, pinned compressor settings. Buys reproducible CI
and makes "did this artifact actually change?" answerable.

### 7. Failure is memoized, reported, non-fatal in the editor

A failed cook writes **no artifact**, reports once through the existing
diagnostics seam, and does not retry-storm — `Assets` already memoizes decode
failures and is the precedent. Red entry in the editor; fatal at runtime load.

### 8. Runtime refuses; editor cooks

The runtime opens `Intermediate/` only. A missing artifact, or one whose
importer version predates the engine's, is a **loud refusal** — the same
philosophy as the plugin ABI gate, which refuses a cross-build mismatch rather
than limping.

**Consequence:** a fresh clone cannot run until it cooks. Normal (UE and Unity
both require it), but setup and CI must include a cook step.

### 9. `arccook.exe` is a separate binary over a shared library

The editor and the CLI must run the *same* importer code — shelling out
per-asset would mean a process spawn per texture. And the importer must not live
in `ArcaneClient`, because keeping interchange parsers out of the runtime is the
whole point.

```
ArcaneAssetPipeline (static lib)   importers + artifact format
        |-- ArcaneEditor           in-process cook on the JobSystem
        \-- arccook.exe            thin CLI over the same lib
ArcaneClient                       reads artifacts only; never links the pipeline
```

## The artifact format (texture, first slice)

```
TextureArtifact = { header, mip descriptors, BC payload, thumbnail RGBA }
```

The **thumbnail is load-bearing, not a nicety.** Once textures are BC blobs,
`Assets::PixelsFor(Guid) -> const PixelData*` — today *the* image route — no
longer serves the editor, which needs RGBA for inspector previews and the asset
browser. Three options were considered:

- editor decodes the source directly — puts the editor back on the source tree,
  defeating the split
- editor decompresses BC for previews — wasteful and lossy for a preview
- **artifact carries a small uncompressed thumbnail mip** — chosen; it is what
  Unity does, and it makes the asset browser fast rather than merely correct

So `PixelsFor` survives as the editor-preview route, served from the artifact's
thumbnail, and the renderer gets a separate artifact route for the BC payload
and mip table.

## Editor ergonomics — the behaviour to hold to

1. Drop a `.png` into `Content/` → it appears in the browser immediately, cooks
   on a `JobSystem` worker, thumbnail resolves when ready. **The UI never blocks
   on a cook.**
2. Change import settings in the inspector → that one asset recooks and
   live-reloads in the viewport.
3. Cook errors land in the Problems pane, clickable to the asset.
4. **There is never a "Reimport All" button you are required to press.** If one
   becomes necessary, the hash key is wrong.
5. Deleting an asset leaves an inert orphan artifact; a sweep at project open
   reconciles the index. Hash-addressing is what makes orphans inert rather than
   wrong.

## Scope — the first slice

| In | Out, and why |
|---|---|
| Textures only: decode → resize → mips → BC → artifact | **Meshes** — needs the mesh arc + cgltf |
| `.meta` gains typed texture import settings | **Compiling `.arc*` JSON** — YAGNI; Source 2/UE do it for shipping scale we do not have |
| Hash-keyed artifact store + rebuildable index | **Shared/team DDC** — solo dev |
| Editor background cook + diagnostics reporting | **Archives/paks** — shipping concern |
| `arccook.exe`, CI-gated | **Platform variants** — Windows only; BC is universal on D3D12 + VK |
| Runtime artifact route; refuses stale | **Cook-on-demand at runtime** — never |

## Vendoring this arc implies

Nothing for the first slice beyond a BC encoder. Full list, with the reasoning
in the session record:

| Library | License | Arrives with |
|---|---|---|
| **bc7enc_rdo** + hand-rolled DDS parsing | MIT | this arc (BC1/5/6H/7; BC6H is what makes T3's cubemap array viable) |
| **MikkTSpace** | permissive, 2 files | T1 — correctness, not convenience: every DCC bakes normal maps against it |
| **meshoptimizer** | MIT | mesh arc — cache/overdraw/fetch optimisation, LOD simplification, meshlet building for the `NRIMeshShader.h` path |
| **cgltf** | MIT | mesh arc — and it lives in `ArcaneAssetPipeline`, never in `ArcaneClient` |

Rejected: **assimp** (deeply nested build system — the house rule is to vendor
drop-ins and reserve vcpkg for exactly that shape; its main draw is FBX, which
is proprietary and unwanted), **FBX SDK** (licensing), **basisu/KTX2** (its value
is transcoding for platform diversity we do not have; revisit only if the
WASM/web target stops being "eventual, not soon").

Every vendored library owes the standing obligations: a `LICENSE` in its subdir,
a row in `NOTICE.md`, and a row in the `ThirdParty/README.md` inventory with a
pinned version and upstream URL.

**Pre-existing gap, unrelated but adjacent:** `AgilitySDK` is the only non-OSS
dependency in the tree (Microsoft SW License Terms, redistribution permitted)
and has **no `NOTICE.md` row**. The Phase 5a record flagged it; NRI got its row
since, AgilitySDK did not.

## Sequencing

**Phase 4 (procedural geometry, no assets) → this arc, texture-only → T1 (needs
BC + MikkTSpace) → mesh arc (needs cgltf + meshoptimizer).**

Phase 4 deliberately touches none of this — it uses procedurally generated
geometry precisely so the renderer slice does not drag the asset pipeline in
behind it.
