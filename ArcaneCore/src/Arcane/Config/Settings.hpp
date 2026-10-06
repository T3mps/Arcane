#pragma once

// Settings structs (settings arc S2; spec s4.3, s4.6): a reflected struct whose
// fields are cvars, read as one typed block of the published snapshot.
//
//     struct ExampleSettings { std::uint32_t substeps = 4; };   // fictional: not an engine type
//     ARC_REFLECT_TYPE(ExampleSettings)
//         ARC_REFLECT_TYPE_ATTR(Settings, "example", SettingScope::Project, ApplyMode::NextWorld, Audience::Game)
//         ARC_REFLECT_FIELD(ExampleSettings, substeps)
//             ARC_REFLECT_ATTR(Range, 1, 16) ARC_REFLECT_ATTR(Tooltip, "Solver substeps") ARC_REFLECT_ATTR(Deterministic)
//     ARC_REFLECT_TYPE_END()
//
//     ARC_SETTINGS(ExampleSettings);          // in ONE .cpp of the declaring module, after that header
//     const ExampleSettings& s = Arcane::Settings<ExampleSettings>();
//
// - The member initializers ARE the defaults. A field's cvar is
//   "<category>.<field>", and the field order is the window's row order.
// - The Settings attribute goes FIRST in the block. A field reflected before
//   it has no codec, and ARC_SETTINGS refuses the struct.
// - Field types: bool, std::int32_t, std::uint32_t, std::int64_t,
//   std::uint64_t, float, double, std::string, CVarColor, CVarVec2/3/4, or a
//   reflected enum. Anything else fails the BUILD (SettingsField.hpp).
// - Every non-Hidden field needs a Tooltip: the registry refuses empty help.
// - A PreferencesMachine or PreferencesProject field is Archive (persisted);
//   Attr::Flags adds Dev/Cheat/Protected.
// - Settings<T>() returns a reference into the published snapshot, valid until
//   at least two more publishes. Read it once per frame or step, and never keep
//   it. A worker that outlives the frame holds SettingsShared<T>() instead,
//   which pins the snapshot.
// - A struct that is not registered (or whose module unloaded) reads as T{}.

#include <Arcane/Config/CVarModule.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Reflection.hpp>

