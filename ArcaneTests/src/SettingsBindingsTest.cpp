// The library and system bindings (settings arc S2, spec s5): pure functions,
// pinned field by field, with defaults that ARE the library defaults (the
// fixtures and goldens unchanged), and their read sites.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/Bindings/AstraBinding.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Jobs/JobSystem.hpp>

#include "Helpers/TestTypeContext.hpp"

using namespace Arcane;

TEST_CASE("ToAstraConfig: the default settings ARE Astra's Registry::Config defaults, field by field", "[settings]")
{
    const Astra::Registry::Config lib{};
    const Astra::Registry::Config ours = ToAstraConfig(AstraMemorySettings{}, nullptr);
    CHECK(ours.chunkPoolConfig.chunkSize == lib.chunkPoolConfig.chunkSize);
    CHECK(ours.chunkPoolConfig.chunksPerBlock == lib.chunkPoolConfig.chunksPerBlock);
    CHECK(ours.chunkPoolConfig.maxChunks == lib.chunkPoolConfig.maxChunks);
    CHECK(ours.chunkPoolConfig.initialBlocks == lib.chunkPoolConfig.initialBlocks);
    CHECK(ours.chunkPoolConfig.useHugePages == lib.chunkPoolConfig.useHugePages);
    CHECK(ours.chunkPoolConfig.minChunkBytes == lib.chunkPoolConfig.minChunkBytes);
    CHECK(ours.chunkPoolConfig.maxChunkBytes == lib.chunkPoolConfig.maxChunkBytes);
    CHECK(ours.chunkPoolConfig.growDivisor == lib.chunkPoolConfig.growDivisor);
    const auto& t = ours.entityManagerConfig.tableConfig;
    const auto& lt = lib.entityManagerConfig.tableConfig;
    CHECK(t.entitiesPerSegment == lt.entitiesPerSegment);
    CHECK(t.entitiesPerSegmentShift == lt.entitiesPerSegmentShift);
    CHECK(t.entitiesPerSegmentMask == lt.entitiesPerSegmentMask);
    CHECK(t.releaseThreshold == lt.releaseThreshold);
    CHECK(t.autoRelease == lt.autoRelease);
    CHECK(t.maxEmptySegments == lt.maxEmptySegments);
    CHECK(t.maxPooledSegments == lt.maxPooledSegments);
    CHECK(t.useHugePages == lt.useHugePages);
    CHECK(ours.resourceStorageConfig.initialResourceCapacity == lib.resourceStorageConfig.initialResourceCapacity);
    CHECK(ours.workScheduler == nullptr);
}

TEST_CASE("ToAstraConfig: every field reaches its Astra field, and the scheduler is passed through", "[settings]")
{
    AstraMemorySettings s;
    s.chunkSize = 32768;
    s.chunksPerBlock = 64;
    s.maxChunks = 8192;
    s.initialBlocks = 2;
    s.chunkHugePages = false;
    s.minChunkBytes = 8192;
    s.maxChunkBytes = 262144;
    s.growDivisor = 4;
    s.entitiesPerSegment = 4096;
    s.entityReleaseThreshold = 0.25f;
    s.entityAutoRelease = false;
    s.maxEmptySegments = 3;
    s.maxPooledSegments = 7;
    s.entityHugePages = false;
    s.initialResourceCapacity = 64;
    JobSystem jobs(1);
    const std::shared_ptr<Astra::IWorkScheduler> sched = jobs.WorkScheduler();
    const Astra::Registry::Config cfg = ToAstraConfig(s, sched);
    CHECK(cfg.chunkPoolConfig.chunkSize == 32768u);
    CHECK(cfg.chunkPoolConfig.chunksPerBlock == 64u);
    CHECK(cfg.chunkPoolConfig.maxChunks == 8192u);
    CHECK(cfg.chunkPoolConfig.initialBlocks == 2u);
    CHECK_FALSE(cfg.chunkPoolConfig.useHugePages);
    CHECK(cfg.chunkPoolConfig.minChunkBytes == 8192u);
    CHECK(cfg.chunkPoolConfig.maxChunkBytes == 262144u);
    CHECK(cfg.chunkPoolConfig.growDivisor == 4u);
    const auto& t = cfg.entityManagerConfig.tableConfig;
    CHECK(t.entitiesPerSegment == 4096u);
    CHECK(t.entitiesPerSegmentShift == 12u);                    // derived by EntityManager::Config's ctor
    CHECK(t.entitiesPerSegmentMask == 4095u);
    CHECK(t.releaseThreshold == 0.25f);
    CHECK_FALSE(t.autoRelease);
    CHECK(t.maxEmptySegments == 3u);
    CHECK(t.maxPooledSegments == 7u);
    CHECK_FALSE(t.useHugePages);
    CHECK(cfg.resourceStorageConfig.initialResourceCapacity == 64u);
    CHECK(cfg.workScheduler == sched);
}

TEST_CASE("astra.memory.* are registered with the inventory's metadata and defaults", "[settings]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const auto maxChunks = reg.Explain("astra.memory.maxChunks");
    REQUIRE(maxChunks);
    CHECK(maxChunks->type == CVarType::UInt64);
    CHECK(maxChunks->published.AsUInt64() == 4096u);
    CHECK_FALSE(HasFlag(maxChunks->flags, CVarFlags::Dev));     // the one Game (not Dev) row: a world-size ceiling
    CHECK(maxChunks->scope == SettingScope::Project);
    CHECK(maxChunks->apply == ApplyMode::NextWorld);
#if !defined(ARC_BUILD_DIST)
    const auto chunkSize = reg.Explain("astra.memory.chunkSize");
    REQUIRE(chunkSize);
    CHECK(HasFlag(chunkSize->flags, CVarFlags::Dev));
    CHECK(chunkSize->published.AsUInt64() == 16384u);
    CHECK(reg.Explain("astra.memory.entitiesPerSegment")->type == CVarType::UInt32);
    CHECK(reg.Explain("astra.memory.entityReleaseThreshold")->type == CVarType::Float32);
#endif
    CHECK(Settings<AstraMemorySettings>().maxChunks == 4096u);
}

