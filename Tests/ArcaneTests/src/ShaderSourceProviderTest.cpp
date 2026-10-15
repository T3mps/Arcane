// ShaderSourceProvider: logical shader/template name -> source text through
// registered root dirs (first hit wins). The seam Slice 4's template stitcher
// and the compile service resolve source through. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("ShaderSourceProvider resolves names through roots in order", "[shadercompile]")
{
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "arcane_shader_source_provider";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / "primary" / "materials");
    fs::create_directories(base / "fallback" / "materials");

    std::ofstream(base / "primary" / "materials" / "a.hlsl", std::ios::binary)
        << "// primary a";
    std::ofstream(base / "fallback" / "materials" / "a.hlsl", std::ios::binary)
        << "// fallback a";
    std::ofstream(base / "fallback" / "materials" / "b.hlsl", std::ios::binary)
        << "// fallback b";

    Arcane::ShaderSourceProvider provider;
    CHECK(provider.RootCount() == 0);
    provider.AddRoot(base / "primary");
    provider.AddRoot(base / "fallback");
    CHECK(provider.RootCount() == 2);

    // First root wins when both have the file.
    auto a = provider.Get("materials/a.hlsl");
    REQUIRE(a.has_value());
    CHECK(*a == "// primary a");

    // Falls through to the second root.
    auto b = provider.Get("materials/b.hlsl");
    REQUIRE(b.has_value());
    CHECK(*b == "// fallback b");

    // Missing everywhere -> nullopt.
    CHECK_FALSE(provider.Get("materials/nope.hlsl").has_value());

    // Escaping / absolute names are rejected, not resolved.
    CHECK_FALSE(provider.Get("../materials/a.hlsl").has_value());
    CHECK_FALSE(provider.Get((base / "primary" / "materials" / "a.hlsl").string()).has_value());
    CHECK_FALSE(provider.Get("").has_value());

    fs::remove_all(base, ec);
}
