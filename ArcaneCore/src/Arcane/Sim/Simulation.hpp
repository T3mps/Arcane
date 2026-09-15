#pragma once

// Simulation: the substrate's host-facing entry. Owns the Registry (configured
// with the injected scheduler) and the per-phase SystemSchedulers. Header-only
// so the host module owns the registry (single-module ownership rule).

#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Core/WorkScheduler.hpp>

#include <memory>
#include <utility>

namespace Arcane
{
    class Simulation
    {
    public:
        explicit Simulation(std::shared_ptr<Mosaic::IWorkScheduler> sched)
            : m_sched(std::move(sched))
            , m_registry(MakeConfig(m_sched))
            , m_schedulers(m_sched) {}

        Astra::Registry&  Registry() noexcept    { return m_registry; }
        SystemSchedulers&  Schedulers() noexcept { return m_schedulers; }

    private:
        static Astra::Registry::Config MakeConfig(std::shared_ptr<Mosaic::IWorkScheduler> s)
        {
            Astra::Registry::Config cfg;
            cfg.workScheduler = std::move(s);
            return cfg;
        }

        std::shared_ptr<Mosaic::IWorkScheduler> m_sched;
        Astra::Registry  m_registry;
        SystemSchedulers m_schedulers;
    };
}
