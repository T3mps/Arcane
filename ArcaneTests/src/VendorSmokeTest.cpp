// Vendored-dependency smoke tests. One test case per ThirdParty dep proving
// it compiles, links, and does one real operation headlessly (no window,
// no GPU, no audio device). Grown task-by-task during M0; the Playground
// (M3) is the *integration* test -- these are arrival gates.

#include <catch2/catch_test_macros.hpp>

// ---------------------------------------------------------------- glm
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

TEST_CASE("glm: vector and matrix math", "[vendor][glm]")
{
    glm::vec3 a(1.0f, 0.0f, 0.0f);
    glm::vec3 b(0.0f, 1.0f, 0.0f);
    REQUIRE(glm::dot(a, b) == 0.0f);
    REQUIRE(glm::cross(a, b) == glm::vec3(0.0f, 0.0f, 1.0f));
}

// ---------------------------------------------------------------- stb
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <cstring>
#include <vector>

namespace {
    void CollectPng(void* ctx, void* data, int size)
    {
        auto* out = static_cast<std::vector<unsigned char>*>(ctx);
        out->insert(out->end(), static_cast<unsigned char*>(data),
                    static_cast<unsigned char*>(data) + size);
    }
}

TEST_CASE("stb: PNG write -> read round-trip in memory", "[vendor][stb]")
{
    const unsigned char pixels[2 * 2 * 4] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    std::vector<unsigned char> png;
    REQUIRE(stbi_write_png_to_func(&CollectPng, &png, 2, 2, 4, pixels, 2 * 4) != 0);

    int w = 0, h = 0, comp = 0;
    unsigned char* decoded = stbi_load_from_memory(png.data(), (int)png.size(),
                                                   &w, &h, &comp, 4);
    REQUIRE(decoded != nullptr);
    REQUIRE(w == 2);
    REQUIRE(h == 2);
    REQUIRE(std::memcmp(decoded, pixels, sizeof(pixels)) == 0);
    stbi_image_free(decoded);
}

// ---------------------------------------------------------------- miniaudio
// Implementation TU lives in MiniaudioImpl.cpp to keep this file readable.
extern "C" const char* ma_version_string(void);

TEST_CASE("miniaudio: implementation links and reports a version", "[vendor][miniaudio]")
{
    const char* v = ma_version_string();
    REQUIRE(v != nullptr);
    REQUIRE(v[0] == '0'); // 0.11.x series
}

// ---------------------------------------------------------------- Astra
#include <Astra/Astra.hpp>

namespace {
    struct SmokePosition { float x, y, z; };
}

TEST_CASE("Astra: registry create/get round-trip", "[vendor][astra]")
{
    Astra::Registry registry;
    Astra::Entity e = registry.CreateEntityWith(SmokePosition{1.0f, 2.0f, 3.0f});

    SmokePosition* p = registry.GetComponent<SmokePosition>(e);
    REQUIRE(p != nullptr);
    REQUIRE(p->x == 1.0f);
    REQUIRE(p->z == 3.0f);
}

// ---------------------------------------------------------------- enkiTS
#include <TaskScheduler.h>
#include <atomic>

TEST_CASE("enkiTS: parallel task set sums a range", "[vendor][enkits]")
{
    enki::TaskScheduler scheduler;
    scheduler.Initialize();

    std::atomic<uint32_t> count{0};
    enki::TaskSet task(10000, [&](enki::TaskSetPartition range, uint32_t) {
        count.fetch_add(range.end - range.start, std::memory_order_relaxed);
    });
    scheduler.AddTaskSetToPipe(&task);
    scheduler.WaitforTask(&task);

    REQUIRE(count.load() == 10000);
}

// ---------------------------------------------------------------- FreeType
#include <ft2build.h>
#include FT_FREETYPE_H

TEST_CASE("FreeType: library init/done and version", "[vendor][freetype]")
{
    FT_Library lib = nullptr;
    REQUIRE(FT_Init_FreeType(&lib) == 0);

    FT_Int maj = 0, min = 0, patch = 0;
    FT_Library_Version(lib, &maj, &min, &patch);
    REQUIRE(maj == 2);
    REQUIRE(min >= 13);

    REQUIRE(FT_Done_FreeType(lib) == 0);
}

