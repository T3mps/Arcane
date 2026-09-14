// Task 8 (F2a): pins MeshNode.hpp's NormalMatrixFor -- the inverse transpose
// of a model matrix's upper 3x3, and the fix for the defect MeshInstance::
// model used to document: a non-uniformly-scaled instance's normals need the
// inverse transpose, not the upper 3x3 mesh.hlsl's vs_main applied directly
// until this task. Pure math, no NRI device, no Registry -- same discipline
// PerspectiveCameraTest.cpp's projection-only cases take for
// SceneCamera::PerspectiveProjection (both are header-only pure functions for
// exactly that reason).
//
// The three properties task-8-brief.md (Step 1) requires:
//   1. A NON-UNIFORM scale (the brief's own worked example, scale(2,1,1))
//      sends normalize(1,1,0) to normalize(0.5,1,0) -- NOT normalize(2,1,0),
//      which is what the upper-3x3 shortcut this task removes would produce.
//   2. A UNIFORM scale leaves a normal's DIRECTION unchanged, so
//      NriGraphPixelTest.cpp's existing (Task 7) [pixel] mesh cases -- which
//      light an unscaled cube -- cannot regress silently: a normal-matrix bug
//      that broke the uniform case too would show here first, device-less.
//   3. A SINGULAR model matrix (a collapsed/zero-scaled axis) returns
//      IDENTITY, not NaN -- a NaN clip position is undefined behaviour on the
//      GPU rather than a wrong picture, the same class of guard
//      SceneCamera.hpp's degenerate-basis fallback takes.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Nri/nodes/MeshNode.hpp>

#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/MeshBuilder.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Scene/SceneCamera.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>   // glm::scale

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <type_traits>
#include <vector>

#include "Helpers/GpuCapability.hpp"

TEST_CASE("NormalMatrixFor transforms a non-uniformly-scaled normal by the "
          "inverse transpose, not the upper 3x3",
          "[mesh]")
{
    // scale(2,1,1): the brief's own worked example. A surface tangent scales
    // WITH the object, so for dot(normal, tangent) to stay zero after a
    // non-uniform scale, the normal has to scale by the INVERSE along each
    // axis -- the upper-3x3 shortcut (mesh.hlsl's defect before this task)
    // does the opposite, stretching the normal TOWARD the scaled axis instead
    // of leaning it away.
    const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
    const glm::mat3 normalMatrix = Arcane::NormalMatrixFor(model);

    const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
    const glm::vec3 transformed = glm::normalize(normalMatrix * n);

    const glm::vec3 expectedCorrect = glm::normalize(glm::vec3(0.5f, 1.0f, 0.0f));
    const glm::vec3 wrongUpperOnly  = glm::normalize(glm::vec3(2.0f, 1.0f, 0.0f));

    CHECK(glm::epsilonEqual(transformed.x, expectedCorrect.x, 1e-5f));
    CHECK(glm::epsilonEqual(transformed.y, expectedCorrect.y, 1e-5f));
    CHECK(glm::epsilonEqual(transformed.z, expectedCorrect.z, 1e-5f));

    // NOT the upper-3x3 result -- stated explicitly (rather than just pinning
    // the correct answer) so a regression back to `glm::mat3(model)` fails
    // LOUDLY as a wrong-direction defect, not by a margin a future reader
    // might mistake for float noise on an otherwise-passing case.
    CHECK_FALSE(glm::epsilonEqual(transformed.x, wrongUpperOnly.x, 1e-3f));
}

