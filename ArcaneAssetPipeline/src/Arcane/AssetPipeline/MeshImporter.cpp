#include "Arcane/AssetPipeline/MeshImporter.hpp"

#include <cgltf.h>

#include <cstring>
#include <fstream>
#include <string>
#include <utility>

namespace Arcane::AssetPipeline
{
    namespace fs = std::filesystem;

    namespace
    {
        // ---- cgltf_data* RAII guard -------------------------------------------------------
        // Written FIRST, before any of the five refusal returns below: a function with five
        // early-return refusals and a hand-written cgltf_free on each is exactly where a leak
        // hides. Never copied or moved (each of ImportMesh/ReadExternalBuffers owns exactly
        // one, as a local) -- copy is deleted so an accidental copy can't double-free; the
        // destructor is the only cgltf_free call site in this whole file.
        struct CgltfDataGuard
        {
            cgltf_data* data = nullptr;
            ~CgltfDataGuard() { if (data) cgltf_free(data); }
            CgltfDataGuard() = default;
            CgltfDataGuard(const CgltfDataGuard&) = delete;
            CgltfDataGuard& operator=(const CgltfDataGuard&) = delete;
        };

        // Whole-file read, used both by ReadExternalBuffers (for a referenced .bin) and
        // nowhere else in this TU -- ImportMesh never reads files itself, it is handed
        // sourceBytes/externalBuffers already read (see MeshImporter.hpp's header comment
        // for why: the cook key and the importer must see the SAME bytes, by construction).
        std::optional<std::vector<std::byte>> ReadWholeFileBytes(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) return std::nullopt;

            in.seekg(0, std::ios::end);
            const std::streamoff len = in.tellg();
            if (len < 0) return std::nullopt;
            in.seekg(0, std::ios::beg);

            std::vector<std::byte> out(static_cast<std::size_t>(len));
            if (!out.empty())
            {
                in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
                if (!in) return std::nullopt;
            }
            return out;
        }

        bool StartsWith(const char* s, const char* prefix) noexcept
        {
            return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
        }

        // cgltf_decode_uri(char*) decodes percent-escapes IN PLACE and null-terminates at
        // the (never longer) decoded length -- it must never run on cgltf's own buffer.uri,
        // which cgltf_load_buffers still needs to resolve buffers itself later. Work on a
        // copy: std::string::data() is a mutable char* since C++17, and the decoded bytes
        // land inside it null-terminated at the shorter length, so reading it back via
        // c_str() (which stops at that new null, ignoring the string's stale size()) is
        // exactly the value fs::path needs.
        fs::path DecodeUriToPath(const char* uri)
        {
            std::string copy(uri);
            cgltf_decode_uri(copy.data());
            return fs::path(copy.c_str());
        }

        std::string CgltfResultName(cgltf_result r)
        {
            switch (r)
            {
                case cgltf_result_success:          return "success";
                case cgltf_result_data_too_short:    return "data_too_short";
                case cgltf_result_unknown_format:    return "unknown_format";
                case cgltf_result_invalid_json:      return "invalid_json";
                case cgltf_result_invalid_gltf:      return "invalid_gltf";
                case cgltf_result_invalid_options:   return "invalid_options";
                case cgltf_result_file_not_found:    return "file_not_found";
                case cgltf_result_io_error:          return "io_error";
                case cgltf_result_out_of_memory:     return "out_of_memory";
                case cgltf_result_legacy_gltf:       return "legacy_gltf";
                default:                             return "unknown(" + std::to_string(static_cast<int>(r)) + ")";
            }
        }

