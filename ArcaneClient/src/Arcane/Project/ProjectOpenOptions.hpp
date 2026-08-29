#pragma once

// Per-open settings for Project::Open, threaded from HostConfig through
// Runtime::OpenProject (HostBoot::OpenOptionsFor derives one from a host's
// parsed command line; ProjectBoot.hpp).
//
// WHY THIS EXISTS: the editor's verify capture included the Assets panel,
// which enumerates <root>/Saved/Diagnostics -- a directory the editor itself
// writes crash and hang captures into. The golden image therefore moved with
// the machine's failure history (measured: 24 anti-aliased pixels of the
// Assets scrollbar thumb, byte-identical coordinates on both backends).
//
// Fixed HERE and not in the editor because the mount is created in the engine
// (Project.cpp's diag:// branch); a symptom fixed one layer up comes back.
//
// Intended to absorb future per-open engine settings -- this is a struct on
// purpose, not a bool wearing a struct's clothes.
//
// BEFORE ADDING THE FIRST NON-TRIVIALLY-COPYABLE FIELD, REVISIT THE BY-VALUE
// SIGNATURES. This struct crosses the ARCANE_API DLL boundary BY VALUE --
// Project::Open and Runtime::OpenProject both take `ProjectOpenOptions opts`
// (Project.hpp, Runtime.hpp), and both are exported. That is free today and
// deliberately so: one bool, trivially copyable, no allocation, so the copy is
// a register move and the layout is stable across the boundary. It stops being
// free the moment a field owns memory (a std::string, a std::vector, an
// std::function). Passing such a type by value across a DLL edge means the
// caller's allocator constructs it and the callee's destroys it -- the
// mismatched-CRT hazard this engine's host/plugin split already has to
// respect. And any field addition changes the struct's SIZE, which is an ABI
// break for every already-compiled plugin that calls either function.
//
// So the next field is the decision point, not a later cleanup: either keep
// the struct trivially copyable, or change both signatures to
// `const ProjectOpenOptions&` and bump the plugin ABI in the same commit. The
// note lives HERE, where that field will actually be added, rather than in a
// plan nobody re-reads.

namespace Arcane
{
    struct ProjectOpenOptions
    {
        // Register diag:// -> <root>/Saved/Diagnostics when that directory
        // exists. Default true: every ordinary open wants it, and every one of
        // the 41 existing Project::Open call sites keeps its old behaviour
        // without being touched.
        //
        // Gates BOTH halves of Project.cpp's diag:// branch -- the
        // MountTable::Mount AND the AssetRegistry::AddContent beside it. The
        // AddContent half is the one that actually feeds the Assets panel, so
        // gating only the mount would leave the defect exactly where it was.
        bool mountDiagnostics = true;
    };
}
