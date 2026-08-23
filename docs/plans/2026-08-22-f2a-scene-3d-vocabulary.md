# F2a — 3D Vocabulary in the Scene: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the scene produce meshes — authored, saved, reopened, rendered in both hosts — so Phase 4 Task 9 becomes implementable, with zero vendored libraries.

**Architecture:** A `.arcmesh` asset names a procedural generator plus topology parameters and a default material; a `MeshRenderer` component references it with an optional per-entity material override. `SceneRenderResolver` grows two caches (`MeshCache`, `MeshMaterialCache`) that publish `Registry` resources, exactly as its three sprite/post caches already do; a `MeshSubmissionSystem` sweeps those into the `std::vector<MeshInstance>` each host hands to `FrameDesc::mesh`. Generators emit **unit** geometry — size comes from `Transform::scale`.

**Tech Stack:** C++23, Astra ECS + reflection, NRI render graph, ImGui (editor), Catch2 + rapidcheck (gate), premake5/msbuild.

**Spec:** `docs/specs/2026-08-22-f2a-scene-3d-vocabulary-design.md`
**Research:** `docs/research/2026-08-22-mesh-asset-ue-source2-comparison.md`
**Predecessor:** F1 @ `b86c74f6`.

## Global Constraints

- **Unit geometry.** Generators emit unit-sized shapes. **Scale expresses size, rotation expresses orientation, the asset expresses shape.** No `sizeMeters`/`radiusMeters` anywhere.
- **Units are METERS (MKS)** throughout, matching `MeshBuilder`'s existing contract.
- **Winding: front-facing is COUNTER-CLOCKWISE as seen from outside the surface.** Every emitted triangle `(v0,v1,v2)` must satisfy `normalize(cross(v1.position - v0.position, v2.position - v0.position))` pointing the *same* way as its averaged vertex normal. `MeshBuilder.hpp` states this; the new generators inherit it. Getting it backwards makes a shape invisible under `CullMode::BACK`.
- **`MeshBuilder` stays PURE AND DEVICE-FREE** — no NRI, no `NriDevice`, no GPU type in it or its `.cpp`.
- **No new vendored libraries.** No cgltf, no meshoptimizer, no BC encoder. If a task seems to need one, it belongs to F2b or F2c — stop and escalate.
- **No tangents.** Tangent generation needs MikkTSpace, which is F2c's.
- **Gate baseline: Debug/Release 50034/1067 · Dist 49973/1062, 0 warnings, all three configs.** State the delta after every task.
- **Run the gate FROM THE EXE DIR** (`bin/<Config>-windows-x86_64-md/ArcaneTests/`), filtered `~[gpu]`. `[gpu]` cases are desk-run only.
- **Build:** `msbuild D:\dev\starworks\Arcane\Arcane.slnx /p:Configuration=<Debug|Release|Dist> /m` with `ARCANE_SDK=D:\dev\starworks\Arcane`. Never build a bare `.vcxproj`.
- **`EditorPanels.cpp`, `EditorApp.cpp` and `EditorAppFrame.cpp` are NOT compiled into ArcaneTests.** A green gate proves nothing about them — pure logic must live in a source-compiled TU or it has no coverage at all.
- **Plugin ABI 16 → 17** at Task 12, not before.

---

## What Phase 4 already did — do NOT rebuild it

Verified in the tree on 2026-08-22, because the Phase 4 plan's Task 9 overstates what is left:

- **`FrameDesc::mesh` ALREADY EXISTS** — `Render/Nri/NriGraphContext.hpp:416`, `const MeshSceneDesc* mesh = nullptr`, with its full borrowing contract documented.
- **`DeclareGraphFrame` ALREADY CONSUMES IT** — `NriGraphContext.cpp:1186` gates on `shape.mesh != nullptr && !shape.mesh->instances.empty()`, and `:1494` wires `shape.mesh = m_mesh ? frame.mesh : nullptr`.
- **The depth transient is ALREADY minted** inside `AddMeshNode` under that same gate (`:1194`).

So Phase 4 Task 9 reduces to **host wiring only** (Task 10 here). Its "Step 1: write the failing structural test" and "Step 3: add the fields and the gate" are already satisfied; `RenderGraphTest.cpp` pins the frame-shape cases.

---

## File Structure

**Create:**

| File | Responsibility |
|---|---|
| `ArcaneClient/src/Arcane/Mesh/MeshAsset.{hpp,cpp}` | `MeshAssetData`, `MeshSource`, save/load `.arcmesh`, `BuildMeshData` + per-source validation |
| `ArcaneClient/src/Arcane/Render/MeshCache.{hpp,cpp}` | Guid → owned `MeshData` + `MeshBounds`; publishes `MeshTable` |
| `ArcaneClient/src/Arcane/Render/MeshMaterialCache.{hpp,cpp}` | Guid → `ResolvedMeshMaterial`; publishes `MeshMaterialTable` |
| `ArcaneClient/src/Arcane/Scene/MeshSubmissionSystem.hpp` | Sweeps the scene into `std::vector<MeshInstance>` |
| `ArcaneEditor/src/Documents/MeshDocument.{hpp,cpp}` | `.arcmesh` editor + live preview |
| `ArcaneTests/src/MeshAssetTest.cpp` | Asset round-trip, generators, validation, bounds |
| `ArcaneTests/src/MeshSubmissionTest.cpp` | Submission sweep + material resolution chain |

**Modify:**

| File | Change |
|---|---|
| `ArcaneClient/src/Arcane/Render/MeshBuilder.{hpp,cpp}` | `BuildPlane`, `BuildCylinder`, `BuildCapsule`, `ComputeMeshBounds` |
| `ArcaneClient/src/Arcane/Scene/Components.hpp` | `MeshRenderer` + reflection |
| `ArcaneClient/src/Arcane/Scene/SceneModule.hpp` | Register `MeshRenderer` |
| `ArcaneClient/src/Arcane/Scene/SceneResources.hpp` | `MeshTable`, `MeshMaterialTable` |
| `ArcaneClient/src/Arcane/Material/MaterialTypes.hpp`, `MaterialSource.cpp`, `MaterialAsset.cpp` | `MaterialSurface::Mesh` + the `"mesh"` kind |
| `ArcaneClient/src/Arcane/Scene/SceneCamera.hpp` | Perspective camera honours pose |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.{hpp,cpp}` | Per-instance normal matrix |
| `data/shaders/mesh.hlsl` | Consume the normal matrix |
| `ArcaneClient/src/Arcane/Host/SceneRenderResolver.{hpp,cpp}` | Own + drive the two new caches |
| `ArcaneEditor/src/App/EditorApp.cpp` | `.arcmesh` document factory |
| `ArcaneEditor/src/Panels/AssetBrowser.cpp` | "Create Mesh" |
| `ArcaneRuntime/src/RuntimeFrame.cpp`, `ArcaneEditor/src/App/EditorAppFrame.cpp` | Populate `FrameDesc::mesh` |
| `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` | ABI 16 → 17 |
| `premake5.lua` | Source-compile `MeshDocument.cpp` into ArcaneTests |

---

## Task 1: `MeshBuilder` gains three generators and a bounds helper

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/MeshBuilder.hpp`, `MeshBuilder.cpp`
- Test: `ArcaneTests/src/MeshBuilderTest.cpp` (extend if present; create otherwise)

**Interfaces:**
- Consumes: `MeshVertex`, `MeshData`, `BuildCube`, `BuildUvSphere` (existing).
- Produces:
  ```cpp
  struct MeshBounds { glm::vec3 min{0.0f}; glm::vec3 max{0.0f}; };
  ARCANE_API MeshBounds ComputeMeshBounds(const MeshData& mesh);
  ARCANE_API MeshData BuildPlane(std::uint32_t subdivisions);
  ARCANE_API MeshData BuildCylinder(std::uint32_t segments);
  ARCANE_API MeshData BuildCapsule(std::uint32_t rings, std::uint32_t segments, float lengthRatio);
  ```

**Unit conventions — these are the contract, not suggestions:**
- `BuildPlane`: the **XZ** plane, normal `+Y`, spanning `[-0.5, +0.5]` in X and Z. `subdivisions` is quads per axis (1 = two triangles). A camera-facing quad is this asset under a −90° X rotation.
- `BuildCylinder`: diameter 1 (radius 0.5), height 1, axis **+Y**, spanning `[-0.5, +0.5]` in Y. Flat caps. `segments` is the radial count.
- `BuildCapsule`: diameter 1 (radius 0.5), axis **+Y**. `lengthRatio` is **total height / diameter**, so total height = `lengthRatio` and the cylindrical section length = `lengthRatio - 1`. At `lengthRatio == 1` it degenerates to a sphere, which is legal.

