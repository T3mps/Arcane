// ReferenceGame: ReferenceProject's game module -- the minimal end-to-end proof of
// the product flow. The SCENE is data (Content/scenes/main.arcscene, loaded by
// the host through the manifest's bootScene); the ENGINE owns the standard
// systems (Runtime::InstallEngineSystems: physics -> transform propagation in
// fixedUpdate, render submission in render); this module's whole job is to EXIST
// on the ABI -- which ARCANE_GAME_MODULE provides in full: the shared TypeContext
// pin, the Mosaic sink/assert installs, ImGui adoption, this module's
// Astra::ComponentModule with the ARCANE_COMPONENT drain (a component added
// under Source/ -- Assets -> Create -> C++ Class, or one ARCANE_COMPONENT line
// by hand -- is live after a rebuild with no edit here), and the registry
// Save/LoadState round-trip for hot reload. Every hook has a default; this
// module overrides NONE -- the proof that the defaults are the whole common
// case. See Arcane/Plugin/GameModule.hpp; HotReloadPlugin.cpp is the same
// macro with overrides.

#include <Arcane/Plugin/GameModule.hpp>

namespace ReferenceGame
{
    struct Module final : Arcane::GameModule {};
}

ARCANE_GAME_MODULE(ReferenceGame::Module)
