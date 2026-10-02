#include "Project/AssetFileOps.hpp"

#include "Panels/CreateAssetDialog.hpp"   // ValidateCreateNameSyntax (rules 0-2), ValidateRenameStemSyntax

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Edit/CommandStack.hpp>

#include <Json.hpp>   // the workspace's vendored nlohmann::json header

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace Arcane::Editor
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::string_view kSourceRefusal   = "C++ source: rename or move it in your IDE (a source file's identity is its path).";
        constexpr std::string_view kReadOnlyRefusal = "Plugin and engine content is read-only here.";
        constexpr std::string_view kDiagRefusal     = "Crash reports can only be deleted.";
        constexpr std::string_view kCrossMount      = "Can't move across mounts.";

        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }
        bool IsImportedKind(AssetKind k)
        {
            return k == AssetKind::Texture || k == AssetKind::Audio || k == AssetKind::Font || k == AssetKind::Model;
        }
        fs::path WithMeta(fs::path p) { p += ".meta"; return p; }
        // The ancestors of `dir` that do not exist yet, shallowest first.
        std::vector<fs::path> MissingDirs(const fs::path& dir)
        {
            std::vector<fs::path> missing;
            for (fs::path d = dir; d.has_relative_path(); d = d.parent_path())
            {
                std::error_code ec;
                if (fs::exists(d, ec) || ec) break;   // a folder that cannot be stat'ed is never claimed
                missing.push_back(d);
            }
            std::reverse(missing.begin(), missing.end());
            return missing;
        }
        // Remove folders a move created, deepest first; one that gained anything stays.
        void PruneCreatedDirs(std::span<const fs::path> created)
        {
            for (std::size_t i = created.size(); i-- > 0;)
            {
                std::error_code ec;
                if (fs::is_directory(created[i], ec) && fs::is_empty(created[i], ec) && !ec)
                    fs::remove(created[i], ec);
            }
        }
        // NTFS compares names case-insensitively (as fs::exists does), so the batch's own
        // bookkeeping must too: the same ASCII fold IsCaseOnlyRename uses.
        std::string FoldKey(const fs::path& p) { return Lower(p.lexically_normal().generic_string()); }
        // s7.3: an imported binary's guid lives in its .meta, which always travels with
        // it; without one the step could not keep the id, so it is refused, never planned.
        std::string MetaMissing(const fs::path& file)
        {
            return file.filename().string() + ".meta is missing. Reopen the project to rescan.";
        }
        bool IsInside(const fs::path& p, const fs::path& root)
        {
            const fs::path rel = p.lexically_normal().lexically_relative(root.lexically_normal());
            return !rel.empty() && *rel.begin() != "..";
        }
        // "textures/" for Content/textures; "Content/" at the root.
        std::string FolderLabel(const fs::path& dir, const fs::path& contentDir)
        {
            const fs::path rel = dir.lexically_normal().lexically_relative(contentDir.lexically_normal());
            if (rel.empty() || rel == ".") return "Content/";
            return rel.generic_string() + "/";
        }
        std::string ExistsMessage(const fs::path& p, const fs::path& contentDir)
        {
            return p.filename().string() + " already exists in " + FolderLabel(p.parent_path(), contentDir) + ".";
        }
        std::string PercentDecode(std::string_view s)
        {
            std::string out;
            out.reserve(s.size());
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%' && i + 2 < s.size() &&
                    std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2])))
                {
                    out.push_back(static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16)));
                    i += 2;
                }
                else
                    out.push_back(s[i]);
            }
            return out;
        }

        std::string_view RebindName(RebindResult r)
        {
            switch (r)
            {
                case RebindResult::Ok:             return "ok";
                case RebindResult::UnknownGuid:    return "unknown asset";
                case RebindResult::NotTrackable:   return "not a trackable asset";
                case RebindResult::OutsideContent: return "outside Content/";
                case RebindResult::CrossMount:     return "another mount";
                case RebindResult::IdMismatch:     return "the file holds another id";
                case RebindResult::PathTaken:      return "path owned by another asset";
                case RebindResult::NoProject:      return "no project open";
            }
            return "unknown";
        }

        struct Planner
        {
            const AssetOpRequest& op;
            const AssetOpFacts&   f;
            AssetOpPlan&          plan;
            std::vector<fs::path> claimed;   // destinations taken earlier in this batch

            void Refuse(const Arcane::Guid& g, std::string why) { plan.refusals.push_back({ g, std::move(why) }); }
            bool Taken(const fs::path& p) const
            {
                const std::string key = FoldKey(p);
                return f.exists(p) || std::any_of(claimed.begin(), claimed.end(), [&](const fs::path& c) { return FoldKey(c) == key; });
            }
            // Every destination free on disk and in the batch; true = refused.
            bool Claim(const Arcane::Guid& g, const AssetMove& m, bool caseOnlyFree)
            {
                for (const FileMove& fm : m.files)
                    if (!(caseOnlyFree && IsCaseOnlyRename(fm.from, fm.to)) && Taken(fm.to))
                    {
                        Refuse(g, ExistsMessage(fm.to, f.contentDir));
                        return true;
                    }
                for (const FileMove& fm : m.files) claimed.push_back(fm.to);
                return false;
            }

            // s7.8: .gltf companions.
            std::unordered_set<Arcane::Guid> moving;    // every guid this plan moves (request + dragged images)
            std::vector<AssetMove> extra;               // registered images a .gltf drags along
            std::unordered_map<std::string, Arcane::Guid> byMount;
            std::optional<std::vector<std::pair<fs::path, fs::path>>> shared;   // companion -> non-moving owner .gltf
            std::unordered_set<std::string> companions; // unregistered companions already planned (FoldKey of `from`)

            std::vector<std::string> Uris(const fs::path& gltf) const
            {
                return f.gltfUris ? f.gltfUris(gltf) : std::vector<std::string>{};
            }

            const std::vector<std::pair<fs::path, fs::path>>& Shared()
            {
                if (!shared)
                {
                    shared.emplace();
                    for (const auto& [g, mount] : f.registry)
                        if (!moving.count(g) && mount.rfind("game://", 0) == 0 &&
                            Lower(fs::path(mount).extension().string()) == ".gltf")
                        {
                            const fs::path other = (f.contentDir / fs::path(mount.substr(7))).lexically_normal();
                            for (const std::string& uri : Uris(other))
                                shared->emplace_back((other.parent_path() / fs::path(uri)).lexically_normal(), other);
                        }
                }
                return *shared;
            }

            std::optional<std::string> AddGltfCompanions(const fs::path& gltf, const fs::path& destDir, AssetMove& m)
            {
                for (const std::string& uri : Uris(gltf))
                {
                    const fs::path rel = fs::path(uri).lexically_normal();
                    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory() ||
                        std::any_of(rel.begin(), rel.end(), [](const fs::path& e) { return e == ".."; }))
                        return "References " + uri + " outside its folder.";
                    const fs::path from = (gltf.parent_path() / rel).lexically_normal();
                    const fs::path to = (destDir / rel).lexically_normal();
                    const std::string fromKey = FoldKey(from);
                    for (const auto& [companion, owner] : Shared())
                        if (FoldKey(companion) == fromKey)
                            return "Shares " + from.filename().string() + " with " + owner.filename().string() + ".";
                    const std::string mount = "game://" + from.lexically_relative(f.contentDir.lexically_normal()).generic_string();
                    if (const auto it = byMount.find(mount); it != byMount.end())
                    {
                        const AssetKind kind = AssetKindOf(mount);
                        const bool sidecar = IsImportedKind(kind);   // only imported kinds keep their id in a .meta
                        if (sidecar && !f.exists(WithMeta(from))) return MetaMissing(from);
                        if (moving.insert(it->second).second)
                        {
                            AssetMove e{ it->second, kind, { { from, to } } };
                            if (sidecar) e.files.push_back({ WithMeta(from), WithMeta(to) });
                            extra.push_back(std::move(e));
                        }
                        continue;   // a registered image moves as its OWN asset (rebound by guid)
                    }
                    // Co-moving .gltf files sharing a buffer: the first one carries it; the rest
                    // must not plan (and then Claim) the same file again. Distinct source folders
                    // give distinct `from` keys, so a real destination collision still refuses.
                    if (!companions.insert(fromKey).second) continue;
                    m.files.push_back({ from, to });
                }
                return std::nullopt;
            }
        };

        void PlanNewFolder(Planner& p)
        {
            const fs::path parent = (p.op.destFolder.empty() ? p.f.contentDir
                                                             : p.f.contentDir / fs::path(p.op.destFolder)).lexically_normal();
            if (!IsInside(parent, p.f.contentDir)) { p.Refuse({}, std::string(kCrossMount)); return; }
            if (const CreateNameCheck c = ValidateCreateNameSyntax(p.op.newStem, parent, ""); !c.ok)
            {
                p.Refuse({}, c.message);
                return;
            }
            const fs::path dir = parent / p.op.newStem;
            if (p.f.exists(dir)) { p.Refuse({}, ExistsMessage(dir, p.f.contentDir)); return; }
            p.plan.moves.push_back(AssetMove{ {}, AssetKind::Other, { FileMove{ {}, dir } } });
            p.plan.label = "New Folder " + FolderLabel(dir, p.f.contentDir);
        }

        std::string Label(const AssetOpRequest& op, const AssetOpPlan& plan, const fs::path& destDir,
                          const fs::path& contentDir)
        {
            if (plan.moves.empty()) return {};
            const std::size_t n = plan.moves.size();
            const fs::path& first = plan.moves[0].files[0].from;
            const std::string many = std::to_string(n) + " assets";
            switch (op.kind)
            {
                case AssetOpKind::Rename:
                    return "Rename " + first.stem().string() + " \xE2\x86\x92 " + plan.moves[0].files[0].to.stem().string();
                case AssetOpKind::Move:      return "Move " + (n == 1 ? first.stem().string() : many) + " to " + FolderLabel(destDir, contentDir);
                case AssetOpKind::Duplicate: return "Duplicate " + (n == 1 ? first.stem().string() : many);
                case AssetOpKind::Delete:    return "Delete " + (n == 1 ? first.filename().string() : many);
                case AssetOpKind::NewFolder: return "New Folder " + FolderLabel(plan.moves[0].files[0].to, contentDir);
            }
            return {};
        }
    }

    bool IsCaseOnlyRename(const fs::path& from, const fs::path& to)
    {
        const std::string a = from.lexically_normal().generic_string();
        const std::string b = to.lexically_normal().generic_string();
        return a != b && Lower(a) == Lower(b);
    }

    std::string NextCopyName(std::string_view stem, const fs::path& dir, std::string_view ext,
                             const std::function<bool(const fs::path&)>& taken)
    {
        std::string_view base = stem;
        if (const std::size_t sp = base.rfind(' '); sp != std::string_view::npos && sp + 1 < base.size() &&
            std::all_of(base.begin() + static_cast<std::ptrdiff_t>(sp + 1), base.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; }))
            base = base.substr(0, sp);
        for (int n = 1; n < 100000; ++n)
        {
            const std::string candidate = std::string(base) + " " + std::to_string(n);
            if (!ValidateCreateNameSyntax(candidate, dir, ext).ok)
                return {};   // longer candidates only get longer
            const fs::path file = dir / (candidate + std::string(ext));
            if (!taken(file) && !taken(WithMeta(file)))
                return candidate;
        }
        return {};
    }

    std::vector<std::string> ReadGltfUris(const fs::path& gltf)
    {
        std::vector<std::string> out;
        if (Lower(gltf.extension().string()) != ".gltf")
            return out;   // .glb is self-contained (s7.8)
        std::ifstream in(gltf, std::ios::binary);
        if (!in) return out;
        const auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
        if (!doc.is_object()) return out;
        for (const char* key : { "buffers", "images" })
            if (const auto list = doc.find(key); list != doc.end() && list->is_array())
                for (const auto& e : *list)
                    if (const auto uri = e.find("uri"); uri != e.end() && uri->is_string())
                    {
                        const std::string s = uri->get<std::string>();
                        if (s.rfind("data:", 0) != 0) out.push_back(PercentDecode(s));
                    }
        return out;
    }

    std::optional<std::string> AssetOpGateRefusal(const AssetOpGates& g, bool inTransaction)
    {
        if (!g.projectOpen) return "No project open";
        if (!g.editMode)    return "Stop Play to change asset files";
        if (inTransaction)  return "Finish the current edit first";   // a file step never joins a gesture
        return std::nullopt;
    }

    AssetFileOpExecutor::AssetFileOpExecutor(AssetFileOpHost& host, Arcane::CommandStack& stack, fs::path contentDir)
        : m_host(host), m_stack(stack), m_contentDir(std::move(contentDir)),
          m_rename([](const fs::path& a, const fs::path& b)
                   {
                       if (IsCaseOnlyRename(a, b))   // one file on NTFS: route through MoveFileExW
                           return OsShell::RenameCaseOnly(a, b) ? std::error_code{} : std::make_error_code(std::errc::io_error);
                       std::error_code ec; fs::rename(a, b, ec); return ec;
                   }),
          m_capture([this](const fs::path& p) { return m_stack.MakePayloadFromFile(p); }),
          m_anchor(std::make_shared<AssetFileOpExecutor*>(this))
    {
    }

    AssetFileOpExecutor::~AssetFileOpExecutor() = default;

    std::string AssetFileOpExecutor::Display(const fs::path& p) const
    {
        const fs::path rel = p.lexically_relative(m_contentDir);
        if (!rel.empty() && *rel.begin() != "..") return rel.generic_string();
        return p.filename().generic_string();
    }

    std::optional<std::string> AssetFileOpExecutor::PreflightMove(std::span<const AssetMove> moves, Side side) const
    {
        for (const AssetMove& m : moves)
            for (std::size_t i = 0; i < m.files.size(); ++i)
            {
                const fs::path& src = side == Side::Forward ? m.files[i].from : m.files[i].to;
                const fs::path& dst = side == Side::Forward ? m.files[i].to : m.files[i].from;
                std::error_code ec;
                if (i == 0 ? Arcane::AssetRegistry::PeekId(src) != m.guid : !fs::exists(src, ec))
                    return Display(src) + " is missing or no longer holds this asset.";
                if (fs::exists(dst, ec) && !IsCaseOnlyRename(src, dst))
                    return Display(dst) + " is occupied by another file.";
            }
        return std::nullopt;
    }

    bool AssetFileOpExecutor::MoveSourceLost(std::span<const AssetMove> moves, Side side) const
    {
        for (const AssetMove& m : moves)
            if (Arcane::AssetRegistry::PeekId(side == Side::Forward ? m.files[0].from : m.files[0].to) != m.guid)
                return true;
        return false;
    }

    std::optional<std::string> AssetFileOpExecutor::CreateFolder(const fs::path& dir)
    {
        std::error_code ec;
        if (fs::exists(dir, ec)) return Display(dir) + " already exists.";
        if (!fs::create_directory(dir, ec) || ec)
            return Display(dir) + " could not be created (" + ec.message() + ").";
        m_host.AssetsChanged({}, {});   // the model lists empty folders (s7.8)
        return std::nullopt;
    }

    std::optional<std::string> AssetFileOpExecutor::RemoveEmptyFolder(const fs::path& dir)
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return Display(dir) + " no longer exists.";
        if (!fs::is_empty(dir, ec)) return Display(dir) + " is not empty.";
        if (!fs::remove(dir, ec) || ec) return Display(dir) + " could not be removed (" + ec.message() + ").";
        m_host.AssetsChanged({}, {});
        return std::nullopt;
    }

    bool AssetFileOpExecutor::FolderLost(const fs::path& dir) const
    {
        std::error_code ec;
        return !fs::is_directory(dir, ec) || !fs::is_empty(dir, ec);
    }

    std::optional<std::string> AssetFileOpExecutor::RollBack(std::span<const FileMove> done)
    {
        for (std::size_t i = done.size(); i-- > 0;)
            if (m_rename(done[i].to, done[i].from))
                return Display(done[i].from) + " could not be moved back from " + Display(done[i].to) + "; it is safe there.";
        return std::nullopt;   // a failed step back STOPS here: nothing is deleted to "clean up"
    }

    std::optional<std::string> AssetFileOpExecutor::ApplyMove(std::span<const AssetMove> moves, Side side,
                                                              std::vector<fs::path>* dirs)
    {
        if (auto refusal = PreflightMove(moves, side))
            return refusal;
        const auto src = [side](const FileMove& f) -> const fs::path& { return side == Side::Forward ? f.from : f.to; };
        const auto dst = [side](const FileMove& f) -> const fs::path& { return side == Side::Forward ? f.to : f.from; };

        std::vector<FileMove> steps;
        for (const AssetMove& m : moves)
            for (const FileMove& f : m.files) steps.push_back({ src(f), dst(f) });
        std::vector<fs::path> created;   // the folders this apply made, shallow before deep
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            std::error_code ec;
            const std::vector<fs::path> missing = MissingDirs(steps[i].to.parent_path());
            fs::create_directories(steps[i].to.parent_path(), ec);
            for (const fs::path& d : missing)   // a partial failure still claims what it made
                if (std::error_code e; fs::is_directory(d, e)) created.push_back(d);
            if (!ec) ec = m_rename(steps[i].from, steps[i].to);
            if (ec)
            {
                std::string error = Display(steps[i].from) + " could not be moved to " + Display(steps[i].to) +
                                    " (" + ec.message() + ").";
                if (auto stuck = RollBack(std::span(steps).first(i))) error += " " + *stuck;
                PruneCreatedDirs(created);
                return error;
            }
        }
        for (std::size_t i = 0; i < moves.size(); ++i)
        {
            const RebindResult r = m_host.Rebind(moves[i].guid, dst(moves[i].files[0]));
            if (r == RebindResult::Ok) continue;
            std::string error = "The asset registry refused " + Display(dst(moves[i].files[0])) + " (" +
                                std::string(RebindName(r)) + ").";
            if (auto stuck = RollBack(steps))   // files first: Rebind reads the id ON DISK
            {
                PruneCreatedDirs(created);
                return error + " " + *stuck;
            }
            for (std::size_t j = 0; j < i; ++j)
                (void)m_host.Rebind(moves[j].guid, src(moves[j].files[0]));
            PruneCreatedDirs(created);
            return error;
        }
        if (dirs)
        {
            PruneCreatedDirs(*dirs);   // the other side's folders: their files just left
            *dirs = std::move(created);
        }
        std::vector<fs::path> touched;
        for (const AssetMove& m : moves)
        {
            m_host.NoteMoved(m.guid, src(m.files[0]), dst(m.files[0]));
            for (const FileMove& f : m.files) { touched.push_back(f.from); touched.push_back(f.to); }
        }
        m_host.EvictPaths(touched);   // both ends: a reused path must never serve old bytes
        m_host.AssetsChanged({}, {});
        return std::nullopt;
    }

    bool AssetFileOpExecutor::FilesLost(std::span<const AssetFiles> assets) const
    {
        for (const AssetFiles& a : assets)
            if (Arcane::AssetRegistry::PeekId(a.files[0]) != a.guid) return true;
        return false;
    }

    bool AssetFileOpExecutor::PayloadsLost(std::span<const AssetPayloads> payloads) const
    {
        for (const AssetPayloads& a : payloads)
            for (const FilePayload& f : a.files)
            {
                std::error_code ec;
                if (f.bytes.Spilled() && !fs::exists(f.bytes.SpillPath(), ec)) return true;   // one stat, never a read
            }
        return false;   // in-memory payloads cannot vanish
    }

    std::optional<std::string> AssetFileOpExecutor::WritePayload(const FilePayload& p, std::vector<fs::path>* created) const
    {
        const std::optional<std::vector<std::byte>> bytes = p.bytes.Load();
        if (!bytes) return "The undo copy of " + Display(p.path) + " could not be read.";
        std::error_code ec;
        const std::vector<fs::path> missing = MissingDirs(p.path.parent_path());
        fs::create_directories(p.path.parent_path(), ec);
        if (created)
            for (const fs::path& d : missing)   // a partial failure still claims what it made
                if (std::error_code e; fs::is_directory(d, e)) created->push_back(d);
        {
            std::ofstream out(p.path, std::ios::binary | std::ios::trunc);
            if (out) out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
            if (!out) return Display(p.path) + " could not be written.";
        }
        fs::last_write_time(p.path, p.mtime, ec);   // the watcher sees no change (s7.4)
        if (ec) return Display(p.path) + " could not keep its modified time (" + ec.message() + ").";
        return std::nullopt;
    }

    std::optional<std::string> AssetFileOpExecutor::RemoveAssets(std::span<const AssetFiles> doomed,
                                                                 std::vector<AssetPayloads>& out, bool discardDirty)
    {
        out.clear();
        if (FilesLost(doomed))
            return "An asset is missing or no longer holds its id.";
        for (const AssetFiles& a : doomed)   // documents close FIRST (s7.5)
            if (!m_host.CloseDocumentFor(a.guid, discardDirty))
                return "Close or save " + Display(a.files[0]) + " first.";

        std::vector<fs::path> all;
        for (const AssetFiles& a : doomed)   // (1) capture everything before anything is recycled
        {
            AssetPayloads& p = out.emplace_back(AssetPayloads{ a.guid, {} });
            for (const fs::path& file : a.files)
            {
                std::error_code ec;
                const std::uint64_t size = fs::file_size(file, ec);
                const fs::file_time_type mtime = ec ? fs::file_time_type{} : fs::last_write_time(file, ec);
                std::optional<Arcane::UndoPayload> bytes = ec ? std::nullopt : m_capture(file);
                if (!bytes)
                {
                    out.clear();
                    return "Could not keep a copy of " + Display(file) + " for undo; nothing was deleted.";
                }
                p.files.push_back(FilePayload{ file, size, mtime, std::move(*bytes) });
                all.push_back(file);
            }
        }
        m_lastRecycle = m_host.Recycle(all);   // (2) ONE call
        bool survived = false;                 // (3) verify
        for (const fs::path& f : all) { std::error_code ec; survived = survived || fs::exists(f, ec); }
        if (survived)
        {
            std::string error = m_lastRecycle.message.empty() ? "Some files could not be moved to the Recycle Bin."
                                                              : m_lastRecycle.message;
            for (const AssetPayloads& a : out)
                for (const FilePayload& f : a.files)
                {
                    std::error_code ec;
                    if (!fs::exists(f.path, ec))
                        if (auto e = WritePayload(f)) error += " " + *e;
                }
            out.clear();
            return error;
        }
        std::vector<Arcane::Guid> removed;     // (4) registry + follow-up
        for (const AssetFiles& a : doomed) { m_host.Unregister(a.guid); removed.push_back(a.guid); }
        m_host.AssetsChanged(removed, {});
        m_host.EvictPaths(all);
        return std::nullopt;
    }

    std::optional<std::string> AssetFileOpExecutor::RestoreAssets(std::span<const AssetPayloads> payloads)
    {
        for (const AssetPayloads& a : payloads)   // (1) occupancy
            for (const FilePayload& f : a.files)
            {
                std::error_code ec;
                if (fs::exists(f.path, ec))
                    return Display(f.path) + " is occupied by another file. Your deleted file is in the Recycle Bin.";
            }
        std::vector<const FilePayload*> order;    // (2) .meta, then primaries, then companions
        const auto isMeta = [](const FilePayload& f) { return Lower(f.path.extension().string()) == ".meta"; };
        for (const AssetPayloads& a : payloads) for (const FilePayload& f : a.files) if (isMeta(f)) order.push_back(&f);
        for (const AssetPayloads& a : payloads) order.push_back(&a.files[0]);
        for (const AssetPayloads& a : payloads)
            for (std::size_t i = 1; i < a.files.size(); ++i) if (!isMeta(a.files[i])) order.push_back(&a.files[i]);

        std::vector<fs::path> written, created;   // created: the folders the writes made (T5-A10's rule)
        const auto undoWrites = [&]
        {
            for (const fs::path& p : written) { std::error_code ec; fs::remove(p, ec); }
            PruneCreatedDirs(created);
        };
        for (const FilePayload* f : order)
        {
            const std::optional<std::string> e = WritePayload(*f, &created);
            written.push_back(f->path);   // even on failure: (1) proved the path was free, so a partial file is ours
            if (e) { undoWrites(); return e; }
        }
        std::vector<Arcane::Guid> restored;       // (3) the recorded guid must come back
        for (const AssetPayloads& a : payloads)
        {
            if (m_host.Register(a.files[0].path) != a.guid)
            {
                for (const Arcane::Guid& g : restored) m_host.Unregister(g);
                m_host.Unregister(a.guid);
                undoWrites();
                return Display(a.files[0].path) + " came back with a different id.";
            }
            restored.push_back(a.guid);
        }
        m_host.AssetsChanged({}, restored);       // (4) the host's AssetsChanged runs ForgetUnresolved for `added`
        m_host.EvictPaths(written);
        return std::nullopt;
    }

    ExecResult AssetFileOpExecutor::Execute(const AssetOpPlan& plan, Arcane::CommandStack& stack)
    {
        ARC_ASSERT(&stack == &m_stack, "AssetFileOpExecutor::Execute: push to the stack the executor was built on");
        if (!plan.refusals.empty())
            return { false, plan.refusals.front().reason };
        if (auto why = AssetOpGateRefusal(m_host.Gates(), stack.InTransaction()))
            return { false, *why };
        if (plan.moves.empty())
            return { true, {} };   // e.g. a drop onto the asset's own folder: nothing pushed

        std::optional<std::string> failure;
        std::unique_ptr<Arcane::ICommand> step;
        switch (plan.kind)
        {
            case AssetOpKind::Rename:
            case AssetOpKind::Move:
            {
                std::vector<fs::path> dirs;
                failure = ApplyMove(plan.moves, Side::Forward, &dirs);
                if (!failure) step = std::make_unique<AssetMoveCommand>(Anchor(), plan.label, plan.moves, std::move(dirs));
                break;
            }
            case AssetOpKind::NewFolder:
                failure = CreateFolder(plan.moves[0].files[0].to);
                if (!failure) step = std::make_unique<NewFolderCommand>(Anchor(), plan.label, plan.moves[0].files[0].to);
                break;
            case AssetOpKind::Delete:
            {
                std::vector<AssetFiles> doomed;
                for (const AssetMove& m : plan.moves)
                {
                    AssetFiles& a = doomed.emplace_back(AssetFiles{ m.guid, {} });
                    for (const FileMove& f : m.files) a.files.push_back(f.from);
                }
                std::vector<AssetPayloads> payloads;
                failure = RemoveAssets(doomed, payloads, /*discardDirty*/ true);   // the s7.5 modal confirmed
                if (!failure)
                    step = std::make_unique<AssetDeleteCommand>(Anchor(), plan.label, std::move(doomed), std::move(payloads));
                break;
            }
            default:   // Duplicate (s7.7) replaces this arm
                failure = "This asset operation is not available yet.";
                break;
        }
        if (failure)
        {
            m_host.ReportError(plan.label + " failed", *failure);   // reported once
            return { false, *failure };
        }
        stack.Push(std::move(step));
        return { true, {} };
    }

    void AssetFileCommand::Step(bool undo)
    {
        AssetFileOpExecutor* exec = Exec();
        if (m_blocked || !exec || m_applied != undo)
            return;   // inert: executor gone, blocked, or the wrong side
        if (auto failure = Run(*exec, undo))
        {
            m_blocked = true;   // reads expired from now on, so the other side is skipped too
            exec->ReportRefusal((undo ? "Can't undo " : "Can't redo ") + m_label, *failure);
            return;
        }
        m_applied = !undo;
    }

    AssetOpPlan PlanAssetOp(const AssetOpRequest& op, const AssetOpFacts& f)
    {
        AssetOpPlan plan;
        plan.kind = op.kind;
        Planner p{ op, f, plan, {} };
        if (op.kind == AssetOpKind::NewFolder) { PlanNewFolder(p); return plan; }
        if (op.kind == AssetOpKind::Rename && op.guids.size() != 1)
        {
            p.Refuse({}, "Select one asset to rename");
            return plan;
        }

        std::unordered_map<Arcane::Guid, std::string> mountOf;
        for (const auto& [g, mount] : f.registry) mountOf.emplace(g, mount);
        const fs::path destDir = (op.destFolder.empty() ? f.contentDir : f.contentDir / fs::path(op.destFolder)).lexically_normal();
        std::unordered_set<Arcane::Guid> done;
        for (const auto& [g, mount] : f.registry) p.byMount.emplace(mount, g);
        p.moving.insert(op.guids.begin(), op.guids.end());

        for (const Arcane::Guid& g : op.guids)
        {
            p.extra.clear();   // a refused .gltf never leaks its dragged images into the next asset's moves
            if (!done.insert(g).second) continue;
            const auto it = mountOf.find(g);
            if (it == mountOf.end()) { p.Refuse(g, "No longer exists."); continue; }
            const std::string& mount = it->second;
            const std::size_t sep = mount.find("://");
            const std::string scheme = sep == std::string::npos ? std::string{} : mount.substr(0, sep);
            fs::path base;
            if (scheme == "source") { p.Refuse(g, std::string(kSourceRefusal)); continue; }
            if (scheme == "game") base = f.contentDir;
            else if (scheme == "diag")
            {
                if (op.kind != AssetOpKind::Delete) { p.Refuse(g, std::string(kDiagRefusal)); continue; }
                base = f.diagDir;
            }
            else { p.Refuse(g, std::string(kReadOnlyRefusal)); continue; }   // plugin/<name>://, engine://

            const fs::path file = (base / fs::path(mount.substr(sep + 3))).lexically_normal();
            if (!f.exists(file)) { p.Refuse(g, "Missing on disk. Reopen the project to rescan."); continue; }
            const AssetKind kind = AssetKindOf(mount);
            const bool imported = IsImportedKind(kind);
            if (imported && !f.exists(WithMeta(file))) { p.Refuse(g, MetaMissing(file)); continue; }
            if (op.kind == AssetOpKind::Delete && g == f.openScene)
            { p.Refuse(g, "This scene is open. Open another scene first."); continue; }
            if (op.kind == AssetOpKind::Delete && g == f.bootScene)
            { p.Refuse(g, "This is the project's boot scene. Set another boot scene first."); continue; }

            const std::string ext = file.extension().string();
            AssetMove m{ g, kind, {} };
            const auto add = [&](const fs::path& to)
            {
                m.files.push_back({ file, to });
                if (imported) m.files.push_back({ WithMeta(file), to.empty() ? fs::path{} : WithMeta(to) });
            };

            switch (op.kind)
            {
                case AssetOpKind::Delete:
                    add({});
                    if (kind == AssetKind::Diagnostic && f.diagSiblings)
                        for (const fs::path& s : f.diagSiblings(file)) m.files.push_back({ s, {} });
                    break;
                case AssetOpKind::Rename:
                    if (op.newStem == file.stem().string()) return plan;   // byte-identical: a no-op, not a step
                    // Rules 0-2, then s7.6's fixed extension ("wall.png" for a .png is refused).
                    if (const CreateNameCheck c = ValidateRenameStemSyntax(op.newStem, file.parent_path(), ext); !c.ok)
                    { p.Refuse(g, c.message); continue; }
                    add(file.parent_path() / (op.newStem + ext));
                    if (p.Claim(g, m, /*caseOnlyFree*/ true)) continue;
                    break;
                case AssetOpKind::Move:
                    if (scheme != "game" || !IsInside(destDir, f.contentDir)) { p.Refuse(g, std::string(kCrossMount)); continue; }
                    if (file.parent_path() == destDir) continue;   // already there: nothing to do
                    add(destDir / file.filename());
                    if (Lower(ext) == ".gltf")
                        if (auto why = p.AddGltfCompanions(file, destDir, m)) { p.Refuse(g, *why); continue; }
                    if (p.Claim(g, m, false)) continue;
                    break;
                case AssetOpKind::Duplicate:
                {
                    const fs::path dir = file.parent_path();
                    const std::string name = NextCopyName(file.stem().string(), dir, ext,
                                                          [&](const fs::path& q) { return p.Taken(q); });
                    if (name.empty()) { p.Refuse(g, "No free copy name for " + file.stem().string() + "."); continue; }
                    add(dir / (name + ext));
                    p.Claim(g, m, false);
                    plan.newGuids.push_back(Arcane::Guid::Generate());
                    break;
                }
                case AssetOpKind::NewFolder:
                    break;   // planned above
            }
            plan.moves.push_back(std::move(m));
            for (AssetMove& e : p.extra)
                if (!p.Claim(e.guid, e, false)) plan.moves.push_back(std::move(e));
        }

        for (const AssetMove& m : plan.moves)
            for (const AssetOpFacts::Doc& d : f.docs)
                if (d.guid == m.guid)
                {
                    plan.openDocs.push_back(d.guid);
                    if (d.dirty) plan.dirtyDocs.push_back(d.guid);
                }
        plan.label = Label(op, plan, destDir, f.contentDir);
        return plan;
    }
}