`ComputeMeshBounds` on an empty mesh returns `{ {0,0,0}, {0,0,0} }` rather than the inverted-infinity sentinel — a caller framing an empty mesh must get a usable (degenerate) box, not a NaN camera.

- [ ] **Step 1: Write the failing tests**

Create/extend `ArcaneTests/src/MeshBuilderTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Arcane/Render/MeshBuilder.hpp>

#include <glm/geometric.hpp>

using namespace Arcane;
using Catch::Matchers::WithinAbs;

namespace
{
    // The winding contract from MeshBuilder.hpp, as a predicate: for every
    // triangle, the geometric normal must agree with the averaged vertex
    // normal. A generator that emits a face backwards is INVISIBLE under
    // CullMode::BACK, and nothing else in the suite would notice.
    bool WindingIsOutward(const MeshData& m)
    {
        for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3)
        {
            const MeshVertex& a = m.vertices[m.indices[i + 0]];
            const MeshVertex& b = m.vertices[m.indices[i + 1]];
            const MeshVertex& c = m.vertices[m.indices[i + 2]];
            const glm::vec3 geo = glm::cross(b.position - a.position, c.position - a.position);
            if (glm::length(geo) < 1e-12f)
                return false;                       // a degenerate triangle is a bug too
            const glm::vec3 avg = a.normal + b.normal + c.normal;
            if (glm::dot(glm::normalize(geo), glm::normalize(avg)) <= 0.0f)
                return false;
        }
        return true;
    }

    MeshBounds BoundsOf(const MeshData& m) { return ComputeMeshBounds(m); }
}

TEST_CASE("BuildPlane is a unit XZ plane with a +Y normal", "[mesh]")
{
    const MeshData p = BuildPlane(1);
    REQUIRE(p.indices.size() == 6);          // one quad
    REQUIRE(p.vertices.size() == 4);

    const MeshBounds b = BoundsOf(p);
    CHECK_THAT(b.min.x, WithinAbs(-0.5f, 1e-6f));
    CHECK_THAT(b.max.x, WithinAbs( 0.5f, 1e-6f));
    CHECK_THAT(b.min.z, WithinAbs(-0.5f, 1e-6f));
    CHECK_THAT(b.max.z, WithinAbs( 0.5f, 1e-6f));
    CHECK_THAT(b.min.y, WithinAbs(0.0f, 1e-6f));   // flat: zero thickness
    CHECK_THAT(b.max.y, WithinAbs(0.0f, 1e-6f));

    for (const MeshVertex& v : p.vertices)
    {
        CHECK_THAT(v.normal.y, WithinAbs(1.0f, 1e-6f));
    }
    CHECK(WindingIsOutward(p));

    // Subdivision multiplies quads per AXIS, so 3 -> 9 quads -> 54 indices.
    const MeshData p3 = BuildPlane(3);
    CHECK(p3.indices.size() == 54);
    CHECK(WindingIsOutward(p3));
}

TEST_CASE("BuildCylinder is a unit-diameter, unit-height Y-axis cylinder", "[mesh]")
{
    const MeshData c = BuildCylinder(16);
    const MeshBounds b = BoundsOf(c);
    CHECK_THAT(b.min.y, WithinAbs(-0.5f, 1e-6f));
    CHECK_THAT(b.max.y, WithinAbs( 0.5f, 1e-6f));
    CHECK_THAT(b.max.x, WithinAbs( 0.5f, 1e-4f));   // radius 0.5, sampled at 16 segments
    CHECK_THAT(b.min.x, WithinAbs(-0.5f, 1e-4f));
    CHECK(WindingIsOutward(c));
}

TEST_CASE("BuildCapsule's total height IS its length ratio", "[mesh]")
{
    // lengthRatio 2 -> total height 2 (Unity's capsule proportions), diameter 1.
    const MeshData c = BuildCapsule(8, 16, 2.0f);
    const MeshBounds b = BoundsOf(c);
    CHECK_THAT(b.max.y - b.min.y, WithinAbs(2.0f, 1e-4f));
    CHECK_THAT(b.max.x, WithinAbs(0.5f, 1e-3f));
    CHECK(WindingIsOutward(c));

    // ratio 1 degenerates to a sphere -- LEGAL, and the height must collapse
    // to the diameter rather than going negative.
    const MeshData s = BuildCapsule(8, 16, 1.0f);
    const MeshBounds sb = BoundsOf(s);
    CHECK_THAT(sb.max.y - sb.min.y, WithinAbs(1.0f, 1e-4f));
    CHECK(WindingIsOutward(s));
}

TEST_CASE("ComputeMeshBounds covers every vertex, and survives an empty mesh", "[mesh]")
{
    const MeshData cube = BuildCube(1.0f);
    const MeshBounds b = ComputeMeshBounds(cube);
    for (const MeshVertex& v : cube.vertices)
    {
        CHECK(v.position.x >= b.min.x - 1e-6f);
        CHECK(v.position.x <= b.max.x + 1e-6f);
        CHECK(v.position.y >= b.min.y - 1e-6f);
        CHECK(v.position.y <= b.max.y + 1e-6f);
        CHECK(v.position.z >= b.min.z - 1e-6f);
        CHECK(v.position.z <= b.max.z + 1e-6f);
    }

    // Zero, not an inverted-infinity sentinel: a caller framing an empty mesh
    // must get a degenerate box it can still build a camera from.
    const MeshBounds empty = ComputeMeshBounds(MeshData{});
    CHECK_THAT(empty.min.x, WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(empty.max.x, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("the existing generators still satisfy the winding contract", "[mesh]")
{
    // A regression net for Task 1's edits -- these two shipped in Phase 4 and
    // their winding is what the [pixel] case already proved on the GPU.
    CHECK(WindingIsOutward(BuildCube(1.0f)));
    CHECK(WindingIsOutward(BuildUvSphere(0.5f, 8, 16)));
}
```

- [ ] **Step 2: Run the tests and confirm they fail**

```bat
msbuild D:\dev\starworks\Arcane\Arcane.slnx /p:Configuration=Debug /m
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[mesh]"
```
Expected: compile error — `BuildPlane`, `BuildCylinder`, `BuildCapsule`, `ComputeMeshBounds`, `MeshBounds` are undeclared.

- [ ] **Step 3: Declare the new surface in `MeshBuilder.hpp`**

Append inside `namespace Arcane`, after `BuildUvSphere`:

```cpp
    // Local axis-aligned bounds of a mesh, in the mesh's own space.
    //
    // F3 owns culling; this exists in F2a because MeshDocument's preview has
    // to FRAME the mesh, and it is computed from vertices rather than derived
    // per-source analytically because F2c's imported meshes need that path
    // regardless. An EMPTY mesh returns a zero box, not the inverted-infinity
    // sentinel a min/max fold starts from -- a caller framing an empty mesh
    // needs a degenerate box it can still build a camera from, and the
    // sentinel would hand it infinities.
    struct MeshBounds
    {
        glm::vec3 min{0.0f, 0.0f, 0.0f};
        glm::vec3 max{0.0f, 0.0f, 0.0f};
    };

    [[nodiscard]] ARCANE_API MeshBounds ComputeMeshBounds(const MeshData& mesh);

    // ---- UNIT generators (F2a) ------------------------------------------
    // Every generator below emits a UNIT shape. Size is the Transform's job
    // (spec: "scale expresses size, rotation expresses orientation, the asset
    // expresses shape"), which is why none of them takes a size in meters the
    // way BuildCube/BuildUvSphere above do -- those two predate the rule and
    // keep their parameters because .arcmesh always passes 1.0 / 0.5.

    // The unit XZ plane: normal +Y, spanning [-0.5, +0.5] in X and Z.
    // `subdivisions` is quads PER AXIS, so 1 is a single quad (two triangles)
    // and 3 is nine quads. A camera-facing quad is this mesh under a -90 degree
    // X rotation -- orientation is the Transform's job for the same reason
    // size is, which is why there is no separate Quad source.
    [[nodiscard]] ARCANE_API MeshData BuildPlane(std::uint32_t subdivisions);

    // Unit cylinder: diameter 1, height 1, axis +Y, spanning [-0.5, +0.5] in Y,
    // with flat caps. `segments` is the radial count.
    //
    // It takes NO length ratio, and that asymmetry with BuildCapsule is the
    // point: a cylinder scaled non-uniformly in Y is still a correct cylinder
    // (its caps are flat discs; nothing distorts), so every cylinder in the
    // family is reachable from this one by scale alone.
    [[nodiscard]] ARCANE_API MeshData BuildCylinder(std::uint32_t segments);

    // Unit-diameter capsule: radius 0.5, axis +Y. `lengthRatio` is TOTAL HEIGHT
    // divided by diameter, so total height == lengthRatio and the cylindrical
    // section is (lengthRatio - 1) long. At exactly 1 the section vanishes and
    // this is a sphere -- legal, and pinned as such.
    //
    // The ratio exists where BuildCylinder needs none because a capsule scaled
    // in Y is NOT a capsule: its hemispherical caps become ellipsoids. That is
    // the test any future shape parameter must pass -- name a family scale
    // cannot reach, or do not exist.
    //
    // `rings` is the arc step count per hemispherical cap; `segments` is the
    // radial count.
    [[nodiscard]] ARCANE_API MeshData BuildCapsule(std::uint32_t rings,
                                                   std::uint32_t segments,
                                                   float lengthRatio);
```

