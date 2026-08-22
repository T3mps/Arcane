#pragma once

// Reflection -> JSON bridge: the first consumer of Astra 3.2's format-agnostic
// IFieldVisitor seam. Drives any reflected component to/from nlohmann::json by
// walking FieldInfo. Astra owns no JSON; this lives entirely in Arcane.
//
// Value dispatch: arithmetic/bool/string via typed accessors; enums by name via
// EnumInfo (raw memory read for the underlying integer); glm::vec2/3/4, mat3/mat4
// and quaternions as JSON number arrays (POD-math fallback); nested reflected
// structs recurse via MetaRegistry. Reader falls back to AliasName for renames; a
// missing key leaves the default-constructed value (forward/back compatible).
//
// No silent drops: a field whose TYPE the bridge cannot represent (containers,
// raw pointers, an unreflected non-math struct) is NOT quietly skipped -- the
// visitor latches an "unsupported field type" error the caller must inspect via
// HasError()/Error(). This keeps the exception-free engine from losing data
// without a diagnostic.
//
// READ TOLERANCE -- the contract turns on ABSENT vs PRESENT-BUT-UNREADABLE, and
// those are deliberately different answers (tightened by Task 3, F1):
//   * a MISSING key leaves the field at its default. Unchanged, and it is the
//     whole forward/back-compatibility story: a file written before a field
//     existed, or after one was removed, still loads.
//   * a key that IS PRESENT but that this bridge cannot read into the field --
//     wrong JSON type, an array of the wrong ARITY, a non-object where a nested
//     struct belongs, a non-unit quaternion -- LATCHES, exactly like an
//     unsupported field type. That is not forward compatibility; it is data
//     loss with the witness sitting right there in the file.
// Before that split, a `"position": [x, y]` left over from the 2D Transform read
// as though the key were ABSENT: every entity in the scene loaded at the origin,
// HasError() stayed false, and nothing anywhere said so. Loading a stale file as
// a silently-zeroed scene is worse than refusing it.
//
// Still tolerated on purpose, because it is a VOCABULARY question and not a
// SHAPE one -- the same call the loader already makes for an unknown component
// type name: an enum whose stored NAME no longer resolves keeps its default. The
// node still has to BE a string.

