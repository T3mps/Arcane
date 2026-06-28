#pragma once

// SolverStages.hpp -- the Box2D-v3 b2SolverStage stage/block machinery that runs
// the whole SoftStep substep loop inside ONE worker region (Gap 1, solver-MT
// scaling rework). This header owns:
//   1. The stage/block descriptors (StageType / SolverBlock / SolverStage) the
//      driver pre-builds once per step into reused scratch (SoftStep::m_stages /
//      m_blocks).
//   2. SolverStageContext -- the per-step bundle ExecuteBlock / SolverWorker read
//      (the constraint data handles, the integrate/overflow/joint callbacks, and
//      the atomic stage-sync word).
//   3. The ring-CAS claim loop (ExecuteStage / ExecuteStageForTest), the per-stage
//      orchestration (ExecuteMainStage), the block dispatch (ExecuteBlock), and
//      the worker entry (SolverWorker).
//
// THREADING MODEL (Box2D src/solver.c). Box2D enqueues `workerCount` tasks ONCE
// per step; the workers run the entire substep loop inside a persistent region,
// advancing across colors/passes/substeps with a single atomic store
// (atomicSyncBits) + a spin-wait -- never re-entering the task system. A color
// barrier costs one atomic store + one spin, NOT an enqueue+join. This replaces
// SoftStep's ~190 fork-join ParallelFor dispatches/step with ONE region.
//
// THIS TASK (Gap 1.1): the region is driven SERIALLY by the main worker only
// (workerIndex == 0; ParallelFor(workerCount,1,...) + thieves arrive in Gap 1.2).
// The atomic protocol is fully present but UNCONTENDED here: the main self-
// completes every block alone (claim-at-execution), so a single pass through
// ExecuteStage claims + runs all blocks and the completionCount barrier exits
// immediately. The stage walk reproduces SoftStep's exact D1 substep order, so
// the result is BYTE-IDENTICAL to the per-color ParallelFor it replaces.
//
// DETERMINISM. Stage order is fixed; a color boundary is a hard barrier (spin
// until completionCount == blockCount -- adjacent colors share bodies, Gauss-
// Seidel). Within a color, blocks partition the color's body-DISJOINT batches, so
// which-block-runs-which and in-what-order cannot change any float. Atomics are
// control-flow ONLY (syncIndex CAS, completionCount, stageSync) -- never in FP
// math -- so there is no atomic-ordering-induced FP non-determinism. Hence
// serial == enki(1) == enki(N), byte-identical.
//
// INCLUDE GRAPH. This header forward-declares SoftStep (only a pointer field
// references it) and pulls the constraint data + SIMD passes from
// ContactConstraintSimd.hpp; it does NOT include SoftStep.hpp (SoftStep.hpp
// includes THIS for its m_stages/m_blocks members). ExecuteBlock reaches the
// solver's data through SolverStageContext handles + FunctionRef callbacks, so
// it never dereferences SoftStep -- keeping the two files decoupled.
//
// PRESENTATION-FREE + C++20-clean: std + sibling Physics headers + Arcane::Simd
// (via ContactConstraintSimd.hpp). namespace Arcane::Physics.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <Arcane/Util/FunctionRef.hpp>
#include <Arcane/Physics/Solver/BodyState.hpp>            // BodyState row
#include <Arcane/Physics/Solver/ContactConstraintSimd.hpp> // SimdSolve range passes
#include <Arcane/Physics/Solver/Solver.hpp>                // SolverContext / ContactConstraint

namespace Arcane
{
    namespace Physics
    {
        class SoftStep; // forward decl -- only a pointer field references it here.

        // The seven stage kinds, in the D1 substep order they are walked. The five
        // colored types (WarmStart/Solve/Relax/Restitution/StoreImpulses) emit one
        // stage per non-empty color; the two body types (IntegrateVelocities/
        // IntegratePositions) emit one stage each over the awake-body range.
        enum class StageType : std::uint8_t
        {
            IntegrateVelocities,
            WarmStart,
            Solve,
            IntegratePositions,
            Relax,
            Restitution,
            StoreImpulses
        };

        // One claimable unit of a stage: a half-open [begin, end) sub-range (batch
        // indices for colored stages, awake-body indices for the body stages) plus
        // the atomic ring-CAS claim token. syncIndex starts at 0 and is CAS-advanced
        // from prevSync -> curSync each time the owning stage runs (it advances once
        // per substep re-visit), so a block is claimed by exactly one worker per run.
        //
        // The relaxed copy ctor/assign exist solely so the structs are vector-
        // storable (std::atomic is otherwise non-copyable, which would make
        // std::vector<SolverBlock>::resize ill-formed). Copies only ever happen
        // SINGLE-THREADED during the per-step scratch rebuild (never inside the
        // concurrent region), so a relaxed value copy is correct.
        struct SolverBlock
        {
            int              begin = 0;
            int              end = 0;
            std::atomic<int> syncIndex{ 0 };

