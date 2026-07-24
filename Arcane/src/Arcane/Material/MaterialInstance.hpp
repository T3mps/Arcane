#pragma once

// MaterialInstance: the VALUES side of a material -- a sparse override list over a
// parent (either the MaterialTemplate directly, or another instance, UE's
// UMaterialInstance chain). GetParam resolves my override -> parent chain ->
// template default through one seam; PackCB is the resolve-then-memcpy loop that
// fills the material cbuffer bytes; textures resolve the same way into the
// parallel texture table (Guids -- the Assets facade turns them into GPU handles
// at bind time, Slice 4). EffectiveSerial() folds the parent chain's dirty serials
// into one monotonic stamp so callers re-pack only when something they resolve
// from actually changed.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Material/MaterialTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API MaterialInstance
    {
    public:
        // Root instance: parented directly on the template.
        explicit MaterialInstance(std::shared_ptr<const MaterialTemplate> templ);
        // Derived instance: parented on another instance (template reached via the chain).
        explicit MaterialInstance(std::shared_ptr<const MaterialInstance> parent);

        // The root template (walks the parent chain).
        const MaterialTemplate& Template() const;

        // Sparse override write. The name must exist in the template with the SAME
        // declared type; returns false (no-op) otherwise. Accepted writes bump the
        // dirty serial (redundant same-value writes do not).
        bool Set(std::uint32_t nameHash, const MatParamValue& value);
        bool Set(std::string_view name, const MatParamValue& value) { return Set(HashParamName(name), value); }

        bool SetFloat(std::string_view name, float x)                                { return Set(name, MatParamValue::MakeFloat(x)); }
        bool SetFloat2(std::string_view name, float x, float y)                      { return Set(name, MatParamValue::MakeFloat2(x, y)); }
        bool SetFloat4(std::string_view name, float x, float y, float z, float w)    { return Set(name, MatParamValue::MakeFloat4(x, y, z, w)); }
        bool SetColor(std::string_view name, float r, float g, float b, float a = 1.0f) { return Set(name, MatParamValue::MakeColor(r, g, b, a)); }
        bool SetTexture(std::string_view name, Guid id)                              { return Set(name, MatParamValue::MakeTexture(id)); }

        bool HasOverride(std::uint32_t nameHash) const;
        bool HasOverride(std::string_view name) const { return HasOverride(HashParamName(name)); }
        // Remove my own override (parent/default shows through again). True if removed.
        bool ClearOverride(std::uint32_t nameHash);
        bool ClearOverride(std::string_view name) { return ClearOverride(HashParamName(name)); }

        std::size_t OverrideCount() const { return m_overrides.size(); }

        // Resolve: my override -> parent chain -> template default. False for a name
        // the template does not declare.
        bool GetParam(std::uint32_t nameHash, MatParamValue& out) const;
        bool GetParam(std::string_view name, MatParamValue& out) const { return GetParam(HashParamName(name), out); }

        // The pack loop: copies the template defaults blob, then writes every numeric
        // param's RESOLVED value at its cbOffset. dst must hold at least
        // Template().CbSize() bytes; packs nothing (and warns) if it doesn't.
        void PackCB(std::uint8_t* dst, std::size_t dstSize) const;

        // The parallel texture table: resolved Guid per texture ordinal
        // (index == ParamDecl::textureIndex; size == Template().TextureCount()).
        std::vector<Guid> ResolveTextures() const;

        // Monotonic change stamp folding in the whole parent chain -- if two calls
        // return the same value, a cached PackCB result is still valid.
        std::uint64_t EffectiveSerial() const;

    private:
        const MatParamValue* FindOverride(std::uint32_t nameHash) const;

        std::shared_ptr<const MaterialTemplate> m_template;   // set iff root-parented
        std::shared_ptr<const MaterialInstance> m_parent;     // set iff instance-parented
        std::vector<std::pair<std::uint32_t, MatParamValue>> m_overrides;
        std::uint64_t m_serial = 0;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