        // Names the primitive a degenerate-triangle warning (or a refusal, if this task
        // ever needed to name one at primitive granularity) points at: its material's own
        // name when present and non-empty, else "primitive N" where N is a flat index over
        // every primitive in the file (mesh-major, primitive-minor) -- stable regardless of
        // how many meshes a file declares, and readable without cross-referencing a mesh
        // index too.
        std::string PrimitiveLabel(const cgltf_primitive& prim, std::size_t flatIndex)
        {
            if (prim.material != nullptr && prim.material->name != nullptr && prim.material->name[0] != '\0')
                return prim.material->name;
            return "primitive " + std::to_string(flatIndex);
        }
    }

    MeshImportResult ImportMesh(std::span<const std::byte> sourceBytes,
                                 std::span<const std::span<const std::byte>> /*externalBuffers*/,
                                 const fs::path& sourcePath,
                                 const Guid& /*sourceGuid*/,
                                 const MeshMetaSettings& /*settings*/)
    {
        MeshImportResult result;
        const std::string fileName = sourcePath.filename().string();

        // ---- Rung 1: parse ------------------------------------------------------------
        cgltf_options options{};
        CgltfDataGuard guard;
        const cgltf_result parseResult =
            cgltf_parse(&options, sourceBytes.data(), sourceBytes.size(), &guard.data);
        if (parseResult != cgltf_result_success)
        {
            result.refusal = "mesh import refused: '" + fileName + "' failed to parse as glTF/GLB (cgltf_parse: "
                + CgltfResultName(parseResult) + ")";
            return result;
        }

        // ---- Rung 2: extensionsRequired -- this importer implements NONE --------------
        // Walk every entry rather than checking against a hand-maintained list of known
        // extensions: that is what makes this rung cover KHR_texture_basisu,
        // EXT_meshopt_compression and every future extension by construction, and what A2
        // part 1's test asserts ("the GENERAL rule, not a hand-list").
        if (guard.data->extensions_required_count > 0)
        {
            std::string names;
            for (cgltf_size i = 0; i < guard.data->extensions_required_count; ++i)
            {
                if (i > 0) names += ", ";
                names += guard.data->extensions_required[i];
            }
            result.refusal = "mesh import refused: '" + fileName
                + "' declares extensionsRequired this importer does not implement: [" + names + "]";
            return result;
        }

        // ---- Rung 3: load buffers ------------------------------------------------------
        const cgltf_result loadResult = cgltf_load_buffers(&options, guard.data, sourcePath.string().c_str());
        if (loadResult != cgltf_result_success)
        {
            // cgltf_load_buffers loads data->buffers[0..N) IN ORDER and returns at the
            // FIRST failure -- every buffer before it already has .data set, so the first
            // one still null (with a non-null uri; the embedded/GLB-BIN case always has
            // .data set before this loop even runs) is the one that actually failed. Named
            // by its own uri when it has one, else its index, so the refusal points at the
            // specific missing file rather than "a buffer, somewhere".
            std::string missingBuffer = "<unknown>";
            for (cgltf_size i = 0; i < guard.data->buffers_count; ++i)
            {
                if (guard.data->buffers[i].data == nullptr)
                {
                    missingBuffer = (guard.data->buffers[i].uri != nullptr)
                        ? guard.data->buffers[i].uri
                        : ("buffer[" + std::to_string(i) + "]");
                    break;
                }
            }
            result.refusal = "mesh import refused: '" + fileName + "' failed to load buffer '" + missingBuffer
                + "' (cgltf_load_buffers: " + CgltfResultName(loadResult) + ")";
            return result;
        }

        // ---- Rung 4: validate -- MANDATORY, never behind a settings flag ---------------
        // The CVE-2026-32845-hardened path (ThirdParty/cgltf/PATCHES.md) this vendor pin
        // exists for (spec s4.5/s10) -- bad_sparse.glb parses fine (cgltf_parse does no
        // bounds checking) and is caught only here.
        const cgltf_result validateResult = cgltf_validate(guard.data);
        if (validateResult != cgltf_result_success)
        {
            result.refusal = "mesh import refused: '" + fileName
                + "' failed cgltf_validate (cgltf_validate: " + CgltfResultName(validateResult) + ")";
            return result;
        }

        // ---- Rung 5: drawability gate + per-primitive degenerate drop ------------------
        if (guard.data->meshes_count == 0)
        {
            result.refusal = "mesh import refused: '" + fileName + "' has nothing drawable (no meshes)";
            return result;
        }

        std::vector<std::string> warnings;
        std::uint64_t survivingTrianglesTotal = 0;
        std::size_t flatPrimitiveIndex = 0;

        for (cgltf_size meshIdx = 0; meshIdx < guard.data->meshes_count; ++meshIdx)
        {
            const cgltf_mesh& mesh = guard.data->meshes[meshIdx];
            for (cgltf_size primIdx = 0; primIdx < mesh.primitives_count; ++primIdx, ++flatPrimitiveIndex)
            {
                const cgltf_primitive& prim = mesh.primitives[primIdx];

                // Only triangle-list primitives carry the "triangle" notion this gate and
                // the degenerate-drop rule both operate on; every fixture in the corpus is
                // mode 4 (triangles) with an index accessor. A primitive with no index
                // accessor or a non-triangles mode contributes zero triangles to the
                // file-wide surviving count rather than being read out of bounds.
                if (prim.indices == nullptr || prim.type != cgltf_primitive_type_triangles)
                    continue;

                const cgltf_accessor& indices = *prim.indices;
                const cgltf_size triangleCount = indices.count / 3;

                cgltf_size droppedInPrimitive = 0;
                cgltf_size survivingInPrimitive = 0;
                for (cgltf_size t = 0; t < triangleCount; ++t)
                {
                    const cgltf_size i0 = cgltf_accessor_read_index(&indices, t * 3 + 0);
                    const cgltf_size i1 = cgltf_accessor_read_index(&indices, t * 3 + 1);
                    const cgltf_size i2 = cgltf_accessor_read_index(&indices, t * 3 + 2);

                    // A triangle whose three indices are not all distinct is degenerate
                    // (zero area) -- dropped, never refused, per primitive.
                    if (i0 == i1 || i1 == i2 || i0 == i2)
                        ++droppedInPrimitive;
                    else
                        ++survivingInPrimitive;
                }

                survivingTrianglesTotal += survivingInPrimitive;

                if (droppedInPrimitive > 0)
                {
                    warnings.push_back(PrimitiveLabel(prim, flatPrimitiveIndex) + ": dropped "
                        + std::to_string(droppedInPrimitive) + " degenerate triangle(s) ("
                        + std::to_string(survivingInPrimitive) + " surviving)");
                }
            }
        }

        // The count is per FILE, not per primitive: one all-degenerate primitive beside a
        // healthy one is a warning (already pushed above), not a refusal.
        if (survivingTrianglesTotal == 0)
        {
            result.refusal = "mesh import refused: '" + fileName
                + "' has nothing drawable (every triangle is degenerate)";
            return result;
        }

        // Task 7 fills in `result.mesh` (vertices/indices/desc) over these same rungs --
        // this task returns the refusal/warning verdict only.
        result.warnings = std::move(warnings);
        return result;
    }

    std::optional<std::vector<std::vector<std::byte>>> ReadExternalBuffers(
        std::span<const std::byte> sourceBytes, const fs::path& sourcePath)
    {
        cgltf_options options{};
        CgltfDataGuard guard;
        const cgltf_result parseResult =
            cgltf_parse(&options, sourceBytes.data(), sourceBytes.size(), &guard.data);
        if (parseResult != cgltf_result_success)
            return std::nullopt;

        std::vector<std::vector<std::byte>> buffers;
        buffers.reserve(guard.data->buffers_count);

        const fs::path baseDir = sourcePath.parent_path();

        for (cgltf_size i = 0; i < guard.data->buffers_count; ++i)
        {
            const cgltf_buffer& buffer = guard.data->buffers[i];

            // Embedded buffers contribute NOTHING -- their bytes already live inside
            // `sourceBytes`. A GLB's BIN-chunk buffer has uri == nullptr; a glTF's inline
            // buffer is a "data:" URI (cgltf_load_buffers resolves it into .data, but we
            // never call that here -- .uri itself already carries the "data:" prefix at
            // this point, which is all the skip test needs).
            if (buffer.uri == nullptr || StartsWith(buffer.uri, "data:"))
                continue;

            const fs::path bufferPath = baseDir / DecodeUriToPath(buffer.uri);
            const std::optional<std::vector<std::byte>> bytes = ReadWholeFileBytes(bufferPath);
            if (!bytes)
                return std::nullopt;   // referenced buffer unreadable -- CookSession turns
                                        // this into a refusal naming it.

            buffers.push_back(*bytes);
        }

        return buffers;
    }
}
