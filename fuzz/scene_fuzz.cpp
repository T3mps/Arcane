// libFuzzer harness: scene asset load -- Scene::ReadSceneFile
// (ArcaneCore/src/Arcane/Serialization/SceneAsset.hpp) followed by
// Scene::ApplySceneDocument -> LoadJson (SceneSerializer.hpp), the reflection
// driven component reader, into a registry with the engine's scene components
// registered (RegisterSceneComponents + Arcane::Physics2D::RegisterComponents).
//
// The input is the .arcscene file. A scene that fails to load must do so by
// returning false / nullopt: a C++ exception escaping either call aborts here
// (libFuzzer reports it as a crash), and ASan/UBSan catch memory errors and UB
// such as an out-of-range float->int conversion in a field reader.

#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>

#include <Astra/Registry/Registry.hpp>

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const std::filesystem::path file = std::filesystem::temp_directory_path() /
        ("arcane-scene-fuzz-" + std::to_string(::getpid()) + ".arcscene");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    std::string error;
    const std::optional<Arcane::Scene::SceneDocument> scene = Arcane::Scene::ReadSceneFile(file, &error);
    if (!scene)
        return 0;

    Astra::Registry reg;
    Arcane::RegisterSceneComponents(reg);
    Arcane::Physics2D::RegisterComponents(reg);
    (void)Arcane::Scene::ApplySceneDocument(*scene, reg);
    return 0;
}
