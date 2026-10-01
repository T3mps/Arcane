#pragma once

// Static registration. The handle lives in the registry, not in the declaring
// translation unit, so unloading the module that included this header does not
// free the value. Dev-flagged cvars still register here; Dist's process
// registry refuses them (CVarRegistry::Get).

#include <Arcane/Config/CVarRegistry.hpp>

#define ARC_CVAR_CAT2(a, b) a##b
#define ARC_CVAR_CAT(a, b) ARC_CVAR_CAT2(a, b)

#define ARC_CVAR(nameLit, typeEnum, defaultExpr, flagExpr, helpLit)                         \
    static const ::Arcane::CVarHandle ARC_CVAR_CAT(arcCVar_, __LINE__) = [] {              \
        ::Arcane::CVarDesc desc;                                                           \
        desc.name = nameLit;                                                              \
        desc.type = ::Arcane::CVarType::typeEnum;                                         \
        desc.defaultValue = defaultExpr;                                                  \
        desc.flags = flagExpr;                                                            \
        desc.help = helpLit;                                                              \
        desc.module = "engine";                                                           \
        return ::Arcane::CVarRegistry::Get().Register(desc);                              \
    }()

// A numeric tunable with its range and its declaring module (spec 2026-09-30
// s2.4): ARC_CVAR hard-codes "engine" and sets no range. min/max clamp both
// the default and every Set (CVarRegistry.cpp:232, :286).
#define ARC_CVAR_RANGED(nameLit, moduleLit, typeEnum, defaultExpr, minExpr, maxExpr, flagExpr, helpLit) \
    static const ::Arcane::CVarHandle ARC_CVAR_CAT(arcCVar_, __LINE__) = [] {              \
        ::Arcane::CVarDesc desc;                                                           \
        desc.name = nameLit;                                                              \
        desc.type = ::Arcane::CVarType::typeEnum;                                         \
        desc.defaultValue = defaultExpr;                                                  \
        desc.min = minExpr;                                                               \
        desc.max = maxExpr;                                                               \
        desc.flags = flagExpr;                                                            \
        desc.help = helpLit;                                                              \
        desc.module = moduleLit;                                                          \
        return ::Arcane::CVarRegistry::Get().Register(desc);                              \
    }()

#define ARC_COMMAND(nameLit, flagExpr, helpLit, fn)                                        \
    static const bool ARC_CVAR_CAT(arcCmd_, __LINE__) =                                    \
        ::Arcane::CVarRegistry::Get().RegisterCommand(nameLit, flagExpr, helpLit, "engine", fn, nullptr)
