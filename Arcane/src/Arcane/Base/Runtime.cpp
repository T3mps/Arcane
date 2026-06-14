#include <Arcane/Base/Runtime.hpp>

#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // RenderContext2D (instantiated IN this module)

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/WorkScheduler.hpp>

namespace Arcane
{
    struct Runtime::Impl
    {
        JobSystem                                   jobs;
        std::shared_ptr<Astra::IWorkScheduler>      sched;
        std::unique_ptr<Astra::TypeContext>         ownedContext;   // null when an external one is injected
        Astra::TypeContext*                         context = nullptr;
        std::shared_ptr<Astra::ComponentRegistry>   components;
        std::unique_ptr<Astra::Registry>            registry;
        std::unique_ptr<SystemSchedulers>           schedulers;
        std::unique_ptr<RunLoop>                    loop;
        RunLoop::Config                             loopCfg;

        explicit Impl(Astra::TypeContext* external) : jobs(), sched(jobs.WorkScheduler())
        {
            if (external) { context = external; }
            else { ownedContext = std::make_unique<Astra::TypeContext>(); context = ownedContext.get(); }

            // Install the shared context in THIS module BEFORE any TypeID/Registry use.
            Astra::SetTypeContext(context);
            components = std::make_shared<Astra::ComponentRegistry>();

            Astra::Registry::Config cfg;
            cfg.workScheduler = sched;
            registry   = std::make_unique<Astra::Registry>(components, cfg);
            schedulers = std::make_unique<SystemSchedulers>(sched);
            loop       = std::make_unique<RunLoop>(*registry, *schedulers, loopCfg);
        }
    };

    Runtime::Runtime(Astra::TypeContext* externalContext)
        : m_impl(std::make_unique<Impl>(externalContext)) {}
    Runtime::~Runtime() = default;   // do not reset the module slot: a later Runtime re-installs

    Astra::Registry&  Runtime::Registry()   noexcept { return *m_impl->registry; }
    SystemSchedulers& Runtime::Schedulers() noexcept { return *m_impl->schedulers; }
    RunLoop&          Runtime::Loop()       noexcept { return *m_impl->loop; }
    Astra::TypeContext*    Runtime::TypeContext()   noexcept { return m_impl->context; }
    Astra::IWorkScheduler* Runtime::WorkScheduler() noexcept { return m_impl->sched.get(); }
    std::shared_ptr<Astra::ComponentRegistry> Runtime::Components() noexcept { return m_impl->components; }

    void Runtime::SetRenderContext(Batcher2D* batcher, glm::vec2 cameraOffset)
    {
        // SetResource<RenderContext2D> runs IN Arcane.dll so TypeID<RenderContext2D>
        // resolves here against the shared context; the host never touches a scene TypeID.
        m_impl->registry->SetResource<RenderContext2D>(RenderContext2D{batcher, cameraOffset});
    }

    std::vector<std::byte> Runtime::SnapshotRegistry() const
    {
        auto r = m_impl->registry->Save();
        return r.IsOk() ? std::move(*r.GetValue()) : std::vector<std::byte>{};
    }

    bool Runtime::RestoreRegistry(std::span<const std::byte> bytes)
    {
        Astra::Registry::Config cfg;
        cfg.workScheduler = m_impl->sched;
        auto r = Astra::Registry::Load(bytes, m_impl->components, cfg);   // 3.3 Config overload
        if (r.IsErr())
            return false;
        m_impl->registry = std::move(*r.GetValue());
        m_impl->loop = std::make_unique<RunLoop>(*m_impl->registry, *m_impl->schedulers, m_impl->loopCfg);
        return true;
    }

    void Runtime::ResetRegistry()
    {
        // Fresh-boot reload: replace the registry with an empty one (same shared
        // ComponentRegistry + scheduler) so the plugin's Init rebuilds its scene.
        Astra::Registry::Config cfg;
        cfg.workScheduler = m_impl->sched;
        m_impl->registry = std::make_unique<Astra::Registry>(m_impl->components, cfg);
        m_impl->loop = std::make_unique<RunLoop>(*m_impl->registry, *m_impl->schedulers, m_impl->loopCfg);
    }

    void Runtime::ClearSystems()
    {
        m_impl->schedulers->fixedUpdate.Clear();
        m_impl->schedulers->update.Clear();
        m_impl->schedulers->render.Clear();
    }
}
