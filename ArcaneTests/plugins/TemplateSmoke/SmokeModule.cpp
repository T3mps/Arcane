// The TemplateSmoke module: the editor's rendered Component + System
// (ClassTemplatesTest asserts these files ARE the renders) compiled as a real
// game module, so a template that stops compiling fails the build.
#include <Arcane/Plugin/GameModule.hpp>

namespace TemplateSmoke
{
    struct Module final : Arcane::GameModule {};
}

ARCANE_GAME_MODULE(TemplateSmoke::Module)
