#include "Project/ClassTemplates.hpp"

#include <cctype>
#include <string_view>

namespace Arcane::Editor::ClassTemplates
{
    const char* KindLabel(Kind kind)
    {
        switch (kind)
        {
            case Kind::Component:  return "Component";
            case Kind::System:     return "System";
            case Kind::PlainClass: return "Plain class";
            case Kind::Count:      break;
        }
        return "Class";
    }

    namespace
    {
        bool IsIdentStart(char c)
        {
            return c == '_' || std::isalpha(static_cast<unsigned char>(c)) != 0;
        }
        bool IsIdentChar(char c)
        {
            return c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0;
        }

        // The C++ keywords a class name could plausibly collide with. Not the
        // whole reserved list -- the identifier scan above already rules out
        // everything that is not [A-Za-z0-9_] -- just the words that ARE
        // identifiers lexically and would still fail to compile as a type name.
        bool IsKeyword(std::string_view s)
        {
            static constexpr std::string_view kKeywords[] = {
                "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool", "break",
                "case", "catch", "char", "class", "compl", "concept", "const", "consteval",
                "constexpr", "constinit", "continue", "decltype", "default", "delete", "do",
                "double", "else", "enum", "explicit", "export", "extern", "false", "float",
                "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
                "new", "noexcept", "not", "nullptr", "operator", "or", "private", "protected",
                "public", "register", "requires", "return", "short", "signed", "sizeof",
                "static", "struct", "switch", "template", "this", "throw", "true", "try",
                "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
                "void", "volatile", "wchar_t", "while", "xor",
            };
            for (std::string_view k : kKeywords)
                if (s == k)
                    return true;
            return false;
        }

        // Replace every "{{TOKEN}}" occurrence in `text` with `value`.
        void ReplaceAll(std::string& text, std::string_view token, std::string_view value)
        {
            std::size_t pos = 0;
            while ((pos = text.find(token, pos)) != std::string::npos)
            {
                text.replace(pos, token.size(), value);
                pos += value.size();
            }
        }

        std::string Fill(std::string_view tmpl, std::string_view cls, std::string_view ns)
        {
            std::string out(tmpl);
            ReplaceAll(out, "{{CLASS}}", cls);
            ReplaceAll(out, "{{NS}}", ns);
            return out;
        }

        // ---- the templates ---------------------------------------------------
        // Kept as raw literals so what the wizard writes is readable HERE, at
        // the one place it is defined. Every literal ends in "\n".

        constexpr std::string_view kComponentHeader = R"(#pragma once

// {{CLASS}}: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARCANE_COMPONENT line in {{CLASS}}.cpp; nothing else to wire.

#include <Astra/Reflection/Reflection.hpp>

namespace {{NS}}
{
    struct {{CLASS}}
    {
        float value = 0.0f;
    };

    ASTRA_REFLECT_TYPE({{CLASS}})
        ASTRA_REFLECT_FIELD({{CLASS}}, value)
    ASTRA_END_REFLECT_TYPE()
}
)";

        constexpr std::string_view kComponentSource = R"(#include "{{CLASS}}.hpp"

#include <Arcane/Plugin/GameComponents.hpp>

// The one registration line: the ARCANE_GAME_MODULE prologue (Arcane/Plugin/
// GameModule.hpp) drains every ARCANE_COMPONENT of the module into its
// ComponentModule (Arcane::Game::RegisterComponents). One .cpp per type.
ARCANE_COMPONENT({{NS}}::{{CLASS}})
)";

        constexpr std::string_view kSystemHeader = R"(#pragma once

// {{CLASS}}: a system -- a functor the scheduler runs over the registry each
// step. Declare what it reads and writes in the SystemTraits so the scheduler
// can order and parallelise it.
//
// PLACEMENT. The engine owns its standard systems (Runtime::InstallEngineSystems:
// PhysicsSystem -> TransformPropagationSystem in fixedUpdate, RenderSubmission
// System in render). Say where THIS one runs relative to them in the traits:
// Astra::Before<Arcane::TransformPropagationSystem> (the default below: move
// things, THEN the engine propagates) or Astra::After<...> (read the propagated
// WorldTransform). Astra orders by the type NAME, so naming an engine system
// from a game module is fine; an anchor the host never installed adds no edge.
//
// Systems are registered EXPLICITLY, because their order is a design act.
// Add this line to your module's OnInit (Arcane/Plugin/GameModule.hpp):
//
//     std::ignore = ctx.engine->Schedulers().fixedUpdate.AddSystem<{{NS}}::{{CLASS}}>();
//
// (fixedUpdate for simulation, render for submission-time work.)

#include <Arcane/Scene/TransformSystems.hpp>   // the placement anchor

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

namespace {{NS}}
{
    struct {{CLASS}}
        : Astra::SystemTraits<Astra::Reads<>, Astra::Writes<>,
                              Astra::Before<Arcane::TransformPropagationSystem>>
    {
        void operator()(Astra::Registry& reg)
        {
            (void)reg;
        }
    };
}
)";

        constexpr std::string_view kPlainHeader = R"(#pragma once

namespace {{NS}}
{
    class {{CLASS}}
    {
    public:
        {{CLASS}}() = default;
    };
}
)";

        constexpr std::string_view kPlainSource = R"(#include "{{CLASS}}.hpp"

namespace {{NS}}
{
}
)";
    }

    std::optional<std::string> ValidateClassName(std::string_view name)
    {
        if (name.empty())
            return "Enter a class name.";
        if (!IsIdentStart(name.front()))
            return "A class name must start with a letter or underscore.";
        for (char c : name)
            if (!IsIdentChar(c))
                return "A class name may only contain letters, digits and underscores.";
        if (IsKeyword(name))
            return "That is a C++ keyword.";
        return std::nullopt;
    }

    std::string NamespaceForProject(std::string_view projectName)
    {
        std::string ns;
        ns.reserve(projectName.size() + 1);
        for (char c : projectName)
            ns.push_back(IsIdentChar(c) ? c : '_');
        if (!ns.empty() && !IsIdentStart(ns.front()))
            ns.insert(ns.begin(), '_');
        if (ns.empty())
            return "Game";
        return ns;
    }

    Rendered Render(Kind kind, std::string_view className, std::string_view projectName)
    {
        const std::string ns = NamespaceForProject(projectName);
        const std::string cls(className);
        Rendered r;
        r.headerName = cls + ".hpp";
        switch (kind)
        {
            case Kind::Component:
                r.header     = Fill(kComponentHeader, cls, ns);
                r.sourceName = cls + ".cpp";
                r.source     = Fill(kComponentSource, cls, ns);
                break;
            case Kind::System:
                r.header = Fill(kSystemHeader, cls, ns);
                break;
            case Kind::PlainClass:
            case Kind::Count:
                r.header     = Fill(kPlainHeader, cls, ns);
                r.sourceName = cls + ".cpp";
                r.source     = Fill(kPlainSource, cls, ns);
                break;
        }
        return r;
    }
}