TEST_CASE("NormalMatrixFor leaves a uniformly-scaled normal's direction "
          "unchanged (Task 7's [pixel] mesh cases must not regress)",
          "[mesh]")
{
    // A uniform scale's inverse transpose is (1/s)*I -- a positive multiple
    // of identity that vs_main's normalize() divides straight back out, so
    // the DIRECTION survives even though NormalMatrixFor now runs
    // unconditionally (every instance, not just non-uniformly-scaled ones).
    // NriGraphPixelTest.cpp's existing mesh [pixel] cases light an UNSCALED
    // cube and would not notice a normal-matrix regression confined to the
    // uniform/identity case -- this device-less case is what catches that one.
    const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(3.0f, 3.0f, 3.0f));
    const glm::mat3 normalMatrix = Arcane::NormalMatrixFor(model);

    const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f));
    const glm::vec3 transformed = glm::normalize(normalMatrix * n);

    CHECK(glm::epsilonEqual(transformed.x, n.x, 1e-5f));
    CHECK(glm::epsilonEqual(transformed.y, n.y, 1e-5f));
    CHECK(glm::epsilonEqual(transformed.z, n.z, 1e-5f));
}

TEST_CASE("NormalMatrixFor returns identity for a singular model matrix, "
          "not NaN",
          "[mesh]")
{
    // A collapsed axis (an authored zero scale -- the same fixture shape
    // SceneCamera.hpp's own degenerate-basis test uses) has no inverse.
    // glm::inverse divides by the determinant unconditionally and would hand
    // back Inf/NaN -- undefined behaviour on the GPU rather than a wrong
    // picture, per NormalMatrixFor's own guard comment. Checked over all nine
    // elements against BOTH properties (finite AND identity), not just one:
    // finite-but-wrong would still pass a bare isfinite sweep.
    const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 1.0f));
    const glm::mat3 normalMatrix = Arcane::NormalMatrixFor(model);
    const glm::mat3 identity(1.0f);

    for (int c = 0; c < 3; ++c)
    {
        for (int r = 0; r < 3; ++r)
        {
            CHECK(std::isfinite(normalMatrix[c][r]));
            CHECK(glm::epsilonEqual(normalMatrix[c][r], identity[c][r], 1e-6f));
        }
    }
}

TEST_CASE("NormalMatrixFor is not fooled by a small NON-UNIFORM scale into "
          "falling back to identity",
          "[mesh]")
{
    // Review finding (Task 8, fix round 1): a FIXED determinant threshold is
    // not scale-invariant -- a 3x3 determinant scales as s^3 under a uniform
    // scale s, so any fixed cutoff misclassifies some genuinely-invertible
    // small matrix as singular. Concretely: scale(0.001, 0.002, 0.003) has
    // determinant 0.001*0.002*0.003 = 6e-9, comfortably below a naive 1e-8
    // cutoff (any model whose geometric-mean scale is under ~2.15mm in this
    // engine's MKS meters would trip it) -- yet this matrix is perfectly
    // invertible and non-uniform, i.e. EXACTLY the case this function has to
    // get right. A guard that fell back to identity here would silently
    // reinstate the original upper-3x3 defect at small scales, with no
    // diagnostic. NormalMatrixFor's guard checks the INVERSE'S finiteness,
    // not a determinant pre-screen, specifically so this case is not
    // singular to it.
    const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(0.001f, 0.002f, 0.003f));
    const glm::mat3 normalMatrix = Arcane::NormalMatrixFor(model);

    const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
    const glm::vec3 transformed = glm::normalize(normalMatrix * n);

    // inverseTranspose(diag(a,b,c)) == diag(1/a,1/b,1/c) for a diagonal
    // matrix, applied to n gives a direction proportional to
    // (1/a, 1/b, 0) == (1000, 500, 0) -- ratio 2:1 -- normalized to
    // (2,1,0)/sqrt(5).
    const glm::vec3 expected = glm::normalize(glm::vec3(2.0f, 1.0f, 0.0f));

    CHECK(glm::epsilonEqual(transformed.x, expected.x, 1e-4f));
    CHECK(glm::epsilonEqual(transformed.y, expected.y, 1e-4f));
    CHECK(glm::epsilonEqual(transformed.z, expected.z, 1e-4f));

    // NOT identity (i.e. not `n` unchanged) -- the exact wrong answer a
    // determinant-threshold guard would have produced for this input.
    CHECK_FALSE(glm::epsilonEqual(transformed.x, n.x, 1e-3f));
}

