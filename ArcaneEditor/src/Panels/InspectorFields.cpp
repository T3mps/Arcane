#include "Panels/InspectorFields.hpp"

// The brief's placeholder `Astra::TypeHash<T>()` does not exist -- FieldInfo::
// typeHash is populated (Detail::MakeFieldInfo, FieldInfo.hpp) from
// `Astra::TypeID<DecayedType>::Hash()`, the SAME public accessor used
// everywhere else in Astra (FieldInfo::Get/Set/GetPtr assert against it). That
// accessor lives in Core/TypeID.hpp and is transitively visible via
// FieldInfo.hpp, but it is included explicitly here for clarity.
#include <Astra/Core/TypeID.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>   // GetMeta(hash) -> TypeMeta -> EnumInfo
#include <Astra/Registry/Registry.hpp>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
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
        static const uint64_t kU32  = Astra::TypeID<uint32_t>::Hash();
        static const uint64_t kVec2 = Astra::TypeID<glm::vec2>::Hash();
        static const uint64_t kVec3 = Astra::TypeID<glm::vec3>::Hash();
        static const uint64_t kVec4 = Astra::TypeID<glm::vec4>::Hash();
        static const uint64_t kQuat = Astra::TypeID<glm::quat>::Hash();
        static const uint64_t kGuid = Astra::TypeID<Arcane::Guid>::Hash();
        static const uint64_t kStr  = Astra::TypeID<std::string>::Hash();

        // Before the hash table: an enum's hash is its OWN type's, never one of
        // the scalars below. Registered (ASTRA_REFLECT_ENUM ran somewhere in
        // this process) -> a combo has names to offer; unregistered -> nothing
        // to name the values with, so it stays ReadOnly like any unknown type.
        if (f.isEnum)
            return EnumInfoOf(f) ? FieldKind::Enum : FieldKind::ReadOnly;

        if (f.typeHash == kBool) return FieldKind::Bool;
        if (f.typeHash == kF32)  return FieldKind::Float;
        if (f.typeHash == kI32)  return FieldKind::Int32;
        if (f.typeHash == kU32)  return FieldKind::UInt32;
        if (f.typeHash == kVec2) return FieldKind::Vec2;
        if (f.typeHash == kVec3) return FieldKind::Vec3;
        if (f.typeHash == kVec4) return FieldKind::Vec4;
        if (f.typeHash == kQuat) return FieldKind::Quat;
        if (f.typeHash == kGuid) return FieldKind::AssetRef;
        if (f.typeHash == kStr)  return FieldKind::String;

        return FieldKind::ReadOnly;
    }

    const Astra::EnumInfo* EnumInfoOf(const Astra::FieldInfo& f) noexcept
    {
        const Astra::TypeMeta* meta = Astra::GetMeta(f.typeHash);
        return meta ? meta->GetEnumInfo() : nullptr;
    }

    std::int64_t ReadEnumValue(const Astra::FieldInfo& f, const void* instance) noexcept
    {
        const Astra::EnumInfo* info = EnumInfoOf(f);
        if (!info || !instance)
            return 0;
        const auto* p = static_cast<const std::byte*>(instance) + f.offset;
        const bool sign = info->isSigned;
        // f.size is the field's true byte width; memcpy at that width, then
        // widen. Sign-extension comes from going through the signed narrow
        // type; unsigned widens zero-filled.
        switch (f.size)
        {
            case 1: { if (sign) { std::int8_t  v; std::memcpy(&v, p, 1); return v; }
                      std::uint8_t  v; std::memcpy(&v, p, 1); return v; }
            case 2: { if (sign) { std::int16_t v; std::memcpy(&v, p, 2); return v; }
                      std::uint16_t v; std::memcpy(&v, p, 2); return v; }
            case 4: { if (sign) { std::int32_t v; std::memcpy(&v, p, 4); return v; }
                      std::uint32_t v; std::memcpy(&v, p, 4); return v; }
            // A uint64 enum value with the top bit set round-trips through the
            // same 8 bytes; int64 is a bit-container here, not a value claim.
            case 8: { std::int64_t v; std::memcpy(&v, p, 8); return v; }
            default: return 0;
        }
    }

    bool IsColorFieldName(std::string_view rawFieldName) noexcept
    {
        // Case-insensitive substring, no allocation.
        const auto contains = [&](std::string_view needle)
        {
            if (rawFieldName.size() < needle.size())
                return false;
            for (size_t i = 0; i + needle.size() <= rawFieldName.size(); ++i)
            {
                size_t j = 0;
                while (j < needle.size()
                       && std::tolower(static_cast<unsigned char>(rawFieldName[i + j]))
                              == needle[j])
                    ++j;
                if (j == needle.size())
                    return true;
            }
            return false;
        };
        return contains("color") || contains("tint");
    }

    void ApplyBoolEdit(const Astra::FieldInfo& f, void* instance, bool v) noexcept
    { if (bool* p = f.GetPtr<bool>(instance)) *p = v; }

    void ApplyIntEdit(const Astra::FieldInfo& f, void* instance, int v) noexcept
    { if (int32_t* p = f.GetPtr<int32_t>(instance)) *p = static_cast<int32_t>(v); }

    void ApplyUIntEdit(const Astra::FieldInfo& f, void* instance, std::uint32_t v) noexcept
    { if (uint32_t* p = f.GetPtr<uint32_t>(instance)) *p = v; }

    void ApplyEnumEdit(const Astra::FieldInfo& f, void* instance, std::int64_t v) noexcept
    {
        if (!instance || f.size == 0 || f.size > 8)
            return;
        // Low f.size bytes of the value, and ONLY those bytes: SpriteShape is
        // uint8-backed, and a wider write would stomp the neighbouring members.
        // Little-endian assumption (x64/ARM64 -- every platform this ships on):
        // the value's low bytes sit first in memory.
        auto* p = static_cast<std::byte*>(instance) + f.offset;
        std::memcpy(p, &v, f.size);
    }

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
            // Quat's STORAGE is 4 floats (w, x, y, z) -- what ComputeFieldMixed
            // below actually diffs -- even though the widget only ever shows 3
            // Euler boxes; InspectorView.cpp's Quat row collapses the 4-bit
            // result to a single mixed/not-mixed verdict rather than trying to
            // map individual raw components onto Euler axes, which have no
            // shared per-axis meaning across two DIFFERENT quaternions.
            case FieldKind::Vec4:
            case FieldKind::Quat: return 4;
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
        float        seedF[4] = {};
        // One integer seed serves Int32, UInt32 AND Enum: int64 holds every
        // value all three can carry, and the comparison below is bit-equality.
        std::int64_t seedI = 0;
        bool         seedB = false;
        Arcane::Guid seedG{};
        std::string  seedS;

        for (Astra::Entity e : selection)
        {
            void* data = reg.GetComponentByHash(e, componentHash);
            if (!data)
                continue;   // dead entity or missing component: not a voter

            float        curF[4] = {};
            std::int64_t curI = 0;
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
                case FieldKind::UInt32:
                    if (const std::uint32_t* p = f.GetPtr<std::uint32_t>(data)) curI = *p;
                    break;
                case FieldKind::Enum:
                    curI = ReadEnumValue(f, data);
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
                case FieldKind::Vec4:
                    if (const glm::vec4* p = f.GetPtr<glm::vec4>(data))
                    { curF[0] = p->x; curF[1] = p->y; curF[2] = p->z; curF[3] = p->w; }
                    break;
                case FieldKind::Quat:
                    // Raw storage components, not Euler -- see FieldComponentCount's
                    // comment on why this is the right comparison basis.
                    if (const glm::quat* p = f.GetPtr<glm::quat>(data))
                    { curF[0] = p->w; curF[1] = p->x; curF[2] = p->y; curF[3] = p->z; }
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
                case FieldKind::UInt32:
                case FieldKind::Enum:
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

    // ---- glm::quat: Euler is a VIEW, the quaternion is the STORAGE --------

    glm::vec3 QuatToEulerRadians(const glm::quat& q) noexcept
    {
        return glm::eulerAngles(q);
    }

    glm::quat QuatFromEulerRadians(const glm::vec3& eulerRadians) noexcept
    {
        return glm::normalize(glm::quat(eulerRadians));
    }

    namespace
    {
        // Whether `a` and `b` represent the SAME rotation, honouring the unit
        // quaternion's double cover (q and -q rotate identically -- see the
        // +-180 degree cases in the round-trip test, where glm::eulerAngles is
        // not sign-canonical). |dot| == 1 exactly for identical rotations;
        // the tolerance is deliberately tight -- this gate decides whether
        // SyncQuatEulerView treats `liveQuat` as "still what the view last
        // wrote", so it must not paper over a genuine external change.
        bool QuatNearlySameRotation(const glm::quat& a, const glm::quat& b) noexcept
        {
            constexpr float kTolerance = 1e-5f;
            const float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
            return std::fabs(std::fabs(d) - 1.0f) < kTolerance;
        }
    }

    glm::vec3 SyncQuatEulerView(QuatEulerView& view, const glm::quat& liveQuat) noexcept
    {
        if (!view.valid || !QuatNearlySameRotation(view.lastQuat, liveQuat))
        {
            view.eulerDisplay = QuatToEulerRadians(liveQuat);
            view.lastQuat     = liveQuat;
            view.valid        = true;
        }
        return view.eulerDisplay;
    }

    glm::quat ApplyQuatEulerEdit(QuatEulerView& view, const glm::vec3& newEulerRadians) noexcept
    {
        const glm::quat q = QuatFromEulerRadians(newEulerRadians);
        view.eulerDisplay = newEulerRadians;
        view.lastQuat     = q;
        view.valid        = true;
        return q;
    }

    glm::vec3 SyncQuatEulerViewDegrees(QuatEulerView& view, const glm::quat& liveQuat) noexcept
    {
        if (!view.valid || !QuatNearlySameRotation(view.lastQuat, liveQuat))
        {
            view.eulerDisplay = glm::degrees(QuatToEulerRadians(liveQuat));
            view.lastQuat     = liveQuat;
            view.valid        = true;
        }
        return view.eulerDisplay;
    }

    glm::quat ApplyQuatEulerEditDegrees(QuatEulerView& view,
                                        const glm::vec3& newEulerDisplayDegrees) noexcept
    {
        const glm::quat q = QuatFromEulerRadians(glm::radians(newEulerDisplayDegrees));
        // Cache the DEGREES the caller passed in verbatim -- not a
        // re-derivation from `q` -- so the next SyncQuatEulerViewDegrees call
        // reports exactly `newEulerDisplayDegrees` rather than risking a
        // different (also valid) decomposition. Same reasoning as
        // ApplyQuatEulerEdit above, one unit over.
        view.eulerDisplay = newEulerDisplayDegrees;
        view.lastQuat     = q;
        view.valid        = true;
        return q;
    }

    glm::quat QuatWithEulerAxisRadians(const glm::quat& liveQuat, int axis,
                                       float newValueRadians) noexcept
    {
        glm::vec3 e = QuatToEulerRadians(liveQuat);
        if (axis >= 0 && axis < 3)
            e[axis] = newValueRadians;
        return QuatFromEulerRadians(e);
    }
}
