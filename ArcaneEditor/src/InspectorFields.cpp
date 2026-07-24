#include "InspectorFields.hpp"

// The brief's placeholder `Astra::TypeHash<T>()` does not exist -- FieldInfo::
// typeHash is populated (Detail::MakeFieldInfo, FieldInfo.hpp) from
// `Astra::TypeID<DecayedType>::Hash()`, the SAME public accessor used
// everywhere else in Astra (FieldInfo::Get/Set/GetPtr assert against it). That
// accessor lives in Core/TypeID.hpp and is transitively visible via
// FieldInfo.hpp, but it is included explicitly here for clarity.
#include <Astra/Core/TypeID.hpp>

#include <cstdint>
#include <glm/glm.hpp>

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

        if (f.typeHash == kBool) return FieldKind::Bool;
        if (f.typeHash == kF32)  return FieldKind::Float;
        if (f.typeHash == kI32)  return FieldKind::Int32;
        if (f.typeHash == kVec2) return FieldKind::Vec2;
        if (f.typeHash == kVec3) return FieldKind::Vec3;
        if (f.typeHash == kGuid) return FieldKind::AssetRef;

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
}
