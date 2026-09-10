# F2c Mesh Import — Plan 2: Residency, Draws, and the Editor Surface

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn Plan 1's resolved `MeshData` into pixels and into a browsable asset. A resident GPU mesh-buffer cache with a byte budget replaces the per-frame upload ring for mesh geometry; sections become per-section draws with their own slot materials; a mesh cook's completion invalidates residency; the browser gains Model chrome and real harvested thumbnails; and one imported mesh joins ReferenceProject's golden scene so both lanes exercise drop-to-pixels every CI run.

**Architecture:** Residency lands device-free first (budget + LRU as pure logic), then as a real NRI cache, then as the vehicle's own object — three tasks that each end green with nothing yet consuming it. Only then does `MeshNode` switch off the ring, in two steps: parity first (one section, same pixels), sections second. Invalidation, the editor surface and the thumbnails follow, and the golden fixture lands last so exactly one re-bless cycle is needed.

**Tech Stack:** C++23, NRI (D3D12 + Vulkan), Dear ImGui, Catch2, msbuild `Arcane.slnx`, `scripts/golden-gate.ps1`.

**Spec:** `docs/specs/2026-09-10-f2c-mesh-import-design.md` (§7, §8, §9's end-to-end net; rulings R2 and R5). Research: `docs/research/2026-09-10-f2c-mesh-import-research.md` §3.4 (the two questions F2a left for F2c) and `docs/research/2026-09-10-f2c-ue-source2-comparison.md` Decisions 7 and 9.

## Global Constraints

- **THIS PLAN BEGINS AT PLAN 1's COMPLETED HEAD.** Every type it consumes — `LoadedClientMesh`, `MeshSectionView`, `MeshData::sections`, `MeshSlot`, `MeshResolveResult`/`MeshResolveState`'s pending/failed split, `Assets::MeshArtifactFor` / `InvalidateMeshArtifact` / `CookPending`, `AssetKind::Model` — is Plan 1's. Do not start Task 1 until `docs/plans/2026-09-10-f2c-mesh-import-plan1-pipeline.md` is complete and its Task 16 handoff note is in hand.
- **THE ABI STAYS 24.** Plan 1 bumped it once (23 → 24, tail-append); **this plan bumps nothing** and touches no `Assets` virtual, no `PluginABI.hpp` constant, and no `.arcproj` stamp. `NriMeshBufferCache` and `NriGraphContext`'s new members are render-internal — `NriGraphContext` is not a plugin-ABI type and the game module never names it.
- **Repo:** `D:\dev\starworks\Arcane`, branch `main`. **`out.txt` at the repo root is the user's — never stage it.**
- **Commit per task, do NOT push.** The user's desk pass at the end of this plan is the gate.
- **Build:** `msbuild Arcane.slnx /p:Configuration=Debug /m` from the repo root (vswhere locates msbuild). **`GenerateProjects.bat`** after ANY premake or file-list change (`ARCANE_SDK` is set).
- **Premake reality:** `ArcaneTests` **globs its own** `src/**.cpp` (a new test file needs no edit) but compiles EDITOR TUs from an **EXPLICIT list** — a new `ArcaneEditor/src/**.cpp` a test drives MUST be added there. `ArcaneEditor`'s own block globs, so the same file needs no entry there. No new ThirdParty lib in this plan.
- **Run tests FROM the exe dir:** `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "~[gpu]"`; the `[gpu]` lanes run as `./ArcaneTests.exe "[gpu]"` from the same directory. **Capture the seed banner** into any report that cites a run. `~[gpu]` is a BASELINE-COMPARABILITY convention, not a hazard gate — CI already runs the full suite.
- **Baseline: 55464 assertions / 1535 cases** at ARC start (`~[gpu]`, Debug), **+69 cases from Plan 1** — so this plan starts from a Plan-1 head whose derived count Task 16 of that plan recorded. **Re-derive that head's numbers in Task 1 Step 1 and use them as this plan's baseline; never carry a recalled number forward.** Every task states its own delta and attributes it to named cases.
- **Every task ends green:** the whole tree compiles, `~[gpu]` passes, and (for the render tasks) the `[gpu]` lane passes on the machine's own backend.
- **Anchors drift.** Every `file:line` is orientation against Plan 1's head. **Re-locate by SYMBOL name before editing.**
- **Golden discipline (Arc 2, non-negotiable):** assert `gatePassed` + per-lane `verdict` **from the gate JSON only** — never a raw process exit code, never a visual impression. **Copy diff artifacts into the workspace BEFORE any re-run** (the gate deletes stale diff PNGs). A bless writes the **SOURCE** tree and must be **restaged to BOTH hosts**. `golden-gate.ps1` stages `Content/` **additively**, so strays accumulate — sweep them.
- Commit message trailer, exactly:
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```

---

### Task 1: The residency budget and its LRU — device-free (spec §7.2, R2)

The eviction answer F2a assigned to F2c by name, landed as pure logic first so the policy is provable without a device. `NriTextureCache` has no budget of its own, so there is no existing shape to mirror here — only the LRU rule the spec states.

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/Nri/MeshResidencyBudget.hpp`
- Test: `ArcaneTests/src/MeshResidencyBudgetTest.cpp` (new; globbed)

**Interfaces:**

```cpp
    // MeshResidencyBudget -- the POLICY half of NriMeshBufferCache (spec s7.2 / R2),
    // split out DEVICE-FREE so the eviction rule is provable without an NRI device at
    // all. Same split NriTextureCache::Bc7RowPitch keeps for the same reason: the
    // arithmetic that decides what happens is testable independently of the API that
    // carries it out.
    //
    // ONE COMBINED CPU+GPU BYTE BUDGET. The CPU copy is KEPT (re-upload after eviction
    // or a device recreate; editor reads) and therefore COUNTED -- a budget that
    // ignored it would under-report by exactly the amount that is easiest to forget.
    //
    // 512 MiB, a compile-time constant for now. It becomes a cvar when the parked cvar
    // arc lands, and that arc's own trigger discipline decides when -- this constant is
    // NOT a placeholder to be "fixed" ahead of it. UE's analogue is r.Streaming.PoolSize
    // over one pool shared with textures (StreamingManagerTexture.cpp:455-458); Arcane
    // has no texture byte budget yet, so a separate mesh budget is the right first move
    // and unification is the eventual shape (comparison Decision 7, recorded there).
    inline constexpr std::uint64_t kMeshResidencyBudgetBytes = 512ull * 1024ull * 1024ull;

    // What one resident mesh costs, and when it was last DRAWN (not last resolved --
    // a mesh resolved every frame by the scene sweep but never visible must still be
    // evictable, or the budget protects exactly the wrong entries).
    struct MeshResidencyEntry
    {
        Guid          id;
        std::uint64_t bytes = 0;
        std::uint64_t lastDrawnFrame = 0;
    };

    // Which entries must be evicted, least-recently-drawn first, to bring `entries`
    // back within `budget`. Returns the guids to drop, in eviction order.
    //
    // NEVER EVICTS AN ENTRY DRAWN THIS FRAME (lastDrawnFrame == currentFrame), even
    // when that leaves the cache over budget -- evicting geometry the frame currently
    // being recorded still references is a use-after-free dressed as a policy, and a
    // frame that genuinely needs more than the budget must be allowed to render and be
    // reported, not silently corrupted. When the protected set alone exceeds the
    // budget this returns everything it CAN evict and the caller reports it once.
    [[nodiscard]] std::vector<Guid> SelectEvictions(
        std::span<const MeshResidencyEntry> entries,
        std::uint64_t budget,
        std::uint64_t currentFrame);

    // Bytes one entry occupies: the CPU copy (vertices + indices + the sections'
    // strings) plus the two GPU buffers, which are the SAME byte counts -- the GPU
    // copy is a verbatim upload of the CPU one, so this is exactly 2x the CPU size
    // plus the section table's own CPU-only cost. Written as one function so the
    // "counted twice, on purpose" fact has a single place to be read.
    [[nodiscard]] std::uint64_t MeshResidencyBytes(std::size_t vertexBytes,
                                                   std::size_t indexBytes,
                                                   std::size_t sectionBytes) noexcept;
```

- [ ] **Step 1: Re-derive this plan's baseline.** Run `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "~[gpu]"` at Plan 1's head, record the assertion/case counts and the seed banner, and write them at the top of this plan's working notes. **This is the number every later delta is attributed against** — derive it, never carry Plan 1's arithmetic forward on trust.

- [ ] **Step 2: Write the failing budget test** (`MeshResidencyBudgetTest.cpp`):

```cpp
TEST_CASE("mesh residency: nothing is evicted while under budget", "[render]")
{
    const std::vector<MeshResidencyEntry> entries = {
        { GuidA(), 100, 5 }, { GuidB(), 100, 7 },
    };
    CHECK(SelectEvictions(entries, 1000, /*currentFrame*/ 9).empty());
}

TEST_CASE("mesh residency: least-recently-DRAWN goes first", "[render]")
{
    const std::vector<MeshResidencyEntry> entries = {
        { GuidA(), 400, 2 },   // oldest
        { GuidB(), 400, 8 },
        { GuidC(), 400, 5 },
    };
    // Budget 1000, resident 1200 -> shed 200+, so exactly one entry goes: the oldest.
    const std::vector<Guid> evicted = SelectEvictions(entries, 1000, /*currentFrame*/ 9);
    REQUIRE(evicted.size() == 1u);
    CHECK(evicted[0] == GuidA());
}

TEST_CASE("mesh residency: eviction stops as soon as the budget is met", "[render]")
{
    // Not "evict until comfortable" -- evict the MINIMUM. A cache that over-sheds
    // re-uploads next frame, which is the cliff s7.2 exists to remove.
    const std::vector<MeshResidencyEntry> entries = {
        { GuidA(), 300, 1 }, { GuidB(), 300, 2 }, { GuidC(), 300, 3 },
    };
    const std::vector<Guid> evicted = SelectEvictions(entries, 700, 9);
    REQUIRE(evicted.size() == 1u);      // 900 - 300 = 600 <= 700; a second is waste
    CHECK(evicted[0] == GuidA());
}

TEST_CASE("mesh residency: an entry drawn THIS frame is never evicted", "[render]")
{
    // THE SAFETY RULE. Dropping geometry the in-flight frame still references is a
    // use-after-free wearing a policy's clothes.
    const std::vector<MeshResidencyEntry> entries = {
        { GuidA(), 800, /*lastDrawn*/ 9 },   // this frame -- PROTECTED
        { GuidB(), 800, /*lastDrawn*/ 3 },
    };
    const std::vector<Guid> evicted = SelectEvictions(entries, 500, /*currentFrame*/ 9);
    REQUIRE(evicted.size() == 1u);
    CHECK(evicted[0] == GuidB());
    // Still over budget after evicting everything evictable -- and that is correct.
    // The caller reports it; the cache does not corrupt the frame to satisfy a number.
}

TEST_CASE("mesh residency: a single mesh larger than the whole budget stays resident",
          "[render]")
{
    const std::vector<MeshResidencyEntry> entries = { { GuidA(), 2000, 9 } };
    CHECK(SelectEvictions(entries, 500, 9).empty());
}

TEST_CASE("mesh residency: the CPU copy is counted, on purpose", "[render]")
{
    // vertices 320 + indices 96 kept on BOTH sides, plus 40 bytes of CPU-only section
    // strings: 2*(320+96) + 40.
    CHECK(MeshResidencyBytes(320, 96, 40) == 872ull);
    // And the constant is what s7.2 pins, spelled so a typo is visible.
    CHECK(kMeshResidencyBudgetBytes == 536870912ull);
}
```

- [ ] **Step 3: Run — expect FAIL.** `./ArcaneTests.exe "[render]"`.
- [ ] **Step 4: Implement `MeshResidencyBytes`** — the one-line sum, with the comment explaining the doubling.
- [ ] **Step 5: Implement `SelectEvictions`** — sum the bytes; return empty when at or under budget; otherwise sort a copy of the evictable entries (`lastDrawnFrame != currentFrame`) ascending by `lastDrawnFrame`, taking guids until the running total is within budget or the evictable set is exhausted. **Tie-break on the guid** so the order is deterministic under equal frames — an unstable eviction order makes a `[gpu]` regression irreproducible.
- [ ] **Step 6: Run — expect PASS.**
- [ ] **Step 7: Full `~[gpu]` suite — green.** Delta: **+6 cases** (`[render]`). Commit — `feat(render): the mesh residency byte budget and its frame-boundary LRU`

---

### Task 2: `NriMeshBufferCache` — residency for real (spec §7.2)

The cache itself, mirroring `NriTextureCache`'s architecture: one object per vehicle, an injected supply, memoized failures, a graveyard-based `Release`/`Invalidate`, and the same device-loss posture.

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.hpp`, `NriMeshBufferCache.cpp`
- Test: `ArcaneTests/src/NriMeshBufferCacheTest.cpp` (new; globbed)

**Interfaces:**

```cpp
    // NriMeshBufferCache -- ONE Guid -> resident vertex/index buffer pair for the whole
    // render path, and the retirement of the per-frame upload ring for mesh geometry.
    //
    // WHY IT EXISTS (research s3.4-1): MeshNode::Record uploaded every visible mesh's
    // streams through the frame's TRANSIENT ring EVERY FRAME -- fine for unit
    // primitives, ~3 MB per frame per mesh for a 100k-triangle import. R2's ruling is
    // one cache owning CPU+GPU lifetime for ALL meshes, primitives included: the ring
    // path for mesh geometry retires outright rather than being kept as a small-mesh
    // fast path, because two upload paths is two lifetimes to reason about and the
    // resident one is correct for both sizes.
    //
    // ARCHITECTURE MIRRORS NriTextureCache, deliberately and point for point: an
    // injected supply (this class owns no file IO and no Assets knowledge -- a RENDER
    // object must not grow a Runtime), failures memoized exactly once, a one-shot WARN
    // latch, Release(graveyard, fence) as the sanctioned teardown with the destructor
    // as a SAFETY NET that says so at WARN, and Invalidate(id, graveyard, fence) as the
    // per-guid escape hatch. Read NriTextureCache.hpp's own banner first; every rule
    // there applies here unless this comment says otherwise.
    //
    // WHAT IS DIFFERENT FROM NriTextureCache, and why:
    //   * A BYTE BUDGET AND AN LRU (MeshResidencyBudget.hpp). NriTextureCache has
    //     neither -- it never evicts a single entry (NriGraphContext's own note says
    //     so). Mesh geometry is where eviction became a real question (F2a's spec
    //     assigned it here by name).
    //   * NO COLOUR SPACE. A vertex buffer has no sRGB question, so the key is a bare
    //     Guid rather than (Guid, space).
    //   * NO PLACEHOLDER. A pending texture gets a checkerboard, which is a real,
    //     visible, honest stand-in. There is no honest stand-in for a shape (s7.1:
    //     "a placeholder cube would fabricate a shape"), so a pending mesh resolves to
    //     NOTHING and the draw is skipped -- quietly while pending, loudly when
    //     missing or refused.
    //
    // NO BARRIERS, and none needed -- uploads go through nri::HelperInterface::
    // UploadData, which submits and waits internally, so these buffers are NOT graph
    // resources. RESOLVE AT DECLARATION TIME ONLY, never inside a node's exec fn: the
    // same rule, and the same reason, NriTextureCache states in full.
    class ARCANE_API NriMeshBufferCache
    {
    public:
        // Guid -> resolved CPU geometry, or a state saying why not. In production this
        // is the frame driver's forward to SceneRenderResolver's MeshTable (already
        // populated by the per-frame Request sweep); the returned pointer is READ
        // INSIDE the call and never stored.
        struct SupplyResult
        {
            const MeshData* mesh = nullptr;   // null unless state == Ready
            MeshResolveState state = MeshResolveState::Failed;
        };
        using MeshSupplyFn = std::function<SupplyResult(const Guid&)>;

        struct Resident
        {
            nri::Buffer*  vertexBuffer = nullptr;
            nri::Buffer*  indexBuffer  = nullptr;
            std::uint32_t indexCount   = 0;
            std::uint64_t bytes        = 0;
            std::uint64_t lastDrawnFrame = 0;
            // The CPU copy is KEPT (re-upload after eviction/device recreate; editor
            // reads) and counted in `bytes` -- see MeshResidencyBudget.hpp.
            MeshData      cpu;
        };

        static std::unique_ptr<NriMeshBufferCache> Create(NriDevice& device);
        ~NriMeshBufferCache();
        NriMeshBufferCache(const NriMeshBufferCache&)            = delete;
        NriMeshBufferCache& operator=(const NriMeshBufferCache&) = delete;

        void SetMeshSupply(MeshSupplyFn supply) { m_supply = std::move(supply); }

        // The resident pair for `id`, uploading on first sight, and stamping
        // `lastDrawnFrame = frameCounter` on every HIT -- which is what makes the LRU
        // mean "least recently DRAWN" rather than "least recently asked about".
        // Null while the mesh is PendingCook (quiet -- retried on the next call, no
        // memo, because a cook queue is expected to promote it) and null-and-memoized
        // when the supply Failed or the upload was refused.
        [[nodiscard]] const Resident* Resolve(const Guid& id, std::uint64_t frameCounter);

        // Frame-boundary eviction (s7.2). Called ONCE per frame by the vehicle, AFTER
        // recording and BEFORE the next frame's declarations, so nothing evicted here
        // can be named by a command buffer still being built. Buries evicted buffers at
        // `fence`, so an ALREADY-RECORDED frame still reading them is never invalidated
        // out from under it -- the same discipline NriTextureCache::Invalidate keeps.
        // Reports ONCE, at WARN, when the protected set alone exceeds the budget.
        void EvictToBudget(std::uint64_t frameCounter, Graveyard& graveyard,
                           std::uint64_t fence);

        // Drops residency for `id` so the NEXT Resolve treats it as brand new --
        // Resident, Pending or Refused, whichever it lands in. The escape hatch a
        // cook-completion callback needs (s7.3), and the un-latch a Refused key would
        // otherwise never get.
        void Invalidate(const Guid& id, Graveyard& graveyard, std::uint64_t fence);

        // Buries every NRI object this cache owns at `fence` and empties it.
        // Idempotent. THE DEVICE-LOSS / RECREATE HOOK TOO: a recreated device's
        // buffers are new objects, and because the CPU copy is kept, the next Resolve
        // re-uploads from memory rather than re-reading an artifact from disk.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        [[nodiscard]] std::size_t ResidentCount() const noexcept;
        [[nodiscard]] std::uint64_t ResidentBytes() const noexcept;

    private:
        // ... m_device, m_helper, m_supply, m_entries, m_refused, m_warnedOverBudget,
        //     m_warnedMiss -- the NriTextureCache member shape, minus the colour-space
        //     key and the placeholder slots.
    };
```

- [ ] **Step 1: Write the failing cache test** (`NriMeshBufferCacheTest.cpp`). Split exactly the way `NriTextureCacheArtifactTest.cpp` splits: **device-less logic cases first**, `[gpu]`-tagged upload cases after.

```cpp
TEST_CASE("mesh buffer cache: a pending mesh resolves to nothing, QUIETLY and retryably",
          "[render]")
{
    // s7.1's posture at the render layer: pending is not a failure, so it must not be
    // memoized -- the supply is asked again next call, and the frame it lands the mesh
    // appears. (Contrast NriTextureCache, which throttles its re-poll because its
    // supply reaches a directory scan; THIS supply is an in-memory table lookup the
    // resolver already refreshed this frame, so an unthrottled retry is free.)
    auto cache = MakeDevicelessCache();
    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) { ++asks; return SupplyResult{ nullptr, MeshResolveState::PendingCook }; });
    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    CHECK(cache->Resolve(GuidA(), 2) == nullptr);
    CHECK(asks == 2);                        // retried, not memoized
    CHECK(cache->ResidentCount() == 0u);
}

