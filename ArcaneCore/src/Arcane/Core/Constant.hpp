#pragma once

// ARC_CONSTANT("reason") -- a REVIEWED numeric constant (settings spec s10.3
// form (a), s16.8). It expands to nothing. Put it on its own line, with no
// semicolon, directly above a numeric constexpr / static const:
//
//     ARC_CONSTANT("file format: the .arcscene version")
//     inline constexpr int kSceneJsonVersion = 6;
//
// ArcaneTests' ConstantGuardTest fails on an unmarked numeric constant in the
// engine or editor sources: a tunable is a setting (ARC_CVAR or a settings
// struct field); only a true invariant -- a file/wire/shader/ABI contract, a
// hardware or OS limit, a math identity, a crash-path capacity, a test oracle,
// a capacity hint, or a value whose change would be a bug -- is marked, with
// the reason written down.
#define ARC_CONSTANT(reason)
