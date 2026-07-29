#pragma once
// FramePerf: opt-in per-phase frame timing for a runtime host's frame loop.
// Mirrors the prior inline acc* accumulators + the 60-frame [PERF] dump,
// lifted out of main. A no-op unless `enabled`. Header-only; depends on
// std::chrono + Arcane::Log. PRESENTATION-FREE + C++23-clean.
#include <chrono>
#include <cstdint>
#include <Arcane/Base/Log.hpp>
namespace Arcane
{
    class FramePerf
    {
    public:
        using Clock = std::chrono::steady_clock;
        explicit FramePerf(bool enabled) : m_on(enabled) {}

        [[nodiscard]] bool On() const noexcept { return m_on; }
        [[nodiscard]] Clock::time_point Now() const { return Clock::now(); }
        // ms between two stamps (caller takes stamps only when On()).
        [[nodiscard]] static double Ms(Clock::time_point a, Clock::time_point b)
        { return std::chrono::duration<double, std::milli>(b - a).count(); }

        void FrameStart() { if (m_on) m_frameStart = Clock::now(); }
        void Add(double& acc, Clock::time_point a, Clock::time_point b) const { if (m_on) acc += Ms(a, b); }

        // Accumulators (public for the loop to add into; only touched when On()).
        double accFrame=0, accSim=0, accRec=0, accEnd=0, accTone=0, accImgui=0, accPresent=0, accPoll=0;

        // Call once per frame end with the frame's batcher stats. Emits + resets every 60 frames.
        void Tick(std::uint32_t quads, std::uint32_t draws)
        {
            if (!m_on) return;
            accFrame += Ms(m_frameStart, Clock::now());
            if (++m_frames < 60) return;
            ARC_INFO("[PERF] {:.2f} ms ({:.1f} FPS) | sim {:.2f} rec {:.2f} end {:.2f} "
                     "tone {:.2f} imgui {:.2f} present {:.2f} poll {:.2f} | quads {} draws {}",
                     accFrame/m_frames, 1000.0*m_frames/accFrame, accSim/m_frames, accRec/m_frames,
                     accEnd/m_frames, accTone/m_frames, accImgui/m_frames, accPresent/m_frames,
                     accPoll/m_frames, quads, draws);
            accFrame=accSim=accRec=accEnd=accTone=accImgui=accPresent=accPoll=0; m_frames=0;
        }
    private:
        bool m_on;
        std::uint64_t m_frames = 0;
        // Per-frame start stamp: only FrameStart()/Tick() read/write it, so it lives
        // private (the loop touches the public acc* accumulators, never this directly).
        Clock::time_point m_frameStart{};
    };
}
