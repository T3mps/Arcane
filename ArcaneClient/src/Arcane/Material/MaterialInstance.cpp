#include <Arcane/Material/MaterialInstance.hpp>

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>

#include <cstring>

namespace Arcane
{
    MaterialInstance::MaterialInstance(std::shared_ptr<const MaterialTemplate> templ)
        : m_template(std::move(templ))
    {
        ARC_ASSERT(m_template != nullptr, "MaterialInstance requires a template");
    }

    MaterialInstance::MaterialInstance(std::shared_ptr<const MaterialInstance> parent)
        : m_parent(std::move(parent))
    {
        ARC_ASSERT(m_parent != nullptr, "MaterialInstance requires a parent");
    }

    const MaterialTemplate& MaterialInstance::Template() const
    {
        const MaterialInstance* i = this;
        while (i->m_parent)
            i = i->m_parent.get();
        return *i->m_template;
    }

    const MatParamValue* MaterialInstance::FindOverride(std::uint32_t nameHash) const
    {
        for (const auto& [hash, value] : m_overrides)
        {
            if (hash == nameHash)
                return &value;
        }
        return nullptr;
    }

    bool MaterialInstance::Set(std::uint32_t nameHash, const MatParamValue& value)
    {
        const ParamDecl* decl = Template().Find(nameHash);
        if (!decl || decl->type != value.type)
            return false;

        for (auto& [hash, existing] : m_overrides)
        {
            if (hash == nameHash)
            {
                if (existing != value)
                {
                    existing = value;
                    ++m_serial;
                }
                return true;
            }
        }

        m_overrides.emplace_back(nameHash, value);
        ++m_serial;
        return true;
    }

    bool MaterialInstance::HasOverride(std::uint32_t nameHash) const
    {
        return FindOverride(nameHash) != nullptr;
    }

    bool MaterialInstance::ClearOverride(std::uint32_t nameHash)
    {
        for (auto it = m_overrides.begin(); it != m_overrides.end(); ++it)
        {
            if (it->first == nameHash)
            {
                m_overrides.erase(it);
                ++m_serial;
                return true;
            }
        }
        return false;
    }

    bool MaterialInstance::GetParam(std::uint32_t nameHash, MatParamValue& out) const
    {
        // Unknown names fail up front so a stale override can never resurrect a
        // parameter the template no longer declares.
        const ParamDecl* decl = Template().Find(nameHash);
        if (!decl)
            return false;

        for (const MaterialInstance* i = this; i != nullptr; i = i->m_parent.get())
        {
            if (const MatParamValue* v = i->FindOverride(nameHash))
            {
                out = *v;
                return true;
            }
        }

        out = decl->def;
        return true;
    }

    void MaterialInstance::PackCB(std::uint8_t* dst, std::size_t dstSize) const
    {
        const MaterialTemplate& t = Template();
        if (dstSize < t.CbSize())
        {
            ARC_WARN("MaterialInstance::PackCB: dst too small ({} < {}), nothing packed",
                     dstSize, t.CbSize());
            return;
        }
        if (t.CbSize() == 0)
            return;

        // Defaults first (covers padding bytes), then resolved values per slot.
        std::memcpy(dst, t.Defaults().data(), t.CbSize());
        for (const ParamDecl& d : t.Params())
        {
            if (d.cbOffset == ParamDecl::kNoSlot)
                continue;
            MatParamValue v;
            if (GetParam(d.nameHash, v))
                std::memcpy(dst + d.cbOffset, v.f, CbByteSize(d.type));
        }
    }

    std::vector<Guid> MaterialInstance::ResolveTextures() const
    {
        const MaterialTemplate& t = Template();
        std::vector<Guid> out(t.TextureCount(), Guid::Nil());
        for (const ParamDecl& d : t.Params())
        {
            if (d.textureIndex == ParamDecl::kNoSlot)
                continue;
            MatParamValue v;
            if (GetParam(d.nameHash, v))
                out[d.textureIndex] = v.tex;
        }
        return out;
    }

    std::uint64_t MaterialInstance::EffectiveSerial() const
    {
        std::uint64_t serial = 0;
        for (const MaterialInstance* i = this; i != nullptr; i = i->m_parent.get())
            serial += i->m_serial;
        return serial;
    }
}
