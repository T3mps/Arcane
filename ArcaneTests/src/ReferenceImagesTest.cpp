// Reference resolution and blessing. Pure filesystem logic, no GPU, no host.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/ReferenceImages.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path TempProject(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_reference_images_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    void WriteSolidPng(const fs::path& p, unsigned char value)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::vector<unsigned char> px(4 * 4 * 4, value);
        for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        REQUIRE(Arcane::WritePngRgba(p, 4, 4, px.data()));
    }
}

TEST_CASE("reference: nothing on disk resolves to None but still names a bless target", "[reference]")
{
    const auto root = TempProject("empty");
    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.path.empty());
    // A first bless must know where to go, so blessTarget is ALWAYS set --
    // the shared level when nothing resolved.
    CHECK(res.blessTarget == root / "Verify" / "References" / "runtime-scene.png");
}

TEST_CASE("reference: a shared image resolves at the Shared level", "[reference]")
{
    const auto root = TempProject("shared");
    WriteSolidPng(root / "Verify" / "References" / "runtime-scene.png", 10);

    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    CHECK(res.level == Arcane::ReferenceLevel::Shared);
    CHECK(res.path == root / "Verify" / "References" / "runtime-scene.png");
    CHECK(res.blessTarget == res.path);       // bless writes back where it came from
}

TEST_CASE("reference: a backend override WINS over the shared image", "[reference]")
{
    const auto root = TempProject("override");
    WriteSolidPng(root / "Verify" / "References" / "editor-ui.png", 10);
    WriteSolidPng(root / "Verify" / "References" / "vulkan" / "editor-ui.png", 20);

    const auto dx12 = Arcane::ResolveReference(root, "editor-ui", "dx12");
    CHECK(dx12.level == Arcane::ReferenceLevel::Shared);

    const auto vk = Arcane::ResolveReference(root, "editor-ui", "vulkan");
    CHECK(vk.level == Arcane::ReferenceLevel::Backend);
    CHECK(vk.path == root / "Verify" / "References" / "vulkan" / "editor-ui.png");
    CHECK(vk.blessTarget == vk.path);
}

TEST_CASE("reference: blessing a fresh name creates the SHARED image", "[reference]")
{
    const auto root = TempProject("bless-new");
    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    REQUIRE(res.level == Arcane::ReferenceLevel::None);

    std::vector<unsigned char> px(4 * 4 * 4, 77);
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    CHECK(Arcane::BlessReference(res, 4, 4, px.data()));

    CHECK(fs::exists(root / "Verify" / "References" / "runtime-scene.png"));

    // Hole B: existence alone passes a bless that wrote nothing or wrote a
    // stale/blank image. Load the file back and check the content it was
    // actually blessed with.
    Arcane::PixelData blessed;
    REQUIRE(Arcane::LoadPngRgba(root / "Verify" / "References" / "runtime-scene.png",
                                blessed.width, blessed.height, blessed.rgba));
    REQUIRE(blessed.Valid());
    CHECK(blessed.rgba[0] == 77);

    // And it now resolves as Shared.
    const auto after = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    CHECK(after.level == Arcane::ReferenceLevel::Shared);
}

TEST_CASE("reference: blessing a BACKEND-resolved image does not touch the shared one", "[reference]")
{
    const auto root = TempProject("bless-backend");
    WriteSolidPng(root / "Verify" / "References" / "editor-ui.png", 10);
    WriteSolidPng(root / "Verify" / "References" / "dx12" / "editor-ui.png", 20);

    const auto res = Arcane::ResolveReference(root, "editor-ui", "dx12");
    REQUIRE(res.level == Arcane::ReferenceLevel::Backend);

    std::vector<unsigned char> px(4 * 4 * 4, 99);
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    CHECK(Arcane::BlessReference(res, 4, 4, px.data()));

    Arcane::PixelData shared;
    REQUIRE(Arcane::LoadPngRgba(root / "Verify" / "References" / "editor-ui.png",
                                shared.width, shared.height, shared.rgba));
    CHECK(shared.rgba[0] == 10);              // untouched

    // Hole B: a bless that silently no-ops (or writes the wrong file) would
    // still pass the "shared untouched" check above. Load the backend file
    // back and check the bless actually landed the new content there.
    Arcane::PixelData backend;
    REQUIRE(Arcane::LoadPngRgba(root / "Verify" / "References" / "dx12" / "editor-ui.png",
                                backend.width, backend.height, backend.rgba));
    REQUIRE(backend.Valid());
    CHECK(backend.rgba[0] == 99);
}

