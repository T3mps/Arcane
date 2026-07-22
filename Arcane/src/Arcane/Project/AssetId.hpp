#pragma once

// AssetId: an opaque handle to an asset. Callers hold an AssetId and never see a
// path or a raw id. Slice 1 backing = a logical mount path ("game://a.png"); Slice 2
// swaps the backing to a Guid + AssetRegistry WITHOUT changing this interface.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace Arcane
{
    class AssetId
    {
    public:
        AssetId() = default;

        static AssetId FromMountPath(std::string_view mountPath)
        {
            return AssetId(std::string(mountPath));
        }

        bool IsValid() const { return !m_key.empty(); }

        // Seam-only accessor (MountTable / Project use it to resolve). NOT a public
        // path API for callers -- treat AssetId as opaque everywhere else.
        const std::string& Key() const { return m_key; }

        bool operator==(const AssetId& o) const { return m_key == o.m_key; }
        bool operator!=(const AssetId& o) const { return !(*this == o); }

    private:
        explicit AssetId(std::string key) : m_key(std::move(key)) {}
        std::string m_key;   // Slice 1: the logical mount path; opaque to callers
    };
}

template <>
struct std::hash<Arcane::AssetId>
{
    std::size_t operator()(const Arcane::AssetId& id) const noexcept
    {
        return std::hash<std::string>{}(id.Key());
    }
};