#include <Astra/Reflection/FieldVisitor.hpp>
#include <Astra/Reflection/FieldInfo.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Core/TypeID.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace Arcane
{
    namespace Detail
    {
        // Math-type hashes computed once (XXHash64 of the type name, cross-module stable).
        inline uint64_t Vec2Hash() { static const uint64_t h = Astra::TypeID<glm::vec2>::Hash(); return h; }
        inline uint64_t Vec3Hash() { static const uint64_t h = Astra::TypeID<glm::vec3>::Hash(); return h; }
        inline uint64_t Vec4Hash() { static const uint64_t h = Astra::TypeID<glm::vec4>::Hash(); return h; }
        inline uint64_t Mat3Hash() { static const uint64_t h = Astra::TypeID<glm::mat3>::Hash(); return h; }
        inline uint64_t Mat4Hash() { static const uint64_t h = Astra::TypeID<glm::mat4>::Hash(); return h; }
        inline uint64_t QuatHash() { static const uint64_t h = Astra::TypeID<glm::quat>::Hash(); return h; }

        // The outcome of trying to read ONE field from ONE JSON node.
        //   NotMine   -- this helper does not own the field's TYPE; try the next.
        //   Ok        -- read and stored.
        //   Malformed -- the helper owns this type, the node is PRESENT, and it
        //                cannot be read into the field. The caller latches.
        // The absent case never reaches these helpers at all: ReflectionJsonReader
        // returns before calling them when Find() comes back null.
        enum class ReadResult : std::uint8_t { NotMine, Ok, Malformed };

        inline bool IsScalarHash(uint64_t h)
        {
            return h == Astra::TypeID<bool>::Hash()     || h == Astra::TypeID<int>::Hash()
                || h == Astra::TypeID<int32_t>::Hash()  || h == Astra::TypeID<uint32_t>::Hash()
                || h == Astra::TypeID<int64_t>::Hash()  || h == Astra::TypeID<uint64_t>::Hash()
                || h == Astra::TypeID<float>::Hash()    || h == Astra::TypeID<double>::Hash()
                || h == Astra::TypeID<std::string>::Hash();
        }

        inline bool WriteScalar(const Astra::FieldInfo& f, void* inst, nlohmann::json& out)
        {
            const uint64_t h = f.typeHash;
            if (h == Astra::TypeID<bool>::Hash())        { out = f.Get<bool>(inst);         return true; }
            if (h == Astra::TypeID<int>::Hash())         { out = f.Get<int>(inst);           return true; }
            if (h == Astra::TypeID<int32_t>::Hash())     { out = f.Get<int32_t>(inst);       return true; }
            if (h == Astra::TypeID<uint32_t>::Hash())    { out = f.Get<uint32_t>(inst);      return true; }
            if (h == Astra::TypeID<int64_t>::Hash())     { out = f.Get<int64_t>(inst);       return true; }
            if (h == Astra::TypeID<uint64_t>::Hash())    { out = f.Get<uint64_t>(inst);      return true; }
            if (h == Astra::TypeID<float>::Hash())       { out = f.Get<float>(inst);         return true; }
            if (h == Astra::TypeID<double>::Hash())      { out = f.Get<double>(inst);        return true; }
            if (h == Astra::TypeID<std::string>::Hash()) { out = f.Get<std::string>(inst);   return true; }
            return false;
        }

        // Reads a scalar field from JSON without ever throwing on a malformed
        // document -- nlohmann's get<T> would otherwise throw type_error through
        // the exception-free engine on a hand-edited file. A node of the wrong
        // JSON type is Malformed, not silently skipped: the key is present and
        // the value it holds is being thrown away.
        inline ReadResult ReadScalar(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            const uint64_t h = f.typeHash;
            if (h == Astra::TypeID<bool>::Hash())        { if (!in.is_boolean()) return ReadResult::Malformed; f.Set<bool>(inst,        in.get<bool>());        return ReadResult::Ok; }
            if (h == Astra::TypeID<int>::Hash())         { if (!in.is_number())  return ReadResult::Malformed; f.Set<int>(inst,         in.get<int>());         return ReadResult::Ok; }
            if (h == Astra::TypeID<int32_t>::Hash())     { if (!in.is_number())  return ReadResult::Malformed; f.Set<int32_t>(inst,     in.get<int32_t>());     return ReadResult::Ok; }
            if (h == Astra::TypeID<uint32_t>::Hash())    { if (!in.is_number())  return ReadResult::Malformed; f.Set<uint32_t>(inst,    in.get<uint32_t>());    return ReadResult::Ok; }
            if (h == Astra::TypeID<int64_t>::Hash())     { if (!in.is_number())  return ReadResult::Malformed; f.Set<int64_t>(inst,     in.get<int64_t>());     return ReadResult::Ok; }
            if (h == Astra::TypeID<uint64_t>::Hash())    { if (!in.is_number())  return ReadResult::Malformed; f.Set<uint64_t>(inst,    in.get<uint64_t>());    return ReadResult::Ok; }
            if (h == Astra::TypeID<float>::Hash())       { if (!in.is_number())  return ReadResult::Malformed; f.Set<float>(inst,       in.get<float>());       return ReadResult::Ok; }
            if (h == Astra::TypeID<double>::Hash())      { if (!in.is_number())  return ReadResult::Malformed; f.Set<double>(inst,      in.get<double>());      return ReadResult::Ok; }
            if (h == Astra::TypeID<std::string>::Hash()) { if (!in.is_string())  return ReadResult::Malformed; f.Set<std::string>(inst, in.get<std::string>()); return ReadResult::Ok; }
            return ReadResult::NotMine;
        }

        inline bool IsGlmVecHash(uint64_t h) { return h == Vec2Hash() || h == Vec3Hash() || h == Vec4Hash(); }
        inline bool IsGlmMatHash(uint64_t h) { return h == Mat3Hash() || h == Mat4Hash(); }

        inline bool WriteGlm(const Astra::FieldInfo& f, void* inst, nlohmann::json& out)
        {
            const uint64_t h = f.typeHash;
            if (h == Vec2Hash()) { auto v = f.Get<glm::vec2>(inst); out = {v.x, v.y};            return true; }
            if (h == Vec3Hash()) { auto v = f.Get<glm::vec3>(inst); out = {v.x, v.y, v.z};       return true; }
            if (h == Vec4Hash()) { auto v = f.Get<glm::vec4>(inst); out = {v.x, v.y, v.z, v.w};  return true; }
            return false;
        }

        // An array of EXACTLY n numbers -- the one arity/shape test every vector,
        // matrix and quaternion read below goes through, which is what makes
        // "wrong arity" a single decision rather than a per-type judgement call.
        // (The per-element IsNumberAt helper this replaced allowed a LONGER array
        // through for vectors; exact arity is the stricter and correct rule.)
        inline bool AllNumbers(const nlohmann::json& in, std::size_t n)
        {
            if (!in.is_array() || in.size() != n) return false;
            for (std::size_t i = 0; i < n; ++i)
                if (!in[i].is_number()) return false;
            return true;
        }

        // Reads a glm vector field from a JSON array without throwing on a
        // malformed document. ARITY IS EXACT (AllNumbers checks in.size()): a
        // two-element array in a vec3 slot is the stale-2D-scene case this
        // guard exists for, and a four-element one in a vec3 slot is a format
        // mismatch in the other direction. Both are Malformed, not "absent".
        inline ReadResult ReadGlm(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            const uint64_t h = f.typeHash;
            if (h == Vec2Hash()) { if (!AllNumbers(in, 2)) return ReadResult::Malformed; f.Set<glm::vec2>(inst, glm::vec2(in[0].get<float>(), in[1].get<float>()));                                        return ReadResult::Ok; }
            if (h == Vec3Hash()) { if (!AllNumbers(in, 3)) return ReadResult::Malformed; f.Set<glm::vec3>(inst, glm::vec3(in[0].get<float>(), in[1].get<float>(), in[2].get<float>()));                     return ReadResult::Ok; }
            if (h == Vec4Hash()) { if (!AllNumbers(in, 4)) return ReadResult::Malformed; f.Set<glm::vec4>(inst, glm::vec4(in[0].get<float>(), in[1].get<float>(), in[2].get<float>(), in[3].get<float>())); return ReadResult::Ok; }
            return ReadResult::NotMine;
        }

        // Matrices serialize as a flat column-major array (mat3 -> 9, mat4 -> 16):
        // for each column c, each row r, push m[c][r]. glm is column-major so this
        // is the natural memory order and reconstructs losslessly.
        inline bool WriteMatrix(const Astra::FieldInfo& f, void* inst, nlohmann::json& out)
        {
            const uint64_t h = f.typeHash;
            if (h == Mat3Hash())
            {
                const glm::mat3 m = f.Get<glm::mat3>(inst);
                out = nlohmann::json::array();
                for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) out.push_back(m[c][r]);
                return true;
            }
            if (h == Mat4Hash())
            {
                const glm::mat4 m = f.Get<glm::mat4>(inst);
                out = nlohmann::json::array();
                for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) out.push_back(m[c][r]);
                return true;
            }
            return false;
        }

        inline ReadResult ReadMatrix(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            const uint64_t h = f.typeHash;
            if (h == Mat3Hash())
            {
                if (!AllNumbers(in, 9)) return ReadResult::Malformed;
                glm::mat3 m(1.0f);
                std::size_t i = 0;
                for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) m[c][r] = in[i++].get<float>();
                f.Set<glm::mat3>(inst, m);
                return ReadResult::Ok;
            }
            if (h == Mat4Hash())
            {
                if (!AllNumbers(in, 16)) return ReadResult::Malformed;
                glm::mat4 m(1.0f);
                std::size_t i = 0;
                for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) m[c][r] = in[i++].get<float>();
                f.Set<glm::mat4>(inst, m);
                return ReadResult::Ok;
            }
            return ReadResult::NotMine;
        }

        // Quaternions serialize as [x, y, z, w] (component order, not glm's memory
        // layout) so the JSON is layout-agnostic. Reconstruct via glm::quat(w,x,y,z).
        //
        // Deliberately NOT norm-guarded, unlike ReadQuat below. Writing verbatim
        // means a grossly non-unit quaternion that some caller put in memory
        // produces a file the loader then REFUSES, naming the field -- which is
        // how the caller finds out. Normalizing here instead would silently
        // rewrite an orientation the running scene did not have, and hide the
        // bug in the only artifact that could have revealed it. Nothing in the
        // engine can reach that state (RotationAboutZ, QuatFromEulerRadians and
        // glm::angleAxis are all unit by construction), so this is a plugin-bug
        // path, and save is best-effort anyway -- see SaveJson's own note on why
        // it does not check HasError().
        inline bool WriteQuat(const Astra::FieldInfo& f, void* inst, nlohmann::json& out)
        {
            if (f.typeHash == QuatHash())
            {
                const glm::quat q = f.Get<glm::quat>(inst);
                out = {q.x, q.y, q.z, q.w};
                return true;
            }
            return false;
        }

        // How far a stored quaternion's SQUARED norm may sit from 1 and still be
        // treated as "a rotation that lost precision" rather than "something
        // that is not a rotation". Squared so the test itself costs no sqrt.
        //
        // 0.05 accepts a norm within ~2.5% of unit. Decimal JSON at float
        // precision lands ~1e-7 away; a human typing 0.71 where 0.70710678
        // belongs lands ~0.8% away -- both are ordinary authoring and must pass.
        // Being generous here is SAFE because everything inside the band is
        // normalized before it is stored, so no scale can survive it; the band's
        // only job is to catch values that read as an intent to scale (norm 2 is
        // a 4x basis) or that cannot be normalized at all.
        inline constexpr float kQuatNormTolerance2 = 0.05f;

        // Quaternions read back from [x, y, z, w].
        //
        // NORM GUARD (Task 3, F1). glm::mat4_cast assumes a UNIT quaternion and
        // does not normalize, so a stored `[0,0,0,2]` bakes a uniform 4x scale
        // into Transform::ToMatrix's basis and the entity silently renders at
        // four times its authored size. The retired `float rotation` could not
        // express an invalid value at all, so this hazard arrived WITH the
        // widening -- on exactly the hand-edited-file path this reader exists to
        // survive.
        //
        // Guarded HERE, at the load boundary, and deliberately NOT inside
        // Transform::ToMatrix(): ToMatrix runs once per entity per propagation
        // pass and must not pay a sqrt re-checking an invariant that can only be
        // broken by data entering the process.
        //
        // Near-unit normalizes SILENTLY -- warning would fire on ordinary
        // authoring. Grossly non-unit latches, and a zero-length quaternion is
        // caught by the same branch: it is the one value that cannot be
        // normalized at all, and normalizing it anyway would hand every matrix
        // downstream a NaN.
        inline ReadResult ReadQuat(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            if (f.typeHash != QuatHash())
                return ReadResult::NotMine;
            if (!AllNumbers(in, 4))
                return ReadResult::Malformed;

            const glm::quat q(in[3].get<float>(), in[0].get<float>(),
                              in[1].get<float>(), in[2].get<float>());
            const float len2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
            if (!std::isfinite(len2) || std::fabs(len2 - 1.0f) > kQuatNormTolerance2)
                return ReadResult::Malformed;

            f.Set<glm::quat>(inst, q / std::sqrt(len2));
            return ReadResult::Ok;
        }

        // A field TYPE the bridge can represent: any scalar, glm vec/mat/quat, an
        // enum, or a nested reflected struct (GetMeta resolves both structs AND
        // enums, but isEnum is checked first for clarity). Anything else is an
        // unsupported field type -> fail loud rather than silently drop.
        inline bool IsHandledType(const Astra::FieldInfo& f)
        {
            const uint64_t h = f.typeHash;
            if (IsScalarHash(h) || IsGlmVecHash(h) || IsGlmMatHash(h) || h == QuatHash())
                return true;
            if (f.isEnum)
                return true;
            return Astra::GetMeta(h) != nullptr;   // nested reflected struct
        }

        // Read an enum's raw integer value from the instance using raw memory access.
        // Avoids the TypeID hash assertion in Get<T> (the enum type hash != int hash).
        // Supports 1/2/4/8-byte underlying types.
        inline int64_t ReadEnumRaw(const Astra::FieldInfo& f, const void* inst)
        {
            const std::byte* ptr = static_cast<const std::byte*>(inst) + f.offset;
            int64_t value = 0;
            std::memcpy(&value, ptr, f.size <= 8 ? f.size : 8);
            return value;
        }

        // Write a raw integer back to the enum field via raw memory.
        inline void WriteEnumRaw(const Astra::FieldInfo& f, void* inst, int64_t value)
        {
            std::byte* ptr = static_cast<std::byte*>(inst) + f.offset;
            std::memcpy(ptr, &value, f.size <= 8 ? f.size : 8);
        }

        // Builds a uniform "unsupported field type" diagnostic (ASCII only).
        inline std::string UnsupportedFieldMessage(const Astra::FieldInfo& f)
        {
            return "unsupported field type for JSON: field '" + std::string(f.name) + "'";
        }

        // The PRESENT-but-unreadable diagnostic. Worded distinctly from the
        // unsupported-TYPE one above because the two imply different next
        // actions: this one means the file has a value for the field in a shape
        // the field cannot take (a stale scene, a hand-edit typo), which is a
        // data problem the user can go fix; the other means the engine has no
        // way to represent that field at all, which is a code problem.
        inline std::string MalformedFieldMessage(const Astra::FieldInfo& f)
        {
            return "malformed JSON value for field '" + std::string(f.name) +
                   "' (key present but not readable as this field's type)";
        }
    }

    class ReflectionJsonWriter : public Astra::IFieldVisitor
    {
    public:
        explicit ReflectionJsonWriter(nlohmann::json& out) : m_out(out) {}

        void Visit(const Astra::FieldInfo& field, void* instance) override
        {
            nlohmann::json value;
            if (Detail::WriteScalar(field, instance, value) ||
                Detail::WriteGlm(field, instance, value)    ||
                Detail::WriteMatrix(field, instance, value) ||
                Detail::WriteQuat(field, instance, value))
            {
                m_out[std::string(field.name)] = std::move(value);
                return;
            }
            // Enum: emit the value's name via EnumInfo.
            // MUST return unconditionally -- even when no name is resolvable, an
            // enum field must never fall through to the nested-struct branch below
            // (GetMeta returns the enum's TypeMeta too, producing a garbage {} object).
            if (field.isEnum)
            {
                const Astra::TypeMeta* em = Astra::GetMeta(field.typeHash);
                if (em && em->GetEnumInfo())
                {
                    const int64_t raw = Detail::ReadEnumRaw(field, instance);
                    if (auto name = em->EnumToString(raw))
                        m_out[std::string(field.name)] = std::string(*name);
                    // else: unresolvable value -> silently skip (not written as {})
                }
                return;   // terminal: never reach the nested-struct branch
            }
            // Nested reflected struct: recurse over the sub-instance.
            if (const Astra::TypeMeta* nested = Astra::GetMeta(field.typeHash))
            {
                nlohmann::json sub;
                ReflectionJsonWriter subWriter(sub);
                void* subInstance = static_cast<std::byte*>(instance) + field.offset;
                for (const Astra::FieldInfo& nf : nested->fields)
                    if (nf.IsSerializable())
                        subWriter.Visit(nf, subInstance);
                if (subWriter.HasError())   // propagate an unsupported sub-field up
                {
                    Fail(subWriter.Error());
                    return;
                }
                m_out[std::string(field.name)] = std::move(sub);
                return;
            }
            // No branch matched: an unsupported field type would be silently
            // dropped here. Fail loud instead so the loss carries a diagnostic.
            Fail(Detail::UnsupportedFieldMessage(field));
        }

        bool IsWriting() const noexcept override { return true; }

        ASTRA_NODISCARD bool HasError() const noexcept { return m_error; }
        ASTRA_NODISCARD const std::string& Error() const noexcept { return m_errorMsg; }

    private:
        void Fail(std::string msg)
        {
            if (!m_error) { m_error = true; m_errorMsg = std::move(msg); }
        }

        nlohmann::json& m_out;
        bool m_error = false;
        std::string m_errorMsg;
    };

    class ReflectionJsonReader : public Astra::IFieldVisitor
    {
    public:
        explicit ReflectionJsonReader(const nlohmann::json& in) : m_in(in) {}

        void Visit(const Astra::FieldInfo& field, void* instance) override
        {
            // Classify by TYPE first so an unsupported field type is reported even
            // when its key is absent -- a silently-dropped field must never pass
            // for the forward-compat "missing key -> keep default" path below.
            if (!Detail::IsHandledType(field))
            {
                Fail(Detail::UnsupportedFieldMessage(field));
                return;
            }

            const nlohmann::json* node = Find(field);
            if (!node) return;   // supported but ABSENT -> keep the default (forward/back compat)

            // PRESENT from here down, so "cannot read it" is data loss, not
            // compatibility. Each helper reports NotMine / Ok / Malformed; the
            // chain stops at the first that owns the field's type.
            using RR = Detail::ReadResult;
            RR r = Detail::ReadScalar(field, instance, *node);
            if (r == RR::NotMine) r = Detail::ReadGlm(field, instance, *node);
            if (r == RR::NotMine) r = Detail::ReadMatrix(field, instance, *node);
            if (r == RR::NotMine) r = Detail::ReadQuat(field, instance, *node);
            if (r == RR::Malformed)
            {
                Fail(Detail::MalformedFieldMessage(field));
                return;
            }
            if (r == RR::Ok)
                return;

            // Enum: read by name. A non-string node is a SHAPE error and latches
            // like any other; a string that no longer resolves to an enumerator
            // is a VOCABULARY question and keeps the default, matching how the
            // scene loader already treats an unknown component type name.
            // MUST return unconditionally -- an enum field must never fall
            // through to the nested-struct branch (same GetMeta ambiguity the
            // writer documents).
            if (field.isEnum)
            {
                if (!node->is_string())
                {
                    Fail(Detail::MalformedFieldMessage(field));
                    return;
                }
                const Astra::TypeMeta* em = Astra::GetMeta(field.typeHash);
                if (em)
                    if (auto v = em->EnumFromString(node->get<std::string>()))
                        Detail::WriteEnumRaw(field, instance, *v);
                return;   // terminal: never reach the nested-struct branch
            }
            if (const Astra::TypeMeta* nested = Astra::GetMeta(field.typeHash))
            {
                // A nested reflected struct writes as a JSON OBJECT; anything
                // else present under that key cannot be walked, and treating it
                // as absent would default the whole sub-struct (e.g. silently
                // nil an asset Guid) with no diagnostic.
                if (!node->is_object())
                {
                    Fail(Detail::MalformedFieldMessage(field));
                    return;
                }
                ReflectionJsonReader subReader(*node);
                void* subInstance = static_cast<std::byte*>(instance) + field.offset;
                for (const Astra::FieldInfo& nf : nested->fields)
                    if (nf.IsSerializable())
                        subReader.Visit(nf, subInstance);
                if (subReader.HasError())   // propagate an unsupported/malformed sub-field up
                    Fail(subReader.Error());
            }
        }

        bool IsWriting() const noexcept override { return false; }

        ASTRA_NODISCARD bool HasError() const noexcept { return m_error; }
        ASTRA_NODISCARD const std::string& Error() const noexcept { return m_errorMsg; }

    private:
        void Fail(std::string msg)
        {
            if (!m_error) { m_error = true; m_errorMsg = std::move(msg); }
        }

        // Try the current name, then any AliasName (former names).
        const nlohmann::json* Find(const Astra::FieldInfo& field) const
        {
            auto it = m_in.find(std::string(field.name));
            if (it != m_in.end()) return &(*it);

            const nlohmann::json* found = nullptr;
            field.ForEachAttribute<Astra::AliasName>([&](const Astra::AliasName& a)
            {
                if (!found)
                {
                    auto ai = m_in.find(std::string(a.name));
                    if (ai != m_in.end()) found = &(*ai);
                }
            });
            return found;
        }

        const nlohmann::json& m_in;
        bool m_error = false;
        std::string m_errorMsg;
    };
}