            SolverBlock() noexcept = default;
            SolverBlock(const SolverBlock& o) noexcept
                : begin(o.begin), end(o.end),
                  syncIndex(o.syncIndex.load(std::memory_order_relaxed)) {}
            SolverBlock& operator=(const SolverBlock& o) noexcept
            {
                begin = o.begin;
                end = o.end;
                syncIndex.store(o.syncIndex.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
                return *this;
            }
        };

        // One stage = a run of body-disjoint blocks of a single type/color. blocks
        // points into the owning SoftStep::m_blocks scratch (a contiguous slice).
        // completionCount counts blocks finished THIS run (reset to 0 after the
        // barrier); syncIndex is the per-stage CAS round (main-advanced single-
        // threaded -- it increments each substep re-visit so the block CAS expects
        // prevSync == the value left by the previous run). Same relaxed-copy rationale
        // as SolverBlock.
        struct SolverStage
        {
            StageType        type = StageType::IntegrateVelocities;
            int              colorIndex = 0;   // active color (colored types); ignored otherwise
            SolverBlock*     blocks = nullptr; // -> m_blocks slice
            int              blockCount = 0;
            std::atomic<int> completionCount{ 0 };
            int              syncIndex = 0;    // per-stage CAS round (main-advanced)

            SolverStage() noexcept = default;
            SolverStage(const SolverStage& o) noexcept
                : type(o.type), colorIndex(o.colorIndex), blocks(o.blocks),
                  blockCount(o.blockCount),
                  completionCount(o.completionCount.load(std::memory_order_relaxed)),
                  syncIndex(o.syncIndex) {}
            SolverStage& operator=(const SolverStage& o) noexcept
            {
                type = o.type;
                colorIndex = o.colorIndex;
                blocks = o.blocks;
                blockCount = o.blockCount;
                completionCount.store(o.completionCount.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
                syncIndex = o.syncIndex;
                return *this;
            }
        };

        // Per-step bundle the region reads. Built on the stack in SoftStep::Solve and
        // passed by reference to SolverWorker. The data handles let ExecuteBlock reach
        // the solver's per-color batches / dense body rows / refs without touching
        // SoftStep's definition; the FunctionRefs forward the body-integrate range +
        // the main-serial overflow/joint passes to SoftStep methods (bound as
        // lambdas in Solve). stageSync is the single "advance to next stage" word
        // (Box2D atomicSyncBits) the thieves will read in Gap 1.2; it is written but
        // unread in this serial-main task.
        struct SolverStageContext
        {
            SoftStep*      solver = nullptr;   // forward-declared; carried for reference
            SolverContext* ctx = nullptr;
            SolverStage*   stages = nullptr;   // -> m_stages.data()
            int            stageCount = 0;
            int            substepCount = 0;
            int            activeColorStages = 0; // non-empty colors per colored phase
            std::atomic<std::uint32_t> stageSync{ 0 };

            // Data ExecuteBlock reads for the colored + store stages.
            std::vector<std::vector<ContactConstraintSimd>>* colorBatches = nullptr;
            std::vector<std::uint32_t>* colorRefs = nullptr; // -> m_colorRefs.data() (kColorCount vectors)
            BodyState*     bodyState = nullptr;
            float          h = 0.0f;
            float          maxBiasVel = 0.0f;
            float          threshold = 0.0f;

            // Body-integrate range callbacks (bound to SoftStep + ctx + h in Solve).
            Arcane::FunctionRef<void(std::size_t, std::size_t)> integrateVel{};
            Arcane::FunctionRef<void(std::size_t, std::size_t)> integratePos{};

            // Main-serial inline callbacks SolverWorker invokes between colored
            // phases (Box2D solves overflow + joints on the main worker while thieves
            // spin). overflowSolve takes useBias (true=biased solve, false=relax).
            Arcane::FunctionRef<void()>     overflowWarmStart{};
            Arcane::FunctionRef<void(bool)> overflowSolve{};
            Arcane::FunctionRef<void()>     overflowRestitution{};
            Arcane::FunctionRef<void()>     jointBridge{}; // Sync->Solve->Sync (no-op if no joints)
        };

        // -----------------------------------------------------------------------
        // Block sizing (Box2D b2ComputeBlockCount). Contact blocks min ~4 wide-
        // constraints (batches), body blocks min ~32, target ~= blocksPerWorker *
        // workerCount. For the serial main (Gap 1.1) workerCount sizes to 1.
        // -----------------------------------------------------------------------
        inline int ComputeBlockCount(int itemCount, int minBlockSize, int targetBlocks) noexcept
        {
            if (itemCount <= 0) { return 0; }
            const int maxBlocks = (itemCount + minBlockSize - 1) / minBlockSize; // ceil
            int blocks = (targetBlocks < maxBlocks) ? targetBlocks : maxBlocks;
            return (blocks < 1) ? 1 : blocks;
        }

