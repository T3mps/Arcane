#include <Arcane/Project/AssetRegistry.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>   // Diag::ReadFile -- .arcdiag's own guid reader (F-7/Task 9)
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>

#include <Json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace Arcane
{
    namespace
    {
        // Lowercased extension of a path ("HERO.PNG" -> ".png"), so extension matching is
        // case-insensitive on case-preserving filesystems (Windows/macOS).
        std::string LowerExt(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        // Imported external originals that cannot embed an id -- their Guid lives in a
        // sibling "<file>.meta" (Unity's model, scoped to imported source assets only).
        bool IsImportedBinary(std::string_view extLower)
        {
            static constexpr std::string_view kBinaryExts[] = {
                ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr",  // images
                ".wav", ".ogg", ".mp3", ".flac",                  // audio
                ".ttf", ".otf",                                   // fonts
            };
            for (std::string_view e : kBinaryExts)
                if (extLower == e)
                    return true;
            return false;
        }

        // Read a canonical Guid out of a parsed object's string field; nullopt if the field
        // is absent, non-string, or not a valid 8-4-4-4-12 guid.
        std::optional<Guid> ReadGuidField(const nlohmann::json& doc, const char* key)
        {
            if (auto it = doc.find(key); it != doc.end() && it->is_string())
                if (auto parsed = Guid::FromString(it->get<std::string>()); parsed && parsed->IsValid())
                    return *parsed;
            return std::nullopt;
        }

        // Native asset: id embedded as a top-level "id". A missing/invalid id is minted and
        // written back into the asset (auto-import), so it is born with a durable identity.
        //
        // `writeFailed`, when non-null, is set true ONLY on the id-persist failure below
        // (never on a read/parse failure) -- the caller (AddFile) turns that one case into
        // the "assets.id.write-failed" diagnostic; the other failure paths already return
        // an invalid Guid, which AddFile treats as "already warned, skip".
        Guid ResolveNativeId(const std::filesystem::path& file, bool* writeFailed = nullptr)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) { ARC_WARN("AssetRegistry: cannot read '{}'", file.generic_string()); return {}; }
            auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
            in.close();
            if (!doc.is_object())
            {
                ARC_WARN("AssetRegistry: '{}' is not a JSON object -- skipped", file.generic_string());
                return {};
            }

            if (auto id = ReadGuidField(doc, "id"))
                return *id;

            Guid id = Guid::Generate();
            doc["id"] = id.ToString();
            std::ofstream out(file, std::ios::binary);
            if (out) out << doc.dump(2) << '\n';
            else
            {
                ARC_WARN("AssetRegistry: cannot write id back to '{}'", file.generic_string());
                if (writeFailed) *writeFailed = true;
            }
            return id;
        }

        // Imported binary original: id lives in a sibling "<file>.meta". The ".meta" is
        // APPENDED to the full filename (Unity convention: hero.png -> hero.png.meta) rather
        // than replacing the extension, so "a.png" and "a.wav" get distinct sidecars. A
        // missing/invalid sidecar is minted and written beside the original (auto-import).
        // Same `writeFailed` contract as ResolveNativeId above: set true only when the
        // sidecar write itself fails, so the caller can raise the same
        // "assets.id.write-failed" code for either identity-persistence route.
        Guid ResolveSidecarId(const std::filesystem::path& file, bool* writeFailed = nullptr)
        {
            std::filesystem::path meta = file;
            meta += ".meta";

            std::error_code ec;
            if (std::filesystem::exists(meta, ec))
            {
                std::ifstream in(meta, std::ios::binary);
                if (in)
                {
                    auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
                    in.close();
                    if (doc.is_object())
                        if (auto id = ReadGuidField(doc, "guid"))
                            return *id;
                }
                ARC_WARN("AssetRegistry: sidecar '{}' has no valid guid -- regenerating",
                         meta.generic_string());
            }

            Guid id = Guid::Generate();
            nlohmann::json doc;
            doc["guid"]    = id.ToString();
            doc["version"] = 1;                 // sidecar format version (import settings land here later)
            std::ofstream out(meta, std::ios::binary);
            if (out) out << doc.dump(2) << '\n';
            else
            {
                ARC_WARN("AssetRegistry: cannot write sidecar '{}'", meta.generic_string());
                if (writeFailed) *writeFailed = true;
            }
            return id;
        }

        // .arcdiag: a crash/hang/gpu-stall report (GPU crash diagnostics arc,
        // Task 9). It is "native" in the sense that it carries an embedded
        // guid rather than a sidecar -- like .arcmat/.arcscene/.arcsprite --
        // but it is deliberately its OWN function, never folded into
        // ResolveNativeId above.
        //
        // CRITICAL cross-task contract: the envelope's guid lives under the
        // top-level key "guid" (DiagEnvelope.hpp's Envelope::guid / Parse
        // contract), NOT "id" -- the key ResolveNativeId reads. Routing
        // .arcdiag through ResolveNativeId would find no "id" field in a
        // real report file, MINT a second, unrelated guid, and WRITE IT BACK
        // into a report nothing should ever mutate after Diagnostics wrote
        // it -- silently minting the wrong identity rather than failing
        // loudly. Diag::ReadFile is the only parser that knows the
        // envelope's real field name and versioning rules (kFormatVersion),
        // so this reads through it and NEVER mints or writes back: a
        // report's guid is fixed the instant WriteReportImpl minted it
        // (Diagnostics.cpp), not at scan time, and Diag::Parse already
        // refuses a missing/unparsable/nil guid on its own.
        Guid ResolveDiagId(const std::filesystem::path& file)
        {
            auto envelope = Diag::ReadFile(file);
            if (!envelope)
            {
                ARC_WARN("AssetRegistry: '{}' is not a valid .arcdiag envelope -- skipped",
                         file.generic_string());
                return {};
            }
            return envelope->guid;
        }
    }

    std::size_t AssetRegistry::ScanContent(const std::filesystem::path& contentDir, std::string_view scheme,
                                           ScanProgressFn onProgress)
    {
        m_byGuid.clear();
        // A full rebuild-from-scratch restarts the "assets" diagnostic set too --
        // see AddContent's Publish() below for why this lives on the member
        // rather than a function-local vector.
        m_scanDiagnostics.clear();

        // No observer: identical to this function's pre-callback shape (and
        // cost) -- a single walk via AddContent, no counting pass. AddContent
        // itself accumulates + publishes "assets" once its walk finishes.
        if (!onProgress)
            return AddContent(contentDir, scheme);

        std::error_code ec;
        if (!std::filesystem::is_directory(contentDir, ec))
            return 0;

        // Denominator: a cheap stat-only pass (is_regular_file only -- no file
        // reads) over the SAME population AddFile below will be offered, so
        // `done` reaching `total` on the real pass is guaranteed rather than
        // hoped for. This is the one added cost this parameter brings, and it
        // is only paid when a caller actually wants progress (see above).
        std::size_t total = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
            if (entry.is_regular_file())
                ++total;

        const std::size_t before = m_byGuid.size();
        std::size_t done = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
        {
            if (!entry.is_regular_file())
                continue;
            // Per-file rules (kind routing, id resolution, mount-path shape) live in
            // AddFile so this progress-reporting walk and AddContent's plain one can
            // never drift apart on what counts as a trackable asset.
            AddFile(entry.path(), contentDir, scheme);
            ++done;
            onProgress(done, total);
        }

        // KEY OWNERSHIP: "assets" -- accumulated across the WHOLE walk above into
        // m_scanDiagnostics (AddFile appends; ScanContent cleared it at entry) and
        // published ONCE here, per the publication-group contract (Publish
        // replaces the key's entire set) -- publishing per-file would leave only
        // the last file's rows visible. Unconditional: an empty vector RETRACTS
        // the previous scan's rows, which is exactly the clean-rescan case.
        Diagnostics::Publish("assets", m_scanDiagnostics);

        return m_byGuid.size() - before;
    }

    std::size_t AssetRegistry::AddContent(const std::filesystem::path& contentDir, std::string_view scheme)
    {
        const std::size_t before = m_byGuid.size();

        std::error_code ec;
        if (!std::filesystem::is_directory(contentDir, ec))
            return 0;

        // Per-file rules (kind routing, id resolution, mount-path shape) live in
        // AddFile so the open-time scan and the editor's incremental registration
        // can never drift apart.
        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
        {
            if (!entry.is_regular_file())
                continue;
            AddFile(entry.path(), contentDir, scheme);
        }

        // KEY OWNERSHIP: "assets" -- same contract as ScanContent's callback
        // branch above (accumulate the whole walk into m_scanDiagnostics, then
        // publish once, unconditionally). Deliberately NOT cleared at entry:
        // ScanContent's no-callback path delegates straight into this function
        // (m_scanDiagnostics was already reset there, so this publishes exactly
        // that one walk's rows), but Project::Open also calls AddContent
        // standalone -- once per enabled plugin -- to fold plugin content into
        // the SAME registry AFTER the initial ScanContent. If this cleared, that
        // second call's publish would retract the first scan's still-true
        // diagnostics instead of adding to them; accumulating keeps every
        // content root's rows visible across the whole open.
        Diagnostics::Publish("assets", m_scanDiagnostics);

        return m_byGuid.size() - before;
    }

    std::optional<Guid> AssetRegistry::AddFile(const std::filesystem::path& file,
                                               const std::filesystem::path& contentDir,
                                               std::string_view scheme)
    {
        const std::string ext = LowerExt(file);

        // A .meta is a sidecar carrying another file's identity, not an asset itself.
        if (ext == ".meta")
            return std::nullopt;

        // Route by asset kind: native JSON embeds its id, imported binaries use
        // a sidecar, anything else is not an asset we track.
        //
        // .arcscene is on the native list because a scene has to be
        // referenceable by Guid rather than by path: ProjectManifest::bootScene
        // stores one, so a scene that moves on disk must keep opening, and the
        // asset browser can only list what this registry knows about.
        //
        // .arcsprite is native for the same reason .arcmat is: SaveSpriteAsset
        // embeds a top-level "id" (SpriteAsset.cpp:14), so it rides the same
        // ResolveNativeId path rather than a sidecar.
        //
        // .arcmesh (the F2a asset format is Task 2's; this registry line is
        // Task 6's, which is where its absence was caught) is native for the
        // identical reason:
        // SaveMeshAsset embeds a top-level "id" (MeshAsset.cpp: `doc["id"] =
        // data.id.ToString()`), so it is a plain ResolveNativeId JSON asset,
        // never an imported binary needing a .meta sidecar. Missing from
        // this list, a project's own MeshCache resolveAsset lambda
        // (Host/SceneRenderResolver.cpp) can never find a Guid a project
        // just registered -- RegisterAsset would return nullopt for every
        // .arcmesh file, silently, since AddFile falls through to the
        // "not an asset we track" branch below rather than warning about a
        // format it does not recognise.
        //
        // .arcdiag (GPU crash diagnostics arc, Task 9) carries an embedded
        // guid too, so it is "native" in the same guid-not-sidecar sense --
        // but it is its OWN branch below, never folded into the
        // ResolveNativeId list above: the envelope's guid lives under
        // "guid" (DiagEnvelope.hpp), not "id", so it needs ResolveDiagId,
        // not ResolveNativeId. See ResolveDiagId's own comment for why
        // conflating the two would be wrong, not just redundant.
        Guid id;
        bool idWriteFailed = false;
        if (ext == ".json" || ext == ".arcmat" || ext == ".arcscene" || ext == ".arcsprite" ||
            ext == ".arcmesh")
            id = ResolveNativeId(file, &idWriteFailed);
        else if (ext == ".arcdiag")
            id = ResolveDiagId(file);   // F-7 CRITICAL: never ResolveNativeId -- see above
        else if (IsImportedBinary(ext))
            id = ResolveSidecarId(file, &idWriteFailed);
        else
            return std::nullopt;

        if (!id.IsValid())
            return std::nullopt;   // read/parse failure -- already warned

        // KEY OWNERSHIP for all three diagnostics below: "assets", owned
        // end-to-end by ScanContent/AddContent (see their Diagnostics::Publish
        // calls) -- AddFile itself never publishes. It only appends to
        // m_scanDiagnostics, the member those callers accumulate across a
        // whole walk and publish as one set. A standalone AddFile call outside
        // a scan (Project::RegisterAsset's incremental New Material/Instance
        // path) still queues into the same member; it surfaces on the next
        // ScanContent/AddContent rather than immediately, which is fine since
        // that path mints a fresh Guid and essentially never collides.
        if (idWriteFailed)
        {
            // Producer: failed id write-back -- native ResolveNativeId or sidecar
            // ResolveSidecarId, same code either way. The mint still SUCCEEDED in
            // memory (id.IsValid() below is true, so Resolve(id) works THIS
            // session), it just did not PERSIST -- a restart mints a different id
            // for the same file, so the reference is not durable yet.
            Diagnostic d;
            d.severity = DiagSeverity::Warning;
            d.scope    = DiagScope::Assets;
            d.code     = "assets.id.write-failed";
            d.message  = "Could not write asset id back to '" + file.generic_string() +
                         "' -- the id will not survive a restart";
            d.locator  = DiagLocator::Asset(id);
            m_scanDiagnostics.push_back(std::move(d));
        }

        // Mount path: "<scheme>://<relative-to-contentDir, forward slashes>". The
        // ORIGINAL file is registered (the .meta only stores the id), so a resolved
        // Guid still points at the real asset the loader reads. A file outside
        // contentDir has no expressible mount path -- refuse it.
        std::error_code ec;
        const auto rel = std::filesystem::relative(file, contentDir, ec);
        if (ec || rel.empty() || rel.is_absolute() || *rel.begin() == "..")
        {
            ARC_WARN("AssetRegistry: '{}' is not under content root '{}' -- not registered",
                     file.generic_string(), contentDir.generic_string());
            // Producer: outside content root -- the file is never registered (no
            // mount path exists), so a File locator is what a click can actually
            // do something with; an Asset locator would point at nothing (this
            // Guid, even if minted above, is never inserted into m_byGuid).
            Diagnostic d;
            d.severity = DiagSeverity::Warning;
            d.scope    = DiagScope::Assets;
            d.code     = "assets.outside-content-root";
            d.message  = "'" + file.generic_string() + "' is not under content root '" +
                         contentDir.generic_string() + "' -- not registered";
            d.locator  = DiagLocator::File(file.string());
            m_scanDiagnostics.push_back(std::move(d));
            return std::nullopt;
        }
        const std::string mountPath = std::string(scheme) + "://" + rel.generic_string();

        if (auto [it, inserted] = m_byGuid.try_emplace(id, mountPath);
            !inserted && it->second != mountPath)
        {
            // Same id from a DIFFERENT file: scan rule -- keep the first, warn.
            // (Same id + same mapping = idempotent re-registration, silent.)
            ARC_WARN("AssetRegistry: duplicate id {} (also '{}') -- keeping first",
                     id.ToString(), file.generic_string());
            // Producer: duplicate embedded id, kept-first. Asset locator -- id IS
            // registered (to the first file), so Resolve(id) navigates somewhere
            // real; this row is what tells the user a second file is being
            // silently ignored.
            Diagnostic d;
            d.severity = DiagSeverity::Warning;
            d.scope    = DiagScope::Assets;
            d.code     = "assets.id.duplicate";
            d.message  = "Duplicate asset id " + id.ToString() + ": kept '" + it->second +
                         "', ignoring '" + file.generic_string() + "'";
            d.locator  = DiagLocator::Asset(id);
            m_scanDiagnostics.push_back(std::move(d));
        }
        return id;
    }

    std::optional<std::string> AssetRegistry::Resolve(const Guid& id) const
    {
        auto it = m_byGuid.find(id);
        if (it == m_byGuid.end())
            return std::nullopt;
        return it->second;
    }

    std::vector<std::pair<Guid, std::string>> AssetRegistry::All() const
    {
        std::vector<std::pair<Guid, std::string>> out;
        out.reserve(m_byGuid.size());
        for (const auto& [id, mountPath] : m_byGuid)
            out.emplace_back(id, mountPath);
        // DETERMINISTIC ORDER, and it is load-bearing. m_byGuid is an
        // unordered_map, so the walk above tracks guid HASHES and insertion
        // history: two trees holding identical files enumerate them
        // differently. All() publishes a CONTRACT -- a total order that is a
        // function of content alone -- so a future consumer inherits
        // determinism instead of having to remember it. Today's two
        // consumers (AssetBrowser's BuildAssetEntries, which re-sorts by
        // (name, mountPath) itself; EditorAppProject's
        // MintOrReuseSpriteForTexture, which only acts at matches == 1) are
        // both order-insensitive, so this is not a fix to a live rendering
        // defect -- it closes a LATENT one: BuildAssetEntries' comparator
        // stops being total once two guids share a mount path, which
        // AddContent permits (dedupes by id, not path) but Project::Open
        // never triggers (every root gets its own scheme). Corrected
        // 2026-08-30 -- the prior "changes the editor-ui golden image" claim
        // here was unproven and false; do not re-litigate it.
        // Sorted by mount path (what the panel displays), Guid breaking ties
        // so the order is TOTAL. std::string's operator< goes through
        // char_traits::compare -- byte-wise, NOT locale-aware -- so the result
        // is identical on every machine. A locale-aware sort would reintroduce
        // exactly the machine-dependence this removes.
        std::sort(out.begin(), out.end(),
                  [](const std::pair<Guid, std::string>& a,
                     const std::pair<Guid, std::string>& b)
                  {
                      if (a.second != b.second) return a.second < b.second;
                      return a.first < b.first;
                  });
        return out;
    }
}
