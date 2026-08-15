#pragma once

// The two things a GOLDEN editor run pins about its viewport, as pure
// registry->answer functions (no ImGui, no device, no EditorApp state), so the
// [editor] units drive them headlessly -- same split as EditorCamera.hpp /
// ViewportInput.hpp: this file decides, EditorApp performs.
//
// WHY A PIN EXISTS AT ALL (NRI Phase 3, Task 13 follow-up). A golden artifact
// must be a pure function of the scene + the command line. Two inputs of the
// EDITOR's viewport are neither:
//
//   * THE VIEW. The editor's viewport camera is the editor's own
//     (EditorCamera, driven by pan/zoom and the F/Home framing keys) and has
//     nothing to do with the scene's authored Camera entity the runtime golden
//     looks through. Nothing pinned it, so a scripted run captured whatever
//     the free camera happened to hold -- and its first-open framing is fitted
//     to the viewport extent of the frame it ran on, which at boot is the
//     1280x720 default rather than the docked panel's real size. The D3c
//     baselines that came out of that were ~90% flat background with the
//     content pushed off the bottom-right corner: exactly the Phase-0 lesson
//     ("99.9% clear colour" goldens prove determinism and nothing else)
//     recurring in the editor.
//
//   * THE SELECTION. Nothing selects, so the editor's `full` stage -- the one
//     stage whose whole point is the pick/JFA/composite chain -- rendered no
//     outline nodes and came out BYTE-IDENTICAL to `post`. A stage that
//     provably equals its predecessor is a dead gate.
//
// Both answers below are derived from the SCENE, never from persisted editor
// state (an imgui.ini camera or a user-left selection would be per-session
// state, i.e. the layout-dependency trap in a worse form).

#include "Viewport/EditorCamera.hpp"

#include <Arcane/Render/PickEmit.hpp>

#include <Astra/Entity/Entity.hpp>
#include <Astra/Registry/Registry.hpp>

#include <optional>
#include <vector>

namespace Arcane::Editor
{
    // THE VIEW PIN: the whole scene's renderable bounds, fitted into a
    // `viewportSize` panel -- i.e. exactly what Home / Frame All computes
    // (SceneFramingBounds + EditorCamera::Frame, both already unit-pinned in
    // EditorCameraTest.cpp), asked for as a value instead of applied to a
    // member. nullopt when the scene has nothing framable, which the caller
    // must read as "leave the camera alone" rather than "recentre".
    //
    // Chosen over pointing the editor camera at the scene's AUTHORED camera
    // (the transform the runtime golden looks through) for three reasons:
    //   * it needs no Camera component, so it frames ANY project's scene --
    //     the editor golden harness is not ReferenceProject-specific;
    //   * it is the editor's OWN framing semantic. The authored view is
    //     already covered pixel-for-pixel by the runtime's `main-*` goldens;
    //     duplicating it here would spend the editor baselines on a picture
    //     another baseline already holds;
    //   * it keeps the scene-camera RECT overlay (SubmitSceneToBatcher draws
    //     the authored camera's frustum in the edit view) a real, closed
    //     rectangle inside the frame. Pinning the editor camera ONTO the
    //     authored one would collapse that overlay onto the viewport border,
    //     where it is at most a 1px edge -- losing coverage of an
    //     editor-only draw path that these baselines are the only gate for.
    //
    // Callers already holding refreshed WorldTransforms should keep them
    // refreshed: SceneFramingBounds reads derived world poses, and Edit mode
    // is the mode where nothing else propagates them.
    [[nodiscard]] inline std::optional<EditorCamera>
    GoldenPinnedCamera(Astra::Registry& reg, glm::vec2 viewportSize)
    {
        const FramingBounds bounds = SceneFramingBounds(reg);
        if (!bounds.Valid())
            return std::nullopt;
        EditorCamera cam;                       // defaults, then fitted
        cam.Frame(bounds.min, bounds.max, viewportSize);
        return cam;
    }

    // THE SELECTION PIN: the entity the id pass assigns HIT-PROXY ID 1.
    //
    // Deliberately the same handle the runtime's `--pick-probe` fabricates
    // (HostConfig.hpp: "hit-proxy id 1"), and deliberately derived from THE
    // emitter rather than from a second registry walk: CollectPickables IS the
    // id assignment (the k-th drawable is id k+1), it is documented
    // archetype-stable, and both editor render arms feed it the same registry
    // -- so "id 1" is the one selection that cannot mean different entities on
    // the NVRHI arm (which captures the baselines) and the graph arm (which is
    // compared against them). Naming an entity any other way -- a scene index,
    // a name, an Identity guid -- would be a SECOND ordering rule to keep in
    // agreement with the id pass, and the outline's seed is that pass's ids.
    //
    // The view is irrelevant to the ANSWER: CollectPickables projects geometry
    // through it but appends in a view-independent order, so the identity
    // transform below cannot influence which entity comes back. Invalid entity
    // when the scene has nothing pickable (the caller must then hold no
    // selection -- an empty scene has no honest outline).
    [[nodiscard]] inline Astra::Entity GoldenPinnedSelection(Astra::Registry& reg)
    {
        std::vector<PickDrawable> drawables;
        CollectPickables(reg, PickView{}, drawables);
        return drawables.empty() ? Astra::Entity{} : drawables.front().entity;
    }
}
