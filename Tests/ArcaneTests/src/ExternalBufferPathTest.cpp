// Fuzz regression for ReadClientExternalBuffers (fuzz/artifact_fuzz.cpp, input in
// fuzz/regressions/artifact/gltf-empty-uri-opens-directory): a glTF buffer uri that
// resolves to a DIRECTORY ("", ".", "..") used to be opened as a file -- on
// libstdc++ that succeeds, tellg() reports LLONG_MAX, and the read buffer's
// allocation threw out of the exception-free asset path. It is a refusal (nullopt),
// the same as any unreadable referenced buffer.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ArtifactReader.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace
{
    std::span<const std::byte> AsBytes(const std::string& s)
    {
        return std::as_bytes(std::span<const char>(s.data(), s.size()));
    }
}

TEST_CASE("ReadClientExternalBuffers refuses a buffer uri that names a directory", "[artifact][fuzz]")
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "arcane-external-buffer-path-test";
    std::filesystem::create_directories(dir / "sub");
    const std::filesystem::path source = dir / "mesh.gltf";

    for (const char* uri : { "", ".", "..", "sub" })
    {
        INFO("uri = \"" << uri << "\"");
        const std::string gltf = std::string(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4},{"uri":")") +
                                 uri + R"("}]})";
        std::optional<std::vector<std::vector<std::byte>>> buffers;
        REQUIRE_NOTHROW(buffers = Arcane::ReadClientExternalBuffers(AsBytes(gltf), source));
        CHECK_FALSE(buffers.has_value());
    }

    // Control: no external buffers at all is still an empty success.
    const std::string embedded = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4}]})";
    const auto none = Arcane::ReadClientExternalBuffers(AsBytes(embedded), source);
    REQUIRE(none.has_value());
    CHECK(none->empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
