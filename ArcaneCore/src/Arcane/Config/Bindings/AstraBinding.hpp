#pragma once

// astra.memory (settings arc S2, spec s5): Astra's Registry::Config, bound
// where Arcane creates a registry. NextWorld: a registry is built at Runtime
// construction, ResetRegistry and RestoreRegistry (Play's stop restore).
// The literals are Astra's defaults today; SettingsBindingsTest pins
// "defaults == Registry::Config{}", so a library change fails a test instead
// of silently moving Arcane's default.

#include <Arcane/Config/Settings.hpp>

#include <Astra/Core/WorkScheduler.hpp>
#include <Astra/Registry/Registry.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace Arcane
{
    struct AstraMemorySettings
    {
        std::uint64_t chunkSize               = 16384;
        std::uint64_t chunksPerBlock          = 128;
        std::uint64_t maxChunks               = 4096;
        std::uint64_t initialBlocks           = 0;
        bool          chunkHugePages          = true;
        std::uint64_t minChunkBytes           = 4096;
        std::uint64_t maxChunkBytes           = 524288;
        std::uint64_t growDivisor             = 2;
        std::uint32_t entitiesPerSegment      = 65536;
        float         entityReleaseThreshold  = 0.1f;
        bool          entityAutoRelease       = true;
        std::uint64_t maxEmptySegments        = 2;
        std::uint64_t maxPooledSegments       = 4;
        bool          entityHugePages         = true;
        std::uint64_t initialResourceCapacity = 32;
    };

    ARC_REFLECT_TYPE(AstraMemorySettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "astra.memory", SettingScope::Project, ApplyMode::NextWorld, Audience::Game)
        ARC_REFLECT_FIELD(AstraMemorySettings, chunkSize)
            ARC_REFLECT_ATTR(Range, 4096.0, 1048576.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Archetype chunk size in bytes (cache tuning).")
        ARC_REFLECT_FIELD(AstraMemorySettings, chunksPerBlock)
            ARC_REFLECT_ATTR(Range, 1.0, 4096.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Chunks allocated per pool block.")
        ARC_REFLECT_FIELD(AstraMemorySettings, maxChunks)
            ARC_REFLECT_ATTR(Range, 64.0, 1e7)
            ARC_REFLECT_ATTR(Tooltip, "Hard ceiling on archetype chunks: the world-size limit.")
        ARC_REFLECT_FIELD(AstraMemorySettings, initialBlocks)
            ARC_REFLECT_ATTR(Range, 0.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Pool blocks preallocated when a registry is created.")
        ARC_REFLECT_FIELD(AstraMemorySettings, chunkHugePages)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Try OS huge pages for chunk blocks (needs the OS permission).")
        ARC_REFLECT_FIELD(AstraMemorySettings, minChunkBytes)
            ARC_REFLECT_ATTR(Range, 4096.0, 1048576.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Smallest grow-as-populate chunk, in bytes.")
        ARC_REFLECT_FIELD(AstraMemorySettings, maxChunkBytes)
            ARC_REFLECT_ATTR(Range, 4096.0, 1048576.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Largest grow-as-populate chunk, in bytes.")
        ARC_REFLECT_FIELD(AstraMemorySettings, growDivisor)
            ARC_REFLECT_ATTR(Range, 1.0, 64.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "A new chunk is about the archetype's bytes divided by this.")
        ARC_REFLECT_FIELD(AstraMemorySettings, entitiesPerSegment)
            ARC_REFLECT_ATTR(Range, 1024.0, 65536.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Entities per entity-table segment (rounded down to a power of two).")
        ARC_REFLECT_FIELD(AstraMemorySettings, entityReleaseThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Segment use below which it is released (serialized; no Astra consumer yet).")
        ARC_REFLECT_FIELD(AstraMemorySettings, entityAutoRelease)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Release empty entity segments automatically.")
        ARC_REFLECT_FIELD(AstraMemorySettings, maxEmptySegments)
            ARC_REFLECT_ATTR(Range, 0.0, 64.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Empty entity segments kept ready.")
        ARC_REFLECT_FIELD(AstraMemorySettings, maxPooledSegments)
            ARC_REFLECT_ATTR(Range, 0.0, 64.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Entity segments pooled for reuse.")
        ARC_REFLECT_FIELD(AstraMemorySettings, entityHugePages)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Try OS huge pages for the entity table.")
        ARC_REFLECT_FIELD(AstraMemorySettings, initialResourceCapacity)
            ARC_REFLECT_ATTR(Range, 0.0, 4096.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Initial registry-resource slots.")
    ARC_END_REFLECT_TYPE()

    // The ONE Registry::Config construction path (settings arc S2). Pure.
    inline Astra::Registry::Config ToAstraConfig(const AstraMemorySettings& s,
                                                 std::shared_ptr<Astra::IWorkScheduler> scheduler)
    {
        Astra::Registry::Config cfg;
        cfg.chunkPoolConfig.chunkSize      = static_cast<std::size_t>(s.chunkSize);
        cfg.chunkPoolConfig.chunksPerBlock = static_cast<std::size_t>(s.chunksPerBlock);
        cfg.chunkPoolConfig.maxChunks      = static_cast<std::size_t>(s.maxChunks);
        cfg.chunkPoolConfig.initialBlocks  = static_cast<std::size_t>(s.initialBlocks);
        cfg.chunkPoolConfig.useHugePages   = s.chunkHugePages;
        cfg.chunkPoolConfig.minChunkBytes  = static_cast<std::size_t>(s.minChunkBytes);
        cfg.chunkPoolConfig.maxChunkBytes  = static_cast<std::size_t>(s.maxChunkBytes);
        cfg.chunkPoolConfig.growDivisor    = static_cast<std::size_t>(s.growDivisor);
        // EntityManager::Config's ctor rounds the segment size to a power of two
        // (min 1024) and derives shift and mask from it.
        cfg.entityManagerConfig = Astra::EntityManager::Config(static_cast<Astra::Entity::StorageType>(s.entitiesPerSegment));
        Astra::EntityTable::Config& table = cfg.entityManagerConfig.tableConfig;
        table.releaseThreshold  = s.entityReleaseThreshold;
        table.autoRelease       = s.entityAutoRelease;
        table.maxEmptySegments  = static_cast<std::size_t>(s.maxEmptySegments);
        table.maxPooledSegments = static_cast<std::size_t>(s.maxPooledSegments);
        table.useHugePages      = s.entityHugePages;
        cfg.resourceStorageConfig.initialResourceCapacity = static_cast<std::size_t>(s.initialResourceCapacity);
        cfg.workScheduler = std::move(scheduler);
        return cfg;
    }
}

