#include <Arcane/Serialization/SceneAsset.hpp>

#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Arcane::Scene
{
    bool SaveSceneFile(const std::filesystem::path& file, const Astra::Registry& reg,
                       const Arcane::Guid& id, std::string* error)
    {
        if (!id.IsValid())
        {
            Detail::SetError(error, "refusing to save a scene with no asset id");
            return false;
        }

        nlohmann::json doc;
        try
        {
            doc = SaveJson(reg);
            // Inserted after SaveJson so the id survives even though SaveJson
            // owns the rest of the document's shape.
            doc["id"] = id.ToString();
        }
        catch (const nlohmann::json::exception& e)
        {
            Detail::SetError(error, std::string("could not serialize the scene: ") + e.what());
            return false;
        }

        // Temp + replace, NOT a truncating open of `file`. std::ios::trunc
        // destroys the destination at OPEN, so any later failure (a throwing
        // dump, a short write, a full disk) would have already taken the
        // previously-saved level with it and left behind remains ReadSceneFile
        // rejects. Project::SetBootScene takes this same route for the
        // .arcproj on the same reasoning; a lost level is the worse loss.
        const std::filesystem::path tmp = file.string() + ".tmp";
        try
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                Detail::SetError(error, "could not open " + tmp.generic_string() + " for writing");
                return false;
            }
            out << doc.dump(2);
            if (!out)
            {
                // The temp exists on disk (open succeeded) but is partial --
                // remove it so a failed write leaves no stray .tmp behind.
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                Detail::SetError(error, "could not write " + tmp.generic_string());
                return false;
            }
        }
        catch (const nlohmann::json::exception& e)
        {
            // dump(2) can throw (invalid UTF-8 in a reflected string field)
            // after `out` already created the temp -- same cleanup as the
            // write-failure case above. This is precisely the window that made
            // the old truncating write lose the level.
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            Detail::SetError(error, std::string("could not serialize the scene: ") + e.what());
            return false;
        }

#ifdef _WIN32
        // ReplaceFileW, not a plain delete+rename: it preserves the destination's
        // NTFS attributes/ACLs and never leaves a window where `file` does not
        // exist. It requires the destination to exist, so a FIRST save (Save As to
        // a new path) renames instead.
        std::error_code existsEc;
        if (std::filesystem::exists(file, existsEc))
        {
            if (!ReplaceFileW(file.c_str(), tmp.c_str(), nullptr,
                              REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
            {
                const DWORD err = GetLastError();   // capture before any other call clobbers it
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                // ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED mean some other handle
                // on `file` lacks FILE_SHARE_DELETE (AV scanner, git, a backup tool, a
                // second editor window) -- distinguish that from a generic replace
                // failure so the user can tell "close the other program and retry"
                // apart from a genuinely broken path.
                if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED)
                {
                    Detail::SetError(error, "could not replace " + file.generic_string() +
                                            " -- another program may have the scene open (err " +
                                            std::to_string(err) + "); close it and try again");
                }
                else
                {
                    Detail::SetError(error, "could not replace " + file.generic_string() +
                                            " (err " + std::to_string(err) + ")");
                }
                return false;
            }
            return true;
        }
#endif
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
        if (ec)
        {
            std::filesystem::remove(tmp, ec);
            Detail::SetError(error, "could not move " + tmp.generic_string() + " into place as " +
                                    file.generic_string());
            return false;
        }
        return true;
    }
}
