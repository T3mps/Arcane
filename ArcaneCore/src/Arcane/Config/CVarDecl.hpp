#pragma once

// Static cvar declaration (settings spec 2026-10-03 s4.3). Use ARC_CVAR in a
// .cpp at namespace scope (an unnamed namespace is fine; a block scope is
// not). It defines a `const CVarRef<T>` named `ident` that code reads with
// ident.Get() -- no string lookup. The value lives in the registry, so
// unloading the declaring module does not free it. A header names the
// handle for other translation units with ARC_CVAR_EXTERN.
//
//   ARC_CVAR(cvar_undoMaxSteps, "editor.undo.maxSteps", std::int32_t, 100,
//            .min = 1, .max = 10000, .audience = Audience::Editor,
//            .scope = SettingScope::PreferencesProject, .help = "Undo history depth in steps.");
//
// Options are CVarSpec<T>'s fields by designated initializer, in its order
// (.min .max .flags .audience .scope .apply .help .displayName .keywords
// .widget .categoryPath .order). .help is required unless .flags has Hidden.
// Dev-flagged cvars still declare here; Dist's process registry refuses them
// and the handle reads the declared default.

#include <Arcane/Config/CVarRef.hpp>

#define ARC_CVAR_CAT2(a, b) a##b
#define ARC_CVAR_CAT(a, b) ARC_CVAR_CAT2(a, b)

#define ARC_CVAR(ident, nameLit, T, defaultExpr, ...)                                          \
    extern const ::Arcane::CVarRef<T> ident =                                                  \
        ::Arcane::Detail::RegisterCVar<T>(nameLit, defaultExpr, ::Arcane::CVarSpec<T>{ __VA_ARGS__ })

#define ARC_CVAR_EXTERN(ident, T) extern const ::Arcane::CVarRef<T> ident

// A rename (settings spec s4.7): the old name keeps working in config files,
// --set and the console, with a one-time warning; the next archive write
// saves the new name. Namespace scope, in a .cpp.
#define ARC_CVAR_ALIAS(oldLit, newLit)                                                         \
    static const bool ARC_CVAR_CAT(arcCVarAlias_, __LINE__) =                                  \
        ::Arcane::Detail::RegisterDeclaredAlias(oldLit, newLit)

#define ARC_COMMAND(nameLit, flagExpr, helpLit, fn)                                        \
    static const bool ARC_CVAR_CAT(arcCmd_, __LINE__) =                                    \
        ::Arcane::CVarRegistry::Get().RegisterCommand(nameLit, flagExpr, helpLit, "engine", fn, nullptr)
