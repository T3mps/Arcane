// libFuzzer harness: the cooked content-artifact reader (.arcart) --
// ArcaneCore/src/Arcane/Assets/ArtifactReader.cpp.
//
// The input is the whole .arcart file. It is written to a private temp dir and
// read through every public entry point that parses it:
//   ReadClientArtifact      (texture kind)
//   ReadClientMeshArtifact  (mesh kind)
//   FindArtifactForGuid     (the kind-agnostic 256-byte header probe)
//   ReadClientExternalBuffers (the same bytes as a .gltf/.glb source)
//
// The reader validates sourceGuid and sourceHash before it hands anything out,
// so the harness passes a fixed guid (kGuid) and EMPTY current-source bytes --
// whose FNV-1a 64 is the offset basis, 0xcbf29ce484222325 -- and the seeds carry
// both. ASan/UBSan catch out-of-bounds reads inside the reader itself.
//
// On top of that, an ACCEPTED artifact must satisfy the contract its consumers
// rely on without rechecking (the reader is the one place that sees the file):
//   texture: thumbRgba holds exactly thumbWidth*thumbHeight*4 bytes (Assets::
//            PixelsFor publishes those dims with those bytes, and the upload
//            reads width*height*4 from rgba.data()); one MipView per declared
//            mip; every mip lies inside payload and holds exactly the bytes its
//            width/height/format imply (the upload reads slicePitch bytes).
//   mesh:    vertices/indices/sections agree with the header, and every section
//            range lies inside the index buffer.
// A broken contract prints the property and aborts.

#include <Arcane/Assets/ArtifactReader.hpp>
#include <Arcane/Guid.hpp>

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace
{
    [[noreturn]] void Fail(const char* property, int line)
    {
        std::fprintf(stderr, "artifact_fuzz: property violated (line %d): %s\n", line, property);
        std::abort();
    }
#define FUZZ_CHECK(cond) do { if (!(cond)) Fail(#cond, __LINE__); } while (0)

    // Matches the seeds in fuzz/corpus/artifact (bytes 9..24 of the header).
    const Arcane::Guid kGuid{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};

    const std::filesystem::path& WorkDir()
    {
        static const std::filesystem::path dir = [] {
            std::filesystem::path d = std::filesystem::temp_directory_path() /
                                      ("arcane-artifact-fuzz-" + std::to_string(::getpid()));
            std::filesystem::create_directories(d / "Artifacts" / "ab");
            return d;
        }();
        return dir;
    }

    void WriteFile(const std::filesystem::path& p, const uint8_t* data, size_t size)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    // The bytes a mip's dims imply -- what NriTextureCache::UploadArtifact reads
    // for it (its slicePitch). Computed exactly in 128 bits: the upload does it
    // in uint32, so a size that only matches after wrapping is still a lie.
    unsigned __int128 MipBytes(Arcane::ArtifactPixelFormatValue fmt, uint64_t w, uint64_t h, bool& known)
    {
        using U = unsigned __int128;
        known = true;
        switch (fmt)
        {
        case Arcane::ArtifactPixelFormatValue::RGBA8: return U(w) * h * 4;
        case Arcane::ArtifactPixelFormatValue::BC7:   return U((w + 3) / 4) * ((h + 3) / 4) * 16;
        default: known = false; return 0;   // reserved formats: the upload refuses them
        }
    }

    void CheckTexture(const Arcane::LoadedClientArtifact& a)
    {
        const uint64_t thumbPixels = uint64_t(a.thumbWidth) * a.thumbHeight;
        FUZZ_CHECK(thumbPixels <= a.thumbRgba.size() / 4 && thumbPixels * 4 == a.thumbRgba.size());
        FUZZ_CHECK(a.mips.size() == a.info.mipCount);
        for (const Arcane::MipView& m : a.mips)
        {
            FUZZ_CHECK(m.offset <= a.payload.size() && m.size <= a.payload.size() - m.offset);
            bool known = false;
            const unsigned __int128 want = MipBytes(a.format, m.width, m.height, known);
            if (known)
            {
                FUZZ_CHECK(m.width > 0 && m.height > 0);
                FUZZ_CHECK(m.size == want);   // the upload reads exactly this many bytes from here
            }
        }
        // Touch every byte the consumers would, so ASan sees any lie above.
        volatile uint8_t sink = 0;
        for (std::byte b : a.thumbRgba) sink ^= static_cast<uint8_t>(b);
        (void)sink;
    }

    void CheckMesh(const Arcane::LoadedClientMesh& m)
    {
        FUZZ_CHECK(m.vertices.size() % 8 == 0);
        for (const Arcane::MeshSectionView& s : m.sections)
            FUZZ_CHECK(uint64_t(s.indexOffset) + s.indexCount <= m.indices.size());
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::filesystem::path& dir = WorkDir();
    const std::filesystem::path artifact = dir / "Artifacts" / "ab" / "input.arcart";
    WriteFile(artifact, data, size);

    const std::span<const std::byte> noSource;

    const Arcane::ArtifactReadResult tex = Arcane::ReadClientArtifact(artifact, noSource, kGuid);
    FUZZ_CHECK((tex.refusal == Arcane::ArtifactRefusal::None) == tex.artifact.has_value());
    if (tex.artifact) CheckTexture(*tex.artifact);

    const Arcane::MeshArtifactReadResult mesh = Arcane::ReadClientMeshArtifact(artifact, noSource, kGuid);
    FUZZ_CHECK((mesh.refusal == Arcane::ArtifactRefusal::None) == mesh.mesh.has_value());
    if (mesh.mesh) CheckMesh(*mesh.mesh);

    if (std::getenv("ARTIFACT_FUZZ_VERBOSE"))
        std::fprintf(stderr, "texture refusal=%d mesh refusal=%d\n",
                     static_cast<int>(tex.refusal), static_cast<int>(mesh.refusal));

    // Accepting as BOTH kinds would mean the contentKind gate is gone.
    FUZZ_CHECK(!(tex.artifact && mesh.mesh));

    (void)Arcane::FindArtifactForGuid(dir, kGuid);

    // The same bytes as a mesh SOURCE: the GLB container walk + buffers[].uri.
    // The source sits in an empty dir, so a relative uri resolves to nothing.
    const auto bytes = std::as_bytes(std::span<const uint8_t>(data, size));
    (void)Arcane::ReadClientExternalBuffers(bytes, dir / "Artifacts" / "ab" / "src.gltf");
    return 0;
}