        // Partition [0, itemCount) into blockCount contiguous near-equal [begin,end)
        // ranges (remainder spread over the leading blocks) and reset each block's
        // syncIndex to 0 for a fresh step. blockCount must be > 0 and <= itemCount's
        // ComputeBlockCount result.
        inline void PartitionBlocks(SolverBlock* blocks, int blockCount, int itemCount) noexcept
        {
            const int base = itemCount / blockCount;
            const int rem = itemCount % blockCount;
            int start = 0;
            for (int b = 0; b < blockCount; ++b)
            {
                const int count = base + (b < rem ? 1 : 0);
                blocks[b].begin = start;
                blocks[b].end = start + count;
                blocks[b].syncIndex.store(0, std::memory_order_relaxed);
                start += count;
            }
        }

        // -----------------------------------------------------------------------
        // ExecuteBlock -- run one block of `stage` (dispatch on the stage type to the
        // matching SimdSolve range overload or the body-integrate callback).
        // -----------------------------------------------------------------------
        inline void ExecuteBlock(SolverStageContext& sc, const SolverStage& stage, int blockIndex)
        {
            const SolverBlock& block = stage.blocks[blockIndex];
            const std::size_t begin = static_cast<std::size_t>(block.begin);
            const std::size_t end = static_cast<std::size_t>(block.end);
            const std::size_t color = static_cast<std::size_t>(stage.colorIndex);

            switch (stage.type)
            {
            case StageType::IntegrateVelocities:
                sc.integrateVel(begin, end);
                break;
            case StageType::IntegratePositions:
                sc.integratePos(begin, end);
                break;
            case StageType::WarmStart:
                SimdSolve::WarmStart((*sc.colorBatches)[color], sc.bodyState, begin, end);
                break;
            case StageType::Solve:
                SimdSolve::SolveNormalAndFriction((*sc.colorBatches)[color], sc.bodyState,
                                                  sc.h, /*useBias=*/true, sc.maxBiasVel, begin, end);
                break;
            case StageType::Relax:
                SimdSolve::SolveNormalAndFriction((*sc.colorBatches)[color], sc.bodyState,
                                                  sc.h, /*useBias=*/false, sc.maxBiasVel, begin, end);
                break;
            case StageType::Restitution:
                SimdSolve::ApplyRestitution((*sc.colorBatches)[color], sc.bodyState,
                                            sc.threshold, begin, end);
                break;
            case StageType::StoreImpulses:
                SimdSolve::StoreImpulses((*sc.colorBatches)[color], sc.ctx->contacts,
                                         sc.colorRefs[color].data(), begin, end);
                break;
            }
        }

        // -----------------------------------------------------------------------
        // ExecuteStage -- the staggered ring-order CAS claim loop (Box2D
        // b2ExecuteStage). Each worker rings ONE full pass from `startIndex`,
        // CAS-claiming every block whose syncIndex == prevSync (prev -> cur). The CAS
        // winner runs the block via runBlock(blockIndex) and bumps the stage's
        // completionCount (release). `visited` advances every iteration, so the loop
        // is exactly one ring (blockCount visits): after it, every block has been
        // claimed by SOME worker. Returns the count this worker executed.
        //
        // For the serial main (Gap 1.1) one full ring claims + runs all blocks.
        // -----------------------------------------------------------------------
        template <class RunBlock>
        inline int ExecuteStage(SolverStage& stage, int prevSync, int curSync,
                                RunBlock&& runBlock, int startIndex = 0) noexcept
        {
            const int blockCount = stage.blockCount;
            if (blockCount <= 0) { return 0; }

            int executed = 0;
            int visited = 0;
            int blockIndex = (blockCount > 0) ? (startIndex % blockCount) : 0;
            while (visited < blockCount)
            {
                int expected = prevSync;
                if (stage.blocks[blockIndex].syncIndex.load(std::memory_order_relaxed) == prevSync &&
                    stage.blocks[blockIndex].syncIndex.compare_exchange_strong(
                        expected, curSync,
                        std::memory_order_acquire, std::memory_order_relaxed))
                {
                    runBlock(blockIndex);
                    stage.completionCount.fetch_add(1, std::memory_order_release);
                    ++executed;
                }
                ++visited;
                ++blockIndex;
                if (blockIndex >= blockCount) { blockIndex = 0; }
            }
            return executed;
        }

        // Test-only shim: expose the ring-CAS claim loop with an injectable per-block
        // callback so the claim protocol is unit-tested in isolation (no SoftStep).
        template <class RunBlock>
        inline int ExecuteStageForTest(SolverStage& stage, int prevSync, int curSync,
                                       RunBlock&& runBlock) noexcept
        {
            return ExecuteStage(stage, prevSync, curSync, std::forward<RunBlock>(runBlock), 0);
        }