- [ ] **Step 4: Implement in `MeshBuilder.cpp`**

Implement the four functions. Requirements the tests enforce:

- `ComputeMeshBounds`: early-return `{}` when `mesh.vertices.empty()`; otherwise seed `min`/`max` from `vertices[0].position` and fold.
- `BuildPlane(subdivisions)`: clamp `subdivisions` to at least 1 defensively (validation is `BuildMeshData`'s job — this is the raw builder). `(subdivisions+1)^2` vertices on the XZ grid, `y = 0`, normal `(0,1,0)`, UV from the normalized grid coordinate. Emit each quad as two triangles wound so the geometric normal points `+Y`.
- `BuildCylinder(segments)`: a side ring at `y = ±0.5` with radial normals (duplicate the seam column so `u` runs 0→1), plus two cap fans with normals `(0,±1,0)` — cap vertices must be **separate** from side vertices, for the same reason `BuildCube` uses 24 vertices rather than 8. Top cap winds CCW seen from `+Y`, bottom CCW seen from `−Y`.
- `BuildCapsule(rings, segments, lengthRatio)`: `const float halfSection = 0.5f * glm::max(lengthRatio - 1.0f, 0.0f);` Top hemisphere centred at `+halfSection`, bottom at `−halfSection`, both radius 0.5; a cylindrical band between them when `halfSection > 0`. Normals are the radial direction from the *nearest cap centre* for cap vertices and the horizontal radial for band vertices.

Write each generator to satisfy `WindingIsOutward` — if a face is inverted the test fails immediately, which is the point of that predicate.

- [ ] **Step 5: Run the tests, then the full gate**

```bat
ArcaneTests.exe "[mesh]"
ArcaneTests.exe "~[gpu]" --order rand
```
Expected: `[mesh]` PASS; gate green. State the assertion/case delta against 50034/1067.

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Render/MeshBuilder.hpp ArcaneClient/src/Arcane/Render/MeshBuilder.cpp ArcaneTests/src/MeshBuilderTest.cpp
git commit -m "feat(mesh): three more unit generators, and bounds to frame them"
```

---

## Task 2: The `.arcmesh` asset

**Files:**
- Create: `ArcaneClient/src/Arcane/Mesh/MeshAsset.hpp`, `MeshAsset.cpp`
- Create: `ArcaneTests/src/MeshAssetTest.cpp`

**Interfaces:**
- Consumes: Task 1's `BuildPlane`/`BuildCylinder`/`BuildCapsule`/`ComputeMeshBounds`, plus `BuildCube`/`BuildUvSphere`.
- Produces:
  ```cpp
  enum class MeshSource : std::uint8_t { Plane = 0, Cube = 1, UvSphere = 2, Cylinder = 3, Capsule = 4 };
  struct MeshAssetData { Guid id; std::string name; MeshSource source;
                         std::uint32_t rings, segments, subdivisions;
                         float capsuleLengthRatio; Guid material; };
  bool operator==(const MeshAssetData&, const MeshAssetData&);
  ARCANE_API bool SaveMeshAsset(const std::filesystem::path&, const MeshAssetData&);
  ARCANE_API std::optional<MeshAssetData> LoadMeshAsset(const std::filesystem::path&);
  ARCANE_API std::optional<std::string> ValidateMeshAsset(const MeshAssetData&);  // nullopt == valid
  ARCANE_API std::optional<MeshData> BuildMeshData(const MeshAssetData&);
  ```

`ValidateMeshAsset` returns the **human-readable reason** (naming the field) on failure so the caller can log it verbatim; `nullopt` means valid. `BuildMeshData` returns `nullopt` exactly when `ValidateMeshAsset` returns a reason.

- [ ] **Step 1: Write the failing tests**

Create `ArcaneTests/src/MeshAssetTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Render/MeshBuilder.hpp>

#include <filesystem>
#include <fstream>

using namespace Arcane;
using Catch::Matchers::WithinAbs;

namespace
{
    std::filesystem::path TempMeshPath(const char* stem)
    {
        std::filesystem::path p =
            std::filesystem::temp_directory_path() / "arcane_mesh_asset_test";
        std::filesystem::create_directories(p);
        return p / (std::string(stem) + ".arcmesh");
    }
}

TEST_CASE("a .arcmesh round-trips through save and load", "[mesh][asset]")
{
    MeshAssetData in;
    in.id                 = Guid::Generate();
    in.name               = "Test Capsule";
    in.source             = MeshSource::Capsule;
    in.rings              = 6;
    in.segments           = 20;
    in.subdivisions       = 4;
    in.capsuleLengthRatio = 3.0f;
    in.material           = Guid::Generate();

    const std::filesystem::path p = TempMeshPath("roundtrip");
    REQUIRE(SaveMeshAsset(p, in));

    const std::optional<MeshAssetData> out = LoadMeshAsset(p);
    REQUIRE(out.has_value());
    CHECK(*out == in);
}

TEST_CASE("a missing or malformed .arcmesh loads as nullopt, not as defaults", "[mesh][asset]")
{
    CHECK_FALSE(LoadMeshAsset(TempMeshPath("does-not-exist-at-all")).has_value());

    const std::filesystem::path junk = TempMeshPath("junk");
    { std::ofstream f(junk); f << "this is not json"; }
    CHECK_FALSE(LoadMeshAsset(junk).has_value());
}

TEST_CASE("validation is PER SOURCE, over the fields that source reads", "[mesh][asset]")
{
    // The flat tagged struct's one real hazard: validating every field
    // regardless of tag would refuse legal assets. A plane has no radial
    // segments, so segments == 0 is as meaningless to it as halfLen is to a
    // Manifold2D Circle -- and therefore must NOT refuse.
    MeshAssetData plane;
    plane.source       = MeshSource::Plane;
    plane.segments     = 0;
    plane.rings        = 0;
    plane.subdivisions = 1;
    CHECK_FALSE(ValidateMeshAsset(plane).has_value());

    plane.subdivisions = 0;
    CHECK(ValidateMeshAsset(plane).has_value());

    // A cube reads NOTHING -- it is valid under every parameter combination.
    MeshAssetData cube;
    cube.source             = MeshSource::Cube;
    cube.rings              = 0;
    cube.segments           = 0;
    cube.subdivisions       = 0;
    cube.capsuleLengthRatio = 0.0f;
    CHECK_FALSE(ValidateMeshAsset(cube).has_value());

    // Thresholds are UE's (SphereGenerator.h:200-201, CapsuleGenerator.h:265-267).
    MeshAssetData sphere;
    sphere.source = MeshSource::UvSphere;
    sphere.rings = 3; sphere.segments = 3;
    CHECK_FALSE(ValidateMeshAsset(sphere).has_value());
    sphere.rings = 2;
    CHECK(ValidateMeshAsset(sphere).has_value());
    sphere.rings = 3; sphere.segments = 2;
    CHECK(ValidateMeshAsset(sphere).has_value());

    // A capsule's CAP-RING floor is 2, not 3 -- an arc needs fewer steps than
    // a closed loop (UE's NumHemisphereArcSteps).
    MeshAssetData capsule;
    capsule.source = MeshSource::Capsule;
    capsule.rings = 2; capsule.segments = 3; capsule.capsuleLengthRatio = 1.0f;
    CHECK_FALSE(ValidateMeshAsset(capsule).has_value());
    capsule.rings = 1;
    CHECK(ValidateMeshAsset(capsule).has_value());
    capsule.rings = 2; capsule.capsuleLengthRatio = 0.99f;
    CHECK(ValidateMeshAsset(capsule).has_value());
}

TEST_CASE("the refusal reason names the offending field", "[mesh][asset]")
{
    MeshAssetData bad;
    bad.source = MeshSource::UvSphere;
    bad.rings = 1; bad.segments = 32;
    const std::optional<std::string> why = ValidateMeshAsset(bad);
    REQUIRE(why.has_value());
    // The message is what a user reads in the Problems pane; a reason that
    // does not name the field is not actionable.
    CHECK(why->find("rings") != std::string::npos);
}

TEST_CASE("BuildMeshData dispatches per source and refuses exactly what validation does",
          "[mesh][asset]")
{
    MeshAssetData d;

    d.source = MeshSource::Cube;
    REQUIRE(BuildMeshData(d).has_value());
    CHECK(BuildMeshData(d)->vertices.size() == BuildCube(1.0f).vertices.size());

    d.source = MeshSource::Plane; d.subdivisions = 1;
    REQUIRE(BuildMeshData(d).has_value());
    CHECK(BuildMeshData(d)->indices.size() == 6);

    d.subdivisions = 0;
    CHECK_FALSE(BuildMeshData(d).has_value());   // refuses, emits nothing
}

TEST_CASE("BuildMeshData is deterministic", "[mesh][asset]")
{
    // Same input, same bytes -- the artifact-determinism rule the cook contract
    // states, applied here so F2c inherits a builder that already obeys it.
    MeshAssetData d;
    d.source = MeshSource::UvSphere;
    d.rings = 12; d.segments = 24;

    const std::optional<MeshData> a = BuildMeshData(d);
    const std::optional<MeshData> b = BuildMeshData(d);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(a->vertices.size() == b->vertices.size());
    REQUIRE(a->indices == b->indices);
    for (std::size_t i = 0; i < a->vertices.size(); ++i)
    {
        CHECK_THAT(a->vertices[i].position.x, WithinAbs(b->vertices[i].position.x, 0.0f));
        CHECK_THAT(a->vertices[i].position.y, WithinAbs(b->vertices[i].position.y, 0.0f));
        CHECK_THAT(a->vertices[i].position.z, WithinAbs(b->vertices[i].position.z, 0.0f));
    }
}

TEST_CASE("the unit rule holds for every source", "[mesh][asset]")
{
    // The spec's load-bearing rule, pinned once per source: nothing a generator
    // emits exceeds unit extent in X or Z. (Capsule is the ONE exception in Y,
    // and only by its ratio -- which is why the ratio is the only size-ish
    // field that survived.)
    const MeshSource all[] = { MeshSource::Plane, MeshSource::Cube,
                               MeshSource::UvSphere, MeshSource::Cylinder,
                               MeshSource::Capsule };
    for (MeshSource s : all)
    {
        MeshAssetData d;
        d.source = s;
        const std::optional<MeshData> m = BuildMeshData(d);
        REQUIRE(m.has_value());
        const MeshBounds b = ComputeMeshBounds(*m);
        CHECK(b.max.x - b.min.x <= 1.0f + 1e-3f);
        CHECK(b.max.z - b.min.z <= 1.0f + 1e-3f);
    }
}
```

- [ ] **Step 2: Run and confirm failure**

```bat
ArcaneTests.exe "[asset][mesh]"
```
Expected: compile error — `Arcane/Mesh/MeshAsset.hpp` not found.

- [ ] **Step 3: Write `MeshAsset.hpp`**

```cpp
#pragma once

// MeshAsset: the .arcmesh file -- a native JSON asset with an embedded
// top-level "id" (rides AssetRegistry::ScanContent's native path, exactly like
// .arcsprite and .arcmat). It names a procedural GENERATOR plus the topology
// that generator needs, and a default material.
//
// THE UNIT RULE, which decides every field here: generators emit UNIT
// geometry. Scale expresses size, rotation expresses orientation, the asset
// expresses shape. There is no sizeMeters and no radiusMeters -- the engine
// already ruled this on SpriteRenderer ("There is NO size field: an entity is
// sized by its Transform scale"), and BOTH reference engines confirm it for
// meshes: neither UStaticMesh nor a Source 2 model stores a size, and UE reads
// streaming scale straight off the component transform
// (StaticMeshComponent.h:684).
//
// FLAT AND TAGGED, not a variant: per-source field meaning is documented
// rather than enforced by the type, which is the same form
// Manifold2D::Physics::Shape uses (halfLen simply means nothing to a Circle).
// Its one real hazard is validating the whole struct regardless of tag, which
// would refuse legal assets -- see ValidateMeshAsset.
//
// F2c's SEAM: an imported mesh becomes another MeshSource plus an artifact
// reference, with no component and no scene change.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/MeshBuilder.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on dll-exported types: benign under /MD
#endif

    // The generator roster -- Unity's built-in set, minus Quad (which is Plane
    // under a -90 degree X rotation; orientation is the Transform's job for the
    // same reason size is). Explicitly uint8_t-backed and explicitly numbered:
    // these values are PERSISTED, so reordering them silently re-authors every
    // .arcmesh in every project.
    enum class MeshSource : std::uint8_t
    {
        Plane    = 0,
        Cube     = 1,
        UvSphere = 2,
        Cylinder = 3,
        Capsule  = 4,
    };

    struct MeshAssetData
    {
        Guid          id{};
        std::string   name;
        MeshSource    source = MeshSource::Cube;

        // ---- TOPOLOGY: density a Transform cannot express ----------------
        std::uint32_t rings        = 16;   // UvSphere; Capsule (arc steps per cap)
        std::uint32_t segments     = 32;   // UvSphere, Cylinder, Capsule (radial)
        std::uint32_t subdivisions = 1;    // Plane (quads per axis; 1 = one quad)

        // ---- SHAPE RATIO: a family no scale can reach --------------------
        // Total height / diameter. A cylinder needs no equivalent because a
        // cylinder scaled in Y is still a cylinder; a capsule scaled in Y is
        // not (its hemispherical caps become ellipsoids). That is the test any
        // future shape parameter must pass.
        float capsuleLengthRatio = 2.0f;

        // ---- The mesh's DEFAULT material, overridable per entity ---------
        // Both reference engines put assignment on the asset:
        // UStaticMesh::StaticMaterials (StaticMesh.h:1095) and Source 2's
        // m_materialGroups. MeshRenderer::materialOverride is
        // UMeshComponent::OverrideMaterials in miniature. Nil = white.
        //
        // SCALAR, not an array, because F2a's primitives are single-section.
        // F2c's imported multi-section meshes grow this into a slot array,
        // which is ADDITIVE -- putting it on the component instead would have
        // forced a later MOVE (a component schema change plus a scene
        // re-author).
        Guid material{};
    };

    // Memberwise equality, for the same reason SpriteAssetData has one: the
    // document's undo bracket compares an activation-time COPY against the live
    // data to decide whether a drag actually moved anything. Memberwise and
    // never memcmp -- `name` is a std::string (its object bytes are a
    // pointer/SSO buffer, not the text) and the struct is padded.
    [[nodiscard]] inline bool operator==(const MeshAssetData& a, const MeshAssetData& b) noexcept
    {
        return a.id == b.id && a.name == b.name && a.source == b.source &&
               a.rings == b.rings && a.segments == b.segments &&
               a.subdivisions == b.subdivisions &&
               a.capsuleLengthRatio == b.capsuleLengthRatio &&
               a.material == b.material;
    }

    // Write `data` as .arcmesh JSON. Only the fields the source actually reads
    // are written, so a cube stays a three-key file. False on IO failure.
    ARCANE_API bool SaveMeshAsset(const std::filesystem::path& path, const MeshAssetData& data);

    // Parse a .arcmesh. nullopt on IO/parse failure or when the file is not a
    // mesh asset (the "type":"mesh" tag IS the discriminator, exactly as
    // .arcsprite works -- no structurally-unique key distinguishes one).
    // Malformed INDIVIDUAL fields fall back to their MeshAssetData default
    // rather than failing the whole load.
    ARCANE_API std::optional<MeshAssetData> LoadMeshAsset(const std::filesystem::path& path);

    // nullopt == valid. Otherwise the human-readable reason, NAMING THE FIELD
    // -- it is what a user reads in the Problems pane, and a reason that does
    // not name the field is not actionable.
    //
    // Evaluated PER SOURCE, over the fields that source actually reads: a Plane
    // with segments == 0 is VALID, because a plane has no radial segments.
    // Validating the whole struct regardless of tag would refuse legal assets
    // and is the flat struct's one real hazard.
    //
    // Thresholds are UE's, from GeometryCore's generators
    // (SphereGenerator.h:200-201, CapsuleGenerator.h:265-267). UE CLAMPS
    // silently where this refuses, and UE is not simply right: its generators
    // are tool-time transients where a clamp is invisible, while .arcmesh is
    // PERSISTED -- a silent clamp leaves the file saying 1 while the mesh is 3,
    // forever. MeshDocument's param panel also bounds every one of these at the
    // WIDGET (bounded DragInt/DragFloat, the idiom SpriteDocument already
    // uses), so in practice refusal only ever fires on a hand-edited file.
    [[nodiscard]] ARCANE_API std::optional<std::string> ValidateMeshAsset(const MeshAssetData& data);

    // Generate the geometry. nullopt exactly when ValidateMeshAsset returns a
    // reason -- an INVALID mesh is an error and emits nothing, where a NIL mesh
    // Guid on a component is not an error at all (it draws nothing, like a nil
    // sprite).
    //
    // DETERMINISTIC: same input, same bytes. F2c inherits this builder into a
    // cook step whose artifacts must be reproducible.
    [[nodiscard]] ARCANE_API std::optional<MeshData> BuildMeshData(const MeshAssetData& data);

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
```

- [ ] **Step 4: Write `MeshAsset.cpp`**

Mirror `SpriteAsset.cpp`'s structure exactly (open the file and follow it — same `nlohmann::json` idiom, same `"type"` tag, same per-field default fallback, same `std::ofstream` write). Specifics:

- `SaveMeshAsset` writes `{"type":"mesh","id":{...},"name":...,"source":"<name>"}` plus **only** the fields the source reads. `source` is written as a **string** (`"plane"`,`"cube"`,`"uvsphere"`,`"cylinder"`,`"capsule"`), not the integer — a hand-editable file should not require knowing the enum ordinals, and it makes an added source obvious in a diff.
- `LoadMeshAsset` rejects a document whose `type` is not `"mesh"`. Unknown `source` strings fall back to `MeshSource::Cube` with one `ARC_WARN`.
- `ValidateMeshAsset` switches on `source` and checks only that source's fields, per the table in the spec. Message form: `"UvSphere needs rings >= 3 (got 1)"`.
- `BuildMeshData` calls `ValidateMeshAsset` first, returns `nullopt` on a reason, else dispatches: `Plane → BuildPlane(subdivisions)`, `Cube → BuildCube(1.0f)`, `UvSphere → BuildUvSphere(0.5f, rings, segments)`, `Cylinder → BuildCylinder(segments)`, `Capsule → BuildCapsule(rings, segments, capsuleLengthRatio)`.

Note the two unit constants: `BuildCube(1.0f)` and `BuildUvSphere(0.5f, …)` — those two generators predate the unit rule and keep their size parameters, so `.arcmesh` always passes the unit value.

- [ ] **Step 5: Add both new files to the build**

`ArcaneClient` globs `src/**`, so `MeshAsset.{hpp,cpp}` are picked up automatically. Re-run premake so the `.vcxproj` file list refreshes:

```bat
cd D:\dev\starworks\Arcane
ThirdParty\premake5\premake5.exe vs2026
```

`ArcaneTests` globs `%{prj.location}/src/**.cpp`, so `MeshAssetTest.cpp` needs no premake edit beyond that same regeneration.

- [ ] **Step 6: Run the tests, then the full gate**

```bat
ArcaneTests.exe "[asset][mesh]"
ArcaneTests.exe "~[gpu]" --order rand
```
Expected: PASS. State the delta.

- [ ] **Step 7: Commit**

```bash
git add ArcaneClient/src/Arcane/Mesh/ ArcaneTests/src/MeshAssetTest.cpp
git commit -m "feat(mesh): .arcmesh names a generator, not a size"
```

---

## Task 3: `MeshRenderer` and the `"mesh"` material kind

**Files:**
- Modify: `ArcaneClient/src/Arcane/Scene/Components.hpp` (component + reflection block)
- Modify: `ArcaneClient/src/Arcane/Scene/SceneModule.hpp` (registration)
- Modify: `ArcaneClient/src/Arcane/Material/MaterialTypes.hpp` (`MaterialSurface::Mesh`)
- Modify: `ArcaneClient/src/Arcane/Material/MaterialSource.cpp:321` (kind → surface)
- Modify: `ArcaneClient/src/Arcane/Material/MaterialAsset.cpp` (`"mesh"` accepted; `passes` refused)
- Test: `ArcaneTests/src/MeshAssetTest.cpp` (extend)

**Interfaces:**
- Consumes: `MeshAssetData` (Task 2).
- Produces: `struct MeshRenderer { Guid mesh; Guid materialOverride; };`, `MaterialSurface::Mesh`.

- [ ] **Step 1: Write the failing tests**

Append to `ArcaneTests/src/MeshAssetTest.cpp`:

```cpp
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Astra/Registry/Registry.hpp>
#include "Helpers/TestTypeContext.hpp"

TEST_CASE("MeshRenderer is registered, reflected, and defaults to drawing nothing",
          "[mesh][scene]")
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*creg);
    Astra::Registry reg{ creg };

    Astra::Entity e = reg.CreateEntity();
    MeshRenderer* mr = reg.AddComponent<MeshRenderer>(e);
    REQUIRE(mr != nullptr);

    // Nil mesh draws nothing and is NOT an error -- same contract as a nil
    // SpriteRenderer::sprite.
    CHECK_FALSE(mr->mesh.IsValid());
    CHECK_FALSE(mr->materialOverride.IsValid());

    // Reflected, or the Inspector cannot render it and the scene writer cannot
    // persist it.
    const Astra::TypeMeta* meta = Astra::GetMeta<MeshRenderer>();
    REQUIRE(meta != nullptr);
    bool sawMesh = false, sawOverride = false;
    for (const Astra::FieldInfo& f : meta->fields)
    {
        if (f.name == "mesh")             sawMesh = true;
        if (f.name == "materialOverride") sawOverride = true;
    }
    CHECK(sawMesh);
    CHECK(sawOverride);
}

TEST_CASE("there is no tint field on MeshRenderer, and that is deliberate", "[mesh][scene]")
{
    // Neither reference engine has a per-instance colour on a mesh component;
    // both express "same mesh, different colour" through material INSTANCES,
    // which Arcane already ships kind-agnostically. A tint would also make a
    // red cube expressible twice -- the two-spellings defect the unit rule
    // rejects elsewhere in the same spec. Pinned so it is not re-added by
    // reflex.
    const Astra::TypeMeta* meta = Astra::GetMeta<MeshRenderer>();
    REQUIRE(meta != nullptr);
    for (const Astra::FieldInfo& f : meta->fields)
    {
        CHECK(f.name != "tint");
        CHECK(f.name != "color");
    }
}
```

- [ ] **Step 2: Run and confirm failure**

```bat
ArcaneTests.exe "[scene][mesh]"
```
Expected: compile error — `MeshRenderer` undeclared.

- [ ] **Step 3: Add the component to `Components.hpp`**

Insert after `SpriteRenderer`:

```cpp
    // The 3D sibling of SpriteRenderer (F2a). An entity carrying one draws its
    // .arcmesh through the opaque mesh pass, posed by its Transform.
    //
    // NO SIZE, for the same reason SpriteRenderer has none -- the Transform's
    // scale is the size, so one asset serves many entities. NO TINT either,
    // and that one IS a departure from SpriteRenderer: neither Unreal nor
    // Source 2 puts a per-instance colour on a mesh component, and both express
    // "the same mesh in a different colour" through material INSTANCES, which
    // Arcane already ships (MaterialAssetData::parent + sparse overrides IS
    // UMaterialInstance, and it is kind-agnostic). A tint would also make a red
    // cube expressible twice -- as a red material, or a white one tinted red --
    // which is the two-spellings defect the unit rule exists to prevent.
    struct MeshRenderer
    {
        // The .arcmesh asset drawn. Nil (the default) or unresolved -> draws
        // NOTHING. That is not an error: a scene may legitimately carry a slot
        // with no geometry yet, exactly as MeshInstance::mesh documents.
        Guid mesh{};

        // Per-entity material override. Nil (the default) falls through to the
        // MESH ASSET's own `material`, and a nil there resolves to white.
        // This is UMeshComponent::OverrideMaterials (MeshComponent.h:29-31,
        // "Per-Component material overrides") in miniature; Source 2 answers
        // the same question with m_materialGroups.
        //
        // An override naming an UNRESOLVABLE material falls through to the mesh
        // default and WARNS ONCE -- it must not silently render white, which
        // would hide the broken reference.
        Guid materialOverride{};
    };
```

- [ ] **Step 4: Add the reflection block**

In the reflection section of `Components.hpp`, beside `ASTRA_REFLECT_TYPE(SpriteRenderer)`:

```cpp
    ASTRA_REFLECT_TYPE(MeshRenderer)
        ASTRA_REFLECT_FIELD(MeshRenderer, mesh)
            ASTRA_REFLECT_ATTR(Category, "Appearance")
            ASTRA_REFLECT_ATTR(Tooltip, "The .arcmesh asset this renderer draws. Nil draws nothing. Size and orientation come from the Transform.")
        ASTRA_REFLECT_FIELD(MeshRenderer, materialOverride)
            ASTRA_REFLECT_ATTR(Category, "Appearance")
            ASTRA_REFLECT_ATTR(Tooltip, "Overrides the mesh asset's own material for this entity only. Nil uses the mesh's default.")
    ASTRA_END_REFLECT_TYPE()
```

- [ ] **Step 5: Register it in `SceneModule.hpp`**

Add `creg.RegisterComponent<MeshRenderer>();` beside the other scene components. **This is load-bearing** — desk bug #1 of the Outliner arc was exactly a component that was reflected but never registered, which made `SceneSerializer` silently DROP it on load.

- [ ] **Step 6: Add the `"mesh"` material kind**

- `MaterialTypes.hpp`: add `Mesh` to `MaterialSurface`.
- `MaterialSource.cpp:321`: extend the kind→surface map so `"mesh"` yields `MaterialSurface::Mesh`.
- `MaterialAsset.cpp:403`: a `"mesh"` material **refuses `passes`**, exactly as `"sprite"` already does — extend that condition rather than adding a parallel one.

Do **not** wire `"mesh"` into `MaterialSource`'s shader-source generation. F2a consumes mesh materials for their param VALUES only and never compiles a variant; that is F2b/Task 8 work, once there is a pipeline surface to compile into.

- [ ] **Step 7: Run the tests, then the gate. Commit**

```bash
git add ArcaneClient/src/Arcane/Scene/Components.hpp ArcaneClient/src/Arcane/Scene/SceneModule.hpp ArcaneClient/src/Arcane/Material/ ArcaneTests/src/MeshAssetTest.cpp
git commit -m "feat(scene): MeshRenderer, and a material kind for surfaces"
```

---

## Task 4: `MeshCache` and `MeshMaterialCache`

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/MeshCache.{hpp,cpp}`, `MeshMaterialCache.{hpp,cpp}`
- Modify: `ArcaneClient/src/Arcane/Scene/SceneResources.hpp`
- Test: `ArcaneTests/src/MeshSubmissionTest.cpp` (create)

**Interfaces:**
- Consumes: `BuildMeshData`, `ComputeMeshBounds`, `LoadMeshAsset`, `MeshAssetData` (Tasks 1–2); `LoadMaterialAsset` (existing).
- Produces:
  ```cpp
  struct MeshEntry { MeshData data; MeshBounds bounds; };
  class MeshCache { using ResolveAssetFn = std::function<std::optional<std::filesystem::path>(const Guid&)>;
                    struct Services { ResolveAssetFn resolveAsset; };
                    void Request(const Guid&); void Invalidate(const Guid&); void Clear();
                    const std::unordered_map<Guid, MeshEntry>& Table() const;
                    const MeshAssetData* AssetFor(const Guid&) const; };
  struct ResolvedMeshMaterial { glm::vec4 baseColor{1.0f}; };
  class MeshMaterialCache { /* same Services/Request/Invalidate/Clear/Table shape */ };
  struct MeshTable { const std::unordered_map<Guid, MeshEntry>* meshes = nullptr;
                     const MeshEntry* Resolve(const Guid&) const; };
  struct MeshMaterialTable { const std::unordered_map<Guid, ResolvedMeshMaterial>* materials = nullptr;
                             const ResolvedMeshMaterial* Resolve(const Guid&) const; };
  ```

**Both caches follow `SpriteMaterialCache`'s failure discipline, not `SpriteCache`'s.** `SpriteCache` caches a placeholder *in* the published table so a broken sprite stays visible; `SpriteMaterialCache` keeps failures *out* in a separate `failed` set so a broken material leaves the sprite on the plain pipeline. Meshes want the second: a failed mesh must resolve to `nullptr` so `MeshSubmissionSystem` can skip the entity entirely — there is no meaningful "placeholder mesh", and drawing a wrong shape is worse than drawing none.

`MeshCache::AssetFor` exposes the loaded `MeshAssetData` because Task 5's material chain needs the mesh's default material Guid, and re-reading the file per frame to get it would be absurd.

- [ ] **Step 1: Write the failing tests** — create `ArcaneTests/src/MeshSubmissionTest.cpp` covering: a resolvable Guid populates `Table()`; a second `Request` is a no-op (same `MeshData` pointer — pin it by address); an unresolvable Guid stays *out* of the table and warns once; an `.arcmesh` that fails validation stays out; `Invalidate` forces a re-resolve; `Clear` empties. Write real bodies using a temp `.arcmesh` on disk and a lambda resolver, mirroring the fixture style of `SceneRenderResolverTest.cpp`.

- [ ] **Step 2: Run and confirm failure.**

- [ ] **Step 3: Implement both caches**, mirroring `SpriteCache.{hpp,cpp}`'s pimpl shape and `SpriteMaterialCache`'s `failed`-set discipline.

- [ ] **Step 4: Add both resources to `SceneResources.hpp`**, mirroring `SpriteTable` (lines 106-119) exactly — a borrowed pointer plus a null-and-nil-safe `Resolve`.

- [ ] **Step 5: Regenerate projects, run tests, run the gate. Commit**

```bash
git commit -m "feat(render): two caches turn mesh and material Guids into something drawable"
```

---

## Task 5: `MeshSubmissionSystem` and the material chain

**Files:**
- Create: `ArcaneClient/src/Arcane/Scene/MeshSubmissionSystem.hpp`
- Test: `ArcaneTests/src/MeshSubmissionTest.cpp` (extend)

**Interfaces:**
- Consumes: `MeshTable`, `MeshMaterialTable`, `MeshRenderer`, `WorldTransform`, `Hidden`, `MeshInstance`.
- Produces: `void CollectMeshInstances(Astra::Registry&, std::vector<MeshInstance>& out);`

A free function rather than an `Astra::System`, because the output vector is **host-owned** (it must outlive the `RenderFrame` call that borrows it) and a system has nowhere to put it. `RenderSubmissionSystem` is a system because its sink — the `Batcher2D` — lives in a resource; there is no equivalent sink here.

**The resolution chain, and the one non-obvious rule:**

```
materialOverride (if valid AND resolvable)
  -> mesh asset's `material` (if valid AND resolvable)
    -> white (1,1,1,1)
