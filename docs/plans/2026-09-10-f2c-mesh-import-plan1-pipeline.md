# F2c Mesh Import — Plan 1: Source → Artifact → Resolved MeshData

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a dropped `.gltf`/`.glb` become a cooked mesh artifact and resolve back into a `MeshData` with sections and named material slots — end to end, headless. No pixels in this plan: Plan 2 owns residency, draws, thumbnails and the golden lanes.

**Architecture:** Bottom-up, each layer landing green before the next stands on it: vendor → fixtures → artifact bytes (both readers) → cook key → importer (refusals first, geometry second) → the generalized cook spine → registry recognition → the `.arcmesh` schema move → CPU resolution + the ABI bump → the reference-graph edges → the editor's import wave (texture extraction, companion mint, material minting). The two invasive moves — the cook spine's kind dispatch (§5.1) and the material scalar growing into `slots[]` (§4.4) — each land in ONE task, with the texture suites as the regression net for the first and the tolerant legacy-key mapping as the compatibility net for the second.

**Tech Stack:** C++23, cgltf (parse), meshoptimizer (remap/optimize), Catch2, msbuild `Arcane.slnx`, `arccook`.

**Spec:** `docs/specs/2026-09-10-f2c-mesh-import-design.md` (rulings R1–R6 in its §3; comparison amendments A1–A5 already folded in). Research: `docs/research/2026-09-10-f2c-mesh-import-research.md` (seam anchors, verified @ `35cf5a91`) and `docs/research/2026-09-10-f2c-ue-source2-comparison.md`.

## Global Constraints

- **Repo:** `D:\dev\starworks\Arcane`, branch `main`. Baseline `16f56445` (the spec + comparison commits). **`out.txt` at the repo root is the user's — never stage it.**
- **Commit per task, do NOT push.** The user's desk pass at the end of Plan 2 is the gate.
- **The ABI bumps ONCE, in Task 11 (23 → 24), tail-append only.** No other task in either plan touches `kGamePluginABIVersion`, and Plan 2 bumps nothing. New `Assets` virtuals go at the END of the interface — the v21/v22 precedent (`ArtifactFor`, `InvalidateArtifact`, `SetCookPendingProbe`, `MaterialSurfaceFor`, `ListAssetReferences`) that `Assets.hpp` states as a rule. Restamp `ReferenceProject.arcproj`; Gacha's Game-DLL rebuild debt deepens by one — **recorded in the ledger entry, not nagged about.**
- **Build:** `msbuild Arcane.slnx /p:Configuration=Debug /m` from the repo root (vswhere locates msbuild). Run **`GenerateProjects.bat`** after ANY premake or file-list change (`ARCANE_SDK` is already set).
- **Premake reality, both halves:**
  - `ArcaneTests` **globs its own** `src/**.cpp` — a new test file needs no premake edit — but compiles EDITOR TUs from an **EXPLICIT list** (root `premake5.lua`, the `ArcaneEditor/src/...` entries under the `ArcaneTests` `files` block). A new editor `.cpp` a test drives MUST be added there or it will not link. `ArcaneEditor`'s own project block globs `src/**`, so the same file needs no entry there.
  - A new **compiled** ThirdParty lib needs its own `ThirdParty/<name>/premake5.lua` + an `IncludeDir` row + a workspace `include` line + a `links` entry in every consumer — `bc7enc_rdo` is the model, verbatim (root `premake5.lua:90`, `:103`, `:200`, `:271`).
  - **`ArcaneAssetPipeline` consumes cgltf/meshoptimizer; `ArcaneClient` NEVER does** (the 08-21 placement rule; `ArcaneClient`'s includedirs carry no AssetPipeline path and must not grow one).
  - `ArcaneTests/data/` is staged wholesale into the exe dir's `data/` by an existing `{COPYDIR}` — **new fixture subdirectories need no premake edit.**
- **Run tests FROM the exe dir:** `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "~[gpu]"`. Order is randomized — **capture the seed banner** into any report that cites a run.
- **Baseline at arc start: 55464 assertions / 1535 cases** (`~[gpu]`, Debug). Every task states its own delta and attributes it to named cases. **Derive counts from the run, never recall them.**
- **Every task ends green:** the whole tree compiles and the full `~[gpu]` suite passes. A task that cannot end green must be split, not merged.
- **Anchors drift.** Every `file:line` below is orientation against `16f56445` / the research doc. **Re-locate by SYMBOL name before editing.**
- **Byte-explicit, always.** Anything that lands on disk is written one byte at a time, little-endian, never a struct memcpy and never `std::hash` — `ArtifactFormat.hpp:10-13` and `CookKey.hpp:3-10` state why, and both new formats inherit it.
- Commit message trailer, exactly:
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```

---

### Task 1: Vendor cgltf + meshoptimizer (spec §10)

Both libraries land under the bc7enc_rdo pattern, both consumed by **ArcaneAssetPipeline only**. cgltf is header-only (stb pattern: header vendored, ONE implementation TU inside the pipeline); meshoptimizer is a premake StaticLib.

**Files:**
- Create: `ThirdParty/cgltf/cgltf.h`, `ThirdParty/cgltf/LICENSE`
- Create: `ThirdParty/meshoptimizer/src/**` (curated), `ThirdParty/meshoptimizer/LICENSE.md`, `ThirdParty/meshoptimizer/premake5.lua`
- Create: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/CgltfImpl.cpp`
- Modify: `premake5.lua` (`IncludeDir` rows, workspace `include`, `ArcaneAssetPipeline` includedirs, `links` on `arccook` / `ArcaneEditor` / `ArcaneTests`), `NOTICE.md`, `ThirdParty/README.md`
- Test: `ArcaneTests/src/VendorSmokeTest.cpp`

**Interfaces:**
- Produces: `#include <cgltf.h>` and `#include <meshoptimizer.h>` resolvable from `ArcaneAssetPipeline` TUs and from `ArcaneTests`; a `meshoptimizer` static-lib project in the workspace.
- **PINS (record them in `ThirdParty/README.md`, by COMMIT, never a bare tag):** cgltf `1.15` **plus the CVE-2026-32845 patch** from `jkuhlmann/cgltf#293` (integer overflow in `cgltf_validate`'s sparse-accessor path — the reason `cgltf_validate` is mandatory in §4.5); meshoptimizer tag `v1.2`. Resolve both to full 40-char SHAs at vendoring time and write the short SHA in the version column, the full one in parentheses — the bc7enc_rdo row's own form (`ThirdParty/README.md:23`).

- [ ] **Step 1: Write the failing vendor smoke cases**

Append to `VendorSmokeTest.cpp` (declarations only for cgltf — the implementation TU lives in ArcaneAssetPipeline, exactly the `stb` precedent this file already documents at its own stb block):

```cpp
// ---------------------------------------------------------------- cgltf
// F2c Task 1: cgltf's IMPLEMENTATION TU lives at ArcaneAssetPipeline/src/Arcane/
// AssetPipeline/CgltfImpl.cpp (MeshImporter.cpp needs cgltf_parse/cgltf_validate),
// and that static lib is linked into this exe -- defining CGLTF_IMPLEMENTATION here
// too would merge a SECOND copy of every cgltf symbol and fail to link (LNK2005).
// Declarations only, same rule as the stb block above.
#include <cgltf.h>

TEST_CASE("cgltf: a minimal glTF parses, validates, and reports its meshes",
          "[vendor][cgltf]")
{
    // The smallest legal glTF 2.0 document: asset block + one empty scene.
    static constexpr char kJson[] =
        R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[]}],"scene":0})";

    cgltf_options options{};
    cgltf_data* data = nullptr;
    REQUIRE(cgltf_parse(&options, kJson, sizeof(kJson) - 1, &data) == cgltf_result_success);
    REQUIRE(data != nullptr);
    // MANDATORY on every import (spec s4.5, and the CVE the pin exists for) --
    // proven callable here so a vendoring slip that drops it is caught at arrival.
    CHECK(cgltf_validate(data) == cgltf_result_success);
    CHECK(data->meshes_count == 0);
    CHECK(data->scenes_count == 1);
    cgltf_free(data);
}

TEST_CASE("cgltf: a truncated document is refused, not crashed", "[vendor][cgltf]")
{
    static constexpr char kTruncated[] = R"({"asset":{"vers)";
    cgltf_options options{};
    cgltf_data* data = nullptr;
    CHECK(cgltf_parse(&options, kTruncated, sizeof(kTruncated) - 1, &data)
          != cgltf_result_success);
    if (data) cgltf_free(data);
}

// -------------------------------------------------------- meshoptimizer
#include <meshoptimizer.h>

TEST_CASE("meshoptimizer: remap deduplicates identical vertices", "[vendor][meshopt]")
{
    struct V { float x, y, z; };
    // Six positions, two of them exact duplicates of earlier ones.
    const V vertices[6] = {
        {0,0,0}, {1,0,0}, {0,1,0},
        {0,0,0}, {1,0,0}, {1,1,0},
    };
    const unsigned int indices[6] = { 0, 1, 2, 3, 4, 5 };

    std::vector<unsigned int> remap(6);
    const size_t unique = meshopt_generateVertexRemap(
        remap.data(), indices, 6, vertices, 6, sizeof(V));
    CHECK(unique == 4);   // {0,0,0}, {1,0,0}, {0,1,0}, {1,1,0}
}

TEST_CASE("meshoptimizer: vertex-cache optimization preserves the triangle set",
          "[vendor][meshopt]")
{
    unsigned int indices[6] = { 0, 1, 2, 2, 1, 3 };
    unsigned int optimized[6] = {};
    meshopt_optimizeVertexCache(optimized, indices, 6, 4);
    // Same triangles, possibly reordered -- the multiset of index values is invariant.
    std::vector<unsigned int> before(std::begin(indices), std::end(indices));
    std::vector<unsigned int> after(std::begin(optimized), std::end(optimized));
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    CHECK(before == after);
}
```

Add `#include <algorithm>` and `#include <vector>` to the file's include block if absent.

- [ ] **Step 2: Run — expect FAIL** (`cgltf.h` / `meshoptimizer.h` not found). `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[vendor]"` — a compile failure is the red step for a new dependency.

- [ ] **Step 3: Vendor cgltf.** Fetch `cgltf.h` at the pinned commit (1.15 + PR #293's sparse-accessor overflow fix — verify the fix is present by grepping the vendored header for the bounds check inside `cgltf_calc_index_bound`; if the PR is still unmerged, apply it and record the patch in a `ThirdParty/cgltf/PATCHES.md` naming the PR and the hunk). Drop the upstream `LICENSE` beside it. **Do NOT vendor `cgltf_write.h`** — nothing in Arcane writes glTF.

- [ ] **Step 4: Vendor meshoptimizer.** Copy the upstream `src/` directory wholesale plus `LICENSE.md`. **Deliberate divergence from bc7enc_rdo's file-level curation, and state it in the premake header comment:** meshoptimizer's `src/` is ~20 small, self-contained TUs with no external dependencies and no build-time configuration, so a file-level subset buys nothing and risks a link error the day a dormant entry point (simplification, `EXT_meshopt_compression` decode, tangent generation — all three named as vendored-but-dormant in spec §2) is switched on. What IS excluded is the tooling that surrounds it: `gltf/` (the gltfpack CLI), `js/`, `demo/`, `tools/`, `tests/`.

- [ ] **Step 5: Write `ThirdParty/meshoptimizer/premake5.lua`** — a byte-for-byte structural copy of `ThirdParty/bc7enc_rdo/premake5.lua` with `project "meshoptimizer"`, `files { "src/*.cpp", "src/*.h" }`, `includedirs { "src" }`, and a header comment carrying: the pinned tag/commit, the "whole src/, tooling excluded" rationale from Step 4, and the list of dormant-but-vendored capabilities (simplification per §2's LOD trigger, meshopt compression decode per §2's `EXT_meshopt_compression` trigger, tangent generation per §2/A3's tangent trigger).

- [ ] **Step 6: Write `CgltfImpl.cpp`** — the stb-pattern implementation TU, mirroring `ArcaneAssetPipeline/src/Arcane/AssetPipeline/StbImpl.cpp`:

```cpp
// The ONE cgltf implementation TU in the whole tree (F2c Task 1) -- the same
// single-TU discipline StbImpl.cpp keeps beside it, and for the same reason: cgltf
// is a single-header library whose implementation is emitted by a macro, so a second
// definer anywhere would merge a second copy of every symbol (LNK2005). Every other
// consumer -- MeshImporter.cpp here, VendorSmokeTest.cpp in ArcaneTests -- includes
// <cgltf.h> for DECLARATIONS ONLY and links against this TU's symbols.
//
// CGLTF_IMPLEMENTATION is defined here and NOWHERE ELSE. Grep-verifiable:
//   grep -rn "CGLTF_IMPLEMENTATION" --include=*.cpp --include=*.hpp .
// must return exactly this file.

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
```

- [ ] **Step 7: Wire premake.** In root `premake5.lua`: add `IncludeDir["cgltf"] = "%{wks.location}/ThirdParty/cgltf"` and `IncludeDir["meshoptimizer"] = "%{wks.location}/ThirdParty/meshoptimizer/src"` beside the `bc7enc_rdo` row (`:90`); add `include "ThirdParty/meshoptimizer"` beside `include "ThirdParty/bc7enc_rdo"` (`:103`); add both `IncludeDir` entries to `ArcaneAssetPipeline`'s `includedirs` with a task-naming comment (beside the `bc7enc_rdo` line at `:200`); add `"meshoptimizer"` to the `links` list of every project that links `ArcaneAssetPipeline` — `arccook` (`:271`), `ArcaneEditor`, `ArcaneTests` — carrying forward the existing comment's reasoning ("a static lib doesn't transitively pull its own links"); add `IncludeDir.cgltf` and `IncludeDir.meshoptimizer` to `ArcaneTests`' includedirs so the smoke test compiles. **`ArcaneClient` gets neither** — verify with `grep -n "cgltf\|meshoptimizer" premake5.lua` that no line inside the `ArcaneClient` project block matches.

- [ ] **Step 8: `GenerateProjects.bat`, full rebuild, run `[vendor]` — expect PASS** (4 new cases).

- [ ] **Step 9: NOTICE.md + ThirdParty/README.md rows.** `NOTICE.md`: two rows in the alphabetical table — `| cgltf | \`ThirdParty/cgltf\` | MIT (\`LICENSE\`) |` and `| meshoptimizer | \`ThirdParty/meshoptimizer\` | MIT (\`LICENSE.md\`) |`. `ThirdParty/README.md`: two rows in the same column shape the `bc7enc_rdo` row uses (`:23`) — name, pinned short SHA with the full SHA in parentheses, licence, consumer (`Arcane`), a purpose sentence naming F2c and the vendored/excluded slice, upstream URL. **cgltf's purpose cell must name the CVE and the patch** ("1.15 + the CVE-2026-32845 sparse-accessor overflow fix from jkuhlmann/cgltf#293, pinned by commit — `cgltf_validate` is mandatory on every import, spec §4.5"), because that is the whole reason the pin is a commit and not a tag.

- [ ] **Step 10: Commit** — `chore(thirdparty): vendor cgltf 1.15+CVE patch and meshoptimizer v1.2 for the mesh cook`

---

### Task 2: The glTF fixture corpus (spec §9)

Nine fixtures, checked in, all produced by **one checked-in generator** so the corpus has a single readable source of truth and is reproducible byte-for-byte. Hand-authored binaries are rejected: a `.glb` is a binary container and a hand-typed base64 buffer is neither diffable nor auditable, while a generator's number tables are both.

**Files:**
- Create: `scripts/make-mesh-fixtures.ps1`
- Create (generated, checked in): `ArcaneTests/data/gltf/{single.glb, multi.glb, nested.gltf, nested.bin, mirrored.glb, embedded_tex.glb, degenerate.glb, empty.glb, bad_sparse.glb, requires_draco.gltf}`
- Test: `ArcaneTests/src/MeshFixtureCorpusTest.cpp` (new; globbed)

**Interfaces:**
- Produces: the corpus at the exe-relative path `data/gltf/` (the existing `{COPYDIR} ArcaneTests/data -> data` postbuild stages it; **no premake change**).
- The generator is the SOURCE OF TRUTH; the committed binaries are its output. Re-running it must leave the working tree clean.

**The corpus, and which spec clause each fixture exists for:**

| Fixture | Shape | Pins |
|---|---|---|
| `single.glb` | 1 mesh, 1 primitive (a unit quad, 4 verts / 2 tris), material `"SingleMat"` | the happy path; §5.3 determinism |
| `multi.glb` | 1 mesh, 3 primitives; materials `"Metal"`, `"Metal"`, `"Paint"` | **A1's dedup pin — 3 sections, 2 slots, sections 0 and 1 share `slotIndex`** |
| `nested.gltf` + `nested.bin` | 2-level node hierarchy, parent TRS × child TRS, buffer via `"uri":"nested.bin"` | §4.3 bake; **§5.4's external-buffer hashing** |
| `mirrored.glb` | one node, `"scale":[-1,1,1]` | §4.3 winding flip (and, per A3, the future handedness assertion) |
| `embedded_tex.glb` | 1 primitive, `baseColorTexture` → an embedded 2×2 PNG in the BIN chunk, image `"name":"albedo"` | §5.5 extraction; §6 material minting |
| `degenerate.glb` | 3 triangles: 1 good, 2 with a repeated corner index | **A2 part 2 — warn and DROP, do not refuse** |
| `empty.glb` | valid glTF, `"meshes":[]` | §4.5 nothing-drawable → refuse loudly |
| `bad_sparse.glb` | an accessor whose `sparse.count` exceeds the accessor's own `count`, with sparse index values past the buffer view | §4.5 mandatory `cgltf_validate`; the CVE-hardened path. **A truncated/overflowing bound, NOT a working exploit** (spec §9 says so explicitly) |
| `requires_draco.gltf` | `"extensionsRequired":["KHR_draco_mesh_compression"]`, otherwise a valid single quad | **A2 part 1 — the general `extensionsRequired` refusal** |

- [ ] **Step 1: Write the failing corpus test** (`MeshFixtureCorpusTest.cpp`):

```cpp
// F2c Task 2: the glTF fixture corpus's ARRIVAL GATE. It asserts the files exist,
// are staged beside the exe, and carry the structural marks each later task depends
// on -- deliberately WITHOUT parsing them through cgltf, so a corpus regression is
// distinguishable from an importer regression. The corpus's own source of truth is
// scripts/make-mesh-fixtures.ps1; re-running it must leave the tree clean, which is
// the property Step 5 checks at authoring time and this file cannot.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // Tests run FROM the exe dir, so the staged corpus is exe-relative.
    fs::path FixtureDir() { return fs::path("data") / "gltf"; }

    std::vector<std::uint8_t> ReadAll(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    }
}

TEST_CASE("gltf corpus: every fixture spec s9 names is staged beside the exe",
          "[fixture][gltf]")
{
    for (const char* name : { "single.glb", "multi.glb", "nested.gltf", "nested.bin",
                              "mirrored.glb", "embedded_tex.glb", "degenerate.glb",
                              "empty.glb", "bad_sparse.glb", "requires_draco.gltf" })
    {
        INFO(name);
        CHECK(fs::exists(FixtureDir() / name));
    }
}

TEST_CASE("gltf corpus: every .glb carries a well-formed GLB container header",
          "[fixture][gltf]")
{
    for (const char* name : { "single.glb", "multi.glb", "mirrored.glb",
                              "embedded_tex.glb", "degenerate.glb", "empty.glb",
                              "bad_sparse.glb" })
    {
        INFO(name);
        const std::vector<std::uint8_t> bytes = ReadAll(FixtureDir() / name);
        REQUIRE(bytes.size() >= 20);
        // magic "glTF", version 2, then a total length that matches the file.
        CHECK(bytes[0] == 'g'); CHECK(bytes[1] == 'l');
        CHECK(bytes[2] == 'T'); CHECK(bytes[3] == 'F');
        const auto u32 = [&](std::size_t at) {
            return static_cast<std::uint32_t>(bytes[at])
                 | (static_cast<std::uint32_t>(bytes[at + 1]) << 8)
                 | (static_cast<std::uint32_t>(bytes[at + 2]) << 16)
                 | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
        };
        CHECK(u32(4) == 2u);
        CHECK(u32(8) == static_cast<std::uint32_t>(bytes.size()));
    }
}

TEST_CASE("gltf corpus: the structural marks each later task depends on",
          "[fixture][gltf]")
{
    const auto text = [](const fs::path& p) {
        const std::vector<std::uint8_t> b = ReadAll(p);
        return std::string(b.begin(), b.end());
    };

    // A1's dedup pin: TWO primitives name the SAME material, a third names another.
    const std::string multi = text(FixtureDir() / "multi.glb");
    CHECK(multi.find("Metal") != std::string::npos);
    CHECK(multi.find("Paint") != std::string::npos);

    // The external-buffer pair (s5.4): the .gltf references the .bin BY URI, so
    // editing the .bin must move the cook key.
    const std::string nested = text(FixtureDir() / "nested.gltf");
    CHECK(nested.find("nested.bin") != std::string::npos);

    // A2 part 1: the refusal rule reads extensionsRequired, so the fixture must
    // actually carry one.
    const std::string draco = text(FixtureDir() / "requires_draco.gltf");
    CHECK(draco.find("extensionsRequired") != std::string::npos);
    CHECK(draco.find("KHR_draco_mesh_compression") != std::string::npos);

    // s4.3's winding-flip fixture needs a genuinely negative determinant.
    const std::string mirrored = text(FixtureDir() / "mirrored.glb");
    CHECK(mirrored.find("-1") != std::string::npos);
}
```

- [ ] **Step 2: Run — expect FAIL** (no `data/gltf`). `./ArcaneTests.exe "[fixture]"`.

- [ ] **Step 3: Write `scripts/make-mesh-fixtures.ps1`.** Structure, and the contract its header comment must state:
  - **One function per fixture**, each returning the glTF JSON object (a PowerShell hashtable) plus the raw BIN payload it needs. Every vertex/index/matrix number is a **literal in this script** — nothing is base64-typed by hand, and nothing is fetched.
  - **`Write-Glb`** packs `{JSON chunk, BIN chunk}` into the GLB container: `"glTF"` magic, `version = 2` (u32 LE), `totalLength` (u32 LE), then per chunk `chunkLength` (u32 LE) + `chunkType` (`0x4E4F534A` `JSON`, `0x004E4942` `BIN`) + payload, **each chunk padded to a 4-byte boundary** — JSON with spaces (`0x20`), BIN with zeros (`0x00`). Padding bytes are part of `chunkLength`, per the GLB spec.
  - **`Write-Gltf`** writes the JSON with `ConvertTo-Json -Depth 12 -Compress` and **`Out-File -Encoding utf8NoBOM`** with **LF line endings** — determinism depends on this; a BOM or CRLF makes the committed bytes host-dependent.
  - **Determinism clause, stated in the header:** the script takes no input, reads no clock, and emits no randomness, so two runs on two machines produce byte-identical files. Step 5 is the proof.
  - `bad_sparse.glb` is built by emitting a structurally valid accessor and then **editing its `sparse.count` upward in the JSON hashtable before serialization** — a bound that overflows, which is what `cgltf_validate` must catch. The header must say, in the same words spec §9 uses, that this is *a truncated/overflowing index bound, not a working exploit*.
  - `-OutDir` defaults to `ArcaneTests/data/gltf` resolved relative to the script; `-Force` overwrites.

- [ ] **Step 4: Run the generator** — `powershell -ExecutionPolicy Bypass -File scripts\make-mesh-fixtures.ps1 -Force` — then `./ArcaneTests.exe "[fixture]"` — **expect PASS** (3 new cases).

- [ ] **Step 5: Prove reproducibility.** `git add ArcaneTests/data/gltf && git status --porcelain ArcaneTests/data/gltf` → staged-new only. Then re-run the generator and `git status --porcelain ArcaneTests/data/gltf` again → **byte-identical, nothing further modified**. If anything differs, the generator has a nondeterminism (encoding, ordering, or padding) that must be fixed here, not tolerated — the whole corpus's value as a determinism fixture (§9's byte-identity test) rests on it.

- [ ] **Step 6: Sanity-parse the corpus through the vendored library.** Add one more case to `MeshFixtureCorpusTest.cpp`? **No** — deliberately not: this file is the corpus's own gate and stays parser-free (its header says why). Instead, run a throwaway check at the desk: a scratch program (or a temporary `[vendor][cgltf]` case, deleted before commit) that `cgltf_parse_file`s each of the seven fixtures that are *meant* to parse (`single`, `multi`, `nested`, `mirrored`, `embedded_tex`, `degenerate`, `requires_draco`) and confirms `cgltf_result_success`, and that `empty.glb` parses but yields `meshes_count == 0`, and that `bad_sparse.glb` parses but **fails `cgltf_validate`**. Record the eight verdicts in the commit body. Task 6 turns these into permanent assertions through the real importer.

- [ ] **Step 7: Full `~[gpu]` suite — green.** Delta vs. baseline: **+3 cases** (`[fixture][gltf]`), assertion delta as the run reports it. Commit — `test(pipeline): checked-in glTF fixture corpus with its generator script`

---

### Task 3: The mesh artifact — writer + pipeline reader (spec §5.2)

`ContentKind::Mesh = 2` and its own writer/reader pair, in the SAME `ArtifactFormat.hpp/.cpp` as the texture pair so both share the byte-explicit `ByteWriter`/`ByteReader` primitives with zero code motion. The two pairs stay independent functions — "a mesh artifact gets its own writer/reader pair" (§5.2) is about the FUNCTIONS, not the file.

**Files:**
- Modify: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactFormat.hpp` (`ContentKind`, `SectionTag`, the mesh desc types, the two new functions), `ArtifactFormat.cpp` (`F32` on both byte classes; `WriteMeshArtifact`/`ReadMeshArtifact`)
- Test: `ArcaneTests/src/MeshArtifactFormatTest.cpp` (new; globbed)

**Interfaces:**

```cpp
    // ArtifactFormat.hpp -- ContentKind grows its second value; the comment that
    // reserved it ("F2c's mesh artifacts discriminate on this same header field")
    // is now FULFILLED and should say so rather than still predicting it.
    enum class ContentKind : std::uint8_t
    {
        Texture = 1,
        Mesh    = 2,
    };

    // SectionTag values are stable on disk -- never renumber. 1-3 are the texture
    // kind's; 4-7 are the mesh kind's. The two kinds share ONE tag space on purpose:
    // the container's skip-unknown rule (ReadTextureArtifact's default: case) is what
    // keeps a reader of one kind safe in the presence of the other's tags, and one
    // space makes that property trivially auditable.
    enum class SectionTag : std::uint32_t
    {
        MipTable     = 1,
        Payload      = 2,
        Thumbnail    = 3,   // MESH ALSO: reserved, UNWRITTEN (R5 harvests editor-side)
        VertexData   = 4,
        IndexData    = 5,
        SectionTable = 6,   // the MESH's per-section records -- NOT the container's
                            // own section table, which is a fixed structure with no tag
        Tangents     = 7,   // reserved, UNWRITTEN (spec s2; A3 records the handedness
                            // obligation this tag inherits when it is first written)
    };

    // One drawable range with its material slot. `slotIndex` is comparison A1: two
    // primitives sharing one glTF material become TWO sections pointing at ONE slot,
    // never two identically-named slots.
    struct MeshArtifactSection
    {
        std::string   name;          // the primitive's material name; empty when absent
        std::uint32_t indexOffset;   // into IndexData, in INDICES (not bytes)
        std::uint32_t indexCount;
        std::uint32_t slotIndex;
    };

    // 32 bytes, interleaved pos/normal/uv -- the pipeline's fixed vertex stride
    // (MeshNode.cpp's vertex input, :207-222). DELIBERATELY NOT Arcane::MeshVertex:
    // that type lives in ArcaneClient (Render/MeshBuilder.hpp) and this library must
    // never include ArcaneClient. The two are hand-mirrored, and the mirror is pinned
    // by MeshArtifactReaderTest.cpp's cross-lib round-trip plus the static_assert on
    // sizeof below -- exactly the discipline ArtifactReader.hpp's own enum mirror keeps.
    struct MeshArtifactVertex
    {
        float px, py, pz;
        float nx, ny, nz;
        float u, v;
    };
    static_assert(sizeof(MeshArtifactVertex) == 32,
                  "the artifact's vertex stride is the pipeline's binding stride");

    // Header, serialized little-endian in DECLARATION ORDER (WriteMeshArtifact's body
    // mirrors this field for field). The first four fields are the KIND-AGNOSTIC
    // COMMON PREFIX every artifact kind shares -- see ArtifactStore::RebuildIndexFromScan
    // and ArtifactReader's ParseCommonPrefix, both of which read only this much.
    struct MeshArtifactDesc
    {
        ContentKind   contentKind = ContentKind::Mesh;
        Guid          sourceGuid;
        std::uint64_t sourceHash;
        std::uint32_t importerVersion;

        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t sectionCount = 0;
        // Comparison A5: DECLARED, not inferred. 4 in v1, asserted == 4 on read, so a
        // future 16-bit path is a reader BRANCH rather than a tag-presence dance
        // duplicated across the two deliberately-independent readers.
        std::uint8_t  indexWidth = 4;

        // The cooked AABB (s7.1): taken from the artifact rather than recomputed on
        // load. ComputeMeshBounds stays the path for GENERATED primitives.
        float aabbMin[3]{ 0.0f, 0.0f, 0.0f };
        float aabbMax[3]{ 0.0f, 0.0f, 0.0f };

        std::vector<MeshArtifactSection> sections;
    };

    // Writes a complete .arcart mesh artifact: the header above, the container's
    // section table, then VertexData / IndexData / SectionTable. Tangents and
    // Thumbnail are NOT written (spec s5.2: reserved) -- a later arc appends them
    // additively under skip-unknown. False on any IO failure.
    [[nodiscard]] bool WriteMeshArtifact(const std::filesystem::path& path,
                                          const MeshArtifactDesc& desc,
                                          std::span<const MeshArtifactVertex> vertices,
                                          std::span<const std::uint32_t> indices);

    struct LoadedMeshArtifact
    {
        MeshArtifactDesc desc;
        std::vector<MeshArtifactVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    // Reads and validates a mesh .arcart. nullopt on bad magic, an unrecognised
    // artifactVersion, a contentKind that is not Mesh, indexWidth != 4, a truncated
    // section, or a section whose declared counts disagree with its body's size. An
    // unrecognised SectionTag is SKIPPED, not an error.
    [[nodiscard]] std::optional<LoadedMeshArtifact> ReadMeshArtifact(
        const std::filesystem::path& path);

    // Slot names, derived from the section table: slot k's name is the name of any
    // section pointing at k (they agree by construction -- the slot array dedups BY
    // NAME, A1). The artifact stores no separate slot list because it needs none;
    // this is the function the editor's companion mint reads authoritative names
    // through (spec s4.2). Sized to max(slotIndex)+1, empty when there are no sections.
    [[nodiscard]] std::vector<std::string> SlotNamesFromSections(
        std::span<const MeshArtifactSection> sections);
```

- [ ] **Step 1: Write the failing format test** (`MeshArtifactFormatTest.cpp`, modeled on `AssetPipelineFormatTest.cpp` — same `TempDir` helper shape, same `[pipeline]` tag, and the same discipline of hand-rolling the corrupt cases against the DOCUMENTED layout rather than reusing the writer):

```cpp
TEST_CASE("mesh artifact: every header field round-trips byte-exactly", "[pipeline]")
{
    const fs::path dir = TempDir("mesh_roundtrip");
    MeshArtifactDesc desc{};
    desc.contentKind = ContentKind::Mesh;
    desc.sourceGuid = Guid::Generate();
    desc.sourceHash = 0x0123456789ABCDEFULL;
    desc.importerVersion = 3;
    desc.vertexCount = 4;
    desc.indexCount = 9;
    desc.sectionCount = 2;
    desc.indexWidth = 4;
    // Every component distinct, so a transposed pair fails the round-trip.
    desc.aabbMin[0] = -1.5f; desc.aabbMin[1] = -2.25f; desc.aabbMin[2] = -3.125f;
    desc.aabbMax[0] =  4.5f; desc.aabbMax[1] =  5.25f; desc.aabbMax[2] =  6.125f;
    desc.sections = {
        { "Metal", 0, 6, 0 },
        { "Metal", 6, 3, 0 },   // A1: two sections, ONE slot
    };

    const std::vector<MeshArtifactVertex> vertices = {
        { 0,0,0,  0,1,0,  0,0 }, { 1,0,0,  0,1,0,  1,0 },
        { 1,0,1,  0,1,0,  1,1 }, { 0,0,1,  0,1,0,  0,1 },
    };
    const std::vector<std::uint32_t> indices = { 0,1,2, 0,2,3, 1,2,3 };

    const fs::path path = dir / "mesh.arcart";
    REQUIRE(WriteMeshArtifact(path, desc, vertices, indices));

    const std::optional<LoadedMeshArtifact> loaded = ReadMeshArtifact(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->desc.contentKind == ContentKind::Mesh);
    CHECK(loaded->desc.sourceGuid == desc.sourceGuid);
    CHECK(loaded->desc.sourceHash == desc.sourceHash);
    CHECK(loaded->desc.importerVersion == 3u);
    CHECK(loaded->desc.vertexCount == 4u);
    CHECK(loaded->desc.indexCount == 9u);
    CHECK(loaded->desc.sectionCount == 2u);
    CHECK(loaded->desc.indexWidth == 4u);
    for (int i = 0; i < 3; ++i)
    {
        CHECK(loaded->desc.aabbMin[i] == desc.aabbMin[i]);
        CHECK(loaded->desc.aabbMax[i] == desc.aabbMax[i]);
    }
    REQUIRE(loaded->desc.sections.size() == 2u);
    CHECK(loaded->desc.sections[0].name == "Metal");
    CHECK(loaded->desc.sections[1].indexOffset == 6u);
    CHECK(loaded->desc.sections[1].indexCount == 3u);
    CHECK(loaded->desc.sections[0].slotIndex == loaded->desc.sections[1].slotIndex);
    REQUIRE(loaded->vertices.size() == 4u);
    CHECK(loaded->vertices[2].u == 1.0f);
    CHECK(loaded->vertices[2].v == 1.0f);
    CHECK(loaded->indices == indices);
}

TEST_CASE("mesh artifact: an empty section NAME round-trips as empty, not as absent",
          "[pipeline]")
{
    // The unnamed slot (a primitive with no material, s4.3) is a REAL slot whose
    // name is the empty string -- a writer that skipped a zero-length name, or a
    // reader that treated it as a truncated record, would lose the slot.
    const fs::path dir = TempDir("mesh_unnamed");
    MeshArtifactDesc desc{};
    desc.sourceGuid = Guid::Generate();
    desc.vertexCount = 3; desc.indexCount = 3; desc.sectionCount = 1;
    desc.sections = { { "", 0, 3, 0 } };
    const std::vector<MeshArtifactVertex> v(3);
    const std::vector<std::uint32_t> i = { 0, 1, 2 };
    REQUIRE(WriteMeshArtifact(dir / "m.arcart", desc, v, i));
    const auto loaded = ReadMeshArtifact(dir / "m.arcart");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->desc.sections.size() == 1u);
    CHECK(loaded->desc.sections[0].name.empty());
}

TEST_CASE("mesh artifact: the texture reader refuses it and vice versa", "[pipeline]")
{
    // The fail-closed contentKind check both readers already carry (ArtifactFormat.cpp's
    // own "spares F2c's mesh-artifact author a texture reader that happily misreads")
    // -- now that kind 2 exists, prove it BOTH ways rather than trusting the comment.
    const fs::path dir = TempDir("mesh_kind_gate");
    MeshArtifactDesc mesh{};
    mesh.sourceGuid = Guid::Generate();
    mesh.vertexCount = 3; mesh.indexCount = 3; mesh.sectionCount = 1;
    mesh.sections = { { "", 0, 3, 0 } };
    REQUIRE(WriteMeshArtifact(dir / "mesh.arcart", mesh,
                              std::vector<MeshArtifactVertex>(3),
                              std::vector<std::uint32_t>{ 0, 1, 2 }));
    CHECK_FALSE(ReadTextureArtifact(dir / "mesh.arcart").has_value());

    TextureArtifactDesc tex{};
    tex.contentKind = ContentKind::Texture;
    tex.sourceGuid = Guid::Generate();
    tex.format = ArtifactPixelFormat::RGBA8;
    tex.dimension = ArtifactDimension::Tex2D;
    tex.arrayOrDepth = 1; tex.width = 1; tex.height = 1; tex.mipCount = 1;
    tex.mips = { MipDesc{ 0, 4, 1, 1 } };
    tex.thumbWidth = 1; tex.thumbHeight = 1;
    const std::vector<std::byte> texel(4, std::byte{ 0xFF });
    REQUIRE(WriteTextureArtifact(dir / "tex.arcart", tex, texel, texel));
    CHECK_FALSE(ReadMeshArtifact(dir / "tex.arcart").has_value());
}

TEST_CASE("mesh artifact: a declared indexWidth other than 4 is refused in v1",
          "[pipeline]")
{
    // Comparison A5: the byte is DECLARED so a future 16-bit path is a branch. In v1
    // the only legal value is 4, and a file claiming otherwise is refused rather than
    // read as though its indices were u32 anyway.
    const fs::path dir = TempDir("mesh_indexwidth");
    MeshArtifactDesc desc{};
    desc.sourceGuid = Guid::Generate();
    desc.vertexCount = 3; desc.indexCount = 3; desc.sectionCount = 1;
    desc.indexWidth = 2;   // v1 writers never produce this; a hand-edited file might
    desc.sections = { { "", 0, 3, 0 } };
    REQUIRE(WriteMeshArtifact(dir / "m.arcart", desc,
                              std::vector<MeshArtifactVertex>(3),
                              std::vector<std::uint32_t>{ 0, 1, 2 }));
    CHECK_FALSE(ReadMeshArtifact(dir / "m.arcart").has_value());
}

TEST_CASE("mesh artifact: an unrecognised section tag is skipped, not fatal",
          "[pipeline]")
{
    // Forward-compat, hand-rolled against the DOCUMENTED layout rather than through
    // the writer -- append a section table entry with tag 999 pointing at a body the
    // reader has no case for, and require the mesh still loads. This is what keeps a
    // future Tangents/Thumbnail section additive.
    /* Executor: build the file with the same explicit little-endian byte emission
       AssetPipelineFormatTest.cpp's own wrong-magic/unknown-section cases use --
       header per MeshArtifactDesc's declaration order, then a 4-entry section table
       (VertexData, IndexData, SectionTable, 999), then the four bodies. */
    // ... assert ReadMeshArtifact succeeds and returns the 3 indices unchanged.
}

