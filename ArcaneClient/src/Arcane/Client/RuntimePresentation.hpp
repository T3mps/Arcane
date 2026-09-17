#pragma once

// RuntimePresentation: the presentation half of what Runtime's substrate used to be
// -- the OS audio device, the host's per-frame input snapshot, the ImGui cross-DLL
// handoff and the ONE ViewTransform the host/scene/plugin pushes. Lifted out of Runtime::Impl
// (Core-DLL split, plan 1 Task 1; spec docs/specs/2026-09-15-core-dll-split-
// design.md s1.3/s2) so that the headless Runtime carries NO Audio/Input include.
// Task 4 made it ClientRuntime's member (Arcane/Client/ClientRuntime.hpp), which is
// its only owner. Client-only; Core never includes it.

#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/glm.hpp>

namespace Arcane
{
    class Assets;

    struct RuntimePresentation
    {
        InputSnapshot          input{};        // latest host-supplied snapshot; plugins read via Input()
        Audio::AudioDeviceDesc audioDesc{};
        Audio::AudioDevice     audio;
        // THE ONE camera (F4 plan 1): identity matrices + viewport 0 until a
        // host pushes one. A host that never pushes a view has no Affine2D
        // (AsAffine2D() is nullopt for a zero viewport): the pixel overlays
        // skip drawing, while sprites -- world quads since F4 plan 1 T5 -- draw
        // through the identity view-projection; RuntimeApp warns when no scene
        // camera exists. There is deliberately no fallback camera.
        ViewTransform          view{};
        void* imguiContext  = nullptr;   // ImGuiContext*      -- all null in a headless host
        void* imguiAlloc    = nullptr;   // ImGuiMemAllocFunc
        void* imguiFree     = nullptr;   // ImGuiMemFreeFunc
        void* imguiUserData = nullptr;

        // enableAudioDevice: opt into a real OS device (interactive hosts); false = the
        // miniaudio null backend (tests/servers/tools/--frames N). The real->null fallback
        // applies either way. `assets` may be null (no audio at all).
        void InitAudio(Assets* assets, bool enableAudioDevice) noexcept
        {
            audioDesc.enableDevice = enableAudioDevice;
            if (!assets) return;
            if (audio.Init(assets, audioDesc)) return;
            if (audioDesc.enableDevice)
            {
                ARC_WARN("Runtime: audio device init failed; falling back to null backend");
                audioDesc.enableDevice = false;
                if (audio.Init(assets, audioDesc)) return;
            }
            ARC_WARN("Runtime: audio subsystem is unavailable");
        }
        void ResetAudio(Assets* assets) noexcept { audio.Shutdown(); InitAudio(assets, audioDesc.enableDevice); }
        ~RuntimePresentation() { audio.Shutdown(); }
    };
}