```

An override that is *valid but unresolvable* must fall through **and warn once**, not resolve to white — silently rendering white would hide a broken asset reference, which is the same class of defect as the empty Inspector header Task 1 of the previous arc fixed.

- [ ] **Step 1: Write the failing tests** — all four chain states (override set; override nil with a mesh default; both nil → white; override set-but-unresolvable → falls to mesh default, warns once), plus: `Hidden` skipped, missing `WorldTransform` skipped, nil `mesh` skipped, unresolved `mesh` skipped, `model` equals `WorldTransform::matrix` exactly, and `out` is cleared on entry so a second call does not append.

- [ ] **Step 2: Run and confirm failure.**

- [ ] **Step 3: Implement** — sweep `reg.CreateView<WorldTransform, MeshRenderer, Astra::Not<Hidden>>()`, resolve, push `MeshInstance{ &entry->data, world.matrix, baseColor }`.

**Lifetime note the implementer must not get wrong:** `MeshInstance::mesh` borrows into `MeshCache`'s table, which the resolver owns and which outlives the frame. Do **not** copy `MeshData` into the instance, and do **not** let the cache rehash between collection and `RenderFrame` — `Request` every referenced Guid *before* collecting, never during.

- [ ] **Step 4: Run tests, run the gate. Commit**

```bash
git commit -m "feat(scene): the scene collects its own mesh instances"
```

---

## Task 6: `SceneRenderResolver` drives the two new caches

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/SceneRenderResolver.{hpp,cpp}`
- Test: `ArcaneTests/src/SceneRenderResolverTest.cpp` (extend)

