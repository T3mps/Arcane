#pragma once

// AssetId: an opaque handle to an asset -- a stable Guid at the destination. Callers
// hold an AssetId and never see a path or the raw id; Project::ResolveAsset turns it
// into a physical file via the AssetRegistry (Guid -> mount path) + the MountTable
// (mount path -> file). Slice 2 swapped the backing from a logical mount path to a
// Guid (the caller-facing shape is unchanged in spirit: an opaque, comparable handle).

#include <Arcane/Guid.hpp>

#include <cstddef>
#include <functional>

namespace Arcane
{
    class AssetId
    {
    public:
        AssetId() = default;

        static AssetId FromGuid(Guid guid) { return AssetId(guid); }

        bool IsValid() const { return m_guid.IsValid(); }

        // Seam-only accessor (AssetRegistry / Project use it to resolve). Treat AssetId
        // as opaque everywhere else.
        Guid Value() const { return m_guid; }

        bool operator==(const AssetId& o) const { return m_guid == o.m_guid; }
        bool operator!=(const AssetId& o) const { return !(*this == o); }

    private:
        explicit AssetId(Guid guid) : m_guid(guid) {}
        Guid m_guid;   // nil == invalid/unset
    };
}

template <>
struct std::hash<Arcane::AssetId>
{
    std::size_t operator()(const Arcane::AssetId& id) const noexcept
    {
        return std::hash<Arcane::Guid>{}(id.Value());
    }
};
