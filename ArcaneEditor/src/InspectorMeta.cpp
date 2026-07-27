#include "InspectorMeta.hpp"

// FieldInfo::typeHash is populated (Detail::MakeFieldInfo) from
// Astra::TypeID<DecayedType>::Hash(), so comparing against that same accessor
// is exact. Visible transitively via FieldInfo.hpp; included for clarity, as
// InspectorFields.cpp does.
#include <Astra/Core/TypeID.hpp>
#include <Astra/Reflection/Attribute.hpp>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace Arcane::Editor
{
    namespace
    {
        bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
        bool IsLower(char c) { return c >= 'a' && c <= 'z'; }
        bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        // A word boundary falls BEFORE index i.
        bool BreaksBefore(std::string_view s, std::size_t i)
        {
            if (i == 0 || i >= s.size()) return false;
            if (!IsUpper(s[i])) return false;

            const char prev = s[i - 1];
            // lower|digit -> Upper: the ordinary camelCase boundary.
            if (IsLower(prev) || IsDigit(prev)) return true;
            // Upper -> Upper followed by lower: the END of an acronym run, so
            // "HTTPServer" splits once (HTTP | Server) rather than per letter.
            if (IsUpper(prev) && i + 1 < s.size() && IsLower(s[i + 1])) return true;
            return false;
        }

        // Case-insensitive substring. ASCII-only folding is sufficient: these
        // are C++ identifiers and author-written attribute strings.
        bool ContainsFold(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty()) return true;
            if (needle.size() > haystack.size()) return false;
            const auto lower = [](char c) {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            };
            for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
            {
                std::size_t j = 0;
                for (; j < needle.size(); ++j)
                    if (lower(haystack[i + j]) != lower(needle[j])) break;
                if (j == needle.size()) return true;
            }
            return false;
        }
    }

    std::string DeriveDisplayName(std::string_view identifier)
    {
        std::string spaced;
        spaced.reserve(identifier.size() + 8);
        for (std::size_t i = 0; i < identifier.size(); ++i)
        {
            const char c = identifier[i];
            if (c == '_') { spaced.push_back(' '); continue; }
            if (BreaksBefore(identifier, i)) spaced.push_back(' ');
            spaced.push_back(c);
        }

        // Split on spaces, capitalise each word, rejoin with single spaces --
        // which collapses runs and trims both ends in one pass.
        std::string out;
        out.reserve(spaced.size());
        bool wordStart = true;
        for (char c : spaced)
        {
            if (c == ' ') { wordStart = true; continue; }
            if (wordStart)
            {
                if (!out.empty()) out.push_back(' ');
                out.push_back(IsLower(c) ? static_cast<char>(c - 'a' + 'A') : c);
                wordStart = false;
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string DisplayNameForField(const Astra::FieldInfo& field)
    {
        if (const Astra::DisplayName* d = field.GetAttribute<Astra::DisplayName>())
            return std::string(d->name);
        return DeriveDisplayName(field.name);
    }

    std::string DisplayNameForComponent(std::string_view typeName)
    {
        const std::size_t sep = typeName.rfind("::");
        const std::string_view leaf =
            (sep == std::string_view::npos) ? typeName : typeName.substr(sep + 2);
        return DeriveDisplayName(leaf);
    }

    std::string_view CategoryOfField(const Astra::FieldInfo& field)
    {
        if (const Astra::Category* c = field.GetAttribute<Astra::Category>())
            return c->category;
        return {};
    }

    std::string_view TooltipOfField(const Astra::FieldInfo& field)
    {
        if (const Astra::Tooltip* t = field.GetAttribute<Astra::Tooltip>())
            return t->text;
        return {};
    }

    std::optional<Astra::Range> RangeOfField(const Astra::FieldInfo& field)
    {
        if (const Astra::Range* r = field.GetAttribute<Astra::Range>())
            return *r;
        return std::nullopt;
    }

    bool FieldIsReadOnly(const Astra::FieldInfo& field)
    {
        return field.GetAttribute<Astra::ReadOnly>() != nullptr;
    }

    bool FieldIsAttributeHidden(const Astra::FieldInfo& field)
    {
        return field.GetAttribute<Astra::Hidden>() != nullptr;
    }

    bool ComponentMatchesFilter(std::string_view componentDisplayName,
                                std::string_view query)
    {
        return ContainsFold(componentDisplayName, query);
    }

    bool MatchesInspectorFilter(std::string_view componentDisplayName,
                                std::string_view fieldDisplayName,
                                std::string_view rawFieldName,
                                std::string_view query)
    {
        if (query.empty()) return true;
        if (ContainsFold(componentDisplayName, query)) return true;
        return ContainsFold(fieldDisplayName, query) || ContainsFold(rawFieldName, query);
    }

    namespace
    {
        // RAII scratch instance: aligned storage + DefaultConstruct on entry,
        // Destruct + aligned delete on exit. EntityInfo holds a std::string, so
        // skipping the destruct leaks heap on every call.
        class ScratchDefault
        {
        public:
            explicit ScratchDefault(const Astra::ComponentDescriptor& d) : m_desc(d)
            {
                if (d.size == 0) return;   // tag component: no storage, no fields
                m_mem = ::operator new(d.size, std::align_val_t(d.alignment));
                d.DefaultConstruct(m_mem);
            }
            ~ScratchDefault()
            {
                if (!m_mem) return;
                m_desc.Destruct(m_mem);
                ::operator delete(m_mem, std::align_val_t(m_desc.alignment));
            }
            ScratchDefault(const ScratchDefault&) = delete;
            ScratchDefault& operator=(const ScratchDefault&) = delete;

            [[nodiscard]] const void* Get() const { return m_mem; }

        private:
            const Astra::ComponentDescriptor& m_desc;
            void* m_mem = nullptr;
        };

        // The field's bytes inside the scratch, or null when there is nothing
        // to read. `desc` and `field` are independent parameters, so a caller
        // pairing a field with the wrong descriptor -- or with a tag component,
        // whose descriptor Astra gives size 0 -- must read nothing rather than
        // run off the end of the allocation this file just made.
        const std::byte* DefaultFieldBytes(const Astra::ComponentDescriptor& desc,
                                           const Astra::FieldInfo& field,
                                           const ScratchDefault& scratch)
        {
            if (!scratch.Get()) return nullptr;
            if (field.offset + field.size > desc.size) return nullptr;
            return static_cast<const std::byte*>(scratch.Get()) + field.offset;
        }

        // Value comparison wherever value and representation diverge. See the
        // header for what each arm can and cannot detect.
        bool FieldValueDiffers(const Astra::FieldInfo& field, const void* a, const void* b)
        {
            // std::string is checked FIRST and by characters: it is not
            // trivially copyable, so it would otherwise fall to the "cannot
            // compare" arm and never offer a revert.
            static const std::uint64_t kString = Astra::TypeID<std::string>::Hash();
            if (field.typeHash == kString)
                return *static_cast<const std::string*>(a) != *static_cast<const std::string*>(b);

            if (field.isTrivial) return std::memcmp(a, b, field.size) != 0;

            return false;
        }
    }

    void ReadDefaultFieldBytes(const Astra::ComponentDescriptor& desc,
                               const Astra::FieldInfo& field,
                               void* outBytes)
    {
        // Refuse before building anything: a raw-byte copy of a non-trivial
        // field would alias storage the scratch destructor frees on the way out.
        if (!outBytes || !field.isTrivial) return;

        ScratchDefault scratch(desc);
        const std::byte* def = DefaultFieldBytes(desc, field, scratch);
        if (!def) return;
        std::memcpy(outBytes, def, field.size);
    }

    bool FieldDiffersFromDefault(const Astra::ComponentDescriptor& desc,
                                 const Astra::FieldInfo& field,
                                 const void* instance)
    {
        if (!instance) return false;

        ScratchDefault scratch(desc);
        const std::byte* def = DefaultFieldBytes(desc, field, scratch);
        if (!def) return false;
        return FieldValueDiffers(field,
                                 static_cast<const std::byte*>(instance) + field.offset,
                                 def);
    }
}