- [ ] **Step 1: Write the failing test** — after `Refresh()`, the registry carries a `MeshTable` and a `MeshMaterialTable` whose pointers are non-null and whose contents include every Guid referenced by a `MeshRenderer` in the scene; `Clear()`/project-switch drops both.

- [ ] **Step 2: Run and confirm failure.**

- [ ] **Step 3: Implement** — add both caches as members, `Request` every referenced Guid during the per-frame sweep, publish both tables as resources. **Do not add a second `Drain()` site**: neither cache compiles anything, so neither touches `ShaderCompiler`. That is what keeps the single-drain rule (`SceneRenderResolver.hpp:22-28`) intact, and it is why the mesh material is params-only in F2a.

- [ ] **Step 4: Run tests, run the gate. Commit**

```bash
git commit -m "feat(host): one resolver, now five caches"
```

---

## Task 7: The perspective camera honours its pose

**Files:**
- Modify: `ArcaneClient/src/Arcane/Scene/SceneCamera.hpp:177-214`
- Test: `ArcaneTests/src/PerspectiveCameraTest.cpp` (extend)

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("the perspective camera reads its entity's real 3D pose", "[camera]")
{
    // F1 pinned this lens at (x, y, 0) looking down -Z, reserving pose for F4.
    // F2a lifts that for the PERSPECTIVE lens only. The deferral's stated
    // reason -- "would silently re-frame every scene" -- is inapplicable here:
    // both authored scenes in the tree default to Orthographic, and
    // ActiveSceneCamera skips any camera whose projection is not Orthographic,
    // so the two sweeps cannot interfere.
    World w;                                  // fixture from this file
    Astra::Entity e = w.reg.CreateEntity();
    Camera* cam = w.reg.AddComponent<Camera>(e);
    cam->projection = CameraProjection::Perspective;
    Transform* t = w.reg.AddComponent<Transform>(e);
    t->position = glm::vec3(1.0f, 2.0f, 3.0f);

    const auto v = ActivePerspectiveSceneCamera(w.reg, 16.0f / 9.0f);
    REQUIRE(v.has_value());

    // The eye is the FULL world translation, Z included -- that is the whole
    // point. inverse(view)[3] recovers it.
    const glm::vec3 eye = glm::vec3(glm::inverse(v->view)[3]);
    CHECK_THAT(eye.x, WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(eye.y, WithinAbs(2.0f, 1e-5f));
    CHECK_THAT(eye.z, WithinAbs(3.0f, 1e-5f));
}

TEST_CASE("a rotated perspective camera looks where it is aimed", "[camera]")
{
    World w;
    Astra::Entity e = w.reg.CreateEntity();
    Camera* cam = w.reg.AddComponent<Camera>(e);
    cam->projection = CameraProjection::Perspective;
    Transform* t = w.reg.AddComponent<Transform>(e);
    // Yaw 90 degrees about +Y: forward turns from -Z to -X.
    t->rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    const auto v = ActivePerspectiveSceneCamera(w.reg, 1.0f);
    REQUIRE(v.has_value());
    // Row 2 of a view matrix is the negated forward axis.
    const glm::mat4 inv = glm::inverse(v->view);
    const glm::vec3 forward = -glm::normalize(glm::vec3(inv[2]));
    CHECK_THAT(forward.x, WithinAbs(-1.0f, 1e-4f));
    CHECK_THAT(forward.z, WithinAbs( 0.0f, 1e-4f));
}

TEST_CASE("a degenerate camera basis falls back rather than emitting NaN", "[camera]")
{
    World w;
    Astra::Entity e = w.reg.CreateEntity();
    Camera* cam = w.reg.AddComponent<Camera>(e);
    cam->projection = CameraProjection::Perspective;
    Transform* t = w.reg.AddComponent<Transform>(e);
    t->scale = glm::vec3(0.0f);               // singular basis

    const auto v = ActivePerspectiveSceneCamera(w.reg, 1.0f);
    REQUIRE(v.has_value());
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(std::isfinite(v->view[c][r]));
}

TEST_CASE("the ORTHOGRAPHIC path is untouched by the pose change", "[camera]")
{
    World w;
    Astra::Entity e = w.reg.CreateEntity();
    Camera* cam = w.reg.AddComponent<Camera>(e);   // defaults to Orthographic
    Transform* t = w.reg.AddComponent<Transform>(e);
    t->position = glm::vec3(4.0f, 5.0f, 99.0f);    // a Z the ortho lens must IGNORE
    t->rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));

    const auto v = ActiveSceneCamera(w.reg, 16.0f / 9.0f);
    REQUIRE(v.has_value());
    CHECK_THAT(v->center.x, WithinAbs(4.0f, 1e-6f));
    CHECK_THAT(v->center.y, WithinAbs(5.0f, 1e-6f));
}
```

- [ ] **Step 2: Run and confirm the first three fail** (the orthographic one must pass already — if it does not, stop: something else regressed).

- [ ] **Step 3: Implement**

Replace the `center`/`eye` derivation in `ActivePerspectiveSceneCamera` with a full-basis read. Orthonormalize the world matrix's columns rather than `glm::quat_cast`, because the world matrix carries scale:

```cpp
        glm::mat4 world{1.0f};
        // ... captured in the ForEach, from WorldTransform then Transform, the
        // same two-step fallback ActiveSceneCamera uses ...

        glm::vec3 eye     = glm::vec3(world[3]);
        glm::vec3 forward = -glm::vec3(world[2]);
        glm::vec3 up      =  glm::vec3(world[1]);
        const float fLen = glm::length(forward);
        const float uLen = glm::length(up);
        if (fLen < 1e-6f || uLen < 1e-6f)
        {
            // A singular basis (zero scale) has no direction to read.
            // lookAtRH would divide by zero and hand every subsequent pass a
            // NaN clip position, which is undefined behaviour on the GPU
            // rather than a wrong picture -- so fall back to F1's pinned
            // orientation and say so once.
            ARC_WARN("camera: entity has a degenerate basis (zero scale?) -- "
                     "falling back to forward -Z / up +Y");
            forward = glm::vec3(0.0f, 0.0f, -1.0f);
            up      = glm::vec3(0.0f, 1.0f,  0.0f);
        }
        else
        {
            forward /= fLen;
            up      /= uLen;
        }
        v.view = glm::lookAtRH(eye, eye + forward, up);