TEST_CASE("mesh buffer cache: a FAILED supply is memoized exactly once", "[render]")
{
    auto cache = MakeDevicelessCache();
    int asks = 0;
    cache->SetMeshSupply([&](const Guid&) { ++asks; return SupplyResult{ nullptr, MeshResolveState::Failed }; });
    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    CHECK(cache->Resolve(GuidA(), 2) == nullptr);
    CHECK(asks == 1);                        // attempted once, not once per frame
}

TEST_CASE("mesh buffer cache: Invalidate un-latches a memoized failure", "[render]")
{
    // Without this, a mesh whose cook had not started when it was first asked for
    // would stay Refused forever once the cook finally landed.
    /* Executor: fail once, Invalidate, then supply Ready and assert the next Resolve
       asks the supply again. Device-less, so assert on the ASK COUNT, not on a buffer. */
}

TEST_CASE("mesh buffer cache: no supply installed is a quiet, memoized miss", "[render]")
{
    auto cache = MakeDevicelessCache();
    CHECK(cache->Resolve(GuidA(), 1) == nullptr);
    CHECK(cache->Resolve(Guid{}, 1) == nullptr);   // a nil guid is the ordinary
                                                    // untextured case, silent
}

TEST_CASE("pixel: a mesh uploads once and stays resident across frames",
          "[gpu][meshcache]")
{
    // UPLOAD-ONCE is otherwise unobservable from outside -- ResidentCount is what
    // makes this a cache rather than a loader, exactly as NriTextureCache::
    // ResidentCount does for images.
    /* Executor: a real device via the [gpu] vehicle fixture NriTextureCacheArtifactTest
       already uses; supply BuildCube(1.0f); Resolve on frames 1..5; assert
       ResidentCount()==1 throughout, the returned pointer is stable, indexCount==36,
       and ResidentBytes() == MeshResidencyBytes(...) for that cube. */
}

