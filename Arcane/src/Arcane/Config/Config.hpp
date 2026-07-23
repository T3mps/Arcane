#pragma once

// Arcane/Config: layered JSON configuration. Up to three layers, later wins,
// deep-merged per key: engine defaults (shipped, read-only) -> project Config/
// (committed) -> user overrides (local, gitignored). Per-category by file stem
// (engine/game/input/...). Owned by Runtime: engine defaults load at Runtime init
// (so a host with no project still has config -- e.g. input bindings); OpenProject
// re-layers the project + user files on top. Supersedes the Slice-1b input_actions
// bridge: input_actions.json becomes the "input" category.

#include <Arcane/Base/Api.hpp>

#include <Json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unordered_map<...> member on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API Config
    {
    public:
        Config() = default;

        // Load ONLY the engine-default layer (every *.json in engineConfigDir, keyed by
        // file stem, deep-merged if a stem repeats). Clears any prior state first, so
        // this is the reset-to-base call. A missing/empty dir yields an empty config
        // (valid) -- this is the base a host always has, even with no project open.
        void LoadEngineDefaults(const std::filesystem::path& engineConfigDir);

        // Deep-merge the project layer then the user layer on top of the current
        // (engine-default) categories. Either dir may be absent (skipped). Runtime
        // rebuilds from defaults (LoadEngineDefaults) before each call so re-opening a
        // project never accumulates a previous project's layers.
        void LayerProject(const std::filesystem::path& projectConfigDir,
                          const std::filesystem::path& userConfigDir);

        // The merged document for a category ("input" -> merged input.json). Returns a
        // shared empty object (never null) when the category is absent.
        const nlohmann::json& Category(std::string_view name) const;
        bool HasCategory(std::string_view name) const;

    private:
        // RFC-7386-style deep merge: objects merge by key with src winning; every other
        // type (scalar/array/type-change) replaces dst wholesale.
        static void DeepMerge(nlohmann::json& dst, const nlohmann::json& src);
        // Load every *.json in `dir` (non-recursive), deep-merging each into
        // m_categories[stem]. No-op if `dir` is not a directory.
        void LayerDir(const std::filesystem::path& dir);

        std::unordered_map<std::string, nlohmann::json> m_categories;   // stem -> merged doc
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