TEST_CASE("mesh artifact: SlotNamesFromSections derives one name per slot", "[pipeline]")
{
    const std::vector<MeshArtifactSection> sections = {
        { "Metal", 0, 6, 0 }, { "Paint", 6, 3, 1 }, { "Metal", 9, 3, 0 },
    };
    const std::vector<std::string> names = SlotNamesFromSections(sections);
    REQUIRE(names.size() == 2u);
    CHECK(names[0] == "Metal");
    CHECK(names[1] == "Paint");
    CHECK(SlotNamesFromSections({}).empty());
}
```

- [ ] **Step 2: Run — expect FAIL** (`WriteMeshArtifact` undeclared). `./ArcaneTests.exe "[pipeline]"`.

- [ ] **Step 3: Add `F32` to both byte classes** in `ArtifactFormat.cpp`'s anonymous namespace — the ONE new primitive both new functions need:

```cpp
            // Floats go through their EXACT bit pattern, never a numeric conversion:
            // std::bit_cast preserves every bit including a signed zero or a NaN
            // payload, which a float -> integer conversion would not. Same
            // byte-explicit, endianness-explicit discipline as U32 above -- an AABB
            // that round-trips approximately is an AABB that fails the mesh's own
            // framing math on the second load.
            void F32(float v) { U32(std::bit_cast<std::uint32_t>(v)); }
```
and on `ByteReader`:
```cpp
            [[nodiscard]] bool F32(float& out) noexcept
            {
                std::uint32_t bits = 0;
                if (!U32(bits)) return false;
                out = std::bit_cast<float>(bits);
                return true;
            }
```
Add `#include <bit>` and `#include <string>` to the TU.

- [ ] **Step 4: Implement `WriteMeshArtifact`.** Body order, mirroring `WriteTextureArtifact` exactly:
  1. Build the `SectionTable` body: `U32(sections.size())`, then per section `U16(name.size())` + the name's bytes + `U32(indexOffset)` + `U32(indexCount)` + `U32(slotIndex)`. (Add `void U16(std::uint16_t)` to `ByteWriter` beside `U8`/`U32`; the spec pins name as **u16 length + UTF-8**. A name longer than 65535 bytes is clamped with one comment saying so — glTF material names are identifiers, not documents.)
  2. Header: magic, `kArtifactVersion`, `contentKind`, guid hi/lo, `sourceHash`, `importerVersion`, `vertexCount`, `indexCount`, `sectionCount`, `indexWidth`, then six `F32`s (min.xyz then max.xyz).
  3. Three container sections in fixed order — `VertexData` (`vertices` reinterpreted as bytes, `vertexCount * 32`), `IndexData` (`indices` as bytes, `indexCount * 4`), `SectionTable` (the body from 1).
     **Vertices and indices are emitted through `ByteWriter::F32`/`U32` field by field, NOT a `std::span` memcpy of the structs** — `MeshArtifactVertex` is eight floats with no padding on every toolchain we build, but the artifact's whole determinism contract is "never trust a struct's memory layout", and honouring it here costs one loop.
  4. Running offsets + the file write: identical to the texture writer's tail.

- [ ] **Step 5: Implement `ReadMeshArtifact`.** Mirror `ReadTextureArtifact`'s shape: whole-file read, `ByteReader`, magic + version gates, `contentKind != Mesh` → nullopt, header fields, `indexWidth != 4` → nullopt, the container section-table read **with the same cheap sanity bound** (`sectionCount * kSectionEntrySize > raw.size()` → nullopt), then the per-tag switch:
  - `VertexData`: refuse unless `body.size() == vertexCount * 32`; decode `vertexCount` vertices field by field.
  - `IndexData`: refuse unless `body.size() == indexCount * 4`; decode.
  - `SectionTable`: read the count, **apply the same reserve-bound guard the mip table has** (minimum on-disk entry width is `2 + 4 + 4 + 4 = 14` bytes, so `count * 14 > body.size()` → nullopt, which is what stops a crafted count from throwing `bad_alloc` out of the read), then per entry read the u16 length, bounds-check it against the body's remainder, read the name, offsets, `slotIndex`.
  - `default:` skip — carrying the texture reader's own forward-compat comment.
  **One extra validation the texture reader has no analogue for:** after the switch, refuse when any section's `indexOffset + indexCount > indexCount` (the header's) or when `sectionCount` disagrees with `sections.size()`. A section pointing past the index buffer is a corrupt file, and letting it through would hand Plan 2's draw path an out-of-range `CmdDrawIndexed`.

