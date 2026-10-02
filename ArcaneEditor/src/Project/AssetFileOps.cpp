#include "Project/AssetFileOps.hpp"

#include "Panels/CreateAssetDialog.hpp"   // ValidateCreateNameSyntax (rules 0-2), ValidateRenameStemSyntax

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

        struct Planner
        {
            const AssetOpRequest& op;
            const AssetOpFacts&   f;
            AssetOpPlan&          plan;
            std::vector<fs::path> claimed;   // destinations taken earlier in this batch

            void Refuse(const Arcane::Guid& g, std::string why) { plan.refusals.push_back({ g, std::move(why) }); }
            bool Taken(const fs::path& p) const
            {
                return f.exists(p) || std::any_of(claimed.begin(), claimed.end(), [&](const fs::path& c) { return c == p; });
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
                    for (const auto& [companion, owner] : Shared())
                        if (companion == from)
                            return "Shares " + from.filename().string() + " with " + owner.filename().string() + ".";
                    const std::string mount = "game://" + from.lexically_relative(f.contentDir.lexically_normal()).generic_string();
                    if (const auto it = byMount.find(mount); it != byMount.end())
                    {
                        if (moving.insert(it->second).second)
                            extra.push_back(AssetMove{ it->second, AssetKindOf(mount),
                                                       { { from, to }, { WithMeta(from), WithMeta(to) } } });
                        continue;   // a registered image moves as its OWN asset (rebound by guid)
                    }
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
            if (op.kind == AssetOpKind::Delete && g == f.openScene)
            { p.Refuse(g, "This scene is open. Open another scene first."); continue; }
            if (op.kind == AssetOpKind::Delete && g == f.bootScene)
            { p.Refuse(g, "This is the project's boot scene. Set another boot scene first."); continue; }

            const AssetKind kind = AssetKindOf(mount);
            const bool imported = IsImportedKind(kind);
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
