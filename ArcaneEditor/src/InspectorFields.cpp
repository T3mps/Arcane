#include "InspectorFields.hpp"

// The brief's placeholder `Astra::TypeHash<T>()` does not exist -- FieldInfo::
// typeHash is populated (Detail::MakeFieldInfo, FieldInfo.hpp) from
// `Astra::TypeID<DecayedType>::Hash()`, the SAME public accessor used
// everywhere else in Astra (FieldInfo::Get/Set/GetPtr assert against it). That
// accessor lives in Core/TypeID.hpp and is transitively visible via
// FieldInfo.hpp, but it is included explicitly here for clarity.
#include <Astra/Core/TypeID.hpp>
#include <Astra/Registry/Registry.hpp>

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>

namespace Arcane::Editor
{
    FieldKind ClassifyField(const Astra::FieldInfo& f) noexcept
    {
        if (f.isPointer) return FieldKind::ReadOnly;

        // bool IS distinguishable from int8 by hash: TypeID<T>::Hash() hashes the
        // compiler-derived type name ("bool" vs "signed char"/"__int8"/"char"),
        // so a direct comparison against TypeID<bool>::Hash() is exact -- no
        // separate size==1 fallback is needed. (Verified by reading
        // ThirdParty/Astra/include/Astra/Core/TypeID.hpp: FieldInfo::typeHash and
        // this accessor both resolve to Detail::TypeHash<T>() = XXHash64 of the
        // compile-time type-name string, so they are guaranteed to agree for the
        // same T and to differ for distinctly-named types.)
        static const uint64_t kBool = Astra::TypeID<bool>::Hash();
        static const uint64_t kF32  = Astra::TypeID<float>::Hash();
        static const uint64_t kI32  = Astra::TypeID<int32_t>::Hash();
        static const uint64_t kVec2 = Astra::TypeID<glm::vec2>::Hash();
        static const uint64_t kVec3 = Astra::TypeID<glm::vec3>::Hash();
        static const uint64_t kGuid = Astra::TypeID<Arcane::Guid>::Hash();
        static const uint64_t kStr  = Astra::TypeID<std::string>::Hash();

        if (f.typeHash == kBool) return FieldKind::Bool;
        if (f.typeHash == kF32)  return FieldKind::Float;
        if (f.typeHash == kI32)  return FieldKind::Int32;
        if (f.typeHash == kVec2) return FieldKind::Vec2;
        if (f.typeHash == kVec3) return FieldKind::Vec3;
        if (f.typeHash == kGuid) return FieldKind::AssetRef;
        if (f.typeHash == kStr)  return FieldKind::String;

        return FieldKind::ReadOnly;
    }

    void ApplyBoolEdit(const Astra::FieldInfo& f, void* instance, bool v) noexcept
    { if (bool* p = f.GetPtr<bool>(instance)) *p = v; }

    void ApplyIntEdit(const Astra::FieldInfo& f, void* instance, int v) noexcept
    { if (int32_t* p = f.GetPtr<int32_t>(instance)) *p = static_cast<int32_t>(v); }

    void ApplyFloatEdit(const Astra::FieldInfo& f, void* instance, float v) noexcept
    { if (float* p = f.GetPtr<float>(instance)) *p = v; }

    void ApplyGuidEdit(const Astra::FieldInfo& f, void* instance, const Arcane::Guid& v) noexcept
    { if (Arcane::Guid* p = f.GetPtr<Arcane::Guid>(instance)) *p = v; }

    void ApplyStringEdit(const Astra::FieldInfo& f, void* instance, const std::string& v) noexcept
    { if (std::string* p = f.GetPtr<std::string>(instance)) *p = v; }

    int FieldComponentCount(FieldKind kind) noexcept
    {
        switch (kind)
        {
            case FieldKind::Vec3: return 3;
            case FieldKind::Vec2: return 2;
            default:              return 1;
        }
    }

    FieldMixedMask ComputeFieldMixed(Astra::Registry& reg,
                                     std::span<const Astra::Entity> selection,
                                     std::uint64_t componentHash,
                                     const Astra::FieldInfo& f)
    {
        FieldMixedMask mask;
        const FieldKind kind = ClassifyField(f);
        if (kind == FieldKind::ReadOnly)
            return mask;   // nothing comparable: never mixed
        const int count = FieldComponentCount(kind);

        // Seed from the FIRST LIVE CARRIER, then mark any later disagreement --
        // UE's CacheDetails shape. `seeded` stands in for UE's "ObjectIndex == 0"
        // branch, which is not simply index 0 here because a selection entry can
        // be dead or lack the component.
        bool seeded = false;
        float        seedF[3] = {};
        std::int32_t seedI = 0;
        bool         seedB = false;
        Arcane::Guid seedG{};
        std::string  seedS;

        for (Astra::Entity e : selection)
        {
            void* data = reg.GetComponentByHash(e, componentHash);
            if (!data)
                continue;   // dead entity or missing component: not a voter

            float        curF[3] = {};
            std::int32_t curI = 0;
            bool         curB = false;
            Arcane::Guid curG{};
            std::string  curS;

            switch (kind)
            {
                case FieldKind::Bool:
                    if (const bool* p = f.GetPtr<bool>(data)) curB = *p;
                    break;
                case FieldKind::Int32:
                    if (const std::int32_t* p = f.GetPtr<std::int32_t>(data)) curI = *p;
                    break;
                case FieldKind::Float:
                    if (const float* p = f.GetPtr<float>(data)) curF[0] = *p;
                    break;
                case FieldKind::Vec2:
                    if (const glm::vec2* p = f.GetPtr<glm::vec2>(data))
                    { curF[0] = p->x; curF[1] = p->y; }
                    break;
                case FieldKind::Vec3:
                    if (const glm::vec3* p = f.GetPtr<glm::vec3>(data))
                    { curF[0] = p->x; curF[1] = p->y; curF[2] = p->z; }
                    break;
                case FieldKind::AssetRef:
                    if (const Arcane::Guid* p = f.GetPtr<Arcane::Guid>(data)) curG = *p;
                    break;
                case FieldKind::String:
                    if (const std::string* p = f.GetPtr<std::string>(data)) curS = *p;
                    break;
                default:
                    return mask;
            }

            if (!seeded)
            {
                seeded = true;
                seedB = curB; seedI = curI; seedG = curG; seedS = curS;
                for (int i = 0; i < count; ++i) seedF[i] = curF[i];
                continue;
            }

            // Exact comparison on purpose: the question is "did the user author
            // the same value", not "are these near each other". UE compares with
            // == too (Loc.X == CurLoc.X).
            switch (kind)
            {
                case FieldKind::Bool:
                    if (curB != seedB) mask.bits |= 1u;
                    break;
                case FieldKind::Int32:
                    if (curI != seedI) mask.bits |= 1u;
                    break;
                case FieldKind::AssetRef:
                    if (curG != seedG) mask.bits |= 1u;
                    break;
                case FieldKind::String:
                    if (curS != seedS) mask.bits |= 1u;
                    break;
                default:
                    for (int i = 0; i < count; ++i)
                        if (curF[i] != seedF[i])
                            mask.bits |= (1u << i);   // sticky: never cleared
                    break;
            }
        }
        return mask;
    }
}
