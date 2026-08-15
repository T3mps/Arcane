#pragma once
// GoldenHarness: the golden-image capture/compare artifact writer + the
// scene-compile warm-up drain, shared by BOTH host executables. Extracted at
// NRI Phase 3, Task 13 (verbatim moves -- see the task-13 report for the
// diff-match) from two anonymous-namespace helpers that had grown a SECOND
// caller (EditorApp's own golden mode) with no legal way to reach them:
//
//   * GoldenArtifact  -- was in ArcaneRuntime/src/RuntimeFrame.cpp's
//     anonymous namespace (moved there VERBATIM from RuntimeApp.cpp at Task
//     4; see that file's own history comment for the earlier hop).
//   * DrainSceneCompiles -- was in ArcaneRuntime/src/RuntimeApp.cpp's
//     anonymous namespace.
//
// An anonymous-namespace function has internal linkage: it cannot be called
// from a second translation unit, let alone a second executable. Living in
// Host/ (Arcane.dll, ARCANE_API) rather than duplicating either body into
// ArcaneEditor is what keeps the artifact naming, the comparator, the
// tolerances and the exit-code convention IDENTICAL by construction instead
// of by two authors agreeing to copy carefully -- exactly the same reasoning
// that already governs SceneRenderResolver, GpuContext and HostConfig in
// this same directory.
//
// RUNTIME BEHAVIOUR IS BYTE-IDENTICAL after this move: RuntimeApp.cpp and
// RuntimeFrame.cpp's call sites are mechanical renames (drop the anonymous-
// namespace qualification, add the include), nothing about either function's
// body changed. See the task-13 report's three-pass diff-walk for the proof.
#include <Arcane/Base/Api.hpp>
#include <Arcane/Host/HostConfig.hpp>

#include <cstdint>
#include <vector>

namespace Arcane
{
    class SceneRenderResolver;
    class ShaderCompiler;

    // Bound on the golden warm-up below. Generous -- a cold dxcompiler.dll
    // plus a dozen first-time compiles is a few hundred ms at worst -- but
    // BOUNDED: an unbounded wait turns a stuck compile into a hung capture,
    // which is a worse outcome than a loud refusal.
    inline constexpr double kGoldenWarmupTimeoutSeconds = 60.0;

    // Bring the scene's asset resolution to QUIESCENCE before the first
    // counted frame. False on timeout (the caller refuses the run).
    //
    // WHY THIS EXISTS. Golden mode pins the frame clock, which makes Time --
    // and therefore every animated shader input -- a pure function of the
    // frame index. It does NOT pin what the frame CONTAINS. Sprite materials
    // and post chains bind asynchronously: SceneRenderResolver::Refresh
    // submits the compiles, the `shader.compile` worker finishes them in
    // WALL CLOCK, and whichever later Refresh happens to run after that
    // drains and binds them. The compile cache is in-memory only, so every
    // process pays the cold cost. With --no-vsync on a four-quad scene the
    // entire 120-frame run can be over in well under the time a cold dxc
    // needs for the scene's ~12 compiles, so the captured frame would show
    // an unshaded sprite and no post chain -- silently, with no failed
    // assertion anywhere, and differently on a faster or slower machine. A
    // baseline captured from that is a baseline of the wrong picture, and
    // every compare afterwards inherits the same race.
    //
    // Draining HERE rather than "letting the loop run more frames" is what
    // keeps the pinned clock honest: the caller never advances its host
    // clock across this loop, so Time at counted frame N stays exactly N/60
    // and --frames never quietly becomes part of the golden contract.
    ARCANE_API bool DrainSceneCompiles(SceneRenderResolver& resolver, ShaderCompiler& compiler,
                                        float viewportWidth, float viewportHeight);

    // The golden artifact tail, shared by every render path on both hosts.
    // It takes display-referred RGBA8 pixels of the frame that was just
    // presented and nothing else -- so a caller inherits the artifact
    // NAMING, the comparator, the tolerances and the exit code by
    // construction rather than by a second implementation that could drift
    // from it. Returns 0, or 3 on any capture-write or compare failure (the
    // documented golden exit code on both hosts).
    //
    // `namePrefix` defaults to "main" -- ArcaneRuntime's own stem, and the
    // EXACT string the pre-extraction function used, so every existing
    // caller (RuntimeFrame.cpp) is byte-identical without passing anything.
    // ArcaneEditor passes kEditorGoldenNamePrefix ("editor") so its own
    // artifacts land as editor-<stage>-<backend>.png rather than colliding
    // with the runtime's main-<backend>.png in the same --golden-capture
    // directory -- the same stem derivation, one different word.
    ARCANE_API int GoldenArtifact(const HostConfig& config, std::uint32_t width, std::uint32_t height,
                                   const std::vector<unsigned char>& actual,
                                   const char* namePrefix = "main");

    // The editor's own artifact prefix (NRI Phase 3, Task 13) -- see
    // GoldenArtifact's `namePrefix` doc above. A named constant rather than
    // a string literal at each of the editor's two call sites (NVRHI +
    // graph) so the two cannot drift to different spellings.
    inline constexpr const char* kEditorGoldenNamePrefix = "editor";
}