TEST_CASE("pixel: EvictToBudget drops the least-recently-drawn mesh and re-uploads it"
          " on the next ask", "[gpu][meshcache]")
{
    /* Executor: resolve three meshes on distinct frames; EvictToBudget with a budget
       that fits two; assert ResidentCount()==2 and the oldest guid's next Resolve
       returns a NEW pointer (re-uploaded from the KEPT CPU copy -- assert the supply
       was NOT asked again, which is what proves the CPU copy is doing its job). */
}

TEST_CASE("pixel: Release buries everything and is idempotent", "[gpu][meshcache]")
{
    /* Executor: resolve two, Release, assert ResidentCount()==0; Release again --
       no crash, no double-bury (the graveyard's own in-order reaping is the check). */
}

TEST_CASE("pixel: a mesh with no indices or no vertices is refused, not uploaded",
          "[gpu][meshcache]")
{
    /* Executor: supply a MeshData with empty vectors; assert null, memoized, and that
       ResidentCount stays 0 -- a zero-size nri::Buffer is an NRI error, not a mesh. */
}
```

- [ ] **Step 2: Run — expect FAIL.**
- [ ] **Step 3: Implement `Create`/`~`/`Release`** — copy `NriTextureCache`'s exact shape: resolve `nri::HelperInterface` once, borrow the device, and give the destructor the same SAFETY-NET-NOT-THE-PATH posture (destroy behind a `DeviceWaitIdle` and say so at WARN, because there is no fence value to bury against there).
- [ ] **Step 4: Implement `Resolve`** — the entry is inserted BEFORE the first early return (the memoize-failures-exactly-once discipline `NriTextureCache` states), the supply is consulted, and the three states branch: Ready → upload; PendingCook → **erase the entry and return null** (no memo, so the next call retries); Failed → keep the null entry (memoized). On a hit, stamp `lastDrawnFrame = frameCounter` and return.
- [ ] **Step 5: Implement the upload** — two `nri::Buffer`s (`VERTEX_BUFFER` and `INDEX_BUFFER` usage, DEVICE-local), one `UploadData` call carrying both, then the byte accounting through `MeshResidencyBytes`. Any NRI failure → `Refused`, memoized, reported once through the shared one-shot latch, **with the partially-created buffer still stored so `Release` finds and destroys it** — the exact "never leaks the object it created even when it fails partway through" property `NriTextureCache::UploadArtifact` documents.
- [ ] **Step 6: Implement `EvictToBudget`** over `SelectEvictions`, burying each evicted entry's two buffers at `fence` and erasing it (**the CPU copy goes with it — that is the whole point of a byte budget that counts both**). One WARN, latched, when the protected set alone exceeds the budget, naming the resident bytes and the budget.
- [ ] **Step 7: Implement `Invalidate`** — bury this guid's buffers at `fence`, erase the entry (memoized-failure entries included), no-op for an unknown guid.
- [ ] **Step 8: Add the TU to premake?** No — `ArcaneClient`'s project block globs `src/**`, and `ArcaneTests` links `ArcaneClient`. Nothing to edit. **Verify by building rather than by assuming** — this is the one place a wrong assumption costs a confusing link error.
- [ ] **Step 9: Run `[render]` — expect PASS; run `[gpu][meshcache]` on the machine's own backend — expect PASS.**
- [ ] **Step 10: Full `~[gpu]` suite — green.** Delta: **+4 `~[gpu]` cases, +4 `[gpu]` cases**. Commit — `feat(render): NriMeshBufferCache -- resident mesh geometry with a byte-budget LRU`

---

### Task 3: The vehicle owns it (spec §7.2)

`NriGraphContext` creates, supplies, releases and evicts the cache — the same lifecycle `m_textures` already has, in the same declaration slot, for the same ordering reasons.

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp/.cpp`, `ArcaneClient/src/Arcane/Host/SceneRenderResolver.hpp/.cpp` (the supply's source)
- Test: `ArcaneTests/src/NriMeshBufferCacheTest.cpp` (extend), `ArcaneTests/src/OffscreenVehicleTest.cpp` (extend)

**Interfaces:**

```cpp
        // NriGraphContext.hpp -- beside SetPixelSupply/SetArtifactSupply.
        // Installed once by the frame driver, right after Create(). Without it every
        // mesh misses (loudly, once) and every mesh instance draws nothing.
        using MeshSupplyFn = NriMeshBufferCache::MeshSupplyFn;
        void SetMeshSupply(MeshSupplyFn supply)
        {
            if (m_meshBuffers) m_meshBuffers->SetMeshSupply(std::move(supply));
        }

        // The render-side half of a mesh cook's completion (s7.3) -- forwards to
        // NriMeshBufferCache::Invalidate with THIS context's own graveyard/fence, the
        // pair InvalidateContentTexture already uses. No-op without a cache.
        void InvalidateMeshGeometry(const Guid& id)
        {
            if (m_meshBuffers)
                m_meshBuffers->Invalidate(id, m_graves,
                                          m_graph ? m_graph->DebugSubmitCount() : 0);
        }

        [[nodiscard]] NriMeshBufferCache* MeshBuffers() noexcept { return m_meshBuffers.get(); }
```

**Declaration slot, and it is the contract:** `m_meshBuffers` goes **immediately after `m_textures`** — above `m_graph` and the nodes, so it is destroyed AFTER them (a node's recorded command buffer names this cache's buffers, exactly as `m_textures`' own placement comment argues for descriptor sets naming its views). Copy that comment's reasoning into the new member's.

- [ ] **Step 1: Write the failing vehicle test** (append to `NriMeshBufferCacheTest.cpp`):

```cpp
TEST_CASE("pixel: the vehicle owns one mesh buffer cache, released on teardown",
          "[gpu][meshcache]")
{
    /* Executor: create an offscreen vehicle (the OffscreenVehicleTest fixture);
       assert MeshBuffers() != nullptr; install a supply; resolve one mesh; assert
       ResidentCount()==1; destroy the vehicle and assert no NRI validation errors and
       RenderErrorCount()==0 -- the teardown-order property this task's declaration
       slot exists for. */
}

TEST_CASE("pixel: ResizeOffscreen does NOT release resident mesh buffers",
          "[gpu][meshcache]")
{
    // Resident meshes are PERSISTENT and are not pool tenants -- the same reason
    // m_textures is "deliberately NOT released on Resize" (its own declaration
    // comment). Re-uploading every mesh on a viewport drag would be the ring cliff
    // reintroduced through a different door.
    /* Executor: resolve a mesh, ResizeOffscreen to a new extent, assert
       ResidentCount() is unchanged and the pointer is the SAME. */
}
```

- [ ] **Step 2: Run — expect FAIL.**
- [ ] **Step 3: Add the member, the create call, the forwarders, and the teardown.** Create beside `m_textures` in the vehicle's init; release it in `~NriGraphContext` **before** the graveyard drain and beside the other Release calls; do **not** release it on `Resize`/`ResizeOffscreen` (with the comment saying why).
- [ ] **Step 4: Fill the supply from the resolver.** `SceneRenderResolver` already owns the `MeshCache` whose `Table()` the scene reads. Add `SceneRenderResolver::MeshSupply(const Guid&) -> {const MeshData*, MeshResolveState}` — a lookup in that table, returning `Ready` on a hit, and otherwise asking the cache which of Pending/Failed it is (Plan 1 Task 11 gave `MeshCache` the pending tri-state, so this is a query, not a re-resolve). The host wires `context.SetMeshSupply(...)` beside its existing `SetArtifactSupply` call.
- [ ] **Step 5: Add the eviction call.** Once per frame, **after** the frame's recording and before the next frame's declarations — the vehicle's own end-of-frame point, beside where it advances `m_frameIndex`. Pass the frame counter, `m_graves`, and the graph's submit count as the fence, the same triple `InvalidateContentTexture` uses.
- [ ] **Step 6: Run `[gpu][meshcache]` — expect PASS.**
- [ ] **Step 7: Full `~[gpu]` suite + the `[gpu]` lane — green.** Delta: **+2 `[gpu]` cases**. Commit — `feat(render): the vehicle owns the mesh buffer cache and evicts at frame boundaries`

---

### Task 4: `MeshNode` draws from the cache — the ring path retires (spec §7.2, §7.4 first half)

Parity step: the geometry source changes, the pixels do not. One draw per instance still, over the whole index range — sections arrive in Task 5. Splitting it this way is what makes the golden lanes a real check: **if `inprocess-lit-cube.png` moves by one pixel here, the upload path changed the picture and that is a bug, not a re-bless.**

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp/.cpp`, `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp` (the `AddMeshNode` declaration site)
- Test: `ArcaneTests/src/MeshNodeTest.cpp` (extend), `ArcaneTests/src/RenderGraphTest.cpp` (the device-less frame-shape cases)

**Interfaces:**

```cpp
    // MeshNode.hpp -- MeshInstance's geometry reference changes shape.
    struct MeshInstance
    {
        // THE MESH'S ASSET GUID, not a borrowed CPU pointer (F2c s7.2). Geometry is
        // resolved through the vehicle's NriMeshBufferCache at DECLARATION time and is
        // already RESIDENT on the device by the time this node records -- so the
        // borrow-lifetime contract this field used to carry (a raw MeshData* that had
        // to outlive the RenderFrame call, copied into the ring every frame) is gone
        // with the ring path it existed for.
        //
        // NIL means "draw nothing", which is not an error (a scene may legitimately
        // carry a slot with no geometry yet) -- unchanged.
        Guid mesh{};

        glm::mat4 model{1.0f};
        glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
        std::uint32_t materialSlot = BindlessTable::kInvalidSlot;
        // Task 5 appends `section`; this task leaves the whole mesh as one draw.
    };
```

