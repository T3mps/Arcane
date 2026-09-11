#include "Project/MeshImportWave.hpp"

#include <Arcane/AssetPipeline/GltfSurvey.hpp>

#include <fstream>
#include <optional>

namespace Arcane::Editor
{
    namespace fs = std::filesystem;

    namespace
    {
        std::optional<std::vector<std::byte>> ReadWholeFile(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return std::nullopt;

            in.seekg(0, std::ios::end);
            const std::streamoff len = in.tellg();
            if (len < 0)
                return std::nullopt;
            in.seekg(0, std::ios::beg);

            std::vector<std::byte> out(static_cast<std::size_t>(len));
            if (!out.empty())
            {
                in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
                if (!in)
                    return std::nullopt;
            }
            return out;
        }

        // A glTF image name is arbitrary UTF-8 -- ANY character that could act as a
        // path separator or reserved Windows filename character is replaced with '_'
        // (byte-wise; every offending character here is single-byte ASCII, so this
        // never splits a multi-byte UTF-8 sequence). This is what keeps
        // ImageFileStem("../../evil", ...) from ever steering a write outside the
        // folder ExtractEmbeddedTextures computed for it.
        std::string SanitizeForFilename(const std::string& raw)
        {
            std::string out;
            out.reserve(raw.size());
            for (char c : raw)
            {
                switch (c)
                {
                    case '/': case '\\': case ':': case '*': case '?':
                    case '"': case '<': case '>': case '|':
                        out.push_back('_');
                        break;
                    default:
                        out.push_back(static_cast<unsigned char>(c) < 0x20 ? '_' : c);
                        break;
                }
            }
            return out;
        }

        // The loose sibling's extension, implied by the glTF image's own declared
        // MIME type. The corpus's only embedded kind today is image/png
        // (embedded_tex.glb's fixture) -- anything else this engine has not been
        // handed a fixture for falls back to .png rather than growing an untested
        // branch.
        std::string ExtensionForMime(const std::string& mimeType)
        {
            if (mimeType == "image/jpeg" || mimeType == "image/jpg")
                return ".jpg";
            return ".png";
        }
    }

    fs::path UniqueSiblingPath(const fs::path& dir, const std::string& stem, const std::string& ext)
    {
        fs::path candidate = dir / (stem + ext);
        for (int suffix = 1; fs::exists(candidate); ++suffix)
            candidate = dir / (stem + "-" + std::to_string(suffix) + ext);
        return candidate;
    }

    std::string ImageFileStem(const std::string& imageName, const std::string& sourceStem,
                               std::size_t index)
    {
        if (!imageName.empty())
            return SanitizeForFilename(imageName);
        return sourceStem + "-" + std::to_string(index);
    }

    std::vector<fs::path> ExtractEmbeddedTextures(const fs::path& source)
    {
        std::vector<fs::path> written;

        const std::optional<std::vector<std::byte>> bytes = ReadWholeFile(source);
        if (!bytes)
            return written;   // unreadable -- the cook will refuse it too, quietly.

        const std::optional<Arcane::AssetPipeline::GltfSurvey> survey =
            Arcane::AssetPipeline::SurveyGltf(*bytes, source);
        if (!survey)
            return written;   // parse/validate failure -- same "say nothing extra" rule.

        const fs::path dir = source.parent_path();
        const std::string sourceStem = source.stem().string();

        for (std::size_t index = 0; index < survey->images.size(); ++index)
        {
            const Arcane::AssetPipeline::GltfImage& image = survey->images[index];
            if (!image.embedded)
                continue;   // external images are not this task's concern -- they
                            // already have their own file on disk.

            const std::string stem = ImageFileStem(image.name, sourceStem, index);
            const fs::path dest = dir / (stem + ExtensionForMime(image.mimeType));

            // A4's no-overwrite half, checked BEFORE any write: a destination that
            // already exists -- a user's edited or replaced .png, or a PRIOR
            // extraction (this source's own, or another source's own collision at
            // the same name) -- is left exactly as it is.
            if (fs::exists(dest))
                continue;

            std::ofstream out(dest, std::ios::binary | std::ios::trunc);
            if (!out)
                continue;
            out.write(reinterpret_cast<const char*>(image.bytes.data()),
                       static_cast<std::streamsize>(image.bytes.size()));
            if (!out)
                continue;

            written.push_back(dest);
        }

        return written;
    }
}
