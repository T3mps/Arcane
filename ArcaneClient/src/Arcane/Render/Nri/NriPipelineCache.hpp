#pragma once

// NRI substrate (Phase 2): the graph path's pipeline/pipeline-layout cache.
//
// PLACEHOLDER, landed by Task 6 -- TASK 7 FILLS THIS IN. The plan gives the
// class its real shape there (a GraphicsKey of {shaderPairId, layoutId,
// colorFormats, depthFormat, colorCount, ...} -> nri::Pipeline*, a dedup'd
// RegisterLayout, and a Clear(Graveyard&, fence) for project switch /
// shutdown -- see docs/plans/2026-08-13-nri-phase2-framegraph-2d-cutover.md,
// Task 7). Extend this class in place; do not create a second one.
//
// Why it exists a task early: RgExecuteDesc's frozen field is
// `NriPipelineCache& pipelines`, and RenderGraphNodeContext carries the same
// reference through to every node's exec fn. A reference to an INCOMPLETE
// type is fine inside RenderGraph.hpp (which only forward-declares this, and
// never calls through it), but Task 6's headless [nri] integration tests
// have to CONSTRUCT one to fill that field -- and a forward declaration
// cannot be constructed. So the type is real and empty rather than
// hypothetical, which also means Task 7's fill-in changes no signature
// anywhere.
//
// Deliberately NOT ARCANE_API and deliberately header-only while empty: an
// exported class with no exported members buys nothing, and the test exe
// constructing its own instance of an empty placeholder is exactly the
// intent. Task 7 exports it when it gains state that lives in the DLL.
//
// Include-order rule (same as every file in this directory -- see
// NriCommon.hpp): NRI headers first, because Extensions/NRIDeviceCreation.h
// declares nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp ->
// spdlog) #defines ERROR via wingdi.h.
#include <NRI.h>

namespace Arcane
{
    class NriPipelineCache
    {
    };
}