`MeshNode::Prepare` gains a second parameter — the resolved residency, looked up at declaration time:

```cpp
        // Resolves the PIPELINE **and** this frame's mesh residency. The residency half
        // is here for the same reason the pipeline half is: NriMeshBufferCache::Resolve
        // uploads through HelperInterface::UploadData, which SUBMITS AND WAITS
        // INTERNALLY -- doing that inside an open command buffer is exactly the shape
        // the graph's no-hand-barriers rule exists to prevent (NriTextureCache.hpp's NO
        // BARRIERS section states it for images; geometry is the same hazard with a
        // bigger payload). Record() therefore only ever LOOKS UP what Prepare made
        // resident, and never resolves.
        void Prepare(nri::Format canvasFormat, const MeshSceneDesc& scene,
                     NriMeshBufferCache& meshBuffers, std::uint64_t frameCounter);
```

- [ ] **Step 1: Write the failing node test** (extend `MeshNodeTest.cpp`):

```cpp
TEST_CASE("mesh node: an instance names its mesh by GUID, not by borrowed pointer",
          "[nri]")
{
    // A compile-shaped assertion, deliberately: the field's TYPE is the contract, and
    // this case is what makes a future revert to a raw pointer fail loudly instead of
    // quietly reintroducing the ring's lifetime rule.
    MeshInstance instance;
    instance.mesh = GuidA();
    CHECK(instance.mesh.IsValid());
    static_assert(std::is_same_v<decltype(MeshInstance::mesh), Arcane::Guid>);
}

TEST_CASE("pixel: a cube drawn from the resident cache matches the ring's own pixels",
          "[gpu][meshnode]")
{
    // THE PARITY CASE, and the reason this task is separate from Task 5. The
    // in-process lit-cube golden (GoldenImageTest.cpp's own [gpu][golden] case) is the
    // authoritative version of this check; this one exists so a mismatch is diagnosed
    // HERE, against a two-triangle-simple scene, rather than as an unexplained golden
    // diff three tasks later.
    /* Executor: render the same hand-authored lit cube the golden case builds, capture
       it, and compare against ReferenceProject/Verify/References/inprocess-lit-cube.png
       through the SAME ImageCompare the golden case uses. Byte-identical is not
       required -- the comparator's own tolerance is; what is required is that the
       verdict is a MATCH. */
}

TEST_CASE("pixel: an instance whose mesh is not resident is SKIPPED, not drawn wrong",
          "[gpu][meshnode]")
{
    /* Executor: a scene with two instances, one naming a guid the supply refuses;
       assert the frame renders (RenderErrorCount()==0) and the refused instance
       contributes nothing -- never a stale buffer from the other instance, which is
       what a missing null check would produce. */
}
```

- [ ] **Step 2: Run — expect FAIL.**
- [ ] **Step 3: Retarget `MeshInstance::mesh` to a `Guid`** and update its comment in full — the borrow-lifetime paragraph becomes the residency paragraph, and `MeshSceneDesc::instances`' own borrowing note keeps only the SPAN half (the elements no longer point anywhere).
- [ ] **Step 4: Move residency resolution into `Prepare`.** For every distinct guid in `scene.instances`, call `meshBuffers.Resolve(guid, frameCounter)` and store the results in a `std::vector<std::pair<Guid, const Resident*>>` **member** (a reserved member, not a local — the same steady-state-allocates-nothing rule `m_uploads` carried, and the same honest caveat: past the reservation it grows, once per high-water mark). `m_uploads`, `Upload`, `kInitialUploadSlots`, `m_warnedRingOverflow` and the whole `uploadFor` lambda are **DELETED** — that is the ring path retiring, and there is no small-mesh fast path left behind.
- [ ] **Step 5: `Record` looks up, never resolves.** Replace the `uploadFor(instance.mesh)` call with a lookup in the Prepare-built table; a miss (`nullptr`) `continue`s, exactly as an empty mesh did. `CmdSetVertexBuffers`/`CmdSetIndexBuffer` bind the resident buffers at offset 0; the `lastMesh` dedupe stays (**keyed on the Guid now**, which is strictly better: two instances of one mesh still bind once, and the comparison is a value compare rather than a pointer identity that only held because the table deduped for it).
- [ ] **Step 6: Update the declaration site.** `AddMeshNode`'s Setup already calls `Prepare(canvasFormat)`; it gains the scene, the vehicle's `MeshBuffers()` and the frame counter. `AddMeshNode`'s own `context`-may-be-null contract is unchanged — a null context skips Prepare entirely, which is what keeps the device-less `[nri]` frame-shape cases in `RenderGraphTest.cpp` working.
- [ ] **Step 7: Update `CollectMeshInstances`** (`MeshSubmissionSystem.hpp`) — `out.push_back(MeshInstance{ &entry->data, ... })` becomes `{ renderer.mesh, ... }`, i.e. the component's own guid. The `MeshTable` lookup stays (it is still what decides whether the entity is drawable at all) but the `&entry->data` borrow disappears, and with it that function's whole NO MeshData COPY / borrow-contract paragraph — **replace it, do not just delete it**, with the residency contract that took its place.
- [ ] **Step 8: Update the harvester.** `MaterialPreviewHarvester.cpp` builds a `MeshInstance` with `mi.mesh = &sphere` for its mesh-material thumbnails. Its preview vehicle needs a mesh supply of its own — install one that answers a synthetic guid with the session sphere (the same synthetic-guid idiom it already uses for thumbnail textures). **Do this here, in the same commit that changes the field**, or mesh-material thumbnails silently go blank between tasks.
- [ ] **Step 9: Run `[nri]` + `[gpu][meshnode]` + `[gpu][golden]` — expect PASS.** The in-process lit-cube golden is the sharp edge: **it must MATCH without a re-bless.** If it does not, the upload path changed the picture — find out why before continuing; a re-bless here would launder a real regression.
- [ ] **Step 10: Full `~[gpu]` suite + `[gpu]` lane — green.** Delta: **+1 `~[gpu]` case, +2 `[gpu]` cases**. Commit — `refactor(render): mesh geometry draws from the resident cache; the ring path retires`

---

### Task 5: Per-section draws (spec §7.4, §4.4, A1)

One draw per section, each with its own slot's material. `MeshInstance` gains a section, and `CollectMeshInstances` emits one instance per section.

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp/.cpp`, `ArcaneClient/src/Arcane/Scene/MeshSubmissionSystem.hpp`, `ArcaneClient/src/Arcane/Host/SceneRenderResolver.cpp` (the per-slot material Request sweep Plan 1 Task 10 already widened)
- Test: `ArcaneTests/src/MeshSubmissionTest.cpp` (extend), `ArcaneTests/src/MeshNodeTest.cpp` (extend)

**Interfaces:**

```cpp
    // MeshInstance gains the section it draws.
        // WHICH SECTION of `mesh` this instance draws (F2c s7.4). A multi-section prop
        // becomes sections.size() instances, one per section, each carrying the
        // material its slot resolved to -- which is why the 128-byte zero-headroom
        // MeshConstants block (mesh.hlsl:20-28) is UNTOUCHED by sections: the material
        // identity already travels per draw, in materialSlot.
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount  = 0;   // 0 == "the whole mesh", the F2a shape
```

**The chain, restated (§4.4):** component `materialOverride` (scalar; **when set, repaints ALL sections** — §2's non-goal, kept and now actually exercised) → `slots[section.slotIndex].material` → white.

- [ ] **Step 1: Write the failing submission test** (extend `MeshSubmissionTest.cpp`):

```cpp
TEST_CASE("mesh submission: a three-section mesh emits three instances with per-slot"
          " materials", "[scene]")
{
    // A1 end to end: two sections share slot 0, the third uses slot 1 -- so two
    // instances carry the SAME material and the third carries the other. Emitting one
    // instance per SLOT instead would silently merge the two ranges into one draw and
    // lose the third's geometry.
    MeshEntry entry;
    entry.data.sections = { { "Metal", 0, 6, 0 }, { "Metal", 6, 3, 0 }, { "Paint", 9, 3, 1 } };
    entry.slots = { { "Metal", metalMat }, { "Paint", paintMat } };
    /* publish MeshTable + MeshMaterialTable with distinct baseColors, one entity */
    std::vector<MeshInstance> out;
    CollectMeshInstances(reg, out);
    REQUIRE(out.size() == 3u);
    CHECK(out[0].indexOffset == 0u);  CHECK(out[0].indexCount == 6u);
    CHECK(out[1].indexOffset == 6u);  CHECK(out[1].indexCount == 3u);
    CHECK(out[2].indexOffset == 9u);  CHECK(out[2].indexCount == 3u);
    CHECK(out[0].baseColor == out[1].baseColor);        // same slot -> same material
    CHECK(out[2].baseColor != out[0].baseColor);
    // Every instance names the same geometry -- MeshNode binds it once (the lastMesh
    // dedupe), then issues three draws.
    CHECK(out[0].mesh == out[2].mesh);
}

TEST_CASE("mesh submission: materialOverride repaints EVERY section", "[scene]")
{
    // s2's recorded non-goal, exercised rather than assumed: the override is scalar and
    // when set it wins for all sections. Per-section override ARRAYS wait for a real
    // need -- this test is what makes the current behaviour deliberate.
    /* Executor: the same three-section fixture, plus renderer.materialOverride naming a
       third material; assert all three instances carry ITS baseColor. */
}

TEST_CASE("mesh submission: a section whose slot is unassigned falls through to white",
          "[scene]")
{
    // The chain's tail. An imported mesh mints slots UNASSIGNED (Plan 1 Task 14), so
    // this is the ordinary state of a freshly imported prop, not an edge case.
    /* Executor: slots = { {"Metal", nil} }; assert baseColor == vec4(1) and
       materialSlot == BindlessTable::kInvalidSlot. */
}