- [ ] **Step 6: Implement `SlotNamesFromSections`** — one pass computing `max(slotIndex)+1`, a second filling each slot from the first section that names it.

- [ ] **Step 7: Run `[pipeline]` — expect PASS.** The existing texture cases must be **unchanged in count and outcome** — the `ContentKind`/`SectionTag` growth is additive and the texture writer's bytes did not move.

- [ ] **Step 8: Update `ArtifactFormat.hpp`'s file header.** The banner still describes a texture-only container. Widen its first paragraph to name both kinds, keep the byte-explicit rule verbatim, and extend the **BYTE-CONTRACT PEER** paragraph to say that `ArcaneClient/src/Arcane/Assets/ArtifactReader.hpp` mirrors BOTH pairs by hand (Task 4 lands the mesh half) and that a layout change to either must be mirrored there or the cross-lib round-trip fails loudly.

- [ ] **Step 9: Full `~[gpu]` suite — green.** Delta: **+6 cases** (`[pipeline]`). Commit — `feat(pipeline): the mesh artifact container (ContentKind::Mesh) writer and reader`

---

### Task 4: The ArcaneClient reader mirror + the kind-agnostic prefix split (spec §5.2, §5.1)

The client's independent mesh reader, plus the one seam this arc cannot skip: `ParseHeader` in `ArtifactReader.cpp` **refuses any non-Texture contentKind**, so `FindArtifactForGuid` — the guid → artifact scan every accessor goes through — currently rejects every mesh artifact on sight. It splits into a kind-agnostic common-prefix parse (used by the scan) and the texture-specific tail (used by the texture read).

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/ArtifactReader.hpp` (banner, `LoadedClientMesh`, `ReadClientMeshArtifact`, the mesh importer-version mirror), `ArtifactReader.cpp` (`ParseCommonPrefix` split, the mesh read)
- Test: `ArcaneTests/src/MeshArtifactReaderTest.cpp` (new; globbed)

**Interfaces:**

```cpp
    // ArtifactReader.hpp
    // Mirrors AssetPipeline::MeshArtifactSection field for field (see this file's
    // no-shared-code banner). `indexOffset`/`indexCount` are in INDICES.
    struct MeshSectionView
    {
        std::string   name;
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount  = 0;
        std::uint32_t slotIndex   = 0;
    };

    struct LoadedClientMesh
    {
        std::vector<float>         vertices;   // 8 floats per vertex: pos, normal, uv
        std::vector<std::uint32_t> indices;
        std::vector<MeshSectionView> sections;
        float aabbMin[3]{ 0.0f, 0.0f, 0.0f };
        float aabbMax[3]{ 0.0f, 0.0f, 0.0f };
    };

    // This engine's own copy of AssetPipeline's kMeshImporterVersion -- mirrored BY
    // HAND, never included, exactly like kClientTextureImporterVersionMirror above.
    // Bump IN LOCKSTEP with that one.
    inline constexpr std::uint32_t kClientMeshImporterVersionMirror = 1;

    struct MeshArtifactReadResult
    {
        ArtifactRefusal                  refusal = ArtifactRefusal::Missing;
        std::optional<LoadedClientMesh>  mesh;   // set iff refusal == None
    };

    // The mesh half of ReadClientArtifact, with the SAME refusal discipline: Missing
    // covers "cannot be used and is not one of the two named refusals" (absent,
    // unparseable, wrong contentKind, wrong sourceGuid); HashMismatch when the
    // header's sourceHash disagrees with the CURRENT source bytes; VersionNewer
    // ThanEngine when importerVersion exceeds the mirror above. `currentSourceBytes`
    // for a .gltf with external buffers is the .gltf file's bytes FOLLOWED BY every
    // referenced buffer's, in glTF declaration order -- the exact concatenation
    // ComputeMeshCookKey hashes (Task 5), so the two sides agree by construction.
    [[nodiscard]] ARCANE_API MeshArtifactReadResult ReadClientMeshArtifact(
        const std::filesystem::path& path,
        std::span<const std::byte> currentSourceBytes,
        const Guid& expectedSourceGuid);
```

- [ ] **Step 1: Write the failing cross-lib round-trip test** (`MeshArtifactReaderTest.cpp` — fixtures written through the PIPELINE's `WriteMeshArtifact`, read back through the CLIENT's reader, exactly the shape `ArtifactReaderTest.cpp` uses for textures):

```cpp
TEST_CASE("mesh artifact: pipeline writer -> client reader round-trip", "[artifact]")
{
    const fs::path dir = TempDir("client_mesh_roundtrip");
    const Guid guid = Guid::Generate();
    const std::vector<std::byte> source = PatternBytes(64, 0x11);

    Arcane::AssetPipeline::MeshArtifactDesc desc{};
    desc.sourceGuid = guid;
    desc.sourceHash = FnvOfSource(source);   // the same FNV-1a 64 both sides use
    desc.importerVersion = Arcane::kClientMeshImporterVersionMirror;
    desc.vertexCount = 4; desc.indexCount = 9; desc.sectionCount = 2;
    desc.aabbMin[0] = -1.0f; desc.aabbMax[1] = 2.5f;
    desc.sections = { { "Metal", 0, 6, 0 }, { "Metal", 6, 3, 0 } };
    /* vertices/indices as in MeshArtifactFormatTest's round-trip case */

    REQUIRE(Arcane::AssetPipeline::WriteMeshArtifact(dir / "m.arcart", desc,
                                                      vertices, indices));

    const Arcane::MeshArtifactReadResult r =
        Arcane::ReadClientMeshArtifact(dir / "m.arcart", source, guid);
    REQUIRE(r.refusal == Arcane::ArtifactRefusal::None);
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->vertices.size() == 4u * 8u);
    CHECK(r.mesh->indices == indices);
    REQUIRE(r.mesh->sections.size() == 2u);
    CHECK(r.mesh->sections[0].name == "Metal");
    CHECK(r.mesh->sections[1].slotIndex == 0u);
    CHECK(r.mesh->aabbMin[0] == -1.0f);
    CHECK(r.mesh->aabbMax[1] == 2.5f);
}

TEST_CASE("mesh artifact: the three client refusals", "[artifact]")
{
    // HashMismatch: the artifact is fine, the source moved underneath it.
    // VersionNewerThanEngine: importerVersion == mirror + 1.
    // Missing: expectedSourceGuid does not match the header's.
    /* Executor: three sections in one case, mirroring ArtifactReaderTest.cpp's own
       refusal case structure exactly -- write through the pipeline writer, vary one
       input each time, assert the named refusal and that `mesh` stays nullopt. */
}

TEST_CASE("mesh artifact: FindArtifactForGuid finds a MESH artifact", "[artifact]")
{
    // THE SEAM THIS TASK EXISTS FOR. Before the ParseCommonPrefix split, ParseHeader's
    // fail-closed `contentKind != Texture` check made this scan blind to every mesh
    // artifact -- so a mesh could be cooked, committed, and still resolve as Missing
    // forever. A texture artifact in the SAME store is written alongside it, so this
    // also proves the split did not make the scan kind-BLIND in the other direction:
    // both are found, each by its own guid.
    const fs::path store = TempDir("find_mesh") / "Intermediate";
    /* write one mesh artifact under Artifacts/<hh>/<hex>.arcart for meshGuid, and one
       texture artifact for texGuid, using ArtifactStore::PathFor to place them */
    CHECK(Arcane::FindArtifactForGuid(store, meshGuid).size() == 1u);
    CHECK(Arcane::FindArtifactForGuid(store, texGuid).size() == 1u);
    CHECK(Arcane::FindArtifactForGuid(store, Guid::Generate()).empty());
}
```

- [ ] **Step 2: Run — expect FAIL.** `./ArcaneTests.exe "[artifact]"`.

- [ ] **Step 3: Split `ParseHeader`** in `ArtifactReader.cpp`. Introduce, above it:

```cpp
        // The KIND-AGNOSTIC COMMON PREFIX every artifact kind shares, laid out before
        // any kind-specific field by F2b's own design (ArtifactFormat.hpp's header
        // ordering). Split out at F2c Task 4 because ParseHeader below FAILS CLOSED on
        // a non-Texture contentKind -- correct for a texture READ, fatal for the guid
        // SCAN, which must recognise every kind or a cooked mesh artifact resolves as
        // Missing forever no matter how many times it is cooked.
        struct CommonPrefix
        {
            std::uint8_t  contentKind = 0;
            Guid          sourceGuid;
            std::uint64_t sourceHash = 0;
            std::uint32_t importerVersion = 0;
        };

        [[nodiscard]] bool ParseCommonPrefix(ByteReader& r, CommonPrefix& out) noexcept
        {
            std::uint8_t magic[4]{};
            for (std::uint8_t& b : magic)
                if (!r.U8(b)) return false;
            if (magic[0] != kMagic[0] || magic[1] != kMagic[1] ||
                magic[2] != kMagic[2] || magic[3] != kMagic[3])
                return false;

            std::uint32_t version = 0;
            if (!r.U32(version)) return false;
            if (version != kArtifactVersion) return false;

            if (!r.U8(out.contentKind)) return false;
            // NO contentKind GATE HERE, deliberately: this function's whole purpose is
            // to answer "whose guid is this" for ANY kind. The per-kind readers below
            // apply their own fail-closed check, unchanged.
            if (!r.U64(out.sourceGuid.hi)) return false;
            if (!r.U64(out.sourceGuid.lo)) return false;
            if (!r.U64(out.sourceHash)) return false;
            return r.U32(out.importerVersion);
        }
```
`ParseHeader` keeps its `ParsedHeader` shape but now calls `ParseCommonPrefix` first, applies `contentKind != kContentKindTexture -> false` (comment preserved), and reads only the texture tail. `FindArtifactForGuid` switches from `ReadHeaderOnly` (texture-shaped) to a common-prefix probe over the SAME bounded `ReadFilePrefix` (`kHeaderProbeBytes`) — the prefix is 33 bytes, comfortably inside the existing 256-byte probe, so the scan's documented cost claim is unchanged and the header banner's "a few hundred bytes per candidate" paragraph stays true.

- [ ] **Step 4: Implement `ReadClientMeshArtifact`** — an independent reimplementation of the mesh layout from `ArtifactReader.hpp`'s own written contract (never by including the pipeline header): common prefix → `contentKind != kContentKindMesh` → `Missing`; `sourceGuid != expected` → `Missing`; `importerVersion > kClientMeshImporterVersionMirror` → `VersionNewerThanEngine`; `HashSourceBytes(currentSourceBytes) != sourceHash` → `HashMismatch`; then the mesh header tail, the container section table, the three bodies, the same skip-unknown default, the same `indexWidth == 4` and section-range validations Task 3's reader applies. **Add `kContentKindMesh = 2` beside the existing `kContentKindTexture` constant.**

- [ ] **Step 5: Extend `ArtifactReader.hpp`'s ON-DISK FORMAT banner** with the mesh layout, field for field, in the same style the texture layout is written in — this comment IS the contract the two independent implementations agree through. Include: the common prefix, the mesh header tail, the `VertexData`(4)/`IndexData`(5)/`SectionTable`(6) bodies with the u16-length name encoding, the reserved-unwritten `Tangents`(7)/`Thumbnail`(3) tags, and the sentence that `currentSourceBytes` for a `.gltf` is the file's bytes followed by every external buffer's, in declaration order.

- [ ] **Step 6: Run `[artifact]` — expect PASS.** Every pre-existing texture case in `ArtifactReaderTest.cpp` must still pass unchanged — the prefix split is behaviour-preserving for kind 1.

- [ ] **Step 7: Full `~[gpu]` suite — green.** Delta: **+3 cases** (`[artifact]`). Commit — `feat(engine): client-side mesh artifact reader and a kind-agnostic guid scan`

---

### Task 5: `MeshMetaSettings` + the mesh cook key, external buffers included (spec §5.4)

v1 carries **no user knobs** — but the empty settings block still hashes, so the first future knob changes keys honestly (the divergence-keep the comparison's Decision 2 justifies: UE's most-used import knob is a unit conversion that is 1.0 for us).

**Files:**
- Create: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/MeshMetaSettings.hpp/.cpp`
- Modify: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/CookKey.hpp/.cpp`
- Test: `ArcaneTests/src/MeshCookKeyTest.cpp` (new; globbed)

**Interfaces:**

```cpp
    // MeshMetaSettings.hpp -- the ".meta" "mesh" block. v1 has NO user-facing knobs
    // (spec s5.4, and the comparison's Decision 2 for why that is defensible rather
    // than lazy: UE's ImportUniformScale exists as a UNIT CONVERSION, and glTF's unit
    // is already ours). `settingsVersion` is the honest placeholder -- it hashes, so
    // the day the first real knob lands the key moves for every mesh, exactly once.
    //
    // BINDING, carried verbatim from TextureMetaSettings.hpp: ComputeMeshCookKey
    // hashes these fields EXPLICITLY, one at a time, in declaration order -- never a
    // struct memcpy. A new field here MUST be added there too, or a settings change
    // silently fails to invalidate the cook key and a stale artifact survives it.
    struct MeshMetaSettings
    {
        std::uint32_t settingsVersion = 1;

        // Tolerant, defaulted -- reads only fields present with the expected JSON
        // type; anything absent or mistyped keeps this struct's default.
        [[nodiscard]] static MeshMetaSettings FromMetaJson(const nlohmann::json& j);
        [[nodiscard]] nlohmann::json ToMetaJson() const;
    };
```

```cpp
    // CookKey.hpp -- the mesh half of the triple.
    // COMPOSITE version: bump when importer logic, cgltf, or meshoptimizer change.
    inline constexpr std::uint32_t kMeshImporterVersion = 1;   // {importer v1, cgltf <sha>, meshoptimizer v1.2}

    // hash(source bytes + EVERY external buffer's bytes + settings fields + importer
    // version). The external-buffer term is spec s5.4 and is load-bearing: a .gltf
    // referencing "geometry.bin" changes NOTHING in its own bytes when that buffer is
    // re-exported, so a key over the .gltf alone would serve a stale artifact forever.
    // Buffers are fed in glTF DECLARATION ORDER, each preceded by its own u32 length,
    // so two buffers can never be confused for one longer one.
    //
    // External IMAGES are deliberately NOT here: they are their own registered texture
    // assets with their own cook keys (s5.4), and folding them in would recook the
    // geometry every time an artist touched a texture.
    [[nodiscard]] std::uint64_t ComputeMeshCookKey(
        std::span<const std::byte> sourceBytes,
        std::span<const std::span<const std::byte>> externalBuffers,
        const MeshMetaSettings& settings,
        std::uint32_t importerVersion);
```

- [ ] **Step 1: Write the failing key test** (`MeshCookKeyTest.cpp`):

```cpp
TEST_CASE("mesh cook key: deterministic, and every term moves it", "[pipeline]")
{
    const std::vector<std::byte> src  = PatternBytes(32, 0x10);
    const std::vector<std::byte> buf0 = PatternBytes(16, 0x20);
    const std::span<const std::byte> bufs0[] = { buf0 };
    const MeshMetaSettings settings{};

    const std::uint64_t base = ComputeMeshCookKey(src, bufs0, settings, kMeshImporterVersion);
    CHECK(base == ComputeMeshCookKey(src, bufs0, settings, kMeshImporterVersion));

    const std::vector<std::byte> src2 = PatternBytes(32, 0x11);
    CHECK(ComputeMeshCookKey(src2, bufs0, settings, kMeshImporterVersion) != base);

    MeshMetaSettings bumped; bumped.settingsVersion = 2;
    CHECK(ComputeMeshCookKey(src, bufs0, bumped, kMeshImporterVersion) != base);

    CHECK(ComputeMeshCookKey(src, bufs0, settings, kMeshImporterVersion + 1) != base);
}

TEST_CASE("mesh cook key: editing an external .bin moves the key (spec s5.4)",
          "[pipeline]")
{
    // THE CASE s5.4 EXISTS FOR. The .gltf's own bytes are byte-identical across both
    // calls; only the referenced buffer changed. A key that ignored buffers would be
    // EQUAL here, and a re-exported .bin would never recook.
    const std::vector<std::byte> gltf = PatternBytes(48, 0x30);
    const std::vector<std::byte> binA = PatternBytes(24, 0x40);
    const std::vector<std::byte> binB = PatternBytes(24, 0x41);
    const std::span<const std::byte> a[] = { binA };
    const std::span<const std::byte> b[] = { binB };
    CHECK(ComputeMeshCookKey(gltf, a, MeshMetaSettings{}, kMeshImporterVersion)
          != ComputeMeshCookKey(gltf, b, MeshMetaSettings{}, kMeshImporterVersion));
}

TEST_CASE("mesh cook key: buffer boundaries are not fungible", "[pipeline]")
{
    // Two 8-byte buffers must not hash the same as one 16-byte buffer with the same
    // contents -- the u32 length prefix is what guarantees it. Without the prefix, a
    // re-export that merged two buffers into one would keep the OLD key and serve a
    // stale artifact.
    const std::vector<std::byte> src = PatternBytes(8, 0x50);
    const std::vector<std::byte> lo(8, std::byte{ 0xAA });
    const std::vector<std::byte> hi(8, std::byte{ 0xAA });
    std::vector<std::byte> joined(lo);
    joined.insert(joined.end(), hi.begin(), hi.end());
    const std::span<const std::byte> two[] = { lo, hi };
    const std::span<const std::byte> one[] = { joined };
    CHECK(ComputeMeshCookKey(src, two, MeshMetaSettings{}, kMeshImporterVersion)
          != ComputeMeshCookKey(src, one, MeshMetaSettings{}, kMeshImporterVersion));
}

TEST_CASE("mesh meta settings: a missing or malformed block falls back, never throws",
          "[pipeline]")
{
    CHECK(MeshMetaSettings::FromMetaJson(nlohmann::json::object()).settingsVersion == 1u);
    CHECK(MeshMetaSettings::FromMetaJson(nlohmann::json{ { "settingsVersion", "two" } })
              .settingsVersion == 1u);
    // is_number_unsigned, not is_number -- a hand-edited -3 must fall back, not wrap
    // to 4294967293 the way get<uint32_t>() would (LoadMeshAsset's own readUint rule).
    CHECK(MeshMetaSettings::FromMetaJson(nlohmann::json{ { "settingsVersion", -3 } })
              .settingsVersion == 1u);
    CHECK(MeshMetaSettings::FromMetaJson(MeshMetaSettings{}.ToMetaJson())
              .settingsVersion == 1u);
}
```

- [ ] **Step 2: Run — expect FAIL.** `./ArcaneTests.exe "[pipeline]"`.
- [ ] **Step 3: Implement `MeshMetaSettings`** — the tolerant `contains()` + `is_number_unsigned()` gate `TextureMetaSettings::FromMetaJson` uses (never a bare `.value()`, which throws `type_error` on a hand-edited file), and a `ToMetaJson` that writes the one field.
- [ ] **Step 4: Implement `ComputeMeshCookKey`** in `CookKey.cpp`, reusing the existing file-local `Fnv1a64`: `Update(sourceBytes)`, then per buffer `U32(size)` + `Update(bytes)`, then `U32(settings.settingsVersion)`, then `U32(importerVersion)`. Widen `CookKey.hpp`'s file header to say it now carries **two** kind-specific key builders over one hash, and that per-kind builders are exactly what R6's "generalize the spine, per-kind leaves" prescribes — the spine shares the hash and the triple's SHAPE, never the field list.
- [ ] **Step 5: Run `[pipeline]` — expect PASS.**
- [ ] **Step 6: Full `~[gpu]` suite — green.** Delta: **+4 cases**. Commit — `feat(pipeline): mesh meta settings and the buffer-aware mesh cook key`

---

### Task 6: `MeshImporter` — parse, validate, refuse (spec §4.5, §5.3 first half)

The importer's front half and every refusal it owes, landed before any geometry so the refusal postures are pinned by the corpus rather than retro-fitted around a working happy path.

**Files:**
- Create: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/MeshImporter.hpp`, `MeshImporter.cpp`
- Test: `ArcaneTests/src/MeshImporterRefusalTest.cpp` (new; globbed)

**Interfaces:**

