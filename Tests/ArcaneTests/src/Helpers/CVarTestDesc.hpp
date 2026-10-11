#pragma once

// A CVarDesc with the settings-arc metadata a cvar test cares about. Help is
// never empty: Register refuses empty help on a non-Hidden cvar (S1a).

#include <Arcane/Config/CVarRegistry.hpp>

#include <string_view>
#include <utility>

namespace Arcane::Test
{
    inline CVarDesc Desc(std::string_view name, CVarValue def, Audience audience = Audience::Game,
                         CVarFlags flags = CVarFlags::None, std::string_view module = "test")
    {
        CVarDesc d;
        d.name = name;
        d.type = def.type;
        d.defaultValue = std::move(def);
        d.flags = flags;
        d.help = "Test cvar.";
        d.module = module;
        d.audience = audience;
        return d;
    }
}