        // -----------------------------------------------------------------------
        // ExecuteMainStage -- the main worker drives one stage (Box2D
        // b2ExecuteMainStage): publish the stage-advance word, participate in
        // claiming/running blocks, then spin on the completion barrier and reset.
        // -----------------------------------------------------------------------
        inline void ExecuteMainStage(SolverStageContext& sc, int stageIndex)
        {
            SolverStage& stage = sc.stages[stageIndex];
            const int blockCount = stage.blockCount;
            if (blockCount <= 0) { return; } // empty stage (e.g. no awake bodies)

            const int prevSync = stage.syncIndex;
            const int curSync = prevSync + 1;

            // Single-store "advance to next stage" signal (syncIndex round + stage
            // index) for the thieves (Gap 1.2). Written here; unread in the serial
            // main task. Release so a thief that observes it sees the stage setup.
            const std::uint32_t syncBits =
                (static_cast<std::uint32_t>(curSync) << 16) |
                (static_cast<std::uint32_t>(stageIndex) & 0xFFFFu);
            sc.stageSync.store(syncBits, std::memory_order_release);

            // The main participates in claiming + running blocks (ring from 0). It
            // self-completes every block a thief did not grab, so liveness holds for
            // any worker count (Gap 1.1: it grabs them all).
            ExecuteStage(stage, prevSync, curSync,
                         [&](int blockIndex) { ExecuteBlock(sc, stage, blockIndex); },
                         /*startIndex=*/0);

            // Barrier: wait for every block to finish (self + thieves), then reset
            // the per-run count and advance the per-stage CAS round.
            while (stage.completionCount.load(std::memory_order_acquire) != blockCount)
            {
                // spin (Gap 1.1: already complete -> exits immediately)
            }
            stage.completionCount.store(0, std::memory_order_relaxed);
            stage.syncIndex = curSync;
        }

        // -----------------------------------------------------------------------
        // SolverWorker -- the persistent region entry. workerIndex == 0 is the main
        // driver: it walks the flat stage list in D1's exact substep order, running
        // each colored/body stage via ExecuteMainStage and the overflow + joint
        // passes main-serial inline between them. The thief branch (workerIndex > 0)
        // is added in Gap 1.2; for now only the main path exists, so a serial
        // executor (one partition, begin==0) reproduces the deterministic reference.
        //
        // Stage layout in sc.stages (C = activeColorStages):
        //   [0]            IntegrateVelocities
        //   [1 .. C]       WarmStart per active color
        //   [C+1 .. 2C]    Solve  per active color
        //   [2C+1]         IntegratePositions
        //   [2C+2 .. 3C+1] Relax  per active color
        //   [3C+2 .. 4C+1] Restitution   per active color (once)
        //   [4C+2 .. 5C+1] StoreImpulses per active color (once)
        // The substep-reused stages advance their own syncIndex each substep; the
        // restitution/store stages run once.
        // -----------------------------------------------------------------------
        inline void SolverWorker(SolverStageContext& sc, std::uint32_t workerIndex)
        {
            if (workerIndex != 0)
            {
                // Thief path: Gap 1.2. No-op in the serial-main task.
                return;
            }

            const int C = sc.activeColorStages;
            for (int s = 0; s < sc.substepCount; ++s)
            {
                ExecuteMainStage(sc, 0); // IntegrateVelocities

                for (int c = 0; c < C; ++c) { ExecuteMainStage(sc, 1 + c); }          // WarmStart
                if (sc.overflowWarmStart) { sc.overflowWarmStart(); }
                if (sc.jointBridge) { sc.jointBridge(); }                              // joint pass #1

                for (int c = 0; c < C; ++c) { ExecuteMainStage(sc, 1 + C + c); }      // Solve (bias)
                if (sc.overflowSolve) { sc.overflowSolve(true); }

                ExecuteMainStage(sc, 1 + 2 * C);                                       // IntegratePositions
                if (sc.jointBridge) { sc.jointBridge(); }                              // joint pass #2

                for (int c = 0; c < C; ++c) { ExecuteMainStage(sc, 2 + 2 * C + c); }  // Relax (no bias)
                if (sc.overflowSolve) { sc.overflowSolve(false); }
            }

            for (int c = 0; c < C; ++c) { ExecuteMainStage(sc, 2 + 3 * C + c); }      // Restitution (once)
            if (sc.overflowRestitution) { sc.overflowRestitution(); }
            for (int c = 0; c < C; ++c) { ExecuteMainStage(sc, 2 + 4 * C + c); }      // StoreImpulses (once)
        }

    } // namespace Physics
} // namespace Arcane