```

Leave `ActiveSceneCamera` **completely untouched** and update the F1 note at `:168-176` to record that the perspective half is now live and why the orthographic half stays pinned.

- [ ] **Step 4: Run tests, run the gate. Commit**

```bash
git commit -m "feat(scene): the perspective lens looks where its entity is aimed"
```

---

## Task 8: The normal matrix

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.{hpp,cpp}`, `data/shaders/mesh.hlsl`
- Test: `ArcaneTests/src/MeshNodeTest.cpp` (extend or create), and one `[gpu][pixel]` case

**The defect being fixed:** `MeshInstance::model` is documented "Rotation + translation + UNIFORM scale only: mesh.hlsl transforms normals by the upper 3x3 rather than the inverse transpose." F1 gave `Transform` a `glm::vec3 scale` the Inspector authors freely, so the first scale-handle drag on a mesh hits it.

- [ ] **Step 1: Write the failing unit test** — a pure helper `NormalMatrixFor(const glm::mat4& model) -> glm::mat3` asserted against the analytic answer for a non-uniform scale: with `model = scale(2,1,1)`, a surface normal of `normalize(1,1,0)` must transform to `normalize(0.5,1,0)` normalized — **not** `normalize(2,1,0)`. Also pin that a uniform scale leaves the direction unchanged (so Task 7's `[pixel]` case cannot regress) and that a singular model matrix returns identity rather than NaN.

- [ ] **Step 2: Run and confirm failure.**

- [ ] **Step 3: Implement** — add `NormalMatrixFor` beside `DecomposeTRS`'s neighbours, compute it per instance in `MeshNode::Record` into the existing per-draw constant arena, and consume it in `mesh.hlsl`'s `vs_main` for the normal only. `MeshInstance` gains **no field** — it stays pure scene data and `Record` derives this from `model`.

- [ ] **Step 4: Update `MeshInstance::model`'s doc comment** — it currently states the uniform-scale restriction as a live constraint. It is no longer one; leaving it would send a future reader hunting a bug that was fixed.

- [ ] **Step 5: Write the `[gpu][pixel]` case** — a non-uniformly-scaled lit cube whose lit face brightness matches the analytic Lambert term. **Desk-run only.**

- [ ] **Step 6: Run the headless tests, run the gate. Commit**

```bash
git commit -m "fix(nri): normals survive a non-uniform scale"
```

---

## Task 9: `MeshDocument`

**Files:**
- Create: `ArcaneEditor/src/Documents/MeshDocument.{hpp,cpp}`
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` (factory), `ArcaneEditor/src/Panels/AssetBrowser.cpp` ("Create Mesh")
- Modify: `premake5.lua` (source-compile `MeshDocument.cpp` into ArcaneTests)
- Test: `ArcaneTests/src/MeshDocumentTest.cpp` (create)

Follow `ShaderEditorDocument` for the preview (its own offscreen `NriGraphContext` + `retireGraphPreview` hook, device-less services skipping preview resources in the ctor) and `SpriteDocument` for the data/undo half (`PushDataEdit`'s activation-time copy compared against live data).

- [ ] **Step 1: Add `MeshDocument.cpp` to ArcaneTests in `premake5.lua`**, beside the existing `ShaderEditorDocument.cpp` entry and with a comment in the same voice explaining that the HEADLESS halves are driven directly and `Draw` is never called.

- [ ] **Step 2: Write the failing tests** — construct with device-less services (no preview resources); a source change marks dirty; an undo bracket around a param drag produces exactly one undo step and self-drops on a no-op; `Peek` reports the asset name; building a preview mesh for an INVALID param set yields no geometry and surfaces the validation reason rather than throwing.

- [ ] **Step 3: Run and confirm failure.**

- [ ] **Step 4: Implement the headless half** — `MeshAssetData` state, load/save, dirty tracking, undo bracketing, validation surfacing.

- [ ] **Step 5: Implement the preview + params UI** — a `MeshSource` dropdown, that source's topology numbers only (do not draw fields the source ignores — the flat struct's per-source meaning is a UI concern too), the material Guid picker, and the preview framed from `ComputeMeshBounds`.

**Every topology widget must be BOUNDED at its validation floor** (`DragInt` with an explicit min, or `RangedDragFloat`, `EditorWidgets.hpp:43-49` — the idiom `SpriteDocument.cpp:258-268` already uses for `ppu`/`pivot`). That is what makes `ValidateMeshAsset`'s refusal unreachable from the panel and reachable only from a hand-edited file.

Note this is deliberately **not** `Astra::Range` + the reflected Inspector: no `*AssetData` struct in the tree is reflected, because an asset is edited by its own document rather than by the component Inspector. `Astra::Range` governs `MeshRenderer`'s two fields (Task 3); `MeshAssetData`'s bounds live here.

- [ ] **Step 6: Register the factory and the create-menu entry.**

- [ ] **Step 7: Run tests, run the gate. Commit**

```bash
git commit -m "feat(editor): .arcmesh gets a document and a live preview"
```

---

## Task 10: Host wiring (Phase 4 Task 9)

**Files:**
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp`, `ArcaneEditor/src/App/EditorAppFrame.cpp`

**`FrameDesc::mesh` and `DeclareGraphFrame`'s gate ALREADY EXIST** (see "What Phase 4 already did"). This task is host wiring only.

- [ ] **Step 1: Wire the runtime** — in `RuntimeFrame::RenderGraph`, hold a `std::vector<MeshInstance>` member (not a local — it must outlive the call), call `CollectMeshInstances`, build a `MeshSceneDesc` with the perspective camera view/projection and the scene light, and set `frame.mesh` when the vector is non-empty.

- [ ] **Step 2: Wire the editor** — the same, through `ArmGraphViewportFrame`.

- [ ] **Step 3: Verify both hosts headlessly**

```bat
ArcaneEditor.exe --project D:\dev\starworks\Arcane\ReferenceProject --frames 5
ArcaneRuntime.exe --project D:\dev\starworks\Arcane\ReferenceProject --frames 5
```
Both must exit 0 with no new errors in `diagnostics/`. This harness is a genuine evidence path — the Outliner arc used exactly it to separate "UI not clicking" from "catalog correctly empty".

- [ ] **Step 4: Run the gate. Commit**

```bash
git commit -m "feat(host): both hosts submit a mesh scene"
```

---

## Task 11: ReferenceProject content

**Files:**
- Create: `ReferenceProject/Content/meshes/*.arcmesh`, `ReferenceProject/Content/materials/*.arcmat`
- Modify: `ReferenceProject/Content/scenes/main.arcscene`

- [ ] **Step 1: Author a mesh, a mesh material, and a perspective camera into the reference scene**, positioned so the mesh is in frame.

- [ ] **Step 2: Extend `HostBootTest.cpp`'s fixture assertions** to cover the new entities.

**The trap from F1's desk drive, restated because it will bite again:** re-saving `ReferenceProject` through the editor persists STRUCTURAL edits too. F1's reparent test dropped the root's direct children 4→3 and broke `HostBootTest.cpp:808`. If you want the writer's normalization, **revert the fixture first, then save WITHOUT editing.**

- [ ] **Step 3: Run the gate. Commit**

```bash
git commit -m "test(reference): the reference scene grows a mesh"
```

---

## Task 12: ABI bump and restamp

**Files:**
- Modify: `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp:278` (`kGamePluginABIVersion = 16` → `17`)
- Modify: `ReferenceProject/*.arcproj`, and Gacha's `D:\dev\starworks\Gacha\Game\Aphelyon.arcproj`

- [ ] **Step 1: Bump `kGamePluginABIVersion` to 17** and add the changelog entry in the same voice as the existing ones, naming `MeshRenderer`, the `.arcmesh` asset, `MeshCache`/`MeshMaterialCache`, `MeshTable`/`MeshMaterialTable`, `ComputeMeshBounds` and the three new generators.

- [ ] **Step 2: Restamp both `.arcproj` files.** Gacha is a **separate repo** — commit it there with `chore(game): restamp Aphelyon.arcproj to engine ABI 17`.

- [ ] **Step 3: Verify the scene schema did NOT change.** `MeshRenderer` is a new name-keyed component, which is purely additive — schema stays **v3**. If anything forced a schema bump, stop and escalate: that contradicts the spec and needs a decision, not a quiet version bump.

- [ ] **Step 4: Full rebuild in all three configs from clean**, confirming 0 warnings. Run the gate in all three. State the final delta against 50034/1067 · 49973/1062.

- [ ] **Step 5: Commit**

```bash
git commit -m "chore(abi): bump to 17 for the mesh vocabulary"
```

---

## Task 13: DESK CHECKPOINT (USER)

Not agent-performable. The gate compiles neither `EditorApp.cpp` nor `EditorAppFrame.cpp` nor `EditorPanels.cpp`, so **every item below is desk-verify-only** and a green gate says nothing about any of them.

- [ ] Create a `.arcmesh` from the AssetBrowser; it opens in `MeshDocument` and previews.
- [ ] Change the source to each of the five; the preview reframes each time.
- [ ] A topology field cannot be dragged below its minimum (`MeshDocument`'s bounded widgets).
- [ ] Add `MeshRenderer` to an entity, pick the mesh through the Inspector's asset popup; it renders in the viewport.
- [ ] Scale the entity non-uniformly; shading stays correct (Task 8's fix, at the desk).
- [ ] Assign the hand-authored `ReferenceProject/Content/materials/reference_mesh.arcmat` through the Inspector; the mesh takes its `baseColor`. **Then edit that file's `baseColor` on disk and save** — the mesh follows on the next resolve. (Editing it *inside* the editor is not possible in F2a — see the amendment below.)
- [ ] ~~Create a material INSTANCE of it, assign that as `materialOverride` on a second entity~~ — **NOT PERFORMABLE IN F2a, deferred to F2b.** See the amendment below. The no-`tint` ruling therefore ships without its product-level validation, which is a known and accepted gap, not an oversight.
- [ ] Save the scene, close, reopen; mesh and material both survive.
- [ ] Pose the perspective camera through the Inspector to frame the mesh.
- [ ] The same scene renders identically in `ArcaneRuntime`.
- [ ] Gacha's `Game` project still opens at ABI 17.
- [ ] Run the `[gpu]` suite at the desk, including Task 8's new case and Task 7-of-Phase-4's existing `[pixel]` case.
- [ ] **Expected wrong-looking thing, NOT a bug:** a scene holding both a sprite and a mesh composites wrong — 3D draws over `Batcher2D` content until F5's declarative clear-op. Confirm it looks wrong in exactly that way and no other.

---

## Exit criteria

> **AMENDED 2026-08-22, after the final whole-branch review, by the user's ruling.**
> Criterion 1 as originally written was **not met and is not being met in F2a**. The
> final review established that there is no code path to create or edit a
> `"mesh"`-kind `.arcmat` inside the editor: `EditorApp::CreateMaterialAt` hardcodes
> `kind = "fullscreen"` (`EditorAppProject.cpp:402`), the surface picker offers only
> `Fullscreen\0Sprite\0` (`ShaderEditorDocument.cpp:2121`), and the params panel is
> decl-driven off a bound template — which a mesh material, having no snippet by
> design, does not have, so `baseColor` is not editable there.
>
> **The plan lost this in translation from the spec, and nothing checked the
> compression.** Two other spec requirements went the same way and are deferred with
> it: the `albedo`-declared-but-unbound memoized diagnostic naming the bindless task
> (spec `:242-247`, `:430-431`), and the "ignores `snippet`/`graph` with one
> diagnostic" rule (spec `:253-255`). Only the `passes`/`baseInputs` half of the
> latter shipped.
>
> **All three move to F2b**, which already owns the mesh-material story — it is where
> the cook step lands and where `albedo` actually becomes bindable, so the diagnostic
> that refuses it stops being a placeholder and starts being a real refusal. Task 13
> items 6 and 7 are rewritten above to match.

1. ~~A `.arcmesh` and a `"mesh"`-kind `.arcmat` can be created, edited, saved and reopened entirely inside the editor.~~
   **AMENDED:** A `.arcmesh` can be created, edited, saved and reopened entirely inside the
   editor. A `"mesh"`-kind `.arcmat` is **hand-authored JSON in F2a**; its editor
   authoring path is F2b's.
2. An entity carrying `MeshRenderer` renders in **both** hosts, posed by its `Transform`, from a scene that was saved and reopened.
3. A non-uniformly-scaled mesh shades correctly.
4. A perspective camera entity can be posed through its Transform in the Inspector to frame that mesh.
5. Gate green in all three configs, 0 warnings, from a clean rebuild.