```cpp
    // MeshImporter.hpp -- mesh importer v1, the glTF path. Shape mirrors
    // TextureImporter.hpp:42-55, with ONE deliberate difference: a refusal carries its
    // own REASON rather than collapsing to nullopt. TextureImporter can afford nullopt
    // because there is exactly one way a .png fails (it did not decode); a glTF has
    // several -- nothing drawable, an unsupported required extension, a validate
    // failure -- and s4.5 requires the diagnostic to NAME which. "Naming every
    // unsupported entry in one diagnostic" is not expressible through a bare nullopt.
    struct ImportedMesh
    {
        MeshArtifactDesc                desc;
        std::vector<MeshArtifactVertex> vertices;
        std::vector<std::uint32_t>      indices;
    };

    struct MeshImportResult
    {
        std::optional<ImportedMesh> mesh;        // set iff refusal.empty()
        std::string                 refusal;     // human-readable; non-empty == refused
        std::vector<std::string>    warnings;    // s4.5 tier 2 -- dropped degenerates
    };

    // Parses `sourceBytes` as glTF or GLB (cgltf sniffs the container) and imports it.
    // `externalBuffers` is ReadExternalBuffers' own result for this source, in glTF
    // declaration order -- passed IN rather than re-read so the artifact's sourceHash
    // and ComputeMeshCookKey's first two terms cover the SAME byte sequence, by
    // construction rather than by two functions agreeing. `sourcePath` is still needed
    // for cgltf_load_buffers' base directory and for diagnostics that name the file.
    //
    // REFUSAL LADDER, in the order applied (each rung's own spec clause):
    //   1. cgltf_parse fails                    -> refuse, naming the cgltf result
    //   2. extensionsRequired carries anything   -> refuse, NAMING EVERY unsupported
    //      this importer does not implement         entry IN ONE diagnostic (A2 part 1)
    //   3. cgltf_load_buffers fails              -> refuse, naming the missing buffer
    //   4. cgltf_validate fails                  -> refuse. MANDATORY -- the CVE the
    //                                                vendor pin exists for (s4.5/s10)
    //   5. nothing drawable (no meshes, or       -> refuse, naming the file (s4.5;
    //      every triangle degenerate)               UE's Error_NoPolygonFoundInMesh tier)
    // A degenerate triangle inside an otherwise valid mesh is NOT a refusal: it is
    // dropped with one warning naming its primitive (A2 part 2 -- the case an
    // implementer actually meets; refusing a 50k-triangle prop over three bad faces is
    // precisely the failure that amendment exists to prevent).
    [[nodiscard]] MeshImportResult ImportMesh(
        std::span<const std::byte> sourceBytes,
        std::span<const std::span<const std::byte>> externalBuffers,
        const std::filesystem::path& sourcePath,
        const Guid& sourceGuid,
        const MeshMetaSettings& settings);

    // Every EXTERNAL buffer this file references, in glTF declaration order, read
    // relative to `sourcePath`. An EMBEDDED buffer (a GLB BIN chunk, a data: URI)
    // contributes NOTHING -- its bytes are already inside `sourceBytes`, and hashing
    // them twice would be harmless but would make "source bytes" mean two things.
    // nullopt when the file does not parse or a referenced buffer is unreadable.
    //
    // SEPARATE from ImportMesh because the cook key must be computable WITHOUT
    // running the importer: CheckProject's staleness probe never imports (CookSession.
    // hpp's "PURE, content-addressed check"), and making it import would turn a
    // read-only --check into a full parse of every mesh in the project.
    [[nodiscard]] std::optional<std::vector<std::vector<std::byte>>> ReadExternalBuffers(
        std::span<const std::byte> sourceBytes, const std::filesystem::path& sourcePath);
```

- [ ] **Step 1: Write the failing refusal test** (`MeshImporterRefusalTest.cpp`), driving the real corpus:

```cpp
namespace
{
    fs::path Fixture(const char* name) { return fs::path("data") / "gltf" / name; }

    std::vector<std::byte> ReadFixture(const char* name)
    {
        std::ifstream in(Fixture(name), std::ios::binary);
        const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
        return std::vector<std::byte>(reinterpret_cast<const std::byte*>(raw.data()),
                                       reinterpret_cast<const std::byte*>(raw.data()) + raw.size());
    }

    // Import a fixture exactly the way CookSession will: read the source, read its
    // external buffers, hand BOTH to the importer.
    MeshImportResult ImportFixture(const char* name)
    {
        const std::vector<std::byte> bytes = ReadFixture(name);
        const auto buffers = ReadExternalBuffers(bytes, Fixture(name));
        std::vector<std::span<const std::byte>> spans;
        if (buffers) for (const auto& b : *buffers) spans.emplace_back(b);
        return ImportMesh(bytes, spans, Fixture(name), Guid::Generate(), MeshMetaSettings{});
    }
}

TEST_CASE("mesh import: a file with no meshes refuses loudly (spec s4.5)", "[pipeline]")
{
    const MeshImportResult r = ImportFixture("empty.glb");
    CHECK_FALSE(r.mesh.has_value());
    REQUIRE_FALSE(r.refusal.empty());
    // Actionable, not "import failed": the reason must name the file.
    CHECK(r.refusal.find("empty.glb") != std::string::npos);
}

TEST_CASE("mesh import: an unsupported required extension refuses, naming it (A2)",
          "[pipeline]")
{
    const MeshImportResult r = ImportFixture("requires_draco.gltf");
    CHECK_FALSE(r.mesh.has_value());
    REQUIRE_FALSE(r.refusal.empty());
    // The GENERAL rule, not a hand-list: the entry's own name appears because the
    // importer walked extensionsRequired, which is what covers KHR_texture_basisu,
    // EXT_meshopt_compression and every future extension by construction.
    CHECK(r.refusal.find("KHR_draco_mesh_compression") != std::string::npos);
    CHECK(r.refusal.find("extensionsRequired") != std::string::npos);
}

TEST_CASE("mesh import: a malformed sparse accessor is refused by validate", "[pipeline]")
{
    // The CVE-hardened path. The fixture is a truncated/overflowing index bound, not
    // a working exploit (spec s9) -- what is asserted is that cgltf_validate RAN and
    // its verdict was a GATE, never that a specific overflow was survived.
    const MeshImportResult r = ImportFixture("bad_sparse.glb");
    CHECK_FALSE(r.mesh.has_value());
    CHECK_FALSE(r.refusal.empty());
}

TEST_CASE("mesh import: degenerate triangles are DROPPED with a warning, not refused",
          "[pipeline]")
{
    // A2 part 2, the sharpest edge in s4.5: this fixture has 1 good triangle and 2
    // degenerate ones. Refusing it is the bug the amendment exists to prevent.
    const MeshImportResult r = ImportFixture("degenerate.glb");
    REQUIRE(r.refusal.empty());
    REQUIRE_FALSE(r.warnings.empty());
    // "one WARN naming the primitive" -- the diagnostic must locate the damage.
    CHECK(r.warnings[0].find("degenerate") != std::string::npos);
}

TEST_CASE("mesh import: a mesh whose triangles are ALL degenerate refuses", "[pipeline]")
{
    // The boundary between the two tiers, written down so nobody collapses them:
    // partial damage warns, TOTAL damage refuses ("nothing drawable"). Patched from
    // degenerate.glb's own bytes so the corpus needs no tenth entry.
    /* Executor: read degenerate.glb, rewrite the surviving triangle's index triple in
       its BIN chunk to {0,0,0} (the generator's header comment gives that triple's
       byte offset), import the patched bytes, assert refusal non-empty. */
}

TEST_CASE("mesh import: external buffers come back in declaration order", "[pipeline]")
{
    const std::vector<std::byte> nested = ReadFixture("nested.gltf");
    const auto buffers = ReadExternalBuffers(nested, Fixture("nested.gltf"));
    REQUIRE(buffers.has_value());
    REQUIRE(buffers->size() == 1u);            // nested.bin
    CHECK_FALSE(buffers->front().empty());

    // A GLB's buffer is EMBEDDED -- it contributes nothing, because its bytes are
    // already inside the source. Asserting it keeps "source bytes" meaning exactly
    // one thing across the cook key, the artifact hash and the client reader.
    const std::vector<std::byte> glb = ReadFixture("single.glb");
    const auto glbBuffers = ReadExternalBuffers(glb, Fixture("single.glb"));
    REQUIRE(glbBuffers.has_value());
    CHECK(glbBuffers->empty());
}
```

- [ ] **Step 2: Run — expect FAIL** (`ImportMesh` undeclared).
- [ ] **Step 3: Implement the parse + refusal ladder.** Zero-initialized `cgltf_options`; `cgltf_parse` over the span. **Write the RAII guard for `cgltf_data*` FIRST, before any of the five exit paths** — a function with five refusal returns and a hand-written `cgltf_free` on each is where the leak is guaranteed, and the guard costs four lines. The extension check walks `data->extensions_required[0 .. extensions_required_count)` and accumulates EVERY entry, because this importer implements NONE (the meshopt decoder is vendored but unwired — spec §2 records the flip as cheap, and implementing one simply removes it from the unsupported set). Then `cgltf_load_buffers(&options, data, sourcePath.string().c_str())`, then `cgltf_validate(data)` — unconditionally, never behind a settings flag.
- [ ] **Step 4: Implement the drawability gate + degenerate drop.** Walk primitives; per primitive read indices via `cgltf_accessor_read_index`; a triangle whose three indices are not all distinct is skipped and counted. After the walk: zero surviving triangles across the whole FILE → refuse, naming the file; otherwise per primitive that lost triangles push one warning naming it (its material name, else `"primitive N"`) and the drop count. **The count is per file, not per primitive** — one all-degenerate primitive beside a healthy one is a warning, not a refusal.
- [ ] **Step 5: Implement `ReadExternalBuffers`** — parse, then per `data->buffers[i]`: skip when the buffer is embedded (cgltf leaves `.uri` null for a GLB BIN chunk, and a `data:` URI is resolved into `.data` by `cgltf_load_buffers`; test `uri == nullptr || starts_with("data:")` and skip both); otherwise read the file at `sourcePath.parent_path() / cgltf_decode_uri(uri)`. An unreadable referenced buffer → nullopt, which CookSession turns into a refusal naming it.
- [ ] **Step 6: Run `[pipeline]` — expect PASS** on the six cases. **The degenerate case asserts only the WARNING here** — the surviving-index-count assertion belongs to Task 7, where vertices exist; say so in the commit body rather than leaving a test asserting something this task did not build.
- [ ] **Step 7: Full `~[gpu]` suite — green.** Delta: **+6 cases**. Commit — `feat(pipeline): MeshImporter parse, mandatory validate, and the refusal ladder`

---

### Task 7: `MeshImporter` — bake, sections, slots, optimize (spec §4.3, §5.3, A1)

The geometry half. Node TRS bakes into vertices, normals through the inverse transpose, a negative-determinant node flips its triangles' winding, one section per primitive, slots deduplicated by material name, then meshoptimizer remap → vertex-cache → vertex-fetch.

**Files:**
- Modify: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/MeshImporter.cpp`, `TextureImporter.cpp` (the shared hash, Step 7), `premake5.lua` (glm for the pipeline)
- Create: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/SourceHash.hpp`
- Test: `ArcaneTests/src/MeshImporterGeometryTest.cpp` (new; globbed), `MeshImporterRefusalTest.cpp` (the deferred assertion)

**Interfaces:** none new on `ImportMesh` — it now fills `MeshImportResult::mesh`. One extraction:

```cpp
    // SourceHash.hpp -- FNV-1a 64 over raw source bytes, the artifact header's
    // `sourceHash` fingerprint. EXTRACTED at F2c Task 7 from TextureImporter.cpp,
    // whose own copy already carried a comment flagging it as a deliberate duplicate
    // of CookKey.cpp's hasher. Two copies were a documented trade; a THIRD (the mesh
    // importer's) is where a drift becomes inevitable and silent -- a drifted hash
    // does not fail to compile, it makes every artifact HashMismatch forever.
    // CookKey.cpp's Fnv1a64 stays separate on purpose: it hashes explicit FIELDS,
    // not a byte span, and merging the two would drag the field discipline into a
    // function that has no fields.
    [[nodiscard]] inline std::uint64_t HashSourceBytes(std::span<const std::byte> bytes) noexcept;
```

**The pipeline, in order, and why each step sits where it does:**
1. **Flatten the node tree.** Walk the scene's roots depth-first, accumulating `parentWorld * cgltf_node_transform_local(node)` per node. A node with no mesh contributes only its transform. (`cgltf_node_transform_world` exists, but the explicit walk is what makes the parent-child product observable in the `nested.gltf` test rather than trusted.)
2. **Bake.** Per primitive of each mesh-bearing node: read POSITION / NORMAL / TEXCOORD_0 through `cgltf_accessor_read_float`; `position = world * vec4(p, 1)`; `normal = normalize(transpose(inverse(mat3(world))) * n)` — the inverse transpose, the same two lines UE spells at `GLTFMeshFactory.cpp:456-457`. **Missing NORMAL → the primitive's own flat face normal, computed from the baked triangle** (derived from geometry that is actually there — never a fabricated up-vector, which is the geometry spelling of §7.1's never-fabricate rule). **Missing TEXCOORD_0 → (0,0)**, the honest "no UV", which the fixed 32-byte stride requires us to write something for. **The bake loop carries one more comment, recording the last unrecorded non-goal (§2, trigger: *a consumer exists*):** a parsed file's `skins`, `animations` and a primitive's `targets` (morph targets) are **deliberately ignored** — no renderer or scene support exists for any of the three, so importing them would be stored-but-unread, which §6's own discipline forbids. `JOINTS_0`/`WEIGHTS_0` attributes are skipped by the same rule and for the same reason the fixed vertex layout skips `COLOR_0`. Say it in the code, beside the attribute reads, so the omission reads as a decision rather than an oversight.
3. **Winding flip.** `determinant(mat3(world)) < 0` → emit each triangle's corners reversed. **The comment must state BOTH halves of the mirror rule (A3):** F2c ships the winding half; the reserved `Tangents` tag inherits the tangent-basis HANDEDNESS half, and `mirrored.glb`'s test grows a handedness assertion when that tag is first written — recorded here because the existing test stays green while a mirrored asset renders with inverted normal-map lighting, which is the silent regression A3 disarms.
4. **Sections and slots (A1).** One section per primitive, `name` = the primitive's material name (empty when absent). Slots dedup **by name**: a `std::vector<std::string>` in first-seen order, one entry per distinct name; primitives with no material share **one unnamed slot appended last**. Each section's `slotIndex` is its name's position. Sections are emitted in walk order, and their index ranges tile the buffer with no gaps.
5. **Remap.** `meshopt_generateVertexRemap` over the concatenated streams, then `meshopt_remapVertexBuffer` / `meshopt_remapIndexBuffer`. **Section ranges survive because remap rewrites index VALUES, never their ORDER** — state this, because it is the property that makes step 6 safe.
6. **Optimize, per section, never across sections.** `meshopt_optimizeVertexCache` on each section's own index range in place; then `meshopt_optimizeVertexFetch` once over the whole buffer (it permutes VERTICES and rewrites indices, so it is range-agnostic). Cache-optimizing across a section boundary would scramble which triangles belong to which range — the exact hazard A1 names when it explains why sections are not merged ("meshopt reordering after the bake does not guarantee a merged material contiguous indices"), answered by never merging in the first place.
7. **AABB.** min/max over the baked positions into the desc — the artifact's stored bounds (§5.2/§7.1), which resolution reads instead of recomputing.

