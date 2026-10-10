// NOT part of any project (outside ArcaneTests/src, which premake globs).
// scripts/settings-compile-fail.ps1 compiles it twice:
//   - with /DARC_COMPILE_FAIL_CONTROL it MUST compile (the include paths and the
//     facade are sound, so the real case's failure means something);
//   - without it, it MUST fail on ReflectField's static_assert, naming badField.
#include <Arcane/Reflection.hpp>

#include <cstdint>
#include <vector>

namespace CompileFail
{
    struct BadSettings
    {
#if defined(ARC_COMPILE_FAIL_CONTROL)
        std::int32_t badField = 1;
#else
        std::vector<int> badField;
#endif
    };

    ARC_REFLECT_TYPE(BadSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "compileFail", ::Arcane::SettingScope::Project,
                                 ::Arcane::ApplyMode::Live, ::Arcane::Audience::Game)
        ARC_REFLECT_FIELD(BadSettings, badField)
            ARC_REFLECT_ATTR(Tooltip, "A field the settings map refuses.")
    ARC_END_REFLECT_TYPE()
}
