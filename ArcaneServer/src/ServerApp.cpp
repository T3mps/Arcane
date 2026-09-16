#include "ServerApp.hpp"

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>       // NetMode / Arcane::ToString(NetMode)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectHost.hpp>        // VerifySharedTypeContext / GameModule / PluginModules / BootScene
#include <Arcane/Scene/PhysicsSystem.hpp>        // Arcane::PhysicsSystem (systems.hasPhysics)
#include <Arcane/Scene/TransformSystems.hpp>     // Arcane::TransformPropagationSystem (systems.hasPropagation)

#include <Astra/Core/TypeContext.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>

// Only for the two GetModuleHandleW census probes below (P10/S1). Windows-only
// until the Linux port: elsewhere the probe has no equivalent worth faking, so
// both report false and the report says nothing rather than something wrong.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Arcane::Server
{
    int ServerApp::Finish(ServerReport& rep, std::string exitReason, int exitCode)
    {
        rep.exitReason = std::move(exitReason);
        if (!m_cfg.reportPath.empty() && !rep.WriteTo(m_cfg.reportPath))
            ARC_ERROR("ArcaneServer: failed to write --report to '{}'", m_cfg.reportPath);
        return exitCode;
    }

    int ServerApp::Run()
    {
        ServerReport rep;   // every field defaulted honestly (opened=false, loaded=false, ...)
        // The exe's OWN imports, sampled BEFORE any module loads -- P10/S1: this
        // exe's link line never names ArcaneClient, so this must read false on a
        // clean build. If it ever reads true, a Core symbol's export/import got
        // routed through Client somewhere and that is a defect to fix, not a
        // value to relax. Windows-only probe (see the include guard above): it
        // reads false on every other platform, where no host ships yet.
#ifdef _WIN32
        rep.clientDllLoadedAtBoot = ::GetModuleHandleW(L"ArcaneClient.dll") != nullptr;
#endif

        Arcane::ProcessContextDesc d;
        d.isDedicatedServerProcess = true;
        m_process = Arcane::ProcessContext::Create(d);
        if (!m_process)
        {
            ARC_ERROR("ArcaneServer: ProcessContext refused -- a second host in this process?");
            return Finish(rep, "process-context-refused", 1);
        }
        // This exe's OWN per-module Astra slot (ArcaneCore.dll's own slot is
        // installed by ProcessContext::Create itself) -- see ProjectHost.hpp's
        // VerifySharedTypeContext for why every module that touches a
        // component type must do this for itself.
        Astra::SetTypeContext(&m_process->TypeContext());

        // The ONE authoritative world: NetMode::DedicatedServer instantiates only
        // the module's Server-masked system factories (Arcane/Plugin/SystemFactory.hpp).
        m_runtime.emplace(*m_process, Arcane::NetMode::DedicatedServer);
        rep.netMode                  = Arcane::ToString(m_runtime->Mode());
        rep.isDedicatedServerProcess = m_process->IsDedicatedServerProcess();

        // Review round 1: make --fixed-dt REAL. RunLoop's internal accumulator
        // ticks at RunLoop::Config::fixedHz (default 60), independent of the
        // realDt passed to Advance() below -- so without this call, --fixed-dt
        // would only repace the host loop and never the physics step size.
        // SetFixedHz(1/cfg.fixedDtSeconds) makes the REQUESTED step the loop's
        // ACTUAL one; the tick loop below still advances by cfg.fixedDtSeconds
        // of wall time per host frame, i.e. one fixed step per frame, as before.
        // rep.fixedDt is derived back FROM THE LOOP, not echoed from m_cfg, so a
        // future refusal/clamp inside SetFixedHz (RunLoop.hpp) is reported
        // honestly rather than optimistically.
        m_runtime->Loop().SetFixedHz(1.0 / m_cfg.fixedDtSeconds);
        rep.fixedDt = 1.0 / m_runtime->Loop().FixedHz();

        if (!m_runtime->OpenProject(m_cfg.projectPath))
            return Finish(rep, "project-open-failed", 1);

        const Arcane::Project* proj = m_runtime->CurrentProject();
        rep.projectOpened = true;
        rep.projectName   = proj->Manifest().name;
        rep.projectAbi    = proj->Manifest().engineAbi;

        if (!Arcane::ProjectHost::VerifySharedTypeContext(m_runtime->Registry(), "ArcaneServer.exe"))
            return Finish(rep, "type-context-mismatch", 1);

        const std::string module = Arcane::ProjectHost::GameModule(proj, m_cfg.pluginPath);
        m_plugin.emplace(*m_process, module.empty() ? std::filesystem::path{} : std::filesystem::path(module));
        m_plugin->AttachRuntime(*m_runtime);
        for (const auto& dll : Arcane::ProjectHost::PluginModules(proj))
            m_plugin->AddPlugin(dll);
        rep.modulePath = module;

        if (!m_plugin->Load())
            return Finish(rep, "module-load-failed", 1);

        rep.moduleLoaded     = true;
        rep.moduleGeneration = m_plugin->Generation();
        // P10: the game module's OWN import of ArcaneClient (the module links
        // both import libs so it can build against the full engine surface),
        // reported honestly rather than hidden -- this exe's own link line
        // still never names ArcaneClient. Windows-only, like its sibling above.
#ifdef _WIN32
        rep.clientDllLoadedAfterModule = ::GetModuleHandleW(L"ArcaneClient.dll") != nullptr;
#endif

        (void)Arcane::ProjectHost::BootScene(*m_runtime, *proj);

        // Fixed-step tick, forever or --frames N. Wall-clock paced by sleeping the
        // remainder of each step (spec s6: tick rate is a RUNTIME value); the loop
        // is unpaused (fresh).
        for (std::uint64_t f = 0; m_cfg.frames == 0 || f < m_cfg.frames; ++f)
        {
            const auto start = std::chrono::steady_clock::now();
            m_runtime->EnsurePhysics();
            m_runtime->Loop().Advance(m_cfg.fixedDtSeconds,
                                      [&](double dt) { m_plugin->FixedUpdateAll(dt); },
                                      [&](double dt, double a) { m_plugin->UpdateAll(dt, a); });
            m_plugin->Poll();
            Arcane::Diagnostics::Heartbeat();
            ++rep.framesTicked;
            std::this_thread::sleep_until(start + std::chrono::duration<double>(m_cfg.fixedDtSeconds));
        }

        rep.fixedUpdate = m_runtime->Schedulers().fixedUpdate.Size();
        rep.update      = m_runtime->Schedulers().update.Size();
        rep.render      = m_runtime->Schedulers().render.Size();
        rep.hasPhysics     = m_runtime->Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem>();
        rep.hasPropagation = m_runtime->Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>();
        rep.hasRenderSubmission = false;   // by construction: no ClientRuntime exists in this process
        rep.clientAttached      = m_runtime->Client() != nullptr;

        return Finish(rep, "frames-complete", 0);
    }
}
