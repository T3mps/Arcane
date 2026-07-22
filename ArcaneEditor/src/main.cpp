// Arcane Editor -- the editor shell entry point. Parses argv into a LoomConfig
// (reused as the host config), constructs the EditorApp object, and returns
// its Run() exit code. All engine boot, the frame loop, and the load-bearing
// teardown order live in EditorApp (EditorApp.hpp/.cpp); main is just the
// wire-up (mirrors Loom/src/main.cpp).

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <LoomConfig.hpp>
#include "EditorApp.hpp"

int main(int argc, char** argv)
{
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
    const LoomConfig::ParseOutcome parsed = LoomConfig::Parse(argc, argv);
    if (!parsed.config) return parsed.exitCode;
    Arcane::Editor::EditorApp app(*parsed.config);
    return app.Run();
}