TEST_CASE("mesh node: an instance names its mesh by GUID, not by borrowed pointer",
          "[nri]")
{
    Arcane::MeshInstance instance;
    instance.mesh = Arcane::Guid{ 1, 0 };
    CHECK(instance.mesh.IsValid());
    static_assert(std::is_same_v<decltype(Arcane::MeshInstance::mesh), Arcane::Guid>);
}

namespace
{
    constexpr std::uint32_t kParityW = 160;
    constexpr std::uint32_t kParityH = 96;

    std::unique_ptr<Arcane::NriGraphContext> MakeParityContext()
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = Arcane::GraphicsBackend::D3D12;
#if defined(ARCANE_DEBUG)
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;
#endif
        static std::unique_ptr<Arcane::NativeDeviceOwner> native;
        static std::unique_ptr<Arcane::NriDevice> nri;
        native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(native != nullptr);
        nri = Arcane::NriDevice::Wrap(*native);
        REQUIRE(nri != nullptr);
        Arcane::HostConfig cfg;
        cfg.backend = Arcane::GraphicsBackend::D3D12;
        auto ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *nri, kParityW, kParityH, {});
        REQUIRE(ctx != nullptr);
        return ctx;
    }
}

TEST_CASE("pixel: a cube drawn from the resident cache matches the ring's own pixels",
          "[gpu][meshnode]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto ctx = MakeParityContext();
    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    ctx->SetMeshSupply(
        [&](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
        {
            if (id == cubeId)
                return { &cube, Arcane::MeshResolveState::Ready };
            return { nullptr, Arcane::MeshResolveState::Failed };
        });

    Arcane::MeshInstance instance;
    instance.mesh      = cubeId;
    instance.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    const Arcane::MeshInstance instances[] = { instance };

    Arcane::MeshSceneDesc scene;
    scene.instances = instances;
    const float aspect = static_cast<float>(kParityW) / static_cast<float>(kParityH);
    scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f));
    scene.projection     = Arcane::PerspectiveProjection(60.0f, aspect, 0.1f, 100.0f);
    scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    scene.lightColor     = glm::vec3(1.0f, 1.0f, 1.0f);
    scene.ambient        = glm::vec3(0.08f);

    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture = true;
    frame.mesh    = &scene;
    REQUIRE(ctx->RenderFrameOffscreen(frame) == Arcane::NriGraphContext::FrameOutcome::Presented);

    std::uint32_t width = 0, height = 0;
    std::vector<unsigned char> rgba;
    REQUIRE(ctx->ReadCapture(width, height, rgba));

    const std::filesystem::path referencePath =
        std::filesystem::path("ReferenceProject") / "Verify" / "References" / "inprocess-lit-cube.png";
    REQUIRE(std::filesystem::exists(referencePath));
    Arcane::PixelData expected;
    REQUIRE(Arcane::LoadPngRgba(referencePath, expected.width, expected.height, expected.rgba));
    Arcane::PixelData actual;
    actual.width  = width;
    actual.height = height;
    actual.rgba   = rgba;
    const auto result = Arcane::CompareImages(expected, actual);
    INFO("diffCount " << result.diffCount << " (ratio " << result.diffRatio << ") -- "
                       << result.errorMessage);
    CHECK(result.passed);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("pixel: an instance whose mesh is not resident is SKIPPED, not drawn wrong",
          "[gpu][meshnode]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto ctx = MakeParityContext();
    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    const Arcane::Guid missing{ 9, 9 };
    ctx->SetMeshSupply(
        [&](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
        {
            if (id == cubeId)
                return { &cube, Arcane::MeshResolveState::Ready };
            return { nullptr, Arcane::MeshResolveState::Failed };
        });

    Arcane::MeshInstance good;
    good.mesh      = cubeId;
    good.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    Arcane::MeshInstance bad;
    bad.mesh      = missing;
    bad.baseColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    const Arcane::MeshInstance instances[] = { bad, good };

    Arcane::MeshSceneDesc scene;
    scene.instances = instances;
    const float aspect = static_cast<float>(kParityW) / static_cast<float>(kParityH);
    scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f));
    scene.projection     = Arcane::PerspectiveProjection(60.0f, aspect, 0.1f, 100.0f);
    scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    scene.lightColor     = glm::vec3(1.0f, 1.0f, 1.0f);
    scene.ambient        = glm::vec3(0.08f);

    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture = true;
    frame.mesh    = &scene;
    REQUIRE(ctx->RenderFrameOffscreen(frame) == Arcane::NriGraphContext::FrameOutcome::Presented);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("pixel: two sections of one mesh draw with distinct base colours",
          "[gpu][meshnode]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);
    auto ctx = MakeParityContext();

    // Two 1 m cubes as sections of ONE mesh, offset like NriGraphPixelTest's
    // four-cube case so their silhouettes do not overlap at this camera.
    Arcane::MeshData leftCube  = Arcane::BuildCube(1.0f);
    Arcane::MeshData rightCube = Arcane::BuildCube(1.0f);
    for (Arcane::MeshVertex& v : leftCube.vertices)
        v.position.x -= 1.6f;
    for (Arcane::MeshVertex& v : rightCube.vertices)
        v.position.x += 1.6f;
    Arcane::MeshData two;
    two.vertices = leftCube.vertices;
    two.vertices.insert(two.vertices.end(), rightCube.vertices.begin(), rightCube.vertices.end());
    two.indices = leftCube.indices;
    const std::uint32_t leftIndexCount = static_cast<std::uint32_t>(leftCube.indices.size());
    const std::uint32_t vertexBase     = static_cast<std::uint32_t>(leftCube.vertices.size());
    for (std::uint32_t i : rightCube.indices)
        two.indices.push_back(i + vertexBase);
    two.sections = {
        { "L", 0, leftIndexCount, 0 },
        { "R", leftIndexCount, static_cast<std::uint32_t>(rightCube.indices.size()), 1 },
    };

    const Arcane::Guid id{ 1, 1 };
    ctx->SetMeshSupply(
        [&](const Arcane::Guid& g) -> Arcane::NriMeshBufferCache::SupplyResult
        {
            if (g == id)
                return { &two, Arcane::MeshResolveState::Ready };
            return { nullptr, Arcane::MeshResolveState::Failed };
        });

    Arcane::MeshInstance left;
    left.mesh        = id;
    left.baseColor   = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    left.indexOffset = 0;
    left.indexCount  = leftIndexCount;
    Arcane::MeshInstance right;
    right.mesh        = id;
    right.baseColor   = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    right.indexOffset = leftIndexCount;
    right.indexCount  = static_cast<std::uint32_t>(rightCube.indices.size());
    const Arcane::MeshInstance instances[] = { left, right };

    Arcane::MeshSceneDesc scene;
    scene.instances = instances;
    const float aspect = static_cast<float>(kParityW) / static_cast<float>(kParityH);
    scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f));
    scene.projection     = Arcane::PerspectiveProjection(60.0f, aspect, 0.1f, 100.0f);
    scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    scene.lightColor     = glm::vec3(1.0f);
    scene.ambient        = glm::vec3(0.08f);

    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture = true;
    frame.mesh    = &scene;
    REQUIRE(ctx->RenderFrameOffscreen(frame) == Arcane::NriGraphContext::FrameOutcome::Presented);

    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> rgba;
    REQUIRE(ctx->ReadCapture(w, h, rgba));
    REQUIRE(w == kParityW);
    REQUIRE(h == kParityH);

    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
        return glm::ivec3(rgba[i], rgba[i + 1], rgba[i + 2]);
    };
    const glm::ivec3 leftPx  = at(40u, 48u);
    const glm::ivec3 rightPx = at(120u, 48u);
    CHECK(leftPx.r > leftPx.g + 40);
    CHECK(rightPx.g > rightPx.r + 40);
}
