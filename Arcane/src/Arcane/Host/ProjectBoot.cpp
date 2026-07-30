#include <Arcane/Host/ProjectBoot.hpp>

#include <Arcane/Host/GpuContext.hpp>

#include <type_traits>

namespace Arcane::HostBoot
{
    // Binding proof (2026-07-30 review): BootContext::gpu must be a real
    // Arcane::GpuContext*, not a phantom Arcane::HostBoot::GpuContext* that an
    // elaborated-type-specifier ("class GpuContext*") would silently conjure
    // up if no Arcane::GpuContext were visible at the point BootContext is
    // declared. Kept permanent rather than thrown away: it costs nothing at
    // compile time and fails loudly if the forward declarations in
    // ProjectBoot.hpp are ever removed or reordered past BootContext.
    static_assert(std::is_same_v<decltype(BootContext::gpu), Arcane::GpuContext*>,
                  "BootContext::gpu must bind Arcane::GpuContext -- see the "
                  "Arcane-namespace forward declarations at the top of "
                  "ProjectBoot.hpp");

    namespace
    {
        BootStage Make(std::string id, std::vector<std::string> deps,
                       BootThread thread, BootPolicy policy, std::uint32_t weight,
                       std::function<bool()> run)
        {
            BootStage s;
            s.id = std::move(id);
            s.dependsOn = std::move(deps);
            s.thread = thread;
            s.policy = policy;
            s.weight = weight;
            s.run = std::move(run);
            return s;
        }
    }

    std::vector<BootStage> CoreStages(BootContext& ctx)
    {
        std::vector<BootStage> s;
        // Bodies are wired in Task 8. A null ctx member means the facility is
        // absent (the parity tests construct an all-null context), so every body
        // must tolerate that and return true rather than crash.
        s.push_back(Make("runtime_create",       {},                                  BootThread::Main,   BootPolicy::Fatal,     5, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("type_context_install", {"runtime_create"},                  BootThread::Main,   BootPolicy::Fatal,     1, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("gpu_core",             {},                                  BootThread::Main,   BootPolicy::Fatal,    25, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("project_open",         {"runtime_create"},                  BootThread::Worker, BootPolicy::Optional, 45, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("render_bridge",        {"gpu_core", "runtime_create"},      BootThread::Main,   BootPolicy::Fatal,     3, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("input_config",         {"project_open", "gpu_core"},        BootThread::Main,   BootPolicy::Optional,  2, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("sprite_tables",        {"project_open", "render_bridge"},   BootThread::Main,   BootPolicy::Optional,  2, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("plugin_load",          {"project_open", "render_bridge"},   BootThread::Main,   BootPolicy::Fatal,     9, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("finalize",             {"plugin_load", "input_config",
                                                  "sprite_tables"},                   BootThread::Main,   BootPolicy::Fatal,     1, [&ctx] { (void)ctx; return true; }));
        return s;
    }

    std::vector<std::string> CoreStageIds()
    {
        BootContext ctx{};
        std::vector<std::string> ids;
        for (const BootStage& s : CoreStages(ctx)) ids.push_back(s.id);
        return ids;
    }

    std::vector<BootStage> RuntimeStages(BootContext& ctx)
    {
        return CoreStages(ctx);   // appends nothing -- that is the point
    }

    std::vector<BootStage> EditorStages(BootContext& ctx)
    {
        std::vector<BootStage> s = CoreStages(ctx);
        s.push_back(Make("editor_fonts",  {"gpu_core"},      BootThread::Main,   BootPolicy::Fatal,    5, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("splash_ready",  {"editor_fonts"},  BootThread::Main,   BootPolicy::Fatal,    2, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("editor_shell",  {"editor_fonts"},  BootThread::Main,   BootPolicy::Fatal,    3, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("editor_lock",   {"project_open"},  BootThread::Worker, BootPolicy::Optional, 1, [&ctx] { (void)ctx; return true; }));
        return s;
    }

    std::vector<std::string> EditorStageIdsForTest(BootContext& ctx)
    {
        std::vector<std::string> ids;
        for (const BootStage& s : EditorStages(ctx)) ids.push_back(s.id);
        return ids;
    }

    std::vector<std::string> RuntimeStageIdsForTest(BootContext& ctx)
    {
        std::vector<std::string> ids;
        for (const BootStage& s : RuntimeStages(ctx)) ids.push_back(s.id);
        return ids;
    }
}