TEST_CASE("reference: diff artifacts land under Saved/, which is gitignored", "[reference]")
{
    const auto root = TempProject("diffpath");
    const auto p = Arcane::DiffArtifactPath(root, "runtime-scene", "vulkan");

    CHECK(p == root / "Saved" / "Verify" / "runtime-scene-vulkan-diff.png");
}

TEST_CASE("reference: a name with a path separator is REFUSED, not resolved", "[reference]")
{
    // A reference name is a NAME. Accepting "../../etc/passwd" would let a
    // command line write outside the project.
    const auto root = TempProject("traversal");
    const auto res = Arcane::ResolveReference(root, "../escape", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());           // refused: nowhere safe to write
}

// --- Hole C: NameIsSafe's guard is a conjunction of independent checks. The
// case above trips the '/' AND the ".." checks at once ("../escape"), so
// deleting either check alone still leaves the name refused and the suite
// none the wiser. Each check below is pinned with a name that trips exactly
// one -- deleting that specific check (and only that one) is what turns each
// case from PASS to FAIL.
//
// NameIsSafe actually has FOUR checks past the empty-string guard: '/', '\',
// a ".." substring, and a leading '.'. The dispatch's audit named only three
// (it reads as an oversight, not a decision) and offered "..hidden" for the
// dots case -- but "..hidden" trips BOTH the ".." check and the leading-dot
// check at once (its execution stops at the first, ".."), so it does not
// isolate either. "sub..dir" isolates the ".." check (dots in the middle, no
// leading dot); ".hidden" isolates the leading-dot check on its own.

TEST_CASE("reference: a name containing a slash (no dots) is refused", "[reference]")
{
    const auto root = TempProject("unsafe-slash");
    const auto res = Arcane::ResolveReference(root, "sub/dir", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

TEST_CASE("reference: a name containing a backslash (no dots) is refused", "[reference]")
{
    const auto root = TempProject("unsafe-backslash");
    const auto res = Arcane::ResolveReference(root, "sub\\dir", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

TEST_CASE("reference: a name with a dot-pair in the middle (no separator, no leading dot) is refused", "[reference]")
{
    const auto root = TempProject("unsafe-dotpair-mid");
    const auto res = Arcane::ResolveReference(root, "sub..dir", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

TEST_CASE("reference: a name starting with a dot (no separator, no dot-pair) is refused", "[reference]")
{
    const auto root = TempProject("unsafe-leading-dot");
    const auto res = Arcane::ResolveReference(root, ".hidden", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

TEST_CASE("reference: a backend containing a path separator is refused, not just a name", "[reference]")
{
    // ResolveReference guards both arguments, but a name-only exercise never
    // notices a broken backend guard. Task 8 sources `backend` from the host
    // (a GraphicsBackend rendered to string), not a literal, so this is not
    // hypothetical.
    const auto root = TempProject("unsafe-backend");
    const auto res = Arcane::ResolveReference(root, "editor-ui", "sub/dir");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

// --- Hole A: DiffArtifactPath applied NameIsSafe to neither argument, so
// DiffArtifactPath(root, "../../evil", "dx12") built a path escaping the
// project -- the very path Task 8 writes a diff image to on a comparison
// failure. Fixed to guard both arguments and return an empty path on
// refusal; these two cases are the ones that would have caught it.

TEST_CASE("reference: DiffArtifactPath refuses an unsafe name and returns an empty path", "[reference]")
{
    const auto root = TempProject("diffpath-unsafe-name");
    const auto p = Arcane::DiffArtifactPath(root, "../../evil", "dx12");

    CHECK(p.empty());
}

TEST_CASE("reference: DiffArtifactPath refuses an unsafe backend and returns an empty path", "[reference]")
{
    const auto root = TempProject("diffpath-unsafe-backend");
    const auto p = Arcane::DiffArtifactPath(root, "runtime-scene", "../evil");

    CHECK(p.empty());
}