TEST_CASE("mesh submission: a single-section F2a mesh still emits exactly one instance",
          "[scene]")
{
    // The regression net for every existing scene: a generated primitive carries one
    // whole-range section (Plan 1 Task 10's builder invariant), so it emits one
    // instance covering everything -- byte-identical submission to F2a's.
    /* Executor: BuildCube-backed entry; assert out.size()==1, indexOffset==0,
       indexCount == the cube's own index count. */
}
```

- [ ] **Step 2: Write the failing node test** (extend `MeshNodeTest.cpp`): a `[gpu][meshnode]` case rendering a two-section mesh whose two sections carry visibly different base colours, capturing, and asserting **both colours are present in the frame** — the check that a per-section draw actually happened rather than one draw over the whole range wearing the first section's material.
- [ ] **Step 3: Run — expect FAIL.**
- [ ] **Step 4: Add the two fields to `MeshInstance`** with the comment above; `indexCount == 0` keeps meaning "the whole mesh" so any caller that has not been taught sections still draws correctly (the harvester's sphere is exactly such a caller).
- [ ] **Step 5: `Record` uses them.** `nri::DrawIndexedDesc` gains `baseIndex = instance.indexOffset` and `indexNum = instance.indexCount ? instance.indexCount : resident->indexCount`. Nothing else in the draw loop changes — the layout, the root constants and the bindless set are all per-instance already.
- [ ] **Step 6: `CollectMeshInstances` emits per section.** Loop `entry->data.sections`; per section resolve `materialOverride` → `entry->slots[section.slotIndex].material` (**bounds-checked** — a `slotIndex` past the slot array is a corrupt or hand-edited `.arcmesh`, and it must fall through to white rather than index out of bounds) → white; push one instance. Replace the deferral comment Plan 1 Task 10 left with the real chain.
- [ ] **Step 7: Run `[scene]` + `[gpu][meshnode]` — expect PASS.**
- [ ] **Step 8: Full `~[gpu]` suite + `[gpu]` lane — green.** The in-process lit-cube golden must still **match**: a cube is one section, so its submission is unchanged. Delta: **+4 `~[gpu]` cases, +1 `[gpu]` case**. Commit — `feat(render): per-section mesh draws with per-slot materials`

---

### Task 6: Cook-completion invalidation for meshes (spec §7.3)

Mirrors the texture family wire for wire: cook completion → `Assets::InvalidateMeshArtifact` → the resolver's `MeshCache` entry → the residency entry → the next resolve re-requests.

**Files:**
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp` (`OnCookCompleted`), `ArcaneClient/src/Arcane/Host/SceneRenderResolver.hpp/.cpp` (`InvalidateMeshArtifact`)
- Test: `ArcaneTests/src/SceneRenderResolverTest.cpp` (extend), `ArcaneTests/src/CookQueueTest.cpp` (extend)

**Interfaces:**

```cpp
        // SceneRenderResolver.hpp -- beside InvalidateMesh.
        // A mesh ARTIFACT changed (a cook landed for the .gltf/.glb this mesh imports
        // from). Distinct from InvalidateMesh, which is a .arcmesh SAVE:
        //   * InvalidateMeshArtifact  -> the GEOMETRY changed. Drop the resolved
        //                                MeshEntry AND the resident GPU buffers, and
        //                                re-request.
        //   * InvalidateMesh          -> the .arcmesh changed (a slot reassignment, a
        //                                topology edit). Drop the resolved MeshEntry and
        //                                re-request, but KEEP THE RESIDENT BUFFERS when
        //                                only slots moved -- see Task 7.
        // `id` here is the .arcmesh's guid, not the model's: the caller maps the cooked
        // MODEL guid to its companion through the reference index, because the render
        // side keys everything on the mesh asset the scene actually names.
        void InvalidateMeshArtifact(const Guid& id);
```

- [ ] **Step 1: Write the failing resolver test** (extend `SceneRenderResolverTest.cpp`):

```cpp
TEST_CASE("scene resolver: InvalidateMeshArtifact drops geometry AND residency",
          "[scene]")
{
    // The two-layer drop s7.3 requires. Dropping only the MeshEntry would leave the
    // OLD vertices resident on the device, so a re-exported prop would keep rendering
    // its previous shape until something unrelated evicted it.
    /* Executor: resolve a mesh through a fake supply; note the MeshTable entry and the
       recorded residency-invalidate calls (a counting fake installed through Services);
       call InvalidateMeshArtifact; assert BOTH were dropped and the entry was
       re-requested synchronously (the Invalidate-THEN-Request shape InvalidateSprite/
       InvalidateMesh already keep, and for the same no-gap reason). */
}

TEST_CASE("scene resolver: a PENDING mesh is not latched as failed by invalidation",
          "[scene]")
{
    // The un-latch, from the other direction: invalidating a guid whose cook has not
    // landed must leave it re-askable, not memoized broken.
    /* Executor: supply PendingCook; Invalidate; assert the next Request asks again. */
}
```

