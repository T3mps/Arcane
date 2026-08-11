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
// without a diagnostic. Wrong-typed JSON *data* for a supported field type is
// still tolerated on read (the field keeps its default), matching the
// hand-edited-file contract the scene loader relies on.

#include <Astra/Reflection/FieldVisitor.hpp>
#include <Astra/Reflection/FieldInfo.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Core/TypeID.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Json.hpp>

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
        // document. The return value means "this field is a known scalar type"
        // (so the visitor stops here); a node of the wrong JSON type is skipped,
        // leaving the field at its default -- nlohmann's get<T> would otherwise
        // throw type_error through the exception-free engine on hand-edited files.
        inline bool ReadScalar(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            const uint64_t h = f.typeHash;
            if (h == Astra::TypeID<bool>::Hash())        { if (in.is_boolean()) f.Set<bool>(inst,        in.get<bool>());        return true; }
            if (h == Astra::TypeID<int>::Hash())         { if (in.is_number())  f.Set<int>(inst,         in.get<int>());         return true; }
            if (h == Astra::TypeID<int32_t>::Hash())     { if (in.is_number())  f.Set<int32_t>(inst,     in.get<int32_t>());     return true; }
            if (h == Astra::TypeID<uint32_t>::Hash())    { if (in.is_number())  f.Set<uint32_t>(inst,    in.get<uint32_t>());    return true; }
            if (h == Astra::TypeID<int64_t>::Hash())     { if (in.is_number())  f.Set<int64_t>(inst,     in.get<int64_t>());     return true; }
            if (h == Astra::TypeID<uint64_t>::Hash())    { if (in.is_number())  f.Set<uint64_t>(inst,    in.get<uint64_t>());    return true; }
            if (h == Astra::TypeID<float>::Hash())       { if (in.is_number())  f.Set<float>(inst,       in.get<float>());       return true; }
            if (h == Astra::TypeID<double>::Hash())      { if (in.is_number())  f.Set<double>(inst,      in.get<double>());      return true; }
            if (h == Astra::TypeID<std::string>::Hash()) { if (in.is_string())  f.Set<std::string>(inst, in.get<std::string>()); return true; }
            return false;
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

        // True when the array element at [i] exists and is a number (guards the
        // in[i].get<float>() reads below against short/wrong-typed arrays).
        inline bool IsNumberAt(const nlohmann::json& in, std::size_t i)
        {
            return i < in.size() && in[i].is_number();
        }

        inline bool AllNumbers(const nlohmann::json& in, std::size_t n)
        {
            if (!in.is_array() || in.size() != n) return false;
            for (std::size_t i = 0; i < n; ++i)
                if (!in[i].is_number()) return false;
            return true;
        }

        // Reads a glm vector field from a JSON array without throwing on a
        // malformed document. Like ReadScalar, the return value means "this field
        // is a known vector type"; a node that is not an array of enough numbers
        // is skipped, leaving the field at its default.
        inline bool ReadGlm(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            const uint64_t h = f.typeHash;
            if (h == Vec2Hash()) { if (in.is_array() && IsNumberAt(in, 0) && IsNumberAt(in, 1))                                    f.Set<glm::vec2>(inst, glm::vec2(in[0].get<float>(), in[1].get<float>()));                                        return true; }
            if (h == Vec3Hash()) { if (in.is_array() && IsNumberAt(in, 0) && IsNumberAt(in, 1) && IsNumberAt(in, 2))               f.Set<glm::vec3>(inst, glm::vec3(in[0].get<float>(), in[1].get<float>(), in[2].get<float>()));                     return true; }
            if (h == Vec4Hash()) { if (in.is_array() && IsNumberAt(in, 0) && IsNumberAt(in, 1) && IsNumberAt(in, 2) && IsNumberAt(in, 3)) f.Set<glm::vec4>(inst, glm::vec4(in[0].get<float>(), in[1].get<float>(), in[2].get<float>(), in[3].get<float>())); return true; }
            return false;
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

        inline bool ReadMatrix(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            const uint64_t h = f.typeHash;
            if (h == Mat3Hash())
            {
                if (AllNumbers(in, 9))
                {
                    glm::mat3 m(1.0f);
                    std::size_t i = 0;
                    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) m[c][r] = in[i++].get<float>();
                    f.Set<glm::mat3>(inst, m);
                }
                return true;
            }
            if (h == Mat4Hash())
            {
                if (AllNumbers(in, 16))
                {
                    glm::mat4 m(1.0f);
                    std::size_t i = 0;
                    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) m[c][r] = in[i++].get<float>();
                    f.Set<glm::mat4>(inst, m);
                }
                return true;
            }
            return false;
        }

        // Quaternions serialize as [x, y, z, w] (component order, not glm's memory
        // layout) so the JSON is layout-agnostic. Reconstruct via glm::quat(w,x,y,z).
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

        inline bool ReadQuat(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            if (f.typeHash == QuatHash())
            {
                if (AllNumbers(in, 4))
                    f.Set<glm::quat>(inst, glm::quat(in[3].get<float>(), in[0].get<float>(),
                                                     in[1].get<float>(), in[2].get<float>()));
                return true;
            }
            return false;
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
            if (!node) return;   // supported but missing -> keep default

            if (Detail::ReadScalar(field, instance, *node) ||
                Detail::ReadGlm(field, instance, *node)    ||
                Detail::ReadMatrix(field, instance, *node) ||
                Detail::ReadQuat(field, instance, *node))
                return;

            // Enum: read by name if the node is a string; silently skip otherwise.
            // MUST return unconditionally -- a non-string node for an enum field must
            // not fall through to the nested-struct branch (same GetMeta ambiguity).
            if (field.isEnum)
            {
                if (node->is_string())
                {
                    const Astra::TypeMeta* em = Astra::GetMeta(field.typeHash);
                    if (em)
                        if (auto v = em->EnumFromString(node->get<std::string>()))
                            Detail::WriteEnumRaw(field, instance, *v);
                }
                return;   // terminal: never reach the nested-struct branch
            }
            if (const Astra::TypeMeta* nested = Astra::GetMeta(field.typeHash))
            {
                ReflectionJsonReader subReader(*node);
                void* subInstance = static_cast<std::byte*>(instance) + field.offset;
                for (const Astra::FieldInfo& nf : nested->fields)
                    if (nf.IsSerializable())
                        subReader.Visit(nf, subInstance);
                if (subReader.HasError())   // propagate an unsupported sub-field up
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
