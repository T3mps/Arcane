#include <Arcane/Material/MaterialTemplate.hpp>

#include <Arcane/Base/Log.hpp>

#include <cstring>
#include <unordered_set>
#include <utility>

namespace Arcane
{
    namespace
    {
        constexpr std::uint32_t kRegister = 16;   // HLSL cbuffer register width (bytes)

        constexpr std::uint32_t AlignUp(std::uint32_t v, std::uint32_t a) noexcept
        {
            return (v + a - 1) / a * a;
        }
    }

    MaterialTemplate MaterialTemplate::Build(std::string name, std::uint64_t sourceHash,
                                             std::vector<ParamDecl> decls)
    {
        MaterialTemplate t;
        t.m_name = std::move(name);
        t.m_sourceHash = sourceHash;
        t.m_params.reserve(decls.size());

        std::unordered_set<std::uint32_t> seen;
        std::uint32_t offset = 0;

        for (ParamDecl& d : decls)
        {
            d.nameHash = HashParamName(d.name);
            if (!seen.insert(d.nameHash).second)
            {
                ARC_WARN("MaterialTemplate '{}': duplicate param '{}' dropped (first wins)",
                         t.m_name, d.name);
                continue;
            }

            d.cbOffset = ParamDecl::kNoSlot;
            d.textureIndex = ParamDecl::kNoSlot;

            if (d.type == MatParamType::Texture)
            {
                d.textureIndex = t.m_textureCount++;
            }
            else
            {
                // HLSL cbuffer packing: members sit on 4-byte slots, but a vector may
                // not straddle a 16-byte register boundary -- bump to the next register
                // when it would. (float4/Color can only ever fit register-aligned.)
                const std::uint32_t size = CbByteSize(d.type);
                if (offset % kRegister + size > kRegister)
                    offset = AlignUp(offset, kRegister);
                d.cbOffset = offset;
                offset += size;
            }

            t.m_params.push_back(std::move(d));
        }

        t.m_cbSize = AlignUp(offset, kRegister);

        t.m_defaults.assign(t.m_cbSize, 0);
        for (const ParamDecl& d : t.m_params)
        {
            if (d.cbOffset == ParamDecl::kNoSlot)
                continue;
            std::memcpy(t.m_defaults.data() + d.cbOffset, d.def.f, CbByteSize(d.type));
        }

        return t;
    }

    const ParamDecl* MaterialTemplate::Find(std::uint32_t nameHash) const
    {
        for (const ParamDecl& d : m_params)
        {
            if (d.nameHash == nameHash)
                return &d;
        }
        return nullptr;
    }
}