- [ ] **Step 2: Write the failing editor test** (extend `CookQueueTest.cpp`): a `CookResult` naming a Model guid drives the editor's completion path, and a counting fake proves `InvalidateMeshArtifact` was called for that model's **companion** guid (found through the reference index), not for the model guid itself.
- [ ] **Step 3: Run — expect FAIL.**
- [ ] **Step 4: Implement `SceneRenderResolver::InvalidateMeshArtifact`** — `meshes->Invalidate(id); meshes->Request(id);` plus a call through a new `Services` seam `invalidateMeshGeometry(const Guid&)` that the host fills with `NriGraphContext::InvalidateMeshGeometry`. **The seam, not a direct call:** `SceneRenderResolver` is device-free by charter and reaches the device only through injected callbacks — the exact shape `resolveMeshAlbedoSlot` already uses (`SceneRenderResolver.hpp`'s own note on why).
- [ ] **Step 5: Wire `OnCookCompleted`.** Inside the existing `cookedGuids` loop, for a guid whose registered path classifies as `AssetKind::Model`: find its companion `.arcmesh` (the `DerivesFrom` edge Plan 1 Task 12 published — the editor's `AssetReferenceIndex` already holds the inverse), and call `m_resolver->InvalidateMeshArtifact(companion)`. **Placement:** after Plan 1 Task 14's `MintOrUpdateCompanionMesh` call for the same guid, so a FIRST cook mints the companion and only then invalidates it (a companion that did not exist a line earlier cannot be invalidated). Add the `AssetActivityKind::Cooked` feed entry the texture path already pushes — it is already in the shared loop, so verify rather than add.
- [ ] **Step 6: The document-preview gap is RE-ACCEPTED, not re-litigated.** F2b ruled document-preview `NriGraphContext` instances are not invalidated on cook completion (viewport-only; heals on close/reopen), and §7.3 re-accepts it by the same ruling. **Write that sentence as a comment at the invalidation site**, citing both specs, so the next reader finds the ruling instead of filing it as a bug.
- [ ] **Step 7: Run `[scene]` + `[editor]` — expect PASS.**
- [ ] **Step 8: Desk check.** With a `.glb` in a scratch project's scene, edit and re-export it (or just touch + edit the `.bin` for a `.gltf`): the viewport picks up the new geometry within a poll interval, without an editor restart. Record it.
- [ ] **Step 9: Full `~[gpu]` suite — green.** Delta: **+3 cases**. Commit — `feat(engine): mesh cook completion invalidates geometry and residency`

---

### Task 7: A slot reassignment keeps the resident buffers (spec §7.3 second half)

The other half of the invalidation split, and the one with a real cost behind it: **re-pointing a material must never re-upload a two-million-triangle prop.**

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/SceneRenderResolver.cpp` (`InvalidateMesh`), `ArcaneClient/src/Arcane/Scene/SceneResources.hpp` (`MeshEntry`), `ArcaneClient/src/Arcane/Render/MeshCache.cpp` (fill the two new fields)
- Test: `ArcaneTests/src/SceneRenderResolverTest.cpp` (extend)

**Interfaces:**

```cpp
    // SceneResources.hpp -- MeshEntry gains the GEOMETRY IDENTITY the split needs.
    struct MeshEntry
    {
        MeshData              data;
        MeshBounds            bounds;
        std::vector<MeshSlot> slots;

        // F2c s7.3: what a .arcmesh save must be compared on to decide whether the
        // RESIDENT BUFFERS survive it. A slot reassignment changes neither of these
        // and must not re-upload a two-million-triangle prop; a source switch, or a
        // re-pointed importedSource, changes the geometry and must. Copied at
        // MeshCache::Request time beside `slots`, and read NOWHERE ELSE -- they exist
        // for exactly this comparison, which is why they say so here.
        MeshSource source = MeshSource::Cube;
        Guid       importedSource{};
    };
```

- [ ] **Step 1: Write the failing test:**

```cpp
TEST_CASE("scene resolver: a .arcmesh SAVE re-resolves slots but keeps residency",
          "[scene]")
{
    // s7.3's distinction, and the reason the two entry points exist at all. A slot
    // reassignment changes WHICH MATERIAL a section draws with -- nothing about the
    // vertices -- so the resident buffers must survive it. Merging the two paths
    // would make every material tweak a full geometry re-upload.
    /* Executor: resolve a mesh (residency-invalidate fake at 0 calls); save a .arcmesh
       whose SLOTS changed but whose source/topology did not; call InvalidateMesh;
       assert the MeshEntry re-resolved with the NEW slot guids AND the residency fake
       was NOT called. */
}

TEST_CASE("scene resolver: a .arcmesh whose SOURCE changed does drop residency",
          "[scene]")
{
    // The boundary, so the optimisation cannot silently become a correctness hole:
    // switching source Cube -> Imported, or re-pointing importedSource, is a GEOMETRY
    // change and must drop the buffers.
    /* Executor: same fixture, but the saved .arcmesh changes `source` (or
       `importedSource`); assert the residency fake WAS called. */
}
```

- [ ] **Step 2: Run — expect FAIL** (today's `InvalidateMesh` drops nothing device-side, so the first case passes vacuously and the second fails — **which is exactly why both are written: the first is the positive control that keeps the second honest**).
- [ ] **Step 3: Implement the split in `InvalidateMesh`.** Before dropping the `MeshEntry`, capture its geometry identity — `{source, importedSource}` — from the CURRENTLY resolved entry; re-resolve; compare. Unchanged → leave residency alone. Changed (or the entry did not exist) → call the `invalidateMeshGeometry` seam. **`MeshEntry` must carry those two fields for this comparison to be possible** — add `MeshSource source` and `Guid importedSource` to it in this task, copied at `MeshCache::Request` time beside `slots`, with a comment saying they exist for exactly this test.
- [ ] **Step 4: Update `InvalidateMesh`'s own doc comment** (`SceneRenderResolver.hpp`) — it currently says nothing about the device side, which was true when there was no device side. State both arms and cite §7.3.
- [ ] **Step 5: Run `[scene]` — expect PASS.**
- [ ] **Step 6: Full `~[gpu]` suite — green.** Delta: **+2 cases**. Commit — `feat(engine): a slot reassignment re-resolves materials without re-uploading geometry`

---

### Task 8: Browser chrome for `AssetKind::Model` (spec §4.1, §8)

The editor half Plan 1 deliberately left: the graph hue row, the browser rail's creatable gate, and the derived fold that puts a companion `.arcmesh` under its source.

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetGraphPanel.cpp` (`KindAccentColor`), `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp` (`RailKindCreatable`), `ArcaneEditor/src/Panels/AssetPanelModel.cpp` (the fold predicate)
- Test: `ArcaneTests/src/AssetPanelModelTest.cpp` (extend), `ArcaneTests/src/AssetGraphViewModelTest.cpp` (extend)

**Interfaces:** none — three table/predicate edits.

- [ ] **Step 1: Write the failing model test** (extend `AssetPanelModelTest.cpp`):

```cpp
TEST_CASE("asset model: a companion .arcmesh folds under its Model source", "[editor]")
{
    // s4.2/s8: the sprite-under-texture foldedUnder machinery, reused with ZERO new
    // code -- all it needed was the DerivesFrom edge (Plan 1 Task 12) and a fold
    // predicate that accepts a Model target as well as a Texture one.
    /* Executor: entries for a .glb (Model) and a .arcmesh (Mesh) whose refs carry one
       DerivesFrom edge at the model; rebuild. */
    CHECK(model.Entry(meshGuid)->foldedUnder == modelGuid);
    CHECK(model.Entry(modelGuid)->derivedChildren == 1u);
}

TEST_CASE("asset model: a .arcmesh deriving from something that is NOT a Model or a"
          " Texture does not fold", "[editor]")
{
    // The predicate stays a whitelist, not "anything with one DerivesFrom" -- a fold
    // under an arbitrary asset would put a mesh inside a scene row.
    /* Executor: a DerivesFrom edge pointing at an .arcmat; assert foldedUnder is nil. */
}

TEST_CASE("asset model: an unreferenced Model reports as unused", "[editor]")
{
    // IsUnusedEligible(Model) was set in Plan 1 Task 9; this is the end-to-end half --
    // a .glb nobody imports (no companion) shows in the Unreferenced card, exactly
    // like a texture no sprite uses.
    /* Executor: a Model entry with zero inbound edges; assert it appears in
       model.UnusedGuids(). */
}
```

- [ ] **Step 2: Write the failing graph test** (extend `AssetGraphViewModelTest.cpp`): a `Model` node's accent is a distinct hue, not the neutral grab gray fallback.
- [ ] **Step 3: Run — expect FAIL.**
- [ ] **Step 4: Widen the fold predicate.** `AssetPanelModel.cpp`'s `derivesFromCount == 1` branch tests `AssetKindOf(*itT->second) == AssetKind::Texture`; it becomes `== Texture || == Model`. **Update the comment above it** — it currently says "a folding sprite's DerivesFrom names a TEXTURE, never another material", which is now half the story.
- [ ] **Step 5: Add the hue row.** `KindAccentColor` gains `case AssetKind::Model:` with a hue distinct from the five §11.3 rows and from each other. **The asset-manager spec's §11.3 pins exactly five rows and this is not one of them**, so this is a NEW value, not a spec quotation — pick it adjacent to `Mesh`'s `#5b9bb0` in the same muted family (a Model and its Mesh are kin and should read as such), record the hex in the comment, and say in that comment that it extends §11.3's table rather than quoting it. Suggested `#5b7fb0` — verify it is visually separable from `Mesh` and from `Sprite`'s `#9b5bb0` at the desk before committing.
- [ ] **Step 6: `RailKindCreatable`** — leave `Model` returning **false**. There is no Create▸Model action and there should not be: a model is imported, never authored in-editor. Add it as an explicit `// Model: imported, never created -- no Create menu entry (s8's F2c/F4 line)` note in the `default:` arm's comment rather than a silent omission.
- [ ] **Step 7: Run `[editor]` — expect PASS.**
- [ ] **Step 8: Desk glance.** Launch the editor against a scratch project holding an imported mesh: the Browser shows a `Models` rail row, the `.glb` row carries its own icon, the companion `.arcmesh` folds under it with a chevron, and the Graph panel draws Model → Mesh → Material as a connected web.
- [ ] **Step 9: Full `~[gpu]` suite — green.** Delta: **+4 cases**. Commit — `feat(editor): Model kind chrome -- graph hue, rail row, and the companion fold`

---

### Task 9: Mesh thumbnails — the harvest (spec §8, R5)

`MaterialPreviewHarvester` already renders a lit UV sphere for a mesh MATERIAL. Extending it to render an arbitrary **mesh asset**, framed by the artifact's AABB, is the whole of R5 — the architecture, the LIFO queue, the one-harvest-per-frame device-idle discipline and the `Saved/Thumbnails/<guid>.png` persistence all carry over unchanged.

**Files:**
- Modify: `ArcaneEditor/src/Project/MaterialPreviewHarvester.hpp/.cpp`
- Test: `ArcaneTests/src/MeshThumbnailFramingTest.cpp` (new; globbed)

**Interfaces:**

```cpp
    // MaterialPreviewHarvester.hpp -- the class name stays (renaming it would touch
    // every call site for no behavioural gain), but its charter widens by one subject
    // and the file header must say so: it harvests thumbnails for MATERIALS and, from
    // F2c, for MESH ASSETS (R5 -- "extend the material-harvester architecture", which
    // is what this is rather than a second harvester competing for the same device
    // idle and the same compile coalesce keys).
    //
    // A MESH thumbnail compiles NOTHING (a mesh-kind .arcmat has no snippet, and the
    // mesh path was already the harvester's one compile-free branch), so it needs no
    // new stage-key slot in the process-wide scheme this file documents. That is worth
    // stating: the stage-key table is the thing a second harvester would have had to
    // extend, and not needing to extend it is the evidence this belongs here.
    void RequestMesh(const Arcane::Guid& mesh);
    void InvalidateMesh(const Arcane::Guid& mesh);
```

```cpp
    // The framing math, pulled out PURE so it is testable without a device -- the same
    // split MeshResidencyBudget takes from NriMeshBufferCache, and for the same reason.
    // Given a local-space AABB and a vertical FOV, where does the camera sit to frame
    // the whole box with a small margin, looking at its centre?
    //
    // MARGIN, not a tight fit: a box that exactly fills the frame reads as cropped at
    // thumbnail size, and the browser draws these at 64px. 15% is the mocks' own feel.
    //
    // A DEGENERATE BOX (a zero-extent AABB -- ComputeMeshBounds' documented answer for
    // an empty mesh) yields a finite camera at a unit distance rather than a division
    // by zero. An empty mesh has nothing to frame, and the harvest will produce an
    // empty picture, which is the honest result -- but it must not produce a NaN
    // transform, which is undefined behaviour on the GPU rather than a blank image.
    struct MeshThumbCamera { glm::vec3 eye; glm::vec3 target; float nearZ; float farZ; };
    [[nodiscard]] MeshThumbCamera FrameMeshBounds(const Arcane::MeshBounds& bounds,
                                                  float fovDegrees);
```

- [ ] **Step 1: Write the failing framing test** (`MeshThumbnailFramingTest.cpp`):

```cpp
TEST_CASE("mesh thumb framing: the camera looks at the box centre from outside it",
          "[editor]")
{
    Arcane::MeshBounds b; b.min = { -1, -2, -3 }; b.max = { 3, 4, 5 };
    const MeshThumbCamera c = FrameMeshBounds(b, 35.0f);
    CHECK(c.target.x == Catch::Approx(1.0f));   // the centre, per axis
    CHECK(c.target.y == Catch::Approx(1.0f));
    CHECK(c.target.z == Catch::Approx(1.0f));
    // Outside the box's own radius, so the whole thing is in front of the camera.
    const float radius = glm::length((b.max - b.min) * 0.5f);
    CHECK(glm::length(c.eye - c.target) > radius);
    CHECK(c.nearZ > 0.0f);
    CHECK(c.farZ > c.nearZ);
}

TEST_CASE("mesh thumb framing: a bigger box pushes the camera further out", "[editor]")
{
    // The property that makes one framing function work for a 1 m crate and a 40 m
    // building without a per-asset knob.
    Arcane::MeshBounds small; small.min = { -1, -1, -1 }; small.max = { 1, 1, 1 };
    Arcane::MeshBounds big;   big.min   = { -10, -10, -10 }; big.max = { 10, 10, 10 };
    const float dSmall = glm::length(FrameMeshBounds(small, 35.0f).eye
                                   - FrameMeshBounds(small, 35.0f).target);
    const float dBig   = glm::length(FrameMeshBounds(big, 35.0f).eye
                                   - FrameMeshBounds(big, 35.0f).target);
    CHECK(dBig > dSmall * 5.0f);
}

TEST_CASE("mesh thumb framing: a degenerate box yields a finite camera", "[editor]")
{
    // ComputeMeshBounds returns a ZERO box for an empty mesh, deliberately (its own
    // comment: "a caller framing an empty mesh needs a degenerate box it can still
    // build a camera from"). This is that caller, and a NaN here would be undefined
    // behaviour on the GPU rather than a blank thumbnail.
    const MeshThumbCamera c = FrameMeshBounds(Arcane::MeshBounds{}, 35.0f);
    CHECK(std::isfinite(c.eye.x)); CHECK(std::isfinite(c.eye.y)); CHECK(std::isfinite(c.eye.z));
    CHECK(c.nearZ > 0.0f);
    CHECK(c.farZ > c.nearZ);
}

TEST_CASE("mesh thumb framing: a narrower FOV pushes the camera further out", "[editor]")
{
    /* Executor: same box, 20 vs 60 degrees; assert the 20-degree distance is greater.
       The check that the fov parameter is USED, not decorative. */
}
```

- [ ] **Step 2: Run — expect FAIL.** `./ArcaneTests.exe "[editor]"`.
- [ ] **Step 3: Add `MeshThumbnailFraming.hpp/.cpp`** — or, better, put `FrameMeshBounds` in `MeshImportWave.hpp/.cpp`, which is **already in `ArcaneTests`' explicit editor TU list** (Plan 1 Task 13). One fewer premake edit and one fewer file; the unit is "pure editor-side import/preview helpers", which this is. **If it goes anywhere else, add that TU to the explicit list — a missing entry is a link error, not a compile error, and it is the single easiest thing to forget in this repo.**
- [ ] **Step 4: Implement `FrameMeshBounds`** — centre, radius, `distance = radius / sin(fov/2) * (1 + margin)`, eye on a fixed three-quarter direction (normalize(1, 0.6, 1) — the direction the mocks' spheres use, so meshes and materials read as one family), near/far derived from the distance and radius so the box is never clipped.
- [ ] **Step 5: Teach the harvester a MESH subject.** Its work item is a material guid today; widen it to `{Guid id, Subject subject}` where `Subject` is `Material` or `Mesh` (an enum, not a second queue — the LIFO order must be shared, or a burst of mesh rows starves the material rows or vice versa). The mesh branch: `ResolveMeshData` through the same seams the resolver uses → one `MeshInstance` per section with the slots' resolved materials → `FrameMeshBounds` for the camera → the existing `MeshSceneDesc` light/ambient values, unchanged → capture → the same PNG write.
- [ ] **Step 6: `RequestMesh`/`InvalidateMesh`** mirror `Request`/`Invalidate` exactly (idempotent, LIFO push-front, clears the failed memo, keeps serving the old thumbnail until the new one lands — the last-good discipline stated in `Invalidate`'s own comment).
- [ ] **Step 7: Run `[editor]` — expect PASS.**
- [ ] **Step 8: Full `~[gpu]` suite — green.** Delta: **+4 cases**. Commit — `feat(editor): the preview harvester renders mesh assets framed by their bounds`

---

### Task 10: Mesh thumbnails — the wiring (spec §8)

Requesting, priming, invalidating, and drawing. Everything the material path already does, extended to the two new rows.

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp` (the visible-row request), `ArcaneEditor/src/App/EditorApp.cpp` (priming + the resolve seam), `ArcaneEditor/src/App/EditorAppProject.cpp` (invalidation on save and on cook)
- Test: `ArcaneTests/src/AssetPanelModelTest.cpp` (extend) — the pure half only; the draw half's net is the golden lane

**Interfaces:** none new.

**Which rows get a thumbnail, and which do not:**
- **`AssetKind::Mesh` (a `.arcmesh`) — YES.** It is the thing with geometry, slots and a resolved appearance.
- **`AssetKind::Model` (a `.gltf`/`.glb`) — NO, it shows its kind icon.** A Model has no material assignment of its own; its appearance IS its companion's, and harvesting both would spend two device idles to produce two near-identical pictures. **State this in the request site's comment** — it is a judgement, and the next reader should find the reasoning rather than assume an oversight. (A Model row folded under nothing is rare; the ordinary shape has the companion right beneath it, wearing the picture.)

- [ ] **Step 1: Write the failing invalidation test** (extend `AssetPanelModelTest.cpp`): the pure predicate `ThumbnailEligible(AssetKind)` returns true for `Material` and `Mesh` and false for `Model`, `Texture` (which resolves its own artifact thumbnail) and everything else. One case; the rest of this task's behaviour has no headless seam and says so.
- [ ] **Step 2: Run — expect FAIL.**
- [ ] **Step 3: Request on visible rows.** The Browse draw already calls `harvester.Request(guid)` for every VISIBLE un-thumbed material (idempotent, one hash lookup in the steady state). Add the `Mesh` arm through `ThumbnailEligible`. **Visible-only, LIFO — do not prime the whole project**; UE's thumbnail pool is LIFO for exactly this reason and the harvester's own header says so.
- [ ] **Step 4: Prime from disk.** `PrimeFromDisk(materials)` gains the project's mesh guids, so a re-opened project loads its existing PNGs with **zero device idles** and queues a harvest only for a mesh with no PNG or whose `.arcmesh` is newer than it. The mtime comparison is against the `.arcmesh`, not the artifact — a slot reassignment changes the picture and a re-cook does too, and the `.arcmesh` moves for the first while Step 5 covers the second.
- [ ] **Step 5: Invalidate on the four events that change a mesh's picture:**
  1. the `.arcmesh` is SAVED (its own document, or an external edit `PollAssetWatch` sees) → `InvalidateMesh(meshGuid)`;
  2. a mesh's **cook** lands (Task 6's completion path, for the companion) → `InvalidateMesh(companion)`;
  3. a **material** a slot names is saved → every mesh naming it. The reference index already answers "who points at this material" — reuse `InvalidateMaterialThumbsForTextures`' own registry-walk shape rather than a second index, and extend that function (or add its sibling) with a comment naming the reuse;
  4. a **texture** a slot's material names finishes cooking → transitively (3), which `InvalidateMaterialThumbsForTextures` already reaches — verify by reading it rather than assuming, and if the walk stops at materials, extend it one hop.
- [ ] **Step 6: Draw it.** The Browse row's thumbnail resolve already calls `ThumbTextureId(guid)`; nothing changes but the eligibility gate — the harvester keys on a synthetic guid per subject and the panel does not know or care which subject produced the picture.
- [ ] **Step 7: Run `[editor]` — expect PASS.**
- [ ] **Step 8: Desk check.** In a scratch project with an imported mesh: the `.arcmesh` row shows a real shaded picture of the prop within a frame or two of becoming visible; `Saved/Thumbnails/<guid>.png` exists; assigning a material to a slot updates the picture; reopening the project shows it instantly with no visible hitch. Record all four.
- [ ] **Step 9: Full `~[gpu]` suite — green.** Delta: **+1 case**. Commit — `feat(editor): mesh thumbnails -- request, prime, invalidate`

---

### Task 11: The ReferenceProject golden-scene fixture (spec §9)

One imported mesh joins the golden scene, so **both lanes exercise drop-to-pixels every CI run** rather than trusting a unit suite about it.

**Files:**
- Create: `ReferenceProject/Content/meshes/golden_prop.glb` (+ its `.meta`, `.arcmesh` companion and minted material, all committed)
- Modify: `ReferenceProject/Content/scenes/main.arcscene`
- Test: `ArcaneTests/src/HostBootTest.cpp` (extend)

**Which fixture, and why:** `multi.glb` from Plan 1's corpus — **the A1 shape** (three sections, two slots, two sections sharing one material). A single-section fixture would exercise the new draw path in its least interesting configuration; the multi-section one puts sections, slot dedup, and per-slot materials all in the golden picture at once, which is exactly what "drop-to-pixels every CI run" should be buying.

**Committed, not generated at build time:** the companion `.arcmesh` and its minted materials are EDITOR products (§5.5's division of labour), and ReferenceProject is staged by a headless `arccook` step that mints neither. So the import is run ONCE at the desk and its outputs are committed — the same way `reference_cube.arcmesh` and `reference_mesh.arcmat` already live in the tree.

- [ ] **Step 1: Write the failing boot test** (extend `HostBootTest.cpp`, beside its existing `reference_cube.arcmesh` assertions):

```cpp
TEST_CASE("host boot: the golden scene's imported prop resolves to sectioned geometry",
          "[host]")
{
    // The END-TO-END pin under the golden lane: the .arcmesh loads, names an
    // importedSource, resolves through the artifact arccook produced during staging,
    // and comes back with THREE sections over TWO slots (A1). A regression anywhere in
    // Plan 1's chain surfaces here as a specific failure rather than as an
    // unexplained golden diff.
    /* Executor: resolve golden_prop.arcmesh through the project the other [host]
       cases already open. */
    REQUIRE(meshData->source == Arcane::MeshSource::Imported);
    REQUIRE(meshData->importedSource.IsValid());
    REQUIRE(meshData->slots.size() == 2u);
    const auto resolved = /* ResolveMeshData through the boot Assets facade */;
    REQUIRE(resolved.state == Arcane::MeshResolveState::Ready);
    REQUIRE(resolved.mesh->sections.size() == 3u);
    CHECK(resolved.mesh->sections[0].slotIndex == resolved.mesh->sections[1].slotIndex);
    CHECK(resolved.mesh->sections[2].slotIndex != resolved.mesh->sections[0].slotIndex);
}
```

- [ ] **Step 2: Run — expect FAIL** (no such asset).
- [ ] **Step 3: Import it at the desk.** Copy `ArcaneTests/data/gltf/multi.glb` to `ReferenceProject/Content/meshes/golden_prop.glb`, open ReferenceProject in the editor, and let the wave run: sidecar minted, cooked, `golden_prop.arcmesh` minted with slots `Metal` and `Paint`, `mesh_import_base.arcmat` minted at `Content/`, two instance materials minted. **Assign a distinct, saturated `baseColor` to each of the two slot materials** — a golden picture in which the two materials are visually identical proves nothing about per-slot resolution. Record the two colours in the commit body.
- [ ] **Step 4: Place it in the scene.** Add ONE entity to `main.arcscene` with `Transform` + `MeshRenderer` naming `golden_prop.arcmesh`, positioned so it is **clearly visible and does not occlude the existing cube** (the existing lit cube is what the `wrong-normal-matrix` and `missing-mesh` traps key on — hiding it would blunt three trap fixtures at once). Bump the scene's `assets` manifest by re-saving through the editor rather than hand-editing, so the v4 manifest (`kSceneJsonVersion` 4) stays exact.
- [ ] **Step 5: Commit the products.** `golden_prop.glb`, `golden_prop.glb.meta`, `golden_prop.arcmesh`, `mesh_import_base.arcmat`, the two instance `.arcmat`s, and `main.arcscene`. **Do NOT commit `ReferenceProject/Intermediate/`** — artifacts are build products the staging step regenerates (verify `.gitignore` covers it rather than assuming).
- [ ] **Step 6: Run `[host]` — expect PASS.** Then run the full `~[gpu]` suite: **the staging postbuild must cook the new `.glb` without error** (`arccook` runs in `ArcaneTests`' own postbuild, so a mesh cook failure here fails the BUILD, which is the loudest possible place for it). Delta: **+1 case** — the `[host]` golden-prop assertion from Step 1, and it is the SAME +1 Task 13 Step 4's arithmetic already counts for this task. **Do not adjust any total.**
- [ ] **Step 7: Confirm the goldens now FAIL, and confirm WHY.** Run `scripts/golden-gate.ps1`. The runtime lane must go red on both backends — a new object entered the scene. **Read the diff artifacts and confirm the change is exactly the new prop and nothing else** (the cube unmoved, the sprite unmoved, no unexplained pixels). **Copy both diffs into the workspace before Task 12 re-runs anything** — the gate deletes stale diff PNGs, and the pre-bless evidence is the only record of what the bless accepted.
- [ ] **Step 8: Commit** — `test(reference): a multi-section imported prop joins the golden scene` (the re-bless is Task 12's, deliberately separate: **a commit that both changes the picture and blesses it leaves no reviewable moment where the diff was visible**).

---

### Task 12: The both-lane re-bless (spec §9, Arc 2 discipline)

**Files:**
- Modify: `ReferenceProject/Verify/References/runtime-scene.png`, `ReferenceProject/Verify/References/vulkan/runtime-scene.png`, and `editor-ui.png` **only if the editor lane actually moved**

**The discipline, stated before the steps because every one of them depends on it:** verdicts come from the gate JSON's `gatePassed` + per-lane `verdict`, never from an exit code and never from looking at a picture. A bless writes the **SOURCE** tree (`ReferenceProject/Verify/`), and the blessed images must then be **restaged to BOTH hosts** — a bless that only reaches one staging directory leaves the other lane comparing against yesterday. Diff artifacts are copied out **before** any re-run.

- [ ] **Step 1: Confirm the pre-bless evidence is in hand** — Task 11 Step 7's two runtime diffs, already copied into the workspace. **If they are not, stop and re-run the gate to regenerate them before blessing anything.** Blessing without having read the diff is how a real regression becomes the new reference.
- [ ] **Step 2: Decide whether the EDITOR lane moved.** It should not: `editor-ui` captures the editor shell, and this arc added no panel, no menu row and no default-layout entry. **But the Browser's asset list is on screen in that capture**, and it now has three more rows (the `.glb`, its companion, its materials) plus, once the harvester runs, real thumbnails. Read the editor-lane diff: if it moved, it moved for THAT reason and the bless is legitimate; if it moved for any other reason, stop.
- [ ] **Step 3: Bless the SOURCE.** Run each failing lane's host with `--bless` (the gate never passes `--bless` itself — `golden-gate.ps1`'s own header says so, and its four combinations are compare-only). Bless writes into `ReferenceProject/Verify/References/` — the SOURCE tree, which is the one that gets committed.
- [ ] **Step 4: Restage BOTH hosts.** Rebuild so every stager's postbuild copies the fresh `Verify/` tree into `ArcaneRuntime`'s, `ArcaneEditor`'s and `ArcaneTests`' output directories. **A bless that is not restaged to both hosts is the Arc 2 lesson this step exists to honour** — it looks green locally in the lane you re-ran and red in the other.
- [ ] **Step 5: Re-run `scripts/golden-gate.ps1` clean.** Assert from `golden-gate-summary.json`: `gatePassed == true`, and each of the four lanes' `verdict` is a match. **Paste the four verdicts into the commit body**, not a summary sentence about them.
- [ ] **Step 6: Sweep the staging strays.** `golden-gate.ps1` stages `Content/` **additively**, so any file that ever passed through it is still sitting in the staged trees. `git status` on `ReferenceProject/` must show only the intended changes, and the staged output directories must contain no `golden_prop`-adjacent leftovers from Plan 1's desk checks (the `multi.glb`/`embedded_tex.glb` copies those tasks told you to clean). Check both.
- [ ] **Step 7: Run `[gpu][golden]`** (the in-process lit-cube case) — it must **still match without a bless**. That case renders its own hand-authored scene, not `main.arcscene`, so the new prop cannot reach it; if it moved, the upload or draw path changed the picture and Tasks 4/5's parity claims were wrong.
- [ ] **Step 8: Commit** — `test(reference): re-bless both golden lanes for the imported prop`

---

### Task 13: Closeout — sweeps, both configurations, and the handoff

- [ ] **Step 1: Zero-legacy sweep** (path-exclude + `-riw`): `kInitialUploadSlots`, `m_warnedRingOverflow`, `uploadFor`, `MeshNode::Upload`, and `MeshInstance::mesh` as a POINTER (`const MeshData*`) across `ArcaneClient ArcaneEditor ArcaneRuntime ArcaneTests scripts` — hits only in docs/specs history. The ring path for mesh geometry is gone, and this is what proves it rather than remembering it.
- [ ] **Step 2: Ring-usage sanity.** `NriUploadRing` is still used by the 2D batch path and must be — the sweep above must NOT come back empty for the ring itself. **Confirm the batch path still allocates from it** (`grep -n "ring.Allocate" ArcaneClient/src`), so "retired for mesh geometry" is verified as the narrow claim it is rather than accidentally widened.
- [ ] **Step 3: Budget-constant sanity.** `grep -rn "kMeshResidencyBudgetBytes"` → the definition, the eviction call, and the test. **No second literal 512 anywhere** — the constant exists so the number has one home, and the parked cvar arc will need exactly one place to move it from.
- [ ] **Step 4: Full suites, Debug AND Release**, `~[gpu]` and `[gpu]`, from the exe dir. Derive the final counts and attribute every delta against **this plan's own Task 1 Step 1 baseline** to the named per-task adds: T1 +6, T2 +4/+4, T3 +0/+2, T4 +1/+2, T5 +4/+1, T6 +3, T7 +2, T8 +4, T9 +4, T10 +1, T11 +1 = **+30 `~[gpu]` cases and +9 `[gpu]` cases**. If the derived numbers differ, find out which task's count was wrong and say so — **derive, never recall.** Ledger both configurations' runs with their seed banners.
- [ ] **Step 5: Re-run `golden-gate.ps1` on the Release build** as well as Debug. **The single-slot `ReferenceProject/Binaries/` trap is real and the gate's own header names it:** flipping configuration without rebuilding ReferenceProject.slnx dies with "plugin: initial load failed" against a `ReferenceGame.dll` built for the wrong CRT. The gate rebuilds it as its step one — let it, and do not skip the flip.
- [ ] **Step 6: A performance observation, recorded not asserted.** With the golden prop in the scene, note the frame's mesh-upload cost before and after this plan — the ring path uploaded every visible mesh's streams every frame; the resident path uploads once. **There is no perf TEST in either plan and there should not be** (a threshold on this machine is not a fact about any other), but the observation is what §12's "per-frame ring re-upload cliff" hazard was raised about, and the closeout is where it gets written down. One paragraph in the commit body: the prop's byte count, and `ResidentBytes()` after a few seconds of running.
- [ ] **Step 7: Hold for the user's desk pass.** Hand over: the two plans' commits; the four golden verdicts; the Debug and Release suite counts with seeds; the desk observations from Tasks 6, 8 and 10; and the standing debts — **Gacha's Game-module rebuild for ABI 24 (recorded in Plan 1's ledger entry, tracked in that repo, NOT actioned here)**, the per-slot material list UI parked to F4/post-F2c, and `kMeshResidencyBudgetBytes` awaiting the parked cvar arc's own trigger. **Push only after the pass.**

---

## Self-review record (run at authoring time)

**Spec coverage — every clause this plan owns, to a task** (Plan 1's own record covers §1–§6, §7.1, §10; the two together cover the spec):

| Spec | Task |
|---|---|
| §3 R2 (resident cache + budget LRU) | T1 (policy), T2 (cache), T3 (vehicle) |
| §3 R5 (thumbnails by editor harvest) | T9 (harvest), T10 (wiring) |
| §7.2 residency, all meshes incl. primitives | T2, T4 (the ring retires for ALL mesh geometry, no small-mesh fast path) |
| §7.2 eviction — combined CPU+GPU budget, frame-boundary LRU, drawn-this-frame protected | T1 (the rule), T2 Step 6 (the call), T3 Step 5 (the frame boundary) |
| §7.2 device loss / recreate hooks | T2 (`Release` doubles as the hook; the kept CPU copy is what makes re-upload cheap), T3 (`ResizeOffscreen` deliberately does NOT release) |
| §7.3 cook completion → invalidate → drop → re-request | T6 |
| §7.3 slot change keeps buffers | T7 |
| §7.3 document-preview gap re-accepted | T6 Step 6 (recorded as a comment citing both specs, not re-litigated) |
| §7.4 per-section draws, `MeshConstants` untouched | T5 |
| §8 browser presence (Model row, companion fold) | T8 |
| §8 thumbnails (AABB framing, LIFO, one per frame, persisted) | T9, T10 |
| §8 F2c/F4 line (no import dialog, no options UI, no re-import button) | Honoured by omission and stated in T8 Step 6 (`RailKindCreatable` stays false, with the reason) |
| §9 golden-scene fixture, both lanes, one re-bless cycle | T11 (fixture), T12 (bless) |
| §9 suites run from the exe dir, deltas attributed per task | Every task's final step; T13 Step 4 for the arc total |
| §12 "per-frame ring re-upload cliff" | T4 (the retirement), T13 Step 6 (the observation) |
| §12 "eviction question left half-answered" | T1 |

**Type consistency across tasks:** `MeshResidencyEntry`/`SelectEvictions`/`MeshResidencyBytes`/`kMeshResidencyBudgetBytes` are named in T1 and consumed unchanged in T2. `NriMeshBufferCache::SupplyResult` speaks `MeshResolveState`, which is **Plan 1 Task 11's type** — the pending/failed distinction is defined once, in `MeshAsset.hpp`, and travels from the facade through the resolver into the render cache without a second spelling. `MeshInstance::mesh` becomes a `Guid` in T4 and gains `indexOffset`/`indexCount` in T5; no task between them assumes the old pointer. `NriGraphContext::InvalidateMeshGeometry` is named in T3 and is the only device-side entry point T6 and T7 use, through the resolver's injected seam rather than directly.

**Known intentional gaps, each with its owner:**
- **No `Model` thumbnail** (T10) — its appearance is its companion's, and two device idles for two near-identical pictures is a cost with no benefit. Recorded at the request site.
- **No performance test** (T13 Step 6) — a frame-time threshold on this machine is not a fact about any other. The observation is recorded in prose instead, which is what §12's hazard actually needed.
- **No `Create▸Model` action** (T8 Step 6) — a model is imported, never authored. Not an omission; an explicit `false` with a comment.
- **The mesh thumbnail's own invalidation on a TEXTURE cook is transitive** (T10 Step 5.4) through the material walk, and the step tells the executor to VERIFY that walk reaches meshes rather than assume it. If it stops at materials, the step says to extend it — that is a real possible finding, not a papered-over one.
- **`kMeshResidencyBudgetBytes` stays a compile-time constant.** The parked cvar arc's own trigger discipline decides when it becomes a cvar; this plan must not pre-empt it (T1's header comment says so in the code, where someone might otherwise "fix" it).
- **The texture side still has no byte budget**, so the two residency caches can each stay under their own limits while together exhausting VRAM — UE's single shared pool is the eventual shape (comparison Decision 7). Out of F2c's scope, recorded here so it is not rediscovered as a surprise.
