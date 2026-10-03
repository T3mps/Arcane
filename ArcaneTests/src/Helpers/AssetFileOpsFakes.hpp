#pragma once

// Shared fixtures for the asset file-op units (spec 2026-09-30 s7.3/s7.4): a real
// Content/ tree in %TEMP% with a real AssetRegistry over it, and a call-recording
// AssetFileOpHost that drives that registry as the editor drives the Runtime's.

#include "Project/AssetFileOps.hpp"

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Project/AssetRegistry.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Arcane::Test
{
    // File steps never resolve the scene registry; a stack built on this throws if one does.
    inline Astra::Registry& NoSceneRegistry() { throw std::logic_error("asset file steps never touch the scene registry"); }

    inline std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    struct AssetOpsWorld
    {
        std::filesystem::path root, content;
        Arcane::AssetRegistry registry;
        std::vector<std::pair<Arcane::Guid, std::string>> all;   // Facts().registry's storage

        explicit AssetOpsWorld(const char* leaf)
            : root(std::filesystem::temp_directory_path() / "arcane_asset_fileops_test" / leaf), content(root / "Content")
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(content);
        }
        ~AssetOpsWorld() { std::error_code ec; std::filesystem::remove_all(root, ec); }

        void WriteRaw(const std::string& rel, const std::string& bytes)
        {
            const auto file = content / rel;
            std::filesystem::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
        }
        Arcane::Guid Write(const std::string& rel, const std::string& bytes)   // + register (.meta minted)
        {
            WriteRaw(rel, bytes);
            const auto id = registry.AddFile(content / rel, content, "game");
            if (!id) throw std::logic_error("not a trackable asset: " + rel);
            return *id;
        }
        std::map<std::string, std::string> Snapshot() const   // every file under Content/
        {
            std::map<std::string, std::string> out;
            for (const auto& e : std::filesystem::recursive_directory_iterator(content))
                if (e.is_regular_file())
                    out[e.path().lexically_relative(content).generic_string()] = Slurp(e.path());
            return out;
        }
        Arcane::Editor::AssetOpFacts Facts()
        {
            all = registry.All();
            Arcane::Editor::AssetOpFacts f;
            f.contentDir = content;
            f.diagDir = root / "Saved" / "Diagnostics";
            f.registry = all;
            f.exists = [](const std::filesystem::path& p) { std::error_code ec; return std::filesystem::exists(p, ec); };
            f.peekId = [](const std::filesystem::path& p) { return Arcane::AssetRegistry::PeekId(p); };
            f.gltfUris = [](const std::filesystem::path& p) { return Arcane::Editor::ReadGltfUris(p); };
            f.diagSiblings = [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{}; };
            return f;
        }
        Arcane::Editor::AssetOpPlan Plan(Arcane::Editor::AssetOpKind kind, std::vector<Arcane::Guid> guids,
                                         std::string stem = {}, std::string dest = {})
        {
            Arcane::Editor::AssetOpRequest r;
            r.kind = kind; r.guids = std::move(guids); r.newStem = std::move(stem); r.destFolder = std::move(dest);
            return Arcane::Editor::PlanAssetOp(r, Facts());
        }
    };

    struct FakeAssetOpHost final : Arcane::Editor::AssetFileOpHost
    {
        AssetOpsWorld& world;
        Arcane::Editor::AssetOpGates gates{ true, true };
        std::vector<std::string> calls;
        std::vector<std::pair<std::string, std::string>> errors;   // (title, message)
        std::set<Arcane::Guid> dirtyDocs;                           // CloseDocumentFor(g, false) refuses these

        explicit FakeAssetOpHost(AssetOpsWorld& w) : world(w) {}
        std::string Rel(const std::filesystem::path& p) const { return p.lexically_relative(world.content).generic_string(); }

        Arcane::Editor::AssetOpGates Gates() const override { return gates; }
        Arcane::RebindResult Rebind(const Arcane::Guid& g, const std::filesystem::path& p) override
        { calls.push_back("Rebind " + Rel(p)); return world.registry.Rebind(g, p, world.content, "game"); }
        bool Unregister(const Arcane::Guid& g) override { calls.push_back("Unregister"); return world.registry.Remove(g); }
        std::optional<Arcane::Guid> Register(const std::filesystem::path& p) override
        { calls.push_back("Register " + Rel(p)); return world.registry.AddFile(p, world.content, "game"); }
        std::map<Arcane::Guid, int> closedDocs;                     // a closed document -> recycleCalls when it closed
        bool CloseDocumentFor(const Arcane::Guid& g, bool discardDirty) override
        {
            calls.push_back(discardDirty ? "Close discard" : "Close");
            if (!discardDirty && dirtyDocs.count(g)) return false;
            closedDocs.emplace(g, recycleCalls);
            return true;
        }
        void NoteMoved(const Arcane::Guid&, const std::filesystem::path&, const std::filesystem::path& to) override
        { calls.push_back("NoteMoved " + Rel(to)); }
        void AssetsChanged(std::span<const Arcane::Guid> removed, std::span<const Arcane::Guid> added) override
        { calls.push_back("AssetsChanged -" + std::to_string(removed.size()) + " +" + std::to_string(added.size())); }
        void Invalidate(const Arcane::Guid&, Arcane::Editor::AssetKind) override { calls.push_back("Invalidate"); }
        void EvictPaths(std::span<const std::filesystem::path> paths) override
        { calls.push_back("EvictPaths " + std::to_string(paths.size())); }
        void Activity(Arcane::Editor::AssetActivityEntry) override {}
        void ReportError(std::string title, std::string message) override { errors.emplace_back(std::move(title), std::move(message)); }
        std::optional<std::filesystem::path> survivor;   // Recycle leaves this one in place
        bool permanently = false;                        // report every item as nuked
        int recycleCalls = 0;
        Arcane::Editor::OsShell::RecycleResult Recycle(std::span<const std::filesystem::path> files) override
        {
            ++recycleCalls;
            Arcane::Editor::OsShell::RecycleResult r{ true, {}, {}, {} };
            for (const auto& f : files)
            {
                if (survivor && *survivor == f) { r.ok = false; r.notRecycled.push_back(f); continue; }
                std::filesystem::remove(f);
                if (permanently) r.permanentlyDeleted.push_back(f);
            }
            return r;
        }
    };
}