#include <Astra/Core/TypeID.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace Arcane
{
    namespace Detail
    {
        template <class T>
        std::shared_ptr<void> MakeSettingsBlock()
        {
            return std::make_shared<T>();
        }

        // The reflected metadata -> one SettingsFieldDesc per field. Sets
        // desc.error (and stops) on a missing Settings attribute or a field
        // without a codec.
        inline void DescribeSettingsFields(const ::Astra::TypeMeta& meta, const void* defaults, SettingsTypeDesc& desc)
        {
            const auto* type = meta.GetAttribute<Attr::Settings>();
            if (!type)
            {
                desc.error = "the reflection block has no ARC_REFLECT_TYPE_ATTR(Settings, ...)";
                return;
            }
            desc.category = std::string(type->category);
            CVarFlags typeFlags = CVarFlags::None;
            if (const auto* f = meta.GetAttribute<Attr::Flags>())
                typeFlags = f->flags;

            for (const ::Astra::FieldInfo& field : meta.fields)
            {
                const auto* codec = field.GetAttribute<SettingsCodec>();
                if (!codec)
                {
                    desc.error = "field '" + std::string(field.name) + "' has no settings codec: reflect it with "
                                 "ARC_REFLECT_FIELD AFTER ARC_REFLECT_TYPE_ATTR(Settings, ...)";
                    return;
                }
                SettingsFieldDesc out;
                out.name         = desc.category + "." + std::string(field.name);
                out.type         = codec->type;
                out.defaultValue = codec->read(defaults);
                if (const auto* range = field.GetAttribute<Attr::Range>())
                {
                    out.min = codec->bound(range->min);
                    out.max = codec->bound(range->max);
                }
                out.flags = typeFlags;
                if (const auto* f = field.GetAttribute<Attr::Flags>())
                    out.flags = out.flags | f->flags;
                if (field.HasAttribute<Attr::Deterministic>())
                    out.flags = out.flags | CVarFlags::Deterministic;
                if (field.IsHidden())
                    out.flags = out.flags | CVarFlags::Hidden;
                out.help = field.GetTooltip();
                if (const auto* d = field.GetAttribute<Attr::DisplayName>()) out.displayName = d->name;
                if (const auto* k = field.GetAttribute<Attr::Keywords>())    out.keywords = k->words;
                if (const auto* w = field.GetAttribute<Attr::Widget>())      out.widget = w->hint;
                if (const auto* c = field.GetAttribute<Attr::Category>())    out.group = c->category;
                out.audience = field.HasAttribute<Attr::PlayerSafe>() ? Audience::PlayerSafe : type->audience;
                out.scope = type->scope;
                if (const auto* s = field.GetAttribute<Attr::Scope>()) out.scope = s->scope;
                out.apply = type->apply;
                if (const auto* a = field.GetAttribute<Attr::Apply>()) out.apply = a->mode;
                if (out.scope != SettingScope::Project)
                    out.flags = out.flags | CVarFlags::Archive;
                out.enumNames = codec->enumNames();
                field.ForEachAttribute<Attr::AliasName>([&](const Attr::AliasName& alias) {
                    const std::string_view old = alias.name;
                    out.aliases.push_back(old.find('.') == std::string_view::npos
                                              ? desc.category + "." + std::string(old)
                                              : std::string(old));
                });
                out.write = codec->write;
                desc.fields.push_back(std::move(out));
            }
        }
    }

    template <class T>
    SettingsTypeDesc DescribeSettings(std::string_view module = {})
    {
        static_assert(std::is_class_v<T> && std::is_default_constructible_v<T>,
                      "ARC_SETTINGS(T): T must be a default-constructible struct");
        SettingsTypeDesc desc;
        desc.typeHash = ::Astra::TypeID<T>::Hash();
        desc.typeName = std::string(::Astra::TypeID<T>::Name());
        desc.module   = std::string(module.empty() ? ::Arcane::Detail::CallerModule() : module);
        desc.make     = &Detail::MakeSettingsBlock<T>;
        if (!::Astra::Detail::MetaFactory<T>::fn)
        {
            desc.error = "no ARC_REFLECT_TYPE block for this type has run: include the header holding the "
                         "struct's reflection block BEFORE ARC_SETTINGS, in the same .cpp";
            return desc;
        }
        const ::Astra::TypeMeta meta = ::Astra::Detail::MetaFactory<T>::fn();
        const T defaults{};
        Detail::DescribeSettingsFields(meta, &defaults, desc);
        return desc;
    }

    template <class T>
    bool RegisterSettings(CVarRegistry& registry, std::string_view module = {})
    {
        return registry.RegisterSettings(DescribeSettings<T>(module));
    }

    template <class T>
    const T& Settings(const CVarRegistry& registry)
    {
        if (const void* block = registry.SettingsBlock(::Astra::TypeID<T>::Hash()))
            return *static_cast<const T*>(block);
        static const T fallback{};
        return fallback;
    }

    template <class T>
    const T& Settings()
    {
        return Settings<T>(CVarRegistry::Get());
    }

    template <class T>
    std::shared_ptr<const T> SettingsShared(const CVarRegistry& registry)
    {
        std::shared_ptr<const CVarSnapshot> snap = registry.Snapshot();
        if (snap)
            if (const void* block = snap->FindSettings(::Astra::TypeID<T>::Hash()))
                return std::shared_ptr<const T>(std::move(snap), static_cast<const T*>(block));
        static const T fallback{};
        return std::shared_ptr<const T>(std::shared_ptr<const T>{}, &fallback);
    }

    template <class T>
    std::shared_ptr<const T> SettingsShared()
    {
        return SettingsShared<T>(CVarRegistry::Get());
    }
}

#define ARC_SETTINGS_CAT2(a, b) a##b
#define ARC_SETTINGS_CAT(a, b) ARC_SETTINGS_CAT2(a, b)
// At namespace scope in ONE .cpp of the declaring module, AFTER the header that
// holds the struct's reflection block. That header's registrar is partially
// ordered and this static is ordered, so the registrar initializes first.
// The static is named with __COUNTER__, not __LINE__, so two expansions on one
// line (from another macro) never collide (S2-H).
#define ARC_SETTINGS(Type) \
    static const bool ARC_SETTINGS_CAT(arcSettings_, __COUNTER__) = \
        ::Arcane::RegisterSettings<Type>(::Arcane::CVarRegistry::Get(), ::Arcane::Detail::CallerModule())
