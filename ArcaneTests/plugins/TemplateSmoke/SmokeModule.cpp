// The TemplateSmoke module: the editor's rendered Component + both System
// headers -- FixedUpdate (SmokeSystem) and Update/Render (SmokeUpdateSystem)
// (ClassTemplatesTest asserts these files ARE the renders) compiled as a real
// game module, so a template that stops compiling fails the build.
#include <Arcane/Plugin/GameModule.hpp>

namespace TemplateSmoke
{
    struct Module final : Arcane::GameModule {};
}

ARC_GAME_MODULE(TemplateSmoke::Module)
