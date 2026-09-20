#pragma once

// The GPU scene's CPU-side vocabulary (F3, spec s5). NO NRI here: the mirror
// is a registry resource, Sync and the batch builder are header-only and
// device-free (GpuSceneSync.hpp), and ArcaneTests drives them under ~[gpu].
// Render/Nri/GpuScene.{hpp,cpp} is the device half.
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialBlendMode.hpp>
#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Scene/Frustum.hpp>

#include <Astra/Container/FlatMap.hpp>
#include <Astra/Core/Tick.hpp>
#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Arcane
{
    // Mirrors BindlessTable::kInvalidSlot (0xFFFFFFFF) without pulling <NRI.h>;
    // GpuScene.cpp static_asserts the two agree.
    inline constexpr std::uint32_t kGpuInvalidMaterialSlot = 0xFFFFFFFFu;

    // Row flags: blend and cull are independent pipeline facts. Keep these in
    // lockstep with data/shaders/gpu_scene.hlsli without changing the 240-byte row.
    inline constexpr std::uint32_t kGpuInstanceFlagTeleported = 1u << 0;
    inline constexpr std::uint32_t kGpuInstanceFlagBlendShift = 1u;
    inline constexpr std::uint32_t kGpuInstanceFlagBlendMask  = 0x3u << kGpuInstanceFlagBlendShift;
    inline constexpr std::uint32_t kGpuInstanceFlagTwoSided   = 1u << 3;
    inline constexpr std::uint32_t kGpuInstanceFlagLive       = 1u << 4;

    // ONE ROW PER (entity, mesh section). 240 bytes, std430; data/shaders/
    // gpu_scene.hlsli carries the same field order -- change both or neither.
    // The normal matrix is NormalMatrixFor(model) computed on the CPU at
    // staging (R8: no per-vertex 3x3 inverse); prevModel is the pose the row
    // was drawn with LAST frame (spec s5.3's G2 contract).
    struct GpuInstance
    {
        glm::mat4     model{1.0f};
        glm::mat4     prevModel{1.0f};
        glm::vec4     normal0{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec4     normal1{0.0f, 1.0f, 0.0f, 0.0f};
        glm::vec4     normal2{0.0f, 0.0f, 1.0f, 0.0f};
        glm::vec4     boundsMin{0.0f};                  // world AABB; w unused
        glm::vec4     boundsMax{0.0f};                  // w = resolved alphaCutoff (stored across blend switches)
        glm::vec4     baseColor{1.0f};
        std::uint32_t materialSlot = kGpuInvalidMaterialSlot;
        std::uint32_t batch        = 0;                 // the batch KEY id (stable per (mesh, section, blend, twoSided))
        std::uint32_t flags        = 0;
        std::uint32_t pad          = 0;
    };
    static_assert(sizeof(GpuInstance) == 240, "GpuInstance must stay 240 bytes -- gpu_scene.hlsli mirrors it");

    struct GpuBatchKey
    {
        Guid          mesh{};
        std::uint32_t     section = 0;
        MaterialBlendMode blend = MaterialBlendMode::Opaque;
        bool              twoSided = false;
        [[nodiscard]] bool operator==(const GpuBatchKey&) const noexcept = default;
    };
    struct GpuBatchKeyHash
    {
        [[nodiscard]] std::size_t operator()(const GpuBatchKey& k) const noexcept
        {
            std::size_t h = std::hash<std::uint64_t>{}(k.mesh.hi);
            h ^= std::hash<std::uint64_t>{}(k.mesh.lo) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            const std::uint64_t state = (std::uint64_t(k.section) << 32)
                                      | (std::uint64_t(static_cast<std::uint8_t>(k.blend)) << 1)
                                      | std::uint64_t(k.twoSided);
            h ^= std::hash<std::uint64_t>{}(state) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        }
    };

    // Contiguous row spans, first-fit, UE's FSpanAllocator shape in miniature:
    // a multi-section entity owns `count` consecutive rows.
    struct RowSpanAllocator
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> freeSpans;   // {first, count}
        std::uint32_t highWater = 0;

        [[nodiscard]] std::uint32_t Allocate(std::uint32_t count)
        {
            for (std::size_t i = 0; i < freeSpans.size(); ++i)
            {
                auto& [first, n] = freeSpans[i];
                if (n >= count)
                {
                    const std::uint32_t out = first;
                    first += count;
                    n     -= count;
                    if (n == 0)
                        freeSpans.erase(freeSpans.begin() + static_cast<std::ptrdiff_t>(i));
                    return out;
                }
            }
            const std::uint32_t out = highWater;
            highWater += count;
            return out;
        }
        void Free(std::uint32_t first, std::uint32_t count) { freeSpans.emplace_back(first, count); }
        [[nodiscard]] std::uint32_t HighWater() const noexcept { return highWater; }
    };

    struct GpuSceneRow
    {
        Astra::Entity entity{};
        Guid          mesh{};
        std::uint32_t section = 0;
        std::uint32_t batch   = 0;
        glm::mat4     lastModel{1.0f};          // the matrix last uploaded -- the prior-pose history (spec s5.3)
        glm::vec4     lastBaseColor{1.0f};      // what the row was last staged with: a resolved material that
        std::uint32_t lastSlot = kGpuInvalidMaterialSlot;   //   changed under it re-stages the row (no generation plumbing)
        MaterialBlendMode lastBlend = MaterialBlendMode::Opaque;
        float             lastAlphaCutoff = 0.5f;
        bool              lastTwoSided = false;
        glm::vec3         boundsCenter{0.0f};   // current WorldBounds centre for CPU transparent sorting
        std::int32_t      renderOrder = 0;
        float             depthSortBias = 0.0f;
        bool          live    = false;
        std::uint64_t touched = 0;              // the sync counter that last saw the entity
    };

    struct GpuSceneMirror
    {
        struct Rows { std::uint32_t first = 0; std::uint32_t count = 0; };

        Astra::FlatMap<Astra::Entity, Rows>                              slots;
        std::vector<GpuSceneRow>                                         rows;          // by row; size == allocator.HighWater()
        RowSpanAllocator                                                 allocator;
        std::unordered_map<GpuBatchKey, std::uint32_t, GpuBatchKeyHash>  batchIds;
        std::vector<GpuBatchKey>                                         batchKeys;     // by id
        std::vector<std::uint32_t>                                       batchRowCount; // by id: live rows with that key
        std::vector<std::uint32_t>                                       dirtyLastFrame;
        std::uint64_t                                                    generation;    // fresh per mirror; the device stamps what it synced
        Astra::Tick                                                      lastSyncTick = 0;
        std::uint64_t                                                    syncCounter  = 0;

        GpuSceneMirror() : generation(NextGeneration()) {}
        static std::uint64_t NextGeneration() noexcept
        {
            static std::atomic<std::uint64_t> counter{ 1 };
            return counter.fetch_add(1);
        }

        // A TRANSIENT registry resource: never serialized, rebuilt on any
        // registry swap (a mirror from a swapped registry has a new
        // generation, and the device side sees the mismatch as a full
        // rebuild). Registry::Save excludes resources entirely regardless;
        // the no-op Serialize keeps the container members off Astra's
        // reflected/trivially-copyable auto-serialization path at
        // registration, the same reason SceneVisibility (VisibilitySystem.hpp)
        // and BoundsSystemState (BoundsSystem.hpp) carry one.
        template<typename Archive> void Serialize(Archive& /*ar*/) {}
    };

    // What one Sync hands the device side: the rows to (re)write this frame.
    struct GpuSceneStage
    {
        std::vector<std::uint32_t> rows;
        std::vector<GpuInstance>   values;       // parallel to `rows`
        std::uint32_t              rowCapacity = 0;   // allocator high water: the buffer must hold this many rows
        bool                       fullRebuild = false;
        std::uint64_t              generation  = 0;   // the mirror's; the device stamps it as synced after a successful Apply
        void Clear() { rows.clear(); values.clear(); rowCapacity = 0; fullRebuild = false; generation = 0; }
    };

    // == nri::DrawIndexedDesc, field for field (GpuScene.cpp static_asserts it).
    struct DrawIndexedArgs
    {
        std::uint32_t indexNum     = 0;
        std::uint32_t instanceNum  = 0;
        std::uint32_t baseIndex    = 0;
        std::int32_t  baseVertex   = 0;
        std::uint32_t baseInstance = 0;
    };
    static_assert(sizeof(DrawIndexedArgs) == 20);

    // One record for every stable batch-key id, including keys which are not
    // emitted this view (and transparent keys). Keep this order in sync with
    // mesh_cull.hlsl: the shader indexes it with GpuInstance::batch.
    struct GpuCullBatch
    {
        std::uint32_t firstOutput = 0;
        std::uint32_t capacity    = 0;
        std::uint32_t argIndex    = 0;
        std::uint32_t emitted     = 0;
    };
    static_assert(sizeof(GpuCullBatch) == 16);

    struct GpuBatchDraw
    {
        Guid          mesh{};
        std::uint32_t section     = 0;
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount  = 0;
        std::uint32_t firstOutput = 0;   // where this batch's visible rows start in visibleIndices
        std::uint32_t capacity    = 0;   // resident rows with this key
        std::uint32_t argIndex    = 0;   // position in `args`
        MaterialBlendMode blend   = MaterialBlendMode::Opaque;
        bool          twoSided    = false;
        float         nearDepth   = 0.0f;
    };

    // One visible transparent row, already fully resolved by the CPU frame
    // builder. MeshNode must draw this record directly; it never needs to
    // look up a mesh section or material state again.
    struct TransparentDraw
    {
        std::uint32_t     row = 0;
        Guid              mesh{};
        std::uint32_t     section = 0;
        std::uint32_t     indexOffset = 0;
        std::uint32_t     indexCount = 0;
        MaterialBlendMode blend = MaterialBlendMode::Transparent;
        bool              twoSided = false;
        std::int32_t      renderOrder = 0;
        float             projectedDepth = 0.0f;
        Astra::Entity     entity{};
    };

    struct GpuSceneFrame
    {
        GpuSceneStage              stage;
        std::vector<GpuBatchDraw>  batches;          // EMITTED, in draw order
        std::vector<DrawIndexedArgs> args;           // by argIndex
        std::vector<GpuCullBatch> cullBatches;        // by stable batch key id; consumed by MeshCullNode
        std::vector<std::uint32_t> visibleIndices;   // device output: rowCapacity entries, initially invalid each frame
        std::vector<std::uint32_t> oracleVisibleIndices; // CPU expectation for later GPU readback comparison
        std::vector<TransparentDraw> transparentDraws; // direct records, ordered far-to-near within render order
        std::uint32_t              rowCount = 0;     // == stage.rowCapacity
        Frustum                    frustum;          // the widened planes the CPU test used (plan 2's cull CB)
        struct Stats { std::uint32_t total = 0, coarseVisible = 0, batches = 0, draws = 0; } stats;

        [[nodiscard]] bool HasDraws() const noexcept { return !batches.empty() || !transparentDraws.empty(); }
    };

    // Mutable execution-order state shared by the sync, cull, and mesh graph
    // callbacks for one declared frame. Reserve runs before graph declaration;
    // Apply updates the ready bits inside the sync callback, and later callbacks
    // must read them there rather than snapshotting them during declaration.
    struct GpuSceneFrameReadiness
    {
        bool registryReserved = false;
        bool registryReady    = false;
        bool adHocReady       = false;
    };

    struct GpuSceneApplyResult
    {
        bool registryReady = false;
        bool adHocReady    = false;
    };
}
