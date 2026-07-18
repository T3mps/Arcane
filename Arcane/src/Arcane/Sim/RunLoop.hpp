#pragma once

// RunLoop: fixed-timestep accumulator + render alpha. Mirrors the proven Lua
// Application cadence (fixed 60 UPS, alpha on draw). Header-only so it operates
// on the host-owned Registry (see SystemSchedulers.hpp for the ownership rule).
//
// Advance() runs >=0 FixedUpdate steps then Update once, through the parallel
// executor. SubmitRender() runs the Render scheduler SEQUENTIALLY on the calling
// thread (Batcher2D is not thread-safe and must be recorded where Begin was
// called). The host brackets SubmitRender() between Batcher2D::Begin/End.
//
// Sim-time control (Epic 04): pause / single-step / time-scale gate the FIXED
// phase only; Update + Render keep running so an editor's camera/UI/input stay
// live while the sim is frozen. Consumers reach these through Runtime::Loop();
// there is no separate Runtime API and no plugin-ABI surface. TIME-SCALE scales
// the SIM CLOCK by scaling the accumulator input -- the fixed step stays canonical
// (1/fixedHz), so the physics step is byte-identical at any scale (determinism is
// preserved; a variable step dt would defeat the fixed-timestep contract). Smooth
// slow-mo is a render-interpolation concern (lerp by Alpha()), not a change to the
// time model.

#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include <functional>

namespace Arcane
{
    class RunLoop
    {
    public:
        struct Config
        {
            double fixedHz = 60.0;
            int    maxStepsPerFrame = 5;   // clamp to avoid the spiral of death
        };

        RunLoop(Astra::Registry& registry, SystemSchedulers& schedulers, Config cfg = {})
            : m_registry(&registry), m_schedulers(&schedulers), m_cfg(cfg) {}

        // ---- sim-time control -------------------------------------------------
        // Freeze/thaw the fixed phase. While paused the sim clock does not advance
        // (no accumulation, zero fixed steps) but Update + Render still run.
        void SetPaused(bool p) noexcept { m_paused = p; }
        [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }

        // Sim-clock rate relative to real time (1.0 == real time). Clamped to >= 0;
        // 0 stalls the sim like a pause but keeps accumulating nothing. Scales the
        // accumulator input only -- the fixed step stays canonical.
        void SetTimeScale(double s) noexcept { m_timeScale = s < 0.0 ? 0.0 : s; }
        [[nodiscard]] double TimeScale() const noexcept { return m_timeScale; }

        // Request exactly one canonical fixed step on the next Advance, even while
        // paused; one-shot (auto-clears after it runs). Ignored if not paused (the
        // running loop already steps), but harmless to call.
        void RequestSingleStep() noexcept { m_singleStep = true; }

        // Advance one real frame. Returns the render alpha in [0,1) for interpolation.
        double Advance(double realDt)
        {
            StepFixed(realDt, nullptr);
            m_schedulers->update.Execute(*m_registry, &m_schedulers->executor);
            return m_alpha;
        }

        // Advance stays on std::function (NOT FunctionRef): RunLoop.hpp is in the
        // plugin-facing Runtime.hpp include chain, which is kept Core-free so the
        // minimal/ABI plugins build without Core/src. Advance is a once-per-frame
        // host call, not a hot path, so std::function is fine here.
        //
        // Host-driven variant: pluginFixed(fixedDt) runs each fixed step BEFORE the engine
        // fixedUpdate scheduler (gameplay moves transforms; propagation reads them after).
        // pluginUpdate(dt, alpha) runs once after the Update scheduler. Same spiral-of-death
        // clamp as Advance(realDt).
        double Advance(double realDt,
                       const std::function<void(double)>& pluginFixed,
                       const std::function<void(double, double)>& pluginUpdate)
        {
            StepFixed(realDt, &pluginFixed);
            m_schedulers->update.Execute(*m_registry, &m_schedulers->executor);
            if (pluginUpdate) pluginUpdate(realDt, m_alpha);
            return m_alpha;
        }

        // Run the Render scheduler on the calling thread (single-threaded).
        void SubmitRender()
        {
            m_schedulers->render.Execute(*m_registry);
        }

        double Alpha() const noexcept { return m_alpha; }

    private:
        // The fixed phase for one real frame, under sim-time control. pluginFixed may
        // be null (the no-callback Advance) or point at the host's std::function.
        void StepFixed(double realDt, const std::function<void(double)>* pluginFixed)
        {
            const double fixedDt = 1.0 / m_cfg.fixedHz;
            const auto runFixed = [&]
            {
                if (pluginFixed && *pluginFixed) (*pluginFixed)(fixedDt);
                m_schedulers->fixedUpdate.Execute(*m_registry, &m_schedulers->executor);
            };

            if (m_singleStep)
            {
                m_singleStep = false;               // one-shot
                runFixed();                          // exactly one canonical tick
                // Deliberately do NOT touch the accumulator: a manual step advances
                // the sim clock by one tick without disturbing the real-time backlog.
            }
            else if (!m_paused)
            {
                m_accumulator += realDt * m_timeScale;  // time-scale scales the SIM CLOCK
                int steps = 0;
                while (m_accumulator >= fixedDt && steps < m_cfg.maxStepsPerFrame)
                {
                    runFixed();
                    m_accumulator -= fixedDt;
                    ++steps;
                }
                if (steps == m_cfg.maxStepsPerFrame && m_accumulator >= fixedDt)
                    m_accumulator = 0.0;             // dropped the backlog; do not accumulate debt
            }
            // paused (and not single-stepping): no accumulation, no fixed steps.

            m_alpha = m_accumulator / fixedDt;
        }

        Astra::Registry*  m_registry;
        SystemSchedulers* m_schedulers;
        Config m_cfg;
        double m_accumulator = 0.0;
        double m_alpha = 0.0;

        // Sim-time control (Epic 04). Defaults == real-time, running, so Advance is
        // byte-identical to the pre-sim-time-control behavior.
        bool   m_paused    = false;
        double m_timeScale = 1.0;
        bool   m_singleStep = false;
    };
}
