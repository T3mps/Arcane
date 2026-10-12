// ArcaneRuntime -- the thin standalone host. Its whole body is the
// ArcaneRuntimeHost library (Arcane/RuntimeHost/RuntimeHost.hpp), which a
// packaged game's generated main links too; this exe is that host with the
// game module loaded as a DLL and the verification vehicle linked.

#include <Arcane/Render/AgilitySdk.hpp>
#include <Arcane/RuntimeHost/RuntimeHost.hpp>

// Agility SDK handshake: the exported version/path pair (Arcane/Render/AgilitySdk.hpp).
// Must live in the exe itself; the macro is empty off Windows.
ARC_AGILITY_SDK_EXPORTS();

int main(int argc, char** argv)
{
    return Arcane::RuntimeHost::Run(argc, argv, nullptr);
}
