#pragma once

// RunLoop: fixed-timestep accumulator + render alpha. Mirrors the proven Lua
// Application cadence (fixed 60 UPS, alpha on draw). Header-only so it operates
// on the host-owned Registry (see SystemSchedulers.hpp for the ownership rule).
//
// Advance() runs >=0 FixedUpdate steps then Update once, through the parallel
// executor. SubmitRender() runs the Render scheduler SEQUENTIALLY on the calling
// thread (Batcher2D is not thread-safe and must be recorded where Begin was
// called). The host brackets SubmitRender() between Batcher2D::Begin/End.

#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

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

        // Advance one real frame. Returns the render alpha in [0,1) for interpolation.
        double Advance(double realDt)
        {
            const double fixedDt = 1.0 / m_cfg.fixedHz;
            m_accumulator += realDt;

            int steps = 0;
            while (m_accumulator >= fixedDt && steps < m_cfg.maxStepsPerFrame)
            {
                m_schedulers->fixedUpdate.Execute(*m_registry, &m_schedulers->executor);
                m_accumulator -= fixedDt;
                ++steps;
            }
            if (steps == m_cfg.maxStepsPerFrame && m_accumulator > fixedDt)
                m_accumulator = 0.0;  // dropped the backlog; do not accumulate debt

            m_schedulers->update.Execute(*m_registry, &m_schedulers->executor);

            m_alpha = m_accumulator / fixedDt;
            return m_alpha;
        }

        // Run the Render scheduler on the calling thread (single-threaded).
        void SubmitRender()
        {
            m_schedulers->render.Execute(*m_registry);
        }

        double Alpha() const noexcept { return m_alpha; }

    private:
        Astra::Registry*  m_registry;
        SystemSchedulers* m_schedulers;
        Config m_cfg;
        double m_accumulator = 0.0;
        double m_alpha = 0.0;
    };
}
