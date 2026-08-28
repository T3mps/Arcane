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