- [ ] **Step 1: Write the failing geometry test** (`MeshImporterGeometryTest.cpp`, reusing `MeshImporterRefusalTest.cpp`'s `ImportFixture` shape — copy the helper rather than cross-including a test TU):

```cpp
TEST_CASE("mesh import: a single-primitive glb imports one named section, one slot",
          "[pipeline]")
{
    const MeshImportResult r = ImportFixture("single.glb");
    REQUIRE(r.refusal.empty());
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->desc.contentKind == ContentKind::Mesh);
    CHECK(r.mesh->desc.indexWidth == 4u);
    CHECK(r.mesh->desc.vertexCount == r.mesh->vertices.size());
    CHECK(r.mesh->desc.indexCount == r.mesh->indices.size());
    REQUIRE(r.mesh->desc.sections.size() == 1u);
    CHECK(r.mesh->desc.sections[0].name == "SingleMat");
    CHECK(r.mesh->desc.sections[0].indexOffset == 0u);
    CHECK(r.mesh->desc.sections[0].slotIndex == 0u);
    CHECK(SlotNamesFromSections(r.mesh->desc.sections).size() == 1u);
}

TEST_CASE("mesh import: two primitives sharing one material share ONE slot (A1)",
          "[pipeline]")
{
    // THE AMENDMENT'S PIN, spelled as spec s9 words it: three sections, TWO slots,
    // and the two Metal sections' slotIndex EQUAL. Get this wrong and a user is asked
    // to assign one material twice while R3's name re-association goes ambiguous.
    const MeshImportResult r = ImportFixture("multi.glb");
    REQUIRE(r.mesh.has_value());
    REQUIRE(r.mesh->desc.sections.size() == 3u);
    CHECK(SlotNamesFromSections(r.mesh->desc.sections).size() == 2u);
    CHECK(r.mesh->desc.sections[0].slotIndex == r.mesh->desc.sections[1].slotIndex);
    CHECK(r.mesh->desc.sections[2].slotIndex != r.mesh->desc.sections[0].slotIndex);

    // Ranges tile the index buffer exactly -- no gap, no overlap, nothing lost. This
    // is also what Plan 2's per-section draws depend on being true.
    std::uint32_t cursor = 0;
    for (const MeshArtifactSection& s : r.mesh->desc.sections)
    {
        CHECK(s.indexOffset == cursor);
        cursor += s.indexCount;
    }
    CHECK(cursor == r.mesh->desc.indexCount);
}

TEST_CASE("mesh import: nested node transforms bake into vertex positions", "[pipeline]")
{
    // The assertion is on the AABB rather than a named vertex, because meshopt's
    // vertex-fetch permutation makes "vertex k" meaningless while the BOUNDS are
    // permutation-invariant -- derived from what survives the pipeline, not from what
    // the generator happened to emit first.
    const MeshImportResult r = ImportFixture("nested.gltf");
    REQUIRE(r.mesh.has_value());
    // The generator places the child quad so its BAKED box is x[1,2] z[3,4] (see
    // scripts/make-mesh-fixtures.ps1's nested tables, which state these four numbers
    // as the fixture's contract). An UNBAKED import leaves it at the origin, so this
    // case fails loudly rather than approximately if the walk is skipped.
    CHECK(r.mesh->desc.aabbMin[0] == Catch::Approx(1.0f));
    CHECK(r.mesh->desc.aabbMax[0] == Catch::Approx(2.0f));
    CHECK(r.mesh->desc.aabbMin[2] == Catch::Approx(3.0f));
    CHECK(r.mesh->desc.aabbMax[2] == Catch::Approx(4.0f));
}

TEST_CASE("mesh import: a negative-determinant node flips triangle winding", "[pipeline]")
{
    // s4.3's mirror rule, winding half -- DERIVED, not eyeballed. For every triangle,
    // cross(v1-v0, v2-v0) must point the SAME way as the averaged vertex normal, which
    // is MeshBuilder.hpp's WINDING contract applied to imported geometry. Under a
    // mirroring node an unflipped import fails this on every face.
    const MeshImportResult r = ImportFixture("mirrored.glb");
    REQUIRE(r.mesh.has_value());
    REQUIRE(r.mesh->indices.size() % 3 == 0);
    REQUIRE(r.mesh->indices.size() >= 3);
    for (std::size_t t = 0; t + 2 < r.mesh->indices.size(); t += 3)
    {
        const auto& a = r.mesh->vertices[r.mesh->indices[t]];
        const auto& b = r.mesh->vertices[r.mesh->indices[t + 1]];
        const auto& c = r.mesh->vertices[r.mesh->indices[t + 2]];
        const glm::vec3 pa(a.px, a.py, a.pz), pb(b.px, b.py, b.pz), pc(c.px, c.py, c.pz);
        const glm::vec3 geo = glm::cross(pb - pa, pc - pa);
        const glm::vec3 avg = glm::normalize(glm::vec3(a.nx, a.ny, a.nz)
                                           + glm::vec3(b.nx, b.ny, b.nz)
                                           + glm::vec3(c.nx, c.ny, c.nz));
        INFO("triangle " << (t / 3));
        CHECK(glm::dot(geo, avg) > 0.0f);
    }
}

TEST_CASE("mesh import: an UNmirrored fixture passes the same winding predicate",
          "[pipeline]")
{
    // POSITIVE CONTROL for the case above. Without it, a predicate that is vacuously
    // true (or a flip applied unconditionally) would pass the mirrored case and nobody
    // would know -- the same control discipline the panel-split plan's digest-chip
    // case uses.
    /* Executor: the identical loop over ImportFixture("single.glb"). */
}

TEST_CASE("mesh import: same input, byte-identical artifact, twice (spec s9)",
          "[pipeline]")
{
    // DETERMINISM proven where it matters -- through the WRITER, on BYTES. Comparing
    // in-memory structs would miss a nondeterministic float that still compares equal
    // and would not exercise the serializer at all.
    const fs::path dir = TempDir("mesh_determinism");
    const Guid guid = Guid::Generate();   // the SAME guid both times: it is a header field
    for (const char* name : { "single.glb", "multi.glb", "nested.gltf", "mirrored.glb" })
    {
        INFO(name);
        /* import twice with `guid`, write both through WriteMeshArtifact to
           dir/"a.arcart" and dir/"b.arcart" */
        CHECK(ReadAllBytes(dir / "a.arcart") == ReadAllBytes(dir / "b.arcart"));
    }
}

TEST_CASE("mesh import: the vertex stream is deduplicated", "[pipeline]")
{
    // Proof that meshopt_generateVertexRemap actually ran. multi.glb's generator emits
    // shared corners as separate vertices per primitive; kMultiGlbRawVertexCount is the
    // count the fixture DECLARES, stated in the generator's header, so a remapped
    // import must come back strictly under it.
    const MeshImportResult r = ImportFixture("multi.glb");
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->desc.vertexCount < kMultiGlbRawVertexCount);
}
```

- [ ] **Step 2: Run — expect FAIL.**
- [ ] **Step 3: glm for the pipeline.** `ArcaneAssetPipeline` has no glm today. Add `"%{IncludeDir.glm}"` to its `includedirs` with a comment naming this task and the bake math, then `GenerateProjects.bat`. Header-only — no link, no new `links` entry anywhere.
- [ ] **Step 4: Implement the flatten + bake** (pipeline steps 1–2 above), including both missing-attribute rules and their reasons.
- [ ] **Step 5: Implement the winding flip** (step 3) with the A3 comment written out in full.
- [ ] **Step 6: Implement sections + slot dedup** (step 4), then **remap + per-section optimize + the AABB** (steps 5–7).
- [ ] **Step 7: Extract `SourceHash.hpp` and fill the desc.** Move `TextureImporter.cpp`'s `HashSourceBytes` into the new header (keeping its comment, extended with the reason for the move), repoint `TextureImporter.cpp` at it, and use it here over **the source bytes followed by every external buffer's bytes, in declaration order** — the exact concatenation `ArtifactReader.hpp`'s `currentSourceBytes` contract (Task 4 Step 5) already names, so the two sides agree by construction rather than by two functions happening to match. Fill `sourceGuid`, `sourceHash`, `importerVersion = kMeshImporterVersion`, counts, `indexWidth = 4`, AABB, sections.
  **Texture behaviour must not move:** `TextureImporter.cpp`'s hash input is unchanged (a `.png` has no external buffers), so every existing texture artifact keeps its exact `sourceHash`. Confirm with the `[pipeline]` texture cases, which compare cooked keys.
- [ ] **Step 8: Land the deferred assertion.** In `MeshImporterRefusalTest.cpp`'s degenerate case, add `CHECK(r.mesh.has_value()); CHECK(r.mesh->indices.size() == 3u);` — the two bad triangles are gone and the good one survives.
- [ ] **Step 9: Run `[pipeline]` — expect PASS.**
- [ ] **Step 10: Full `~[gpu]` suite — green.** Delta: **+7 cases** (6 new geometry + the positive control; the degenerate case grows assertions but not a case). Commit — `feat(pipeline): mesh bake, winding flip, per-primitive sections and name-deduped slots`

---

### Task 8: Generalize the cook spine (spec §5.1, R6)

`CookSession` grows a kind table and iterates it; `ArtifactStore::RebuildIndexFromScan` reads the kind-agnostic prefix; `arccook` covers both kinds under its existing CLI contract. **Texture behaviour stays byte-identical** — the texture suites here, and both golden lanes in Plan 2, are the regression net for this surgery.

R6's recorded fallback (generalize only the store/index, keep sessions separate) is **not taken**, and the reason is worth stating: the kind table carries DATA (extensions, meta-block key) while the per-kind work stays in per-kind private members. Iterating a data table and dispatching on a small closed enum IS the generalized spine; type-erasing four differently-typed operations behind one `std::function` is the "awkward signatures" outcome the fallback was recorded against, wearing the same name.

**Files:**
- Modify: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/CookSession.hpp/.cpp`, `ArtifactFormat.hpp/.cpp` (`ReadArtifactPrefix`), `ArtifactStore.hpp/.cpp`, `arccook/src/main.cpp`
- Test: `ArcaneTests/src/AssetPipelineSessionTest.cpp` (extend), `AssetPipelineStoreTest.cpp` (extend)

**Interfaces:**

```cpp
    // CookSession.hpp
    enum class CookKind : std::uint8_t { Texture, Mesh };

    // ONE ROW PER COOKABLE KIND (spec s5.1 / R6). DATA ONLY: the extensions that
    // identify a source, and the ".meta" object key its settings block lives under.
    // The per-kind WORK -- settings type, cook-key builder, importer, artifact writer
    // -- stays in CookSession's own per-kind private members, because those four
    // differ in their TYPES, not merely their behaviour.
    struct CookKindEntry
    {
        CookKind                          kind;
        std::span<const std::string_view> extensions;    // {".png"} / {".gltf", ".glb"}
        const char*                       metaBlockKey;  // "texture" / "mesh"
    };
    [[nodiscard]] std::span<const CookKindEntry> CookKinds() noexcept;
```

```cpp
    // ArtifactFormat.hpp -- the pipeline-side twin of Task 4's client prefix split.
    // The common prefix every artifact kind shares, read WITHOUT committing to a kind:
    // exactly what ArtifactStore::RebuildIndexFromScan needs to recover a Guid, and
    // nothing more. That scan called ReadTextureArtifact, which fails closed on any
    // other kind (this file's own contentKind gate), so before F2c every mesh artifact
    // was invisible to the index -- and CookProject's supersede-the-old-key self-heal,
    // which reads Lookup, could never fire for one.
    struct ArtifactPrefix
    {
        ContentKind   contentKind = ContentKind::Texture;
        Guid          sourceGuid;
        std::uint64_t sourceHash = 0;
        std::uint32_t importerVersion = 0;
    };

    // Reads a BOUNDED PREFIX, never the whole file: this runs once per artifact in the
    // store on every scan, and a texture artifact's payload is megabytes.
    [[nodiscard]] std::optional<ArtifactPrefix> ReadArtifactPrefix(
        const std::filesystem::path& path);
```

`CookResult` grows nothing — `cookedGuids`/`failures` are already kind-agnostic, which is what lets Plan 2's invalidation dispatch on the guid's own registered kind rather than on anything the result carries.

- [ ] **Step 1: Write the failing session cases** (append to `AssetPipelineSessionTest.cpp`):

```cpp
TEST_CASE("cook session: a mesh source under Content/ cooks to a mesh artifact",
          "[pipeline]")
{
    // A project tree with ONE .glb + its sidecar and nothing else -- so this fails if
    // the spine still enumerates only .png.
    const fs::path project = TempDir("cook_mesh");
    fs::create_directories(project / "Content" / "meshes");
    fs::copy_file(fs::path("data") / "gltf" / "single.glb",
                  project / "Content" / "meshes" / "single.glb");
    const Guid meshGuid = Guid::Generate();
    WriteMetaSidecar(project / "Content" / "meshes" / "single.glb.meta", meshGuid);

    CookSession session;
    const CookResult result = session.CookProject(project);
    CHECK(result.cooked == 1u);
    CHECK(result.failed == 0u);
    REQUIRE(result.cookedGuids.size() == 1u);
    CHECK(result.cookedGuids[0] == meshGuid);

    // And the committed bytes really are a MESH artifact, not a texture one.
    const auto path = session.ResolveCurrentArtifactPath(project, meshGuid);
    REQUIRE(path.has_value());
    CHECK(ReadMeshArtifact(*path).has_value());
}

TEST_CASE("cook session: textures and meshes cook in the SAME pass", "[pipeline]")
{
    // The spine iterates the kind TABLE, not one hardcoded extension.
    /* Executor: stage uv_marker.png + single.glb, each with a sidecar; assert
       result.cooked == 2 and both guids appear in cookedGuids. */
}

TEST_CASE("cook session: editing an external .bin restales the .gltf (spec s5.4)",
          "[pipeline]")
{
    // The END-TO-END form of Task 5's key case: CheckProject must go clean -> stale
    // when ONLY the buffer changed. A key over the .gltf alone passes this wrongly.
    /* Executor: stage nested.gltf + nested.bin + sidecar; CookProject; assert
       CheckProject == false; append one byte to nested.bin; assert CheckProject == true. */
}

TEST_CASE("cook session: a refused mesh reports the importer's OWN reason", "[pipeline]")
{
    // s4.5's diagnostics must survive the trip into CookResult::failures -- a spine
    // that flattened them to "mesh import failed" would make the extensionsRequired
    // rule useless in the Problems pane, which is the only place a user reads it.
    /* Executor: stage requires_draco.gltf + sidecar; assert result.failed == 1 and
       failures[0].second names KHR_draco_mesh_compression. */
}

TEST_CASE("cook session: a mesh failure memoizes exactly like a texture failure",
          "[pipeline]")
{
    // The no-retry-storm contract is kind-agnostic, and it is SHARED code that makes
    // it so -- no mesh-specific memo logic exists. Driven through the injected
    // importer seam so invocations are COUNTED, not inferred, the same shape the
    // existing texture memoization case uses.
    /* Executor: SetMeshImporterForTesting(counting fake returning a refusal); two
       CookProject calls over the same tree; assert the fake ran exactly ONCE. */
}
```

- [ ] **Step 2: Write the failing store case** (append to `AssetPipelineStoreTest.cpp`):

```cpp
TEST_CASE("artifact store: RebuildIndexFromScan recovers a MESH artifact's guid",
          "[pipeline]")
{
    // s5.1's forcing point, and the reason the prefix read exists. A texture artifact
    // is committed alongside so this also proves the change did not make the scan
    // kind-BLIND in the other direction: both are found, each by its own guid.
    const fs::path intermediate = TempDir("store_mesh_scan") / "Intermediate";
    ArtifactStore store(intermediate);
    const Guid meshGuid = Guid::Generate();
    const Guid texGuid  = Guid::Generate();
    /* Commit one mesh artifact under key 0xAAAA and one texture artifact under 0xBBBB
       through store.Commit, so both land at the store's own PathFor locations. */
    store.RebuildIndexFromScan();
    CHECK(store.Lookup(meshGuid) == std::optional<std::uint64_t>(0xAAAAull));
    CHECK(store.Lookup(texGuid)  == std::optional<std::uint64_t>(0xBBBBull));
}
```

- [ ] **Step 3: Run both — expect FAIL.**
- [ ] **Step 4: Implement `ReadArtifactPrefix`** (bounded read: magic + version + kind + guid + hash + importerVersion = 33 bytes; read a generous fixed prefix, mirroring the client's `kHeaderProbeBytes` reasoning). Switch `RebuildIndexFromScan` to it, and update `ArtifactStore.hpp`'s own doc comment — it currently says "the Guid from that artifact's own header via `ReadTextureArtifact`", which becomes false the moment this lands.
- [ ] **Step 5: Generalize enumeration.** `EnumerateTextureSources(contentDir)` → `EnumerateSources(contentDir, std::span<const std::string_view> extensions)`: same recursion, same sibling-`.meta` requirement, same `std::sort` for deterministic order. Its doc comment carries forward verbatim, widened to "every source extension in the kind table". The `.png` list becomes one row of that table.
- [ ] **Step 6: Split `ReadSourceMeta`.** It hardcodes the `"texture"` block. Split into `ReadSourceGuid(metaPath)` (kind-agnostic) and per-kind settings reads keyed by `CookKindEntry::metaBlockKey`. An absent block still means "all defaults", unchanged — and that unchanged-ness is what keeps every existing `.png.meta` in every project cooking to the same key.
- [ ] **Step 7: Restructure the three project walks** (`CookProject`, `CheckProject`, `ResolveCurrentArtifactPath`) as `for (kind : CookKinds()) for (source : EnumerateSources(contentDir, kind.extensions))`, the per-source body dispatching on `entry.kind` for exactly four things: settings type, cook-key call, importer call, artifact writer. **Everything around that dispatch is written ONCE and shared** — the up-to-date existence check, the failure memo (read and write), the supersede-the-old-key removal, `PutIndex`, the progress callback, the result accounting. That sharing is what makes Step 1's memoization case pass for meshes without one line of mesh-specific memo code, and it is the concrete meaning of "generalize the spine, per-kind leaves".
- [ ] **Step 8: Add `SetMeshImporterForTesting`** beside the existing seam, and **rename `SetImporterForTesting` → `SetTextureImporterForTesting`** (its ~3 call sites are all in `AssetPipelineSessionTest.cpp`). The unqualified name stops being meaningful the moment there are two importers, and leaving it is how a test ends up injecting the wrong one.
- [ ] **Step 9: `arccook` covers both kinds.** Drop "textures today" from the `Cli` description and the file header's first paragraph. `--check` and the exit-code table are unchanged (they already read the kind-agnostic `CookResult`). `--dump-dds` gains ONE guard: a guid whose current artifact is a MESH is refused with a message saying `--dump-dds` is a texture-only debug aid — better than handing `ReadTextureArtifact` a mesh file and printing "failed to load", which would read as corruption.
- [ ] **Step 10: Run the full `[pipeline]` suite — expect PASS. The pre-existing texture cases must be unchanged in count AND outcome.** That is the byte-identity claim §5.1 makes, and it is CHECKED here, not asserted. If any texture case moves, stop and find out why before continuing.
- [ ] **Step 11: End-to-end at the desk.** `bin\Debug-windows-x86_64-md\arccook\arccook.exe --project ReferenceProject --verbose` → the existing texture line still reads `OK`, exit 0; `--check` → exit 0. Paste both into the commit body.
- [ ] **Step 12: Full `~[gpu]` suite — green.** Delta: **+6 cases** (5 session + 1 store). Commit — `feat(pipeline): generalize the cook spine to per-kind leaves; mesh sources cook`

---

### Task 9: Registry recognition of `.gltf`/`.glb` + `AssetKind::Model` (spec §4.1)

A dropped glTF becomes a registered asset with an auto-minted `.meta` sidecar, exactly like a texture, and classifies as a NEW kind. `Model` is distinct from `Mesh` for the same reason `Texture` is distinct from `Sprite`: one is the imported source, the other is the authored asset that derives from it.

**Files:**
- Modify: `ArcaneClient/src/Arcane/Project/AssetRegistry.cpp` (`IsImportedBinary`), `ArcaneEditor/src/Panels/AssetPanelModel.hpp` (`AssetKind`, `kAssetKindCount`, `AssetKindOf`, `KindIcon`, `KindLabel`, `AssetKindFilterForFieldName`), `AssetPanelModel.cpp` (`IsUnusedEligible`), `ArcaneEditor/src/Project/ContentDiscovery.hpp/.cpp`
- Test: `ArcaneTests/src/AssetRegistryTest.cpp` (extend), `AssetBrowserTest.cpp` (extend), `ContentDiscoveryTest.cpp` (extend)

**Interfaces:**

```cpp
    // AssetPanelModel.hpp -- AssetKind grows its tenth value.
    // F2c s4.1: .gltf/.glb IMPORTED SOURCES. Distinct from Mesh for the same reason
    // Texture is distinct from Sprite -- Model is the imported original, Mesh is the
    // authored .arcmesh that derives from it (and folds under it in the browser).
    // Same placement rule Diagnostic and Mesh used: ahead of the catch-all.
    enum class AssetKind : int
    {
        Material = 0, Texture, Audio, Font, Data, Scene, Sprite, Diagnostic, Mesh,
        Model,
        Other,
    };
    inline constexpr int kAssetKindCount = 11;
```

`KindIcon` gains `case AssetKind::Model: return ICON_LC_BOXES;` and `KindLabel` gains `case AssetKind::Model: return "Model";`. **Grep `ThirdParty/imgui`'s `IconsLucide.h` for the chosen glyph before using it** — `KindIcon`'s existing cases each carry a comment recording that grep, and an unverified `ICON_LC_*` is a compile error at best and a blank tofu box at worst. If `ICON_LC_BOXES` is absent, fall back to `ICON_LC_PACKAGE`, then `ICON_LC_BOX` (which `Mesh` already uses — acceptable, but prefer a distinct glyph and record why in the comment).

**Deliberate seam with Plan 2:** the GRAPH HUE row (`KindAccentColor`, `AssetGraphPanel.cpp`) and the browser's derived-fold widening are Plan 2's Task 8, not this one. Until then a Model node in the Graph panel wears the theme's neutral grab gray — the documented fallback for a kind with no §11.3 row, not a bug. Say so in the commit body.

- [ ] **Step 1: Write the failing registry test** (append to `AssetRegistryTest.cpp`):

```cpp
TEST_CASE("asset registry: a dropped .gltf/.glb registers with a minted sidecar",
          "[assets]")
{
    // The texture pattern, verbatim (AssetRegistry.cpp's ResolveSidecarId): a .gltf
    // cannot embed an id, so its guid lives in "<file>.gltf.meta" -- appended to the
    // FULL filename, Unity-style, so prop.gltf and prop.png get distinct sidecars.
    const fs::path content = TempDir("registry_gltf") / "Content";
    fs::create_directories(content);
    /* copy single.glb and nested.gltf in; run AssetRegistry::ScanContent */
    CHECK(fs::exists(content / "single.glb.meta"));
    CHECK(fs::exists(content / "nested.gltf.meta"));
    // Both are registered, each under the guid its own sidecar carries.
    CHECK(registry.All().size() == 2u);
    // Case-insensitive, like every other extension in IsImportedBinary.
    /* rename a copy to PROP.GLB and re-scan a fresh registry: still registered */
}
```

- [ ] **Step 2: Write the failing classification test** (append to `AssetBrowserTest.cpp`, beside the existing `.arcmesh` case at `:148-155`):

```cpp
TEST_CASE("asset browser: .gltf/.glb classify as Model, case-insensitively", "[editor]")
{
    // F2c s4.1. Model is NOT Mesh: .arcmesh stays Mesh, and the two must never
    // collapse -- the browser rail, the fold target and the unused-eligibility rule
    // all read this answer.
    CHECK(AssetKindOf("game://models/prop.gltf") == AssetKind::Model);
    CHECK(AssetKindOf("game://models/prop.glb")  == AssetKind::Model);
    CHECK(AssetKindOf("game://models/PROP.GLB")  == AssetKind::Model);
    CHECK(AssetKindOf("game://meshes/prop.arcmesh") == AssetKind::Mesh);
    CHECK(std::string(KindLabel(AssetKind::Model)) == "Model");
    CHECK(KindIcon(AssetKind::Model) != KindIcon(AssetKind::Other));
    // kAssetKindCount is what sizes the rail; a stale count silently drops the row.
    CHECK(static_cast<int>(AssetKind::Other) + 1 == kAssetKindCount);
}

TEST_CASE("asset browser: a Model is unused-eligible; the reference index sees its"
          " consumers", "[editor]")
{
    // s9.1's rule is "kinds whose consumers the reference index fully sees". A Model's
    // one consumer is the companion .arcmesh, whose DerivesFrom edge the index reads
    // (Task 12) -- so a Model with no companion is genuinely unreferenced and should
    // say so, exactly like a Texture with no sprite.
    CHECK(IsUnusedEligible(AssetKind::Model));
}
```

- [ ] **Step 3: Run — expect FAIL.**
- [ ] **Step 4: Add `.gltf` and `.glb` to `IsImportedBinary`** (`AssetRegistry.cpp:34-38`) as a new commented group beside `// images` / `// audio` / `// fonts`: `".gltf", ".glb", // meshes (F2c s4.1)`. Nothing else in `AssetRegistry.cpp` changes — `ResolveSidecarId` is already extension-agnostic.
- [ ] **Step 5: Add the enum value, the count, and the four switch rows.** `AssetKindOf` gains `if (ext == ".gltf" || ext == ".glb") return AssetKind::Model;`. `AssetKindFilterForFieldName`: **add nothing** — no component field names a Model today (`MeshRenderer::mesh` names an `.arcmesh`), and inventing a `"model"` substring rule would be a guess with no call site. Record that decision in the function's own comment beside its existing mesh/material ordering note.
- [ ] **Step 6: `IsUnusedEligible` gains `|| kind == AssetKind::Model`**, with the reasoning from the test above in a comment.
- [ ] **Step 7: Generalize `ContentDiscovery`.** `EnumerateContentPngFiles` → `EnumerateContentSourceFiles(contentDir, std::span<const std::string_view> extensions)`, and `DiscoverUnknownTextureSources` → `DiscoverUnknownSources(contentDir, extensions, knownPaths)`. Update the header's own prose, which currently says ".png" six times and explains itself entirely in texture terms — a mid-session `.glb` drop has exactly the same dead-path problem the header describes, and leaving it texture-only would make drop-to-cook work for one kind and silently not the other. Extend `ContentDiscoveryTest.cpp` with one case proving a dropped `.glb` is discovered and a dropped `.txt` is not.
- [ ] **Step 8: Update `PollAssetWatch`'s discovery block** (`EditorAppProject.cpp:416-451`) to pass both kinds' extensions and to build `knownPaths` from `Texture` **and** `Model` entries. The `.meta` mtime watch loop below it (`:523-604`) gains `Model` alongside `Texture` in its kind gate, so a re-exported `.glb` triggers a recook the same way a re-saved `.png` does — including the C2 first-sighting-counts-as-change rule, which is what heals an uncooked clone.
- [ ] **Step 9: Run `[assets]` + `[editor]` — expect PASS.**
- [ ] **Step 10: Full `~[gpu]` suite — green.** Delta: **+4 cases** (1 registry, 2 browser, 1 discovery). Commit — `feat(engine): register .gltf/.glb as AssetKind::Model with a minted sidecar`

---

### Task 10: The `.arcmesh` schema move — `Imported`, `importedSource`, `slots[]` (spec §4.2, §4.4, A1)

The F2a material scalar grows into a named-slot array, `MeshSource` appends `Imported = 5`, and `MeshData`/`MeshEntry` grow sections. The one invasive schema task in this plan; the tolerant legacy-key mapping is what makes it safe.

**Files:**
- Modify: `ArcaneClient/src/Arcane/Mesh/MeshAsset.hpp/.cpp`, `ArcaneClient/src/Arcane/Render/MeshBuilder.hpp/.cpp`, `ArcaneClient/src/Arcane/Scene/SceneResources.hpp` (`MeshEntry`), `ArcaneClient/src/Arcane/Render/MeshCache.cpp`, `ArcaneClient/src/Arcane/Scene/MeshSubmissionSystem.hpp`, `ArcaneEditor/src/Documents/MeshDocument.cpp`
- Test: `ArcaneTests/src/MeshAssetTest.cpp`, `MeshBuilderTest.cpp`, `MeshSubmissionTest.cpp`, `SceneRenderResolverTest.cpp`, `HostBootTest.cpp` (the `.material` call sites)

**Interfaces:**

```cpp
    // MeshAsset.hpp
    enum class MeshSource : std::uint8_t
    {
        Plane = 0, Cube = 1, UvSphere = 2, Cylinder = 3, Capsule = 4,
        // F2c s4.2: geometry comes from a cooked artifact rather than a generator.
        // APPENDED, never reordered -- these values are persisted. An OLDER engine
        // build reading "imported" gets today's tolerant posture: one ARC_WARN and a
        // Cube fallback (LoadMeshAsset's unknown-source arm), visible not fatal.
        Imported = 5,
    };

    // ONE material slot. `name` is the glTF material name the cook reported, and it is
    // the RE-ASSOCIATION KEY on re-import (R3): a re-export that reorders its
    // materials must not shuffle the user's assignments. Position is the tiebreak for
    // unnamed or duplicate names -- UE's own rule (FbxStaticMeshImport.cpp:1946-1974).
    struct MeshSlot
    {
        std::string name;
        Guid        material{};
    };

    struct MeshAssetData
    {
        Guid          id{};
        std::string   name;
        MeshSource    source = MeshSource::Cube;
        /* ... topology + capsuleLengthRatio, unchanged ... */

        // F2c s4.2: the registered .gltf/.glb this asset's geometry is cooked from.
        // Meaningful ONLY when source == Imported; nil otherwise, and written
        // unconditionally like every other field (SaveMeshAsset's own every-field rule
        // -- a sparse write loses it on a source switch and back).
        Guid importedSource{};

        // F2c s4.4: the F2a SCALAR `material` retired into a named-slot array. The
        // tolerant loader maps a legacy "material" key to ONE unnamed slot, so every
        // .arcmesh already on disk loads unchanged and nothing needs migrating.
        // A generated primitive carries zero or one slot; an imported mesh carries one
        // per DISTINCT glTF material name (A1 -- deduped by name, never per primitive).
        std::vector<MeshSlot> slots;
    };
```

```cpp
    // MeshBuilder.hpp
    // One drawable range with its slot. Mirrors AssetPipeline::MeshArtifactSection
    // (which this file must never include -- ArcaneClient links no AssetPipeline).
    struct MeshSection
    {
        std::string   name;
        std::uint32_t indexOffset = 0;   // in INDICES
        std::uint32_t indexCount  = 0;
        std::uint32_t slotIndex   = 0;
    };

    struct MeshData
    {
        std::vector<MeshVertex>    vertices;
        std::vector<std::uint32_t> indices;
        // F2c s4.3. NEVER EMPTY for a mesh with geometry: every generator ends by
        // emitting exactly one unnamed section covering the whole index range, so no
        // consumer needs an "if empty, draw everything" fallback -- an empty sections
        // list means an empty mesh, and an empty mesh draws nothing.
        std::vector<MeshSection>   sections;
    };
```

`MeshEntry` (`SceneResources.hpp:161-166`): `Guid material` → `std::vector<MeshSlot> slots`, with its long comment updated — the "second link in the chain" prose becomes per-section-through-the-index, and the `MeshData` borrow note is unchanged.

**The resolution chain, restated (§4.4):** component `materialOverride` (scalar; **when set, repaints ALL sections** — §2's non-goal, kept) → `slots[section.slotIndex].material` → white. `CollectMeshInstances` therefore emits **one `MeshInstance` per section** rather than one per entity. That is a submission-shape change, and it is Plan 2's Task 5 that makes the DRAW side consume it — so **this task keeps `CollectMeshInstances` emitting one instance per entity, resolving through `slots[0]` when a slot exists**, which is byte-identical behaviour for every F2a mesh and is what lets this task end green with no render work. State the deferral in the function's comment naming Plan 2 Task 5.

- [ ] **Step 1: Write the failing asset test** (extend `MeshAssetTest.cpp`):

```cpp
TEST_CASE("mesh asset: slots round-trip, and a legacy \"material\" key still loads",
          "[mesh]")
{
    // BACKWARD COMPAT IS THE POINT (s4.4). Every .arcmesh on disk today -- including
    // ReferenceProject's own reference_cube.arcmesh -- carries a scalar "material".
    const fs::path dir = TempDir("mesh_slots");

    MeshAssetData in;
    in.id = Guid::Generate();
    in.name = "Prop";
    in.source = MeshSource::Imported;
    in.importedSource = Guid::Generate();
    in.slots = { { "Metal", Guid::Generate() }, { "Paint", Guid{} } };
    REQUIRE(SaveMeshAsset(dir / "prop.arcmesh", in));

    const auto out = LoadMeshAsset(dir / "prop.arcmesh");
    REQUIRE(out.has_value());
    CHECK(out->source == MeshSource::Imported);
    CHECK(out->importedSource == in.importedSource);
    REQUIRE(out->slots.size() == 2u);
    CHECK(out->slots[0].name == "Metal");
    CHECK(out->slots[0].material == in.slots[0].material);
    CHECK(out->slots[1].name == "Paint");
    CHECK_FALSE(out->slots[1].material.IsValid());   // an UNASSIGNED slot is legal
    CHECK(*out == in);                                // memberwise equality still holds
}

TEST_CASE("mesh asset: a legacy scalar \"material\" maps to one unnamed slot", "[mesh]")
{
    const fs::path dir = TempDir("mesh_legacy");
    const Guid mat = Guid::Generate();
    /* hand-write an F2a-shaped .arcmesh: type/id/name/source/rings/segments/
       subdivisions/capsuleLengthRatio + "material": <mat> and NO "slots" key */
    const auto out = LoadMeshAsset(dir / "legacy.arcmesh");
    REQUIRE(out.has_value());
    REQUIRE(out->slots.size() == 1u);
    CHECK(out->slots[0].name.empty());
    CHECK(out->slots[0].material == mat);
    CHECK_FALSE(out->importedSource.IsValid());
}

TEST_CASE("mesh asset: a nil legacy material yields NO slot, not an empty one", "[mesh]")
{
    // An F2a mesh with no material assigned wrote "00000000-...". Mapping that to a
    // slot would fabricate a material row in the Inspector for an asset that has
    // none -- the same never-fabricate discipline s7.1 applies to geometry.
    /* Executor: hand-write a legacy file with a nil "material"; assert slots.empty(). */
}

TEST_CASE("mesh asset: an Imported mesh validates on importedSource, not on topology",
          "[mesh]")
{
    // ValidateMeshAsset is PER SOURCE over the fields that source READS (its own
    // contract). Imported reads neither rings nor segments, so a zero there is legal;
    // what it does read is importedSource, and a nil one is a refusal that NAMES the
    // field -- the Problems-pane actionability rule.
    MeshAssetData data;
    data.source = MeshSource::Imported;
    data.rings = 0; data.segments = 0;          // meaningless to Imported
    const auto reason = ValidateMeshAsset(data);
    REQUIRE(reason.has_value());
    CHECK(reason->find("importedSource") != std::string::npos);

    data.importedSource = Guid::Generate();
    CHECK_FALSE(ValidateMeshAsset(data).has_value());
}
```

- [ ] **Step 2: Write the failing builder test** (extend `MeshBuilderTest.cpp`):

```cpp
TEST_CASE("mesh builder: every generator emits exactly one whole-range section",
          "[mesh]")
{
    // The invariant that spares every consumer an "if sections is empty" fallback.
    // Exhaustive over the roster rather than sampled: a generator added later without
    // its section line is precisely the silent case this catches.
    for (const MeshData& m : { BuildCube(1.0f), BuildUvSphere(0.5f, 8, 12),
                               BuildPlane(2), BuildCylinder(8), BuildCapsule(3, 8, 2.0f) })
    {
        REQUIRE(m.sections.size() == 1u);
        CHECK(m.sections[0].name.empty());
        CHECK(m.sections[0].indexOffset == 0u);
        CHECK(m.sections[0].indexCount == m.indices.size());
        CHECK(m.sections[0].slotIndex == 0u);
    }
}
```

- [ ] **Step 3: Run both — expect FAIL.**
- [ ] **Step 4: `MeshSource::Imported` + the JSON strings.** `SourceToJsonString`/`JsonStringToSource`/`SourceDisplayName` each gain their `Imported`/`"imported"`/`"Imported"` row. **All three are `switch`es with no `default`** — adding the enumerator without all three rows is exactly the kind of miss `fatalwarnings { "4715" }` does not catch, so add them together.
- [ ] **Step 5: `slots` + `importedSource` in Save/Load.** `SaveMeshAsset` writes `doc["importedSource"]` and `doc["slots"]` as an array of `{"name","material"}` objects, unconditionally (the every-field rule), and **stops writing `"material"`** — the legacy key is READ-ONLY from here. `LoadMeshAsset`: read `slots` when present and well-shaped (`is_array`, each element `is_object` with a string `name` and a string `material`, each field independently tolerant); **otherwise** fall back to the legacy `"material"` key, mapping a VALID guid to one unnamed slot and a nil/absent one to no slot at all. Add one `ARC_WARN`-free note in the comment: the legacy path is not deprecated-with-a-warning, because a warning on every F2a file in every project is noise for a mapping that is exact and lossless.
- [ ] **Step 6: `operator==` and `ValidateMeshAsset`.** Memberwise equality gains `importedSource` and `slots` (`MeshSlot` needs its own `operator==`; memberwise, never memcmp — `name` is a `std::string`, the same reasoning `MeshAssetData`'s own comment gives). `ValidateMeshAsset` gains the `Imported` arm: nil `importedSource` → a reason naming the field; everything else legal (topology fields mean nothing to this source).
- [ ] **Step 7: `BuildMeshData`'s `Imported` arm** — for now, `return std::nullopt;` with an `ARC_WARN` saying an imported mesh resolves through `ResolveMeshData` (Task 11), never through this function. This keeps the switch exhaustive and the behaviour honest in the one commit between the two tasks.
- [ ] **Step 8: `MeshData::sections` + the generator lines.** Add the field and a file-local `FinishSingleSection(MeshData&)` in `MeshBuilder.cpp`; call it as the last line of all five generators. `ComputeMeshBounds` is untouched.
- [ ] **Step 9: `MeshEntry::slots`, and the consumers.** `MeshCache.cpp:89` becomes `entry.slots = std::move(data->slots);`. `MeshSubmissionSystem.hpp`'s chain becomes `materialOverride` → `entry->slots.empty() ? Guid{} : entry->slots[0].material` → white, **with the comment stating the per-section deferral to Plan 2 Task 5 by name**. `SceneRenderResolver.cpp:390`'s `meshMaterials->Request(it->second.material)` becomes a loop over `slots` — every slot's material must be resolved, or a second section renders white the frame Plan 2 turns sections on.
- [ ] **Step 10: `MeshDocument`'s material picker.** Its five `m_data.material` sites (`MeshDocument.cpp:556-589`) retarget to a slot. Minimal correct shape for this task: operate on `slots[0]`, creating one unnamed slot on first assignment and erasing it when cleared — the same single-material UX F2a had, now expressed through the array. **A per-slot list UI for imported meshes is NOT in either plan** — record it in the self-review's intentional gaps, and leave a comment at the call site saying an imported mesh's extra slots are editable by hand until that UI exists.
- [ ] **Step 11: Repoint the test call sites.** `MeshAssetTest.cpp:34`, `MeshSubmissionTest.cpp:85/205/703-711`, `SceneRenderResolverTest.cpp:106/657`, `HostBootTest.cpp:752-753/975` all read `.material`. Each becomes the slot form. **`HostBootTest.cpp:752`'s `REQUIRE(meshData->material.IsValid())` is the sharp one** — it drives ReferenceProject's real `reference_cube.arcmesh`, which still carries the legacy key on disk, so it becomes `REQUIRE(meshData->slots.size() == 1u); REQUIRE(meshData->slots[0].material.IsValid());` and **that assertion is now the legacy-mapping's end-to-end proof**, not just a boot check. Say so in a comment there.
- [ ] **Step 12: Run `[mesh]` + `[scene]` + `[editor]` — expect PASS.**
- [ ] **Step 13: Full `~[gpu]` suite — green.** Delta: **+5 cases** (4 asset + 1 builder). Commit — `feat(engine): .arcmesh grows named material slots and an imported source`

---

### Task 11: CPU resolution of an imported mesh + the ABI bump (spec §7.1, §5.6)

`ResolveMeshData` turns a `MeshSource::Imported` asset into a `MeshData` with sections and the artifact's own AABB, through three tail-appended `Assets` virtuals. **The one ABI bump in the arc: 23 → 24.**

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.hpp/.cpp` (three virtuals + the mesh resolve/refuse path), `ArcaneClient/src/Arcane/Mesh/MeshAsset.hpp/.cpp` (`ResolveMeshData`), `ArcaneClient/src/Arcane/Render/MeshCache.hpp/.cpp` (the supply + the pending tri-state), `ArcaneClient/src/Arcane/Host/SceneRenderResolver.cpp` (fill the supply), `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` (the ledger + the constant), `ReferenceProject/ReferenceProject.arcproj` (restamp)
- Test: `ArcaneTests/src/MeshResolveTest.cpp` (new; globbed), `AssetsTest.cpp` (extend)

**Interfaces:**

```cpp
    // Assets.hpp -- THREE virtuals, APPENDED AT THE END of the interface (the v21/v22
    // precedent this class states as a rule: an append reshuffles no existing vtable
    // slot, and nothing in a game module subclasses Assets).

    // The cooked mesh artifact for `id` -- geometry, sections and the stored AABB --
    // resolved and memoized exactly like ArtifactFor above, through the SAME
    // ResolveArtifact candidate walk and the SAME refusal discipline (a PRESENT-but-
    // invalid artifact refuses loudly and memoizes; a probe-quieted Missing returns
    // null silently with no memo and no latch). Null for an invalid id, a refused
    // artifact, or a guid with no cooked mesh artifact. Owned by this facade's
    // LRU-budgeted cache -- copy it, never hold the pointer (PixelsFor's contract).
    virtual const LoadedClientMesh* MeshArtifactFor(const Guid& id) = 0;

    // The mesh half of InvalidateArtifact: drops every memoized entry (success OR
    // memoized refusal) this facade holds for `id`'s MESH artifact. The un-latch a
    // background mesh cook's completion needs -- without it, a .glb dropped mid-session
    // never promotes past its first "not cooked yet" ask. Plan 2 Task 6 wires the
    // cook-completion callback to it.
    virtual void InvalidateMeshArtifact(const Guid& id) = 0;

    // Is a cook plausibly still pending for `id`? A pure forward to the probe
    // SetCookPendingProbe installed (false when none is). PUBLISHED because the
    // resolution layer above this facade must distinguish "not cooked YET" (draw
    // nothing QUIETLY, retry next frame -- s7.1) from "missing or refused" (draw
    // nothing LOUDLY, memoize), and the accessors' own null answer cannot carry that
    // difference. Generic rather than mesh-specific: it says nothing about kind, and a
    // future consumer of the same distinction needs no fourth virtual.
    [[nodiscard]] virtual bool CookPending(const Guid& id) const = 0;
```

```cpp
    // MeshAsset.hpp
    using MeshArtifactSupplyFn = std::function<const LoadedClientMesh*(const Guid&)>;
    using CookPendingFn        = std::function<bool(const Guid&)>;

    enum class MeshResolveState : std::uint8_t
    {
        Ready,        // `mesh` is set
        PendingCook,  // no artifact YET; retry later. NOT a failure -- see s7.1
        Failed,       // invalid asset, missing/refused artifact, or no supply
    };

    struct MeshResolveResult
    {
        MeshResolveState        state = MeshResolveState::Failed;
        std::optional<MeshData> mesh;
        MeshBounds              bounds;   // the ARTIFACT's stored AABB for Imported;
                                          // ComputeMeshBounds' answer for a primitive
        std::string             reason;   // human-readable, non-empty iff Failed
    };

    // THE one entry point a host resolves a .arcmesh through. A generated source
    // delegates straight to BuildMeshData + ComputeMeshBounds (unchanged, and it is
    // still the pure, device-free, supply-free function every builder test drives);
    // Imported resolves `data.importedSource` through `supply`.
    //
    // THE REFUSAL POSTURE, s13-flavoured for geometry (s7.1): PENDING -> draw nothing
    // QUIETLY (a placeholder cube would FABRICATE a shape, which is worse than an
    // empty space); MISSING or REFUSED -> draw nothing LOUDLY, with a diagnostic.
    // Never render an unknown as a cube.
    [[nodiscard]] ARCANE_API MeshResolveResult ResolveMeshData(
        const MeshAssetData& data,
        const MeshArtifactSupplyFn& supply,
        const CookPendingFn& cookPending);
```

- [ ] **Step 1: Write the failing resolve test** (`MeshResolveTest.cpp` — device-free, driving `ResolveMeshData` with hand-built supplies):

```cpp
TEST_CASE("mesh resolve: a generated source needs no supply at all", "[mesh]")
{
    MeshAssetData data; data.source = MeshSource::Cube;
    const MeshResolveResult r = ResolveMeshData(data, {}, {});
    CHECK(r.state == MeshResolveState::Ready);
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->sections.size() == 1u);
    // The primitive path still computes bounds from vertices (s7.1's split).
    CHECK(r.bounds.max.x == Catch::Approx(0.5f));
}

TEST_CASE("mesh resolve: an imported source decodes sections and the STORED aabb",
          "[mesh]")
{
    // The artifact's AABB is taken, never recomputed -- it was cooked from these exact
    // vertices, and recomputing would be work that can only produce the same answer or
    // a different (wrong) one.
    LoadedClientMesh artifact;
    artifact.vertices = { /* 3 verts x 8 floats */ };
    artifact.indices  = { 0, 1, 2 };
    artifact.sections = { { "Metal", 0, 3, 0 } };
    artifact.aabbMin[1] = -7.0f; artifact.aabbMax[1] = 9.0f;

    MeshAssetData data;
    data.source = MeshSource::Imported;
    data.importedSource = Guid::Generate();
    data.slots = { { "Metal", Guid::Generate() } };

    const MeshResolveResult r = ResolveMeshData(
        data, [&](const Guid& g) { return g == data.importedSource ? &artifact : nullptr; },
        [](const Guid&) { return false; });

    REQUIRE(r.state == MeshResolveState::Ready);
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->vertices.size() == 3u);
    CHECK(r.mesh->indices == std::vector<std::uint32_t>{ 0, 1, 2 });
    REQUIRE(r.mesh->sections.size() == 1u);
    CHECK(r.mesh->sections[0].name == "Metal");
    CHECK(r.mesh->sections[0].slotIndex == 0u);
    CHECK(r.bounds.min.y == Catch::Approx(-7.0f));
    CHECK(r.bounds.max.y == Catch::Approx(9.0f));
}

TEST_CASE("mesh resolve: pending is QUIET, missing is LOUD (spec s7.1)", "[mesh]")
{
    // The distinction s7.1 turns on, and the reason CookPending is published at all:
    // both answer "no geometry", and only one of them is a problem.
    MeshAssetData data;
    data.source = MeshSource::Imported;
    data.importedSource = Guid::Generate();
    const auto noArtifact = [](const Guid&) -> const LoadedClientMesh* { return nullptr; };

    const MeshResolveResult pending =
        ResolveMeshData(data, noArtifact, [](const Guid&) { return true; });
    CHECK(pending.state == MeshResolveState::PendingCook);
    CHECK(pending.reason.empty());            // nothing to say -- it is still cooking
    CHECK_FALSE(pending.mesh.has_value());

    const MeshResolveResult missing =
        ResolveMeshData(data, noArtifact, [](const Guid&) { return false; });
    CHECK(missing.state == MeshResolveState::Failed);
    CHECK_FALSE(missing.reason.empty());      // and the reason must NAME the guid
    CHECK(missing.reason.find(data.importedSource.ToString()) != std::string::npos);
}

TEST_CASE("mesh resolve: a nil importedSource fails without consulting the supply",
          "[mesh]")
{
    // ValidateMeshAsset already refuses this (Task 10). Proven here too because the
    // resolve path is what a host actually calls, and a supply consulted with a nil
    // guid is a directory scan for nothing.
    bool consulted = false;
    MeshAssetData data; data.source = MeshSource::Imported;
    const MeshResolveResult r = ResolveMeshData(
        data, [&](const Guid&) { consulted = true; return nullptr; },
        [](const Guid&) { return false; });
    CHECK(r.state == MeshResolveState::Failed);
    CHECK_FALSE(consulted);
}
```

- [ ] **Step 2: Write the failing facade test** (append to `AssetsTest.cpp`): a mesh artifact cooked into a temp project resolves through `MeshArtifactFor`; a second call is memoized (assert by deleting the file between calls and still getting the same pointer); `InvalidateMeshArtifact` drops the memo (the next call re-scans and now answers null); a `HashMismatch` artifact refuses loudly and sets the process latch; **`CookPending` forwards the installed probe and answers `false` with none installed.** Reset the latch with `ResetContentArtifactRefusal()` — the `[assets]` cases' standing discipline under random order.
- [ ] **Step 3: Run — expect FAIL.**
- [ ] **Step 4: Implement `ResolveMeshData`** — the primitive delegation, the nil guard, the supply call, the pending/missing split, and the artifact → `MeshData` decode (eight floats per vertex into `MeshVertex`; `MeshSectionView` → `MeshSection`; the stored AABB into `MeshBounds`).
- [ ] **Step 5: Implement the three virtuals in `AssetsImpl`.** `MeshArtifactFor` mirrors `ArtifactForResolved` exactly — its own `AssetCache<LoadedClientMesh>` member, the shared `ResolveArtifact` candidate walk (which now needs a mesh-shaped validator: **factor `ResolveArtifact` into a template or a small policy taking the per-kind `ReadClient*Artifact`, rather than copying its 30-line C1(b) candidate-walk comment and logic**), the shared `QuietlyPending`, the shared `RefuseArtifact`. `InvalidateMeshArtifact` mirrors `InvalidateArtifact`'s eviction, on the mesh cache. `CookPending` is a two-line forward to `m_cookPendingProbe`.

  **The `currentSourceBytes` a mesh candidate is validated against** is the source file's bytes followed by every external buffer's — the concatenation Task 7 fixed. `AssetsImpl` must build it the same way, which means it needs `ReadExternalBuffers`… **which lives in ArcaneAssetPipeline, and ArcaneClient must never link it.** Resolve this HERE, not later: the client re-derives the buffer list from the `.gltf`'s own JSON (`buffers[].uri`, skipping absent/`data:` URIs) with a ~20-line reader in `ArtifactReader.cpp`, and its comment names `ReadExternalBuffers` as the peer it must agree with by hand — the same deliberate-reimplementation rule the two artifact readers already live under. A `.glb` needs none of this (no external buffers), so the common case reads one file.
- [ ] **Step 6: Wire `MeshCache`.** `Services` gains `MeshArtifactSupplyFn meshArtifactFor` and `CookPendingFn cookPending`; `Request` calls `ResolveMeshData` and **treats `PendingCook` as "do nothing, ask again next frame" — it must NOT enter the `failed` set**, or the mesh latches broken exactly the way the texture path's `ArtifactMissing` memo did before F2b's desk-fix 2. Add a one-line comment saying so, because the `failed` set is otherwise the obvious place for it. `SceneRenderResolver`'s `MeshCache` construction fills both from the `Assets` facade.
- [ ] **Step 7: THE ABI BUMP — 23 → 24.** Write the ledger entry above `kGamePluginABIVersion` in the v22/v23 house form, covering: **what** (three `Assets` virtuals appended at the end — `MeshArtifactFor`, `InvalidateMeshArtifact`, `CookPending`; two new by-value types, `LoadedClientMesh` and `MeshSectionView`, neither crossing the vtable except as a return pointer); **why it is safe** (an append reshuffles no existing slot, and `AssetsImpl` stays private behind `Assets::Create()`, so nothing subclasses it); **the schema fact riding along** (`.arcmesh` grew `slots[]`/`importedSource` and `MeshSource::Imported`, Task 10 — backward compatible via the legacy-key mapping, so every v3-era `.arcmesh` still loads); and **MEASURED, not assumed**: `grep -rn` for `MeshArtifactFor`, `InvalidateMeshArtifact`, `CookPending`, `LoadedClientMesh`, `MeshSlot`, `MeshSection`, `ResolveMeshData` over BOTH game modules — `ReferenceProject/Source/` and Gacha's `Game/Source/` — and paste the (expected empty) result into the entry. Restamp `ReferenceProject.arcproj`'s `engine.abi` to 24. **Gacha's Game restamp is that repo's follow-up, tracked there — record it, do not act on it and do not nag.**
- [ ] **Step 8: Run `[mesh]` + `[assets]` — expect PASS.**
- [ ] **Step 9: Rebuild `ReferenceProject.slnx`** (the ABI-bump ritual the v21/v22 entries each required) and launch `ArcaneEditor.exe` once against ReferenceProject: it opens, the cube still renders, no plugin ABI refusal in the log.
- [ ] **Step 10: Full `~[gpu]` suite Debug AND Release — green.** Delta: **+5 cases** (4 resolve + 1 assets). Commit — `feat(engine): resolve imported meshes through the Assets facade (ABI 24)`

---

### Task 12: The reference graph — `DerivesFrom` the source, `References` every slot (spec §4.2, §4.4)

`ListAssetReferences`' `.arcmesh` branch (`Assets.cpp:960-965`) walks one scalar `"material"` key today. It grows two edges: the companion's `importedSource` as **DerivesFrom** (what makes the browser fold it under its Model and what makes the Graph panel draw the import web with zero new code), and every slot's material as **References**.

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.cpp` (the `.arcmesh` branch), `Assets.hpp` (the `ListAssetReferences` doc comment's `.arcmesh` row)
- Test: `ArcaneTests/src/AssetReferencesTest.cpp` (extend)

**Interfaces:** no signature change — `ListAssetReferences` returns more edges for a `.arcmesh`, and `.gltf`/`.glb` join the LEAF list (an imported original has no outgoing references of its own; its embedded textures become separate assets at extraction, so the file itself names nothing).

- [ ] **Step 1: Write the failing reference test** (append to `AssetReferencesTest.cpp`):

```cpp
TEST_CASE("references: an imported .arcmesh derives from its source and references"
          " every slot", "[assets]")
{
    // s4.2: the DerivesFrom edge is what folds the companion under its Model in the
    // browser (the sprite-under-texture machinery, reused) and what makes the Graph
    // panel render the import web with no new code.
    /* Executor: write a .arcmesh with importedSource = modelGuid and two slots naming
       matA and matB (one slot deliberately NIL, to prove it contributes nothing);
       register all of them; call ListAssetReferences on the mesh. */
    const auto refs = assets->ListAssetReferences(meshGuid);
    REQUIRE(refs.has_value());
    CHECK(HasEdge(*refs, modelGuid, AssetRefKind::DerivesFrom));
    CHECK(HasEdge(*refs, matA, AssetRefKind::References));
    CHECK(HasEdge(*refs, matB, AssetRefKind::References));
    // EXACTLY ONE DerivesFrom: the browser's fold predicate requires it
    // (AssetPanelModel.cpp's derivesFromCount == 1 gate), so a second one would
    // silently unfold every imported mesh in the project.
    CHECK(CountOfKind(*refs, AssetRefKind::DerivesFrom) == 1u);
    // The nil slot contributes NOTHING -- no phantom nil-guid entry, the same rule
    // the scalar `material` branch already keeps.
    CHECK(refs->size() == 3u);
}

TEST_CASE("references: a legacy scalar-material .arcmesh is unchanged", "[assets]")
{
    // Regression net for the F2a corpus: no importedSource, no slots key, one
    // References edge, no DerivesFrom.
    /* Executor: the legacy hand-written file from Task 10's own legacy case. */
    CHECK(CountOfKind(*refs, AssetRefKind::DerivesFrom) == 0u);
    CHECK(HasEdge(*refs, mat, AssetRefKind::References));
}

TEST_CASE("references: a .gltf/.glb is a LEAF, not unrecognised", "[assets]")
{
    // "empty, NEVER nullopt" -- the distinction Assets.hpp's own doc comment draws
    // between "genuinely no outgoing edges" and "could not read this at all". A Model
    // is a leaf like a .png: its embedded textures become SEPARATE assets at
    // extraction (s5.5), so the file itself names nothing.
    const auto refs = assets->ListAssetReferences(modelGuid);
    REQUIRE(refs.has_value());
    CHECK(refs->empty());
}
```

- [ ] **Step 2: Run — expect FAIL** (the `.gltf` case falls through to the structural scan, which would return a phantom edge or an unrecognised-empty for the wrong reason).
- [ ] **Step 3: Extend the `.arcmesh` branch.** `addGuid(doc["importedSource"], AssetRefKind::DerivesFrom)` when present and valid; loop `doc["slots"]`, `addGuid(slot["material"], AssetRefKind::References)`; keep the legacy `doc["material"]` → `References` arm as the else. `addGuid` already skips nil guids — verify that rather than assume it (it is what makes the nil-slot assertion pass).
- [ ] **Step 4: Add `.gltf`/`.glb` to the `kLeaf` extension list** beside the image/audio/font entries.
- [ ] **Step 5: Update `Assets.hpp`'s `ListAssetReferences` doc comment** — its `.arcmesh` row currently reads `"material": References; the nil guid contributes nothing`. Rewrite it to name all three arms (`importedSource` → DerivesFrom, `slots[].material` → References, legacy `material` → References) and add `.gltf`/`.glb` to the leaf list it enumerates. **This comment is what `AssetPanelModel`'s fold predicate is written against**, so a stale one here is how the browser silently stops folding.
- [ ] **Step 6: Run `[assets]` — expect PASS.**
- [ ] **Step 7: Full `~[gpu]` suite — green.** Delta: **+3 cases**. Commit — `feat(engine): imported-mesh reference edges (DerivesFrom source, References slots)`

---

### Task 13: Embedded-texture extraction at discovery (spec §5.5, A4)

A `.glb`'s embedded images become loose `.png`s beside the source, which the ordinary texture cook path then picks up. **Extraction happens editor-side, at discovery**, because the cook spine is source-file-driven: an in-memory image has no registered source, no sidecar and no cook key, so it cannot ride the existing texture path at all (the comparison's Decision 10 records why our divergence from UE's in-memory decode is forced, not stylistic).

**Files:**
- Create: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/GltfSurvey.hpp/.cpp`
- Create: `ArcaneEditor/src/Project/MeshImportWave.hpp/.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp` (`PollAssetWatch`'s discovery block), `premake5.lua` (the new editor TU in `ArcaneTests`' EXPLICIT list)
- Test: `ArcaneTests/src/MeshImportWaveTest.cpp` (new; globbed)

**Interfaces:**

```cpp
    // GltfSurvey.hpp -- everything the EDITOR needs to know about a glTF that is not
    // geometry. Lives in ArcaneAssetPipeline so cgltf stays inside ONE library (the
    // 08-21 placement rule); the editor links this library already, for CookSession.
    struct GltfImage
    {
        std::string            name;        // the glTF image or texture name; may be empty
        std::string            mimeType;    // "image/png", "image/jpeg", ...
        std::vector<std::byte> bytes;       // EMBEDDED images only -- empty for external
        bool                   embedded = false;
        std::string            uri;         // external images only, as authored
    };

    struct GltfMaterial
    {
        std::string              name;                 // the SLOT name (R3's key)
        float                    baseColorFactor[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
        int                      baseColorImage = -1;  // index into GltfSurvey::images
        // s6: every PBR input this engine has no destination for, named. ONE warn per
        // file per material is emitted from this list by the editor -- the importer
        // does not log, so a headless arccook run stays silent about editor concerns.
        std::vector<std::string> droppedInputs;
    };

    struct GltfSurvey
    {
        std::vector<GltfImage>    images;
        std::vector<GltfMaterial> materials;
    };

    // Parses (and validates -- same mandatory gate) `sourceBytes` and reports its
    // images and materials. nullopt on any parse/validate failure, which the caller
    // treats as "the cook will refuse this anyway, say nothing extra here".
    [[nodiscard]] std::optional<GltfSurvey> SurveyGltf(
        std::span<const std::byte> sourceBytes, const std::filesystem::path& sourcePath);
```

```cpp
    // MeshImportWave.hpp -- the editor's pure helpers for the import wave. PURE by
    // design (no ImGui, no Project, no device) so ArcaneTests drives them directly --
    // the AssetPanelModel/CookQueue/ContentDiscovery pattern.
    // REMINDER: this .cpp must be added to ArcaneTests' EXPLICIT editor TU list in the
    // root premake5.lua, or the test will not link.

    // `dir`/`stem``ext`, suffixed "-1", "-2", ... until the name is free on disk.
    // A4's collision rule, shared by extracted textures, minted companions and minted
    // materials -- one definition, because three near-copies is how two of them drift.
    [[nodiscard]] std::filesystem::path UniqueSiblingPath(
        const std::filesystem::path& dir, const std::string& stem, const std::string& ext);

    // A glTF image's file stem: its own name when it has one, else "<sourceStem>-<index>"
    // (A4's source-derived fallback -- a glTF may name no image at all). Sanitised to
    // filesystem-safe characters, because a glTF name is arbitrary UTF-8 and a '/' in
    // one would silently write outside the intended folder.
    [[nodiscard]] std::string ImageFileStem(const std::string& imageName,
                                            const std::string& sourceStem, std::size_t index);
```

- [ ] **Step 1: Write the failing wave test** (`MeshImportWaveTest.cpp`):

```cpp
TEST_CASE("import wave: UniqueSiblingPath never clobbers an existing file", "[editor]")
{
    const fs::path dir = TempDir("unique_sibling");
    CHECK(UniqueSiblingPath(dir, "albedo", ".png") == dir / "albedo.png");
    Touch(dir / "albedo.png");
    CHECK(UniqueSiblingPath(dir, "albedo", ".png") == dir / "albedo-1.png");
    Touch(dir / "albedo-1.png");
    CHECK(UniqueSiblingPath(dir, "albedo", ".png") == dir / "albedo-2.png");
}

TEST_CASE("import wave: an unnamed glTF image falls back to a source-derived name",
          "[editor]")
{
    CHECK(ImageFileStem("albedo", "prop", 0) == "albedo");
    CHECK(ImageFileStem("", "prop", 2) == "prop-2");
    // Arbitrary UTF-8 is sanitised: a separator in a glTF name must never steer the
    // write out of the folder it was meant for.
    CHECK(ImageFileStem("../../evil", "prop", 0).find('/') == std::string::npos);
    CHECK(ImageFileStem("../../evil", "prop", 0).find('\\') == std::string::npos);
}

TEST_CASE("import wave: an embedded texture extracts ONCE and is never overwritten"
          " (A4)", "[editor]")
{
    // s5.5 as amended: extraction happens only when the destination is ABSENT. A
    // user's edited or replaced .png survives every re-import -- the same
    // import-never-overwrites invariant s6 states for materials, which A4 exists to
    // extend to the equally user-visible extracted image.
    const fs::path dir = TempDir("extract_once");
    fs::copy_file(fs::path("data") / "gltf" / "embedded_tex.glb", dir / "embedded_tex.glb");

    const auto first = ExtractEmbeddedTextures(dir / "embedded_tex.glb");
    REQUIRE(first.size() == 1u);
    CHECK(fs::exists(first[0]));

    // The user edits the extracted file.
    WriteBytes(first[0], std::vector<std::uint8_t>{ 0xDE, 0xAD, 0xBE, 0xEF });

    const auto second = ExtractEmbeddedTextures(dir / "embedded_tex.glb");
    CHECK(second.empty());                                   // nothing re-extracted
    CHECK(ReadBytes(first[0]) == std::vector<std::uint8_t>{ 0xDE, 0xAD, 0xBE, 0xEF });
}

TEST_CASE("import wave: two glb files embedding the same image name do not collide",
          "[editor]")
{
    // Both .glb files embed an image named "albedo". A4's collision rule means the
    // second lands at albedo-1.png -- never overwriting the first, and never handing
    // two different meshes the same texture asset.
    /* Executor: copy embedded_tex.glb twice under different names into one folder;
       extract both; assert two distinct .png paths exist with different bytes. */
}

TEST_CASE("import wave: a glb with no embedded images extracts nothing, quietly",
          "[editor]")
{
    /* Executor: single.glb -> the returned list is empty and no file is written. */
}
```

- [ ] **Step 2: Run — expect FAIL.**
- [ ] **Step 3: Implement `SurveyGltf`.** Same parse + validate front half as `ImportMesh` (**reuse its RAII guard — extract the guard into a small file-local header shared by the two TUs rather than writing a second one**). Images: walk `data->images`, capturing name/mime/uri and, for an embedded one, the `buffer_view` slice (or the decoded `data:` payload) as bytes. Materials: walk `data->materials`, capturing `name`, `pbr_metallic_roughness.base_color_factor`, the `base_color_texture`'s image index, and building `droppedInputs` from **exactly §6's list**: metallic/roughness factors and texture, normal texture, occlusion texture, emissive factor and texture, `COLOR_0` vertex colours (the fixed vertex layout), `double_sided` and `alpha_mode` (backface cull and opaque are baked into the mesh path, `MeshNode.hpp:92-98`), and glTF sampler settings (one immutable trilinear sampler by F2b design, `MeshNode.hpp:414-430`). **Each entry is added only when the material actually SETS it** — a list naming inputs the file never used would train users to ignore the warning.
- [ ] **Step 4: Implement `UniqueSiblingPath` and `ImageFileStem`** in `MeshImportWave.cpp`, plus `ExtractEmbeddedTextures(const fs::path& source)`: survey, then per embedded image compute its destination (`source.parent_path()`, `ImageFileStem`, the extension implied by `mimeType` — `.png`/`.jpg`), **skip when the destination already exists** (A4's no-overwrite half, before any write), otherwise write the bytes and collect the path. Returns only the paths it actually WROTE, which is what makes the second call's empty return the test's proof.
- [ ] **Step 5: Add `MeshImportWave.cpp` to `ArcaneTests`' explicit editor TU list** in the root `premake5.lua` (beside `ContentDiscovery.cpp`, with a comment in the same house form naming the arc, the task, and the "pure logic, no ImGui" reason). `GenerateProjects.bat`.
- [ ] **Step 6: Call it from discovery.** In `PollAssetWatch`'s discovery block, a newly discovered `.gltf`/`.glb` runs `ExtractEmbeddedTextures` **before** `RegisterCreatedAsset` — so the extracted `.png`s are on disk when the same tick's discovery pass sweeps the folder, and both the model and its textures register in ONE poll interval (the same one-interval property the existing comment claims for drop → registered → cook-triggered). Push one `AssetActivityKind::Created` feed entry per extracted texture, and one `ARC_INFO` naming the count.
- [ ] **Step 7: Run `[editor]` — expect PASS.**
- [ ] **Step 8: Full `~[gpu]` suite — green.** Delta: **+5 cases**. Commit — `feat(editor): extract a .glb's embedded textures at discovery, never overwriting`

---

### Task 14: The companion `.arcmesh` mint + slot reconciliation (spec §4.2, R3)

After the FIRST successful cook — the moment authoritative slot names exist — the editor mints a companion `.arcmesh` beside the source. Re-cooks reconcile slots **by name**.

**Files:**
- Modify: `ArcaneEditor/src/Project/MeshImportWave.hpp/.cpp` (`ReconcileSlots`), `ArcaneEditor/src/App/EditorAppProject.cpp` (`OnCookCompleted`), `ArcaneEditor/src/App/EditorApp.hpp` (the mint declaration)
- Test: `ArcaneTests/src/MeshImportWaveTest.cpp` (extend)

**Interfaces:**

```cpp
    // MeshImportWave.hpp -- R3's reconciliation, PURE so it is testable without a
    // project, a registry or a cook.
    struct SlotReconciliation
    {
        std::vector<Arcane::MeshSlot> slots;      // the new slot array to write
        std::vector<std::string>      warnings;   // one per vanished-but-kept name
    };

    // `existing` is the companion's current slots; `authoritative` is the freshly
    // cooked artifact's slot names (SlotNamesFromSections' answer, in slot order).
    //
    // THE RULES, from R3 / s4.2, and they are UE's own (FbxStaticMeshImport.cpp:
    // 1946-1974, read rather than inferred):
    //   * match by NAME -- an existing slot whose name appears in `authoritative`
    //     keeps its material assignment, wherever it moved to;
    //   * APPEND unmatched authoritative names, in authoritative order;
    //   * NEVER DELETE -- an existing slot whose name vanished is KEPT and WARNED.
    //     Deleting a user's material assignment over a re-export hiccup is worse than
    //     carrying a harmless orphan; UE keeps it SILENTLY, and keep-and-WARN is a
    //     strict improvement on that (the comparison's Decision 3 says so in as many
    //     words);
    //   * POSITION is the tiebreak for unnamed slots and duplicate names -- the case
    //     A1's by-name dedup makes rare but cannot make impossible (two DISTINCT glTF
    //     materials may both be unnamed).
    [[nodiscard]] SlotReconciliation ReconcileSlots(
        const std::vector<Arcane::MeshSlot>& existing,
        const std::vector<std::string>& authoritative);
```

- [ ] **Step 1: Write the failing reconciliation test** (append to `MeshImportWaveTest.cpp`):

```cpp
TEST_CASE("slot reconciliation: a re-export that REORDERS materials keeps assignments",
          "[editor]")
{
    // THE CASE R3 EXISTS FOR. Positional slots would silently swap the two materials
    // here, and the user would find their prop repainted after a re-export.
    const Guid metal = Guid::Generate(), paint = Guid::Generate();
    const std::vector<MeshSlot> existing = { { "Metal", metal }, { "Paint", paint } };
    const SlotReconciliation r = ReconcileSlots(existing, { "Paint", "Metal" });
    REQUIRE(r.slots.size() == 2u);
    CHECK(r.slots[0].name == "Paint");
    CHECK(r.slots[0].material == paint);
    CHECK(r.slots[1].name == "Metal");
    CHECK(r.slots[1].material == metal);
    CHECK(r.warnings.empty());
}

TEST_CASE("slot reconciliation: a new material appends; a vanished one is KEPT + warned",
          "[editor]")
{
    const Guid metal = Guid::Generate(), old = Guid::Generate();
    const std::vector<MeshSlot> existing = { { "Metal", metal }, { "Retired", old } };
    const SlotReconciliation r = ReconcileSlots(existing, { "Metal", "Trim" });
    // Metal keeps its assignment; Trim appends UNASSIGNED; Retired survives.
    REQUIRE(r.slots.size() == 3u);
    CHECK(r.slots[0].material == metal);
    CHECK(r.slots[1].name == "Trim");
    CHECK_FALSE(r.slots[1].material.IsValid());
    CHECK(r.slots[2].name == "Retired");
    CHECK(r.slots[2].material == old);
    REQUIRE(r.warnings.size() == 1u);
    CHECK(r.warnings[0].find("Retired") != std::string::npos);
}

TEST_CASE("slot reconciliation: unnamed and duplicate names fall back to POSITION",
          "[editor]")
{
    // Two DISTINCT unnamed glTF materials are the case A1's by-name dedup makes rare
    // but cannot make impossible. Positional tiebreak, UE's own fallback.
    const Guid a = Guid::Generate(), b = Guid::Generate();
    const std::vector<MeshSlot> existing = { { "", a }, { "", b } };
    const SlotReconciliation r = ReconcileSlots(existing, { "", "" });
    REQUIRE(r.slots.size() == 2u);
    CHECK(r.slots[0].material == a);
    CHECK(r.slots[1].material == b);
    CHECK(r.warnings.empty());
}

TEST_CASE("slot reconciliation: a first mint takes the authoritative names verbatim",
          "[editor]")
{
    const SlotReconciliation r = ReconcileSlots({}, { "Metal", "Paint" });
    REQUIRE(r.slots.size() == 2u);
    CHECK(r.slots[0].name == "Metal");
    CHECK_FALSE(r.slots[0].material.IsValid());
    CHECK(r.warnings.empty());
}
```

- [ ] **Step 2: Run — expect FAIL.**
- [ ] **Step 3: Implement `ReconcileSlots`** per the four rules. Consume matched existing entries so a duplicate name matches each existing slot at most once (which is what makes the positional case come out right rather than assigning both to the first).
- [ ] **Step 4: Implement `EditorApp::MintOrUpdateCompanionMesh(const Guid& modelGuid)`** in `EditorAppProject.cpp`, in the `MintOrReuseSpriteForTexture` lineage (that function's own reuse-or-mint scan is the structural model, and its comments about registering immediately and `MarkAllDirty` carry over):
  1. Resolve `modelGuid` to its source path; read its CURRENT mesh artifact through `Assets::MeshArtifactFor` — **null means the cook has not landed, so do nothing and return**; this function is only ever called from a cook-completion callback, so that is not an error.
  2. Compute `authoritative = SlotNamesFromSections(artifact.sections)`. **The pipeline's `SlotNamesFromSections` is not reachable from the editor's mesh path** — the artifact the editor holds is a `LoadedClientMesh` — so add the same two-line derivation as a `MeshImportWave.hpp` helper over `MeshSectionView`, with a comment naming the pipeline function it mirrors.
  3. Look for an EXISTING companion: a registered `.arcmesh` whose `importedSource == modelGuid` (a registry scan, exactly the shape `MintOrReuseSpriteForTexture` uses, with the same "registries are small today" note). **Zero or several → mint a fresh sibling; exactly one → update it.** Never guess among duplicates — the same rule, for the same reason.
  4. Mint: `UniqueSiblingPath(source.parent_path(), source.stem(), ".arcmesh")`, `source = Imported`, `importedSource = modelGuid`, `slots = ReconcileSlots({}, authoritative).slots`, `name = the path's stem`; `SaveMeshAsset`; `RegisterCreatedAsset` **checked** (an unregistered mint hands back a guid that can never resolve — `MintOrReuseSpriteForTexture`'s own account of why the return is checked); `m_assetModel.MarkAllDirty()`; one `AssetActivityKind::Created` feed entry.
  5. Update: `LoadMeshAsset`, `ReconcileSlots(existing.slots, authoritative)`, write back only when the slots actually CHANGED (a no-op re-cook must not rewrite the file and re-trigger the watcher — the self-save feedback loop `PollAssetWatch`'s material branch already guards against), emit each warning as one `ARC_WARN` naming the mesh and the vanished slot.
- [ ] **Step 5: Call it from `OnCookCompleted`.** In the `cookedGuids` loop, for a guid whose registered path classifies as `AssetKind::Model`, call `MintOrUpdateCompanionMesh(guid)`. **Placement matters:** after `InvalidateArtifact`/`InvalidateMeshArtifact` for that guid (so step 4.1's `MeshArtifactFor` reads the FRESH artifact, not the memo the cook just invalidated) and inside the same `if (m_runtime)` block. Add the mesh-side invalidation calls here too — `AssetsFacade().InvalidateMeshArtifact(guid)` beside the existing `InvalidateArtifact(guid)`; the RENDER-side mesh invalidation is Plan 2 Task 6's and gets its own line there.
- [ ] **Step 6: Run `[editor]` — expect PASS.**
- [ ] **Step 7: Desk check.** Copy `ArcaneTests/data/gltf/multi.glb` into `ReferenceProject/Content/meshes/`, launch the editor, wait for the watcher: the `.glb` registers, cooks, and a `multi.arcmesh` appears beside it with two slots named `Metal` and `Paint`, both unassigned. Then re-save the `.glb` (touch it) and confirm the companion is NOT rewritten (unchanged mtime). **Remove the fixture and the minted companion afterwards** — ReferenceProject's own imported-mesh fixture is Plan 2 Task 11's, deliberately, and a stray one here would change the golden lane a plan early. Record both observations in the commit body.
- [ ] **Step 8: Full `~[gpu]` suite — green.** Delta: **+4 cases**. Commit — `feat(editor): mint and reconcile the companion .arcmesh after a mesh cook`

---

### Task 15: Material minting — reuse by name, else instance the import base (spec §6, R4)

Per glTF material, in order: **reuse-by-name** first; else mint an INSTANCE of a shared import parent. **Import never overwrites an existing material asset.**

**Files:**
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp` (the mint), `ArcaneEditor/src/Project/MeshImportWave.hpp/.cpp` (the pure name-matching half)
- Test: `ArcaneTests/src/MeshImportWaveTest.cpp` (extend)

**Interfaces:**

```cpp
    // MeshImportWave.hpp -- the pure half of R4's step (1). Given the registry's
    // material assets (guid + mount path + surface) and a glTF material name, which
    // existing asset should the slot point at?
    //
    // R4's rule, and the shape UE takes (FbxImportUI.h:188-194, FbxMaterialImport.cpp:
    // 571/624-689): reuse an existing MESH-surface material whose STEM equals the glTF
    // material name. EXACTLY ONE match reuses; zero or several mint fresh -- the same
    // never-guess-among-duplicates rule MintOrReuseSpriteForTexture already keeps, and
    // for the same reason: picking one of two identically-named materials would be a
    // coin flip the user cannot see.
    struct MaterialCandidate { Arcane::Guid guid; std::string stem; bool meshSurface = false; };

    [[nodiscard]] Arcane::Guid FindReusableMeshMaterial(
        std::span<const MaterialCandidate> candidates, const std::string& gltfMaterialName);
```

**The shared parent (§6):** the first import mints `Content/mesh_import_base.arcmat` — `MaterialSurface::Mesh`, `baseColor` white, `albedo` nil — at that FIXED path; every later import finds and reuses it. `CreateMaterialAt(path, MaterialSurface::Mesh)` already produces exactly that file (`EditorAppProject.cpp:1186-1204` writes `kind = "mesh"` plus the two params and deliberately no snippet/graph), so the base mint is a call to it, not new code.

**Minted instances** set `parent` = the base's guid and sparse overrides `baseColor` = `baseColorFactor` and, when a base-colour texture exists, `albedo` = the extracted/registered texture's guid. Arcane's parent + sparse-override chain does the rest with zero new machinery — the 08-22 research's A1 ("it IS `UMaterialInstance`").

**Create-invariant note, pre-empting review:** these are companion mints in the `MintOrReuseSpriteForTexture` lineage — registry/import-time automation. The "no creation path may bypass `CreateAssetRequest`" invariant governs the USER DIALOG path and is untouched. §6 states this; repeat it at the call site so a later reviewer does not have to re-derive it.

- [ ] **Step 1: Write the failing reuse test** (append to `MeshImportWaveTest.cpp`):

```cpp
TEST_CASE("material reuse: exactly one same-named mesh material is reused", "[editor]")
{
    const Guid metal = Guid::Generate();
    const std::vector<MaterialCandidate> candidates = {
        { metal, "Metal", true },
        { Guid::Generate(), "Paint", true },
    };
    CHECK(FindReusableMeshMaterial(candidates, "Metal") == metal);
    CHECK_FALSE(FindReusableMeshMaterial(candidates, "Trim").IsValid());
}

TEST_CASE("material reuse: a SPRITE-surface material of the same name is not reused",
          "[editor]")
{
    // A mesh slot pointing at a sprite material would resolve to nothing at draw time
    // (MeshMaterialCache reads baseColor/albedo off a mesh-kind chain). The surface is
    // part of the match, not a detail.
    const std::vector<MaterialCandidate> candidates = {
        { Guid::Generate(), "Metal", /*meshSurface*/ false },
    };
    CHECK_FALSE(FindReusableMeshMaterial(candidates, "Metal").IsValid());
}

TEST_CASE("material reuse: two same-named candidates reuse NEITHER", "[editor]")
{
    // Never guess among duplicates -- MintOrReuseSpriteForTexture's own rule.
    const std::vector<MaterialCandidate> candidates = {
        { Guid::Generate(), "Metal", true }, { Guid::Generate(), "Metal", true },
    };
    CHECK_FALSE(FindReusableMeshMaterial(candidates, "Metal").IsValid());
}

TEST_CASE("material reuse: an UNNAMED glTF material never matches anything", "[editor]")
{
    // An empty name would otherwise match every candidate with an empty stem, which
    // no real asset has -- but the guard is what makes that a rule rather than luck.
    const std::vector<MaterialCandidate> candidates = { { Guid::Generate(), "", true } };
    CHECK_FALSE(FindReusableMeshMaterial(candidates, "").IsValid());
}
```

- [ ] **Step 2: Run — expect FAIL.**
- [ ] **Step 3: Implement `FindReusableMeshMaterial`** — empty name → nil; count `meshSurface && stem == name` matches; return the guid on exactly one, nil otherwise.
- [ ] **Step 4: Implement `EditorApp::EnsureMeshImportBaseMaterial()`** — resolve `Content/mesh_import_base.arcmat` under the project root; if a registered asset already sits there, return its guid; else `CreateMaterialAt(thatPath, MaterialSurface::Mesh)`. **The fixed path is the identity** (§6 says "at that fixed path"), so this never uniquifies and never overwrites — an existing file is REUSED, which is the whole point.
- [ ] **Step 5: Implement `EditorApp::MintImportMaterials(const Guid& modelGuid, const GltfSurvey& survey)`**, returning a `name → Guid` map:
  - Build `MaterialCandidate`s from the registry (`AssetKindOf == Material` + `Assets::MaterialSurfaceFor(guid) == MaterialSurface::Mesh` — the v22 accessor exists for exactly this shape of question).
  - Per glTF material: `FindReusableMeshMaterial` → reuse and CONTINUE (**nothing is created; §6's step 1**). Else mint an instance at `UniqueSiblingPath(source.parent_path(), material.name.empty() ? sourceStem + "-mat" : material.name, ".arcmat")` with `parent` = the base, `params` = `baseColor` (from `baseColorFactor`) and, when `baseColorImage >= 0`, `albedo` = the guid the extracted `.png` registered under (Task 13 wrote it; resolve it from the registry by path). `RegisterCreatedAsset` checked; `MarkAllDirty`; one activity entry.
  - **`droppedInputs`: one `ARC_WARN` per file, per material**, naming every entry — §6's exact wording. Not one per input, which would be a wall; not one per file, which would lose which material dropped what.
  - **THE INVARIANT: never touch an existing `.arcmat`.** The reuse arm writes nothing; the mint arm goes through `UniqueSiblingPath`, so it cannot land on one. Assert this in a comment; it is what makes "user edits to minted materials are permanent" true.
- [ ] **Step 6: Wire it into the companion mint.** In `MintOrUpdateCompanionMesh` (Task 14), after reconciliation: for every slot with a NIL material whose name has an entry in `MintImportMaterials`' map, assign it. **A slot the user already assigned is never reassigned** — that is the same never-overwrite rule one level up, and it is what makes a re-cook safe.
- [ ] **Step 7: Run `[editor]` — expect PASS.**
- [ ] **Step 8: Desk check.** Re-run Task 14's Step-7 desk check with `embedded_tex.glb`: the `.png` extracts, registers and cooks; `mesh_import_base.arcmat` appears once at `Content/`; one instance material appears named after the glTF material with `albedo` pointing at the extracted texture; the companion's slot points at it; the log carries exactly one dropped-inputs WARN naming the metallic/roughness/normal set. Drop the SAME file a second time under a different name and confirm the base is reused (still one file) and the extracted `.png` is not overwritten. **Clean the fixtures out afterwards** (Plan 2 Task 11 owns ReferenceProject's fixture). Paste the log lines into the commit body.
- [ ] **Step 9: Full `~[gpu]` suite — green.** Delta: **+4 cases**. Commit — `feat(editor): mint import materials by reuse-or-instance, dropping unmapped inputs loudly`

---

### Task 16: Plan 1 close — end-to-end, suites, and the handoff to Plan 2

- [ ] **Step 1: End-to-end, headless.** In a scratch copy of ReferenceProject, drop `multi.glb` and `nested.gltf`+`nested.bin` into `Content/meshes/`, mint their sidecars by hand (a headless `arccook` run on a never-opened project cooks GEOMETRY ONLY — §5.5 says so and it is correct, not a gap), then `arccook --project <copy> --verbose`: both cook `OK`, exit 0; `--check` → exit 0. Then edit one byte of `nested.bin` and `--check` → **exit 2 (stale)**, which is §5.4's whole point observed at the CLI. Record the four verdicts.
- [ ] **Step 2: Zero-legacy sweep** (path-exclude + `-riw`): `EnumerateTextureSources`, `SetImporterForTesting`, `DiscoverUnknownTextureSources`, `EnumerateContentPngFiles`, `MeshAssetData::material` across `ArcaneClient ArcaneEditor ArcaneAssetPipeline ArcaneTests arccook scripts` — hits only in docs/specs history and in the deliberately-kept legacy-key READ arm of `LoadMeshAsset`.
- [ ] **Step 3: `CGLTF_IMPLEMENTATION` sweep** — `grep -rn "CGLTF_IMPLEMENTATION" --include=*.cpp --include=*.hpp .` returns exactly `CgltfImpl.cpp`.
- [ ] **Step 4: Placement-rule sweep** — no line inside `premake5.lua`'s `ArcaneClient` project block names `cgltf` or `meshoptimizer`, and `grep -rn "cgltf\|meshoptimizer" ArcaneClient/src` is empty. This is the 08-21 rule, and it is checked, not assumed.
- [ ] **Step 5: Full suites, Debug AND Release, from the exe dir.** Derive the final counts and attribute every delta against the **55464 / 1535** baseline to the named per-task adds: T1 +4, T2 +3, T3 +6, T4 +3, T5 +4, T6 +6, T7 +7, T8 +6, T9 +4, T10 +5, T11 +5, T12 +3, T13 +5, T14 +4, T15 +4 = **+69 cases**. If the derived number differs, find out which task's count was wrong and say so — **derive, never recall.** Ledger both runs with their seed banners.
- [ ] **Step 6: Hand off.** Plan 2 begins at this commit. State in the handoff note: the ABI is now **24**; `ReferenceProject.arcproj` is restamped and `ReferenceProject.slnx` rebuilt; **Gacha's Game-module rebuild debt is one deeper (recorded, not actioned)**; `CollectMeshInstances` still emits one instance per entity resolving through `slots[0]` (Plan 2 Task 5 makes it per-section); `AssetKind::Model` has no graph hue and no browser fold yet (Plan 2 Task 8); imported meshes resolve to `MeshData` on the CPU but nothing uploads them (Plan 2 Tasks 1-5).
- [ ] **Step 7: Commit** — `docs: F2c plan 1 closeout notes` (only if any doc changed; otherwise this task ends with no commit of its own, which is fine and should be said in the handoff).

---

## Self-review record (run at authoring time)

**Spec coverage — every clause to a task:**

| Spec | Task |
|---|---|
| §1 (glTF-only, zero conversion; cgltf + meshoptimizer) | T1 (vendor), T7 (the bake that needs no conversion) |
| §2 non-goals | Each recorded in the code comment its trigger names: Draco/basisu/meshopt-compression → T6 (the general `extensionsRequired` refusal covers all three); LOD + tangents → T1 Step 5 (vendored-dormant list) and T7 Step 5 (A3); 16-bit indices → T3 (`indexWidth`, A5); per-mesh split → T5 (`MeshMetaSettings`' own comment); per-section overrides → T10 Step 9; normalize-on-import REJECTED → T7 (no scale term exists to remove); **skinning / animation / morph targets → T7 pipeline step 2** (the bake loop's own comment: `skins`, `animations`, primitive `targets` and `JOINTS_0`/`WEIGHTS_0` are parsed-and-ignored, trigger "a consumer exists") |
| §2 ABI clause | T11 Step 7 |
| §3 R1 (one asset per file, TRS baked, primitive = section) | T7 |
| §3 R2 (residency) | **Plan 2** |
| §3 R3 (named slots) | T10 (schema), T14 (reconciliation) |
| §3 R4 (materials at import) | T15 |
| §3 R5 (thumbnails) | **Plan 2** |
| §3 R6 (cook spine) | T8 |
| §4.1 registry + `AssetKind::Model` | T9 |
| §4.2 companion `.arcmesh` | T10 (schema), T14 (mint + reconcile), T12 (DerivesFrom edge) |
| §4.3 bake / unit rule / sections / mirror rule | T7 (all four; the unit rule is the ABSENCE of a scale term, stated in T7's comment) |
| §4.4 slot array + resolution chain + classifier | T10 (schema + chain), T12 (classifier) |
| §4.5 validation + refusals + degenerate drop | T6 |
| §5.1 generalized spine | T8 |
| §5.2 mesh artifact | T3 (writer + pipeline reader), T4 (client mirror) |
| §5.3 MeshImporter | T6 (front), T7 (geometry) |
| §5.4 cook key + external buffers | T5 (unit), T8 (end-to-end), T16 (CLI) |
| §5.5 division of labor + extraction + A4 | T13 |
| §5.6 refusal/pending/engine seams + ABI | T11 |
| §6 materials at import + dropped inputs + create-invariant | T15 (mint), T13 (the `droppedInputs` survey) |
| §7.1 CPU resolution + pending-quiet/missing-loud | T11 |
| §7.2 / §7.3 / §7.4 | **Plan 2** |
| §8 editor surface | T9 (kind), **Plan 2** (hue, fold, thumbnails) |
| §9 fixture corpus + tests | T2 (corpus); the test list is spread T3–T15 as each capability lands |
| §9 golden-scene fixture + re-bless | **Plan 2** |
| §10 vendoring | T1 |
| §11 relationship to prior specs | No task — it is a statement about this spec, discharged by the tasks that fulfil F2a's seam (T10/T11) and extend F2b's disciplines (T3/T4/T8) |
| §12 hazards | Each lands where §12's table says: ring cliff → Plan 2; kind-closed cook machinery → T8; `RebuildIndexFromScan` → T8; unmapped PBR → T15; re-export shuffle → T14; two-primitives-one-material → T3/T7; external `.bin` → T5/T8; untrusted input → T1/T6; 2D/3D +Y → stated once in T7's bake comment; eviction → Plan 2 |

**Type consistency across tasks:** `MeshArtifactSection`/`MeshArtifactVertex`/`MeshArtifactDesc` are named in T3 and used unchanged through T7/T8. `MeshSection`/`MeshSlot`/`MeshData::sections` are named in T10 and consumed unchanged in T11 and Plan 2. `LoadedClientMesh`/`MeshSectionView` are named in T4 and are the type T11's `MeshArtifactSupplyFn` and Plan 2's cache both speak. `SlotNamesFromSections` exists in TWO places on purpose — the pipeline's (T3) and the editor's `MeshSectionView` mirror (T14 Step 4.2) — and T14 requires the mirror to name its peer. `ImportMesh`'s signature gains `externalBuffers` in T7, and T7 Step 7 explicitly updates T6's tests rather than leaving two shapes.

**Known intentional gaps, each with its owner:**
- **No per-slot material list UI.** `MeshDocument` edits `slots[0]` only (T10 Step 10); an imported mesh's second and later slots are hand-editable in the `.arcmesh` until the F4/post-F2c import-options UI that spec §8 parks. Recorded at the call site.
- **`CollectMeshInstances` stays one-instance-per-entity** through this plan (T10 Step 9), so the draw side is untouched until Plan 2 Task 5. This is what lets the schema move land green.
- **`AssetKind::Model` has no graph hue and no browser fold** until Plan 2 Task 8. Deliberate split per the arc brief; a Model node wears the theme's documented neutral fallback in the interim, not a wrong colour.
- **`arccook --dump-dds` stays texture-only** (T8 Step 9), refusing a mesh guid by name. A `--dump-obj` analogue is not in either plan and has no trigger yet.
- **No mesh thumbnail in the artifact.** The `Thumbnail` tag is reserved and unwritten (R5); the editor harvest is Plan 2 Task 9.
- **The `Tangents` tag is reserved and unwritten**, carrying A3's handedness obligation as a written comment (T7 Step 5) rather than a test — there is nothing to assert until something writes it, and the comment is what makes the mirrored fixture's future assertion findable.
