// ClassTemplates: the PURE text half of Assets -> Create -> C++ Class (the
// third step of the editor<->IDE surface). Name validation and the three
// rendered templates -- Component, System, Plain class -- as strings; no
// filesystem, no ImGui. What the wizard then does with the strings (write,
// register, regenerate, open in VS) lives in EditorApp and is desk-verify.

#include <catch2/catch_test_macros.hpp>

#include <Project/ClassTemplates.hpp>

#include <string>
#include <string_view>

using namespace Arcane::Editor;

namespace
{
    bool Has(const std::string& text, std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }
}

TEST_CASE("ClassTemplates::ValidateClassName accepts C++ identifiers and refuses everything else with a reason", "[editor]")
{
    CHECK_FALSE(ClassTemplates::ValidateClassName("Health").has_value());
    CHECK_FALSE(ClassTemplates::ValidateClassName("_Private").has_value());
    CHECK_FALSE(ClassTemplates::ValidateClassName("Wave2").has_value());

    CHECK(ClassTemplates::ValidateClassName("").has_value());
    CHECK(ClassTemplates::ValidateClassName("9Lives").has_value());      // leading digit
    CHECK(ClassTemplates::ValidateClassName("Health Bar").has_value());  // whitespace
    CHECK(ClassTemplates::ValidateClassName("my-class").has_value());    // punctuation
    CHECK(ClassTemplates::ValidateClassName("Health.hpp").has_value());  // an extension is not a name
    CHECK(ClassTemplates::ValidateClassName("class").has_value());       // keyword
    CHECK(ClassTemplates::ValidateClassName("namespace").has_value());   // keyword
    CHECK(ClassTemplates::ValidateClassName("Arcane::Health").has_value()); // no qualification
}

TEST_CASE("ClassTemplates::Render Component: a reflected struct in the header, the registrar line in the source", "[editor]")
{
    const ClassTemplates::Rendered r =
        ClassTemplates::Render(ClassTemplates::Kind::Component, "Health", "Aphelyon");

    CHECK(r.headerName == "Health.hpp");
    CHECK(r.sourceName == "Health.cpp");

    // Header: the shape Components.hpp / HotReloadShared.hpp use.
    CHECK(Has(r.header, "#pragma once"));
    CHECK(Has(r.header, "#include <Astra/Reflection/Reflection.hpp>"));
    CHECK(Has(r.header, "namespace Aphelyon"));
    CHECK(Has(r.header, "struct Health"));
    CHECK(Has(r.header, "ASTRA_REFLECT_TYPE(Health)"));
    CHECK(Has(r.header, "ASTRA_END_REFLECT_TYPE()"));

    // Source: ONE registrar line, in exactly one TU (a header would register
    // once per including TU), qualified with the project namespace.
    CHECK(Has(r.source, "#include \"Health.hpp\""));
    CHECK(Has(r.source, "#include <Arcane/Plugin/GameComponents.hpp>"));
    CHECK(Has(r.source, "ARCANE_COMPONENT(Aphelyon::Health)"));
    CHECK(Has(r.source, "ARCANE_GAME_MODULE"));          // the prologue that drains it
    CHECK_FALSE(Has(r.source, "GamePlugin_Init"));

    // No template token survives, and both files end in a newline.
    CHECK_FALSE(Has(r.header, "{{"));
    CHECK_FALSE(Has(r.source, "{{"));
    CHECK(r.header.back() == '\n');
    CHECK(r.source.back() == '\n');
}

TEST_CASE("ClassTemplates::Render System: a registered header/source pair with safe defaults", "[editor]")
{
    const ClassTemplates::Rendered r =
        ClassTemplates::Render(ClassTemplates::Kind::System, "Movement", "Aphelyon");

    CHECK(r.headerName == "Movement.hpp");
    CHECK(r.sourceName == "Movement.cpp");

    CHECK(Has(r.header, "#pragma once"));
    CHECK(Has(r.header, "#include <Astra/Registry/Registry.hpp>"));
    CHECK(Has(r.header, "#include <Astra/System/System.hpp>"));
    CHECK(Has(r.header, "namespace Aphelyon"));
    CHECK(Has(r.header, "struct Movement"));
    CHECK(Has(r.header, "Astra::SystemTraits<"));
    CHECK(Has(r.header, "void operator()(Astra::Registry& reg)"));
    CHECK(Has(r.header, "#include <Arcane/Scene/TransformSystems.hpp>"));
    CHECK(Has(r.header, "Astra::Before<Arcane::TransformPropagationSystem>"));

    CHECK(Has(r.source, "#include \"Movement.hpp\""));
    CHECK(Has(r.source, "#include <Arcane/Plugin/GameSystems.hpp>"));
    CHECK(Has(r.source, "ARCANE_SYSTEM("));
    CHECK(Has(r.source, "Aphelyon::Movement"));
    CHECK(Has(r.source, "Arcane::RoleMask::Both"));
    CHECK(Has(r.source, "Arcane::SystemPhase::FixedUpdate"));
    CHECK(Has(r.source, "ARCANE_GAME_MODULE discovers"));
    CHECK(Has(r.source, "scheduler order belongs in traits"));
    CHECK_FALSE(Has(r.header, "{{"));
    CHECK_FALSE(Has(r.source, "{{"));
    CHECK(r.header.back() == '\n');
    CHECK(r.source.back() == '\n');
}

TEST_CASE("ClassTemplates::Render System maps every phase and role choice", "[editor]")
{
    struct PhaseCase
    {
        int index;
        const char* spelling;
        bool transformAnchor;
    };

    for (const PhaseCase c : {
             PhaseCase{0, "Arcane::SystemPhase::FixedUpdate", true},
             PhaseCase{1, "Arcane::SystemPhase::Update", false},
             PhaseCase{2, "Arcane::SystemPhase::Render", false},
         })
    {
        const auto options = ClassTemplates::SystemOptionsForChoiceIndices(c.index, 0);
        const auto rendered = ClassTemplates::Render(
            ClassTemplates::Kind::System, "Movement", "Aphelyon", options);
        CHECK(Has(rendered.source, c.spelling));
        CHECK(Has(rendered.header, "Astra::Before<Arcane::TransformPropagationSystem>")
              == c.transformAnchor);
        CHECK(Has(rendered.header, "#include <Arcane/Scene/TransformSystems.hpp>")
              == c.transformAnchor);
        CHECK(Has(rendered.header, "scheduler order"));
        CHECK(rendered.header.back() == '\n');
        CHECK(rendered.source.back() == '\n');
        CHECK_FALSE(Has(rendered.header, "{{"));
        CHECK_FALSE(Has(rendered.source, "{{"));
    }

    struct RoleCase { int index; const char* spelling; };
    for (const RoleCase c : {
             RoleCase{0, "Arcane::RoleMask::Both"},
             RoleCase{1, "Arcane::RoleMask::Server"},
             RoleCase{2, "Arcane::RoleMask::Client"},
         })
    {
        const auto options = ClassTemplates::SystemOptionsForChoiceIndices(0, c.index);
        const auto rendered = ClassTemplates::Render(
            ClassTemplates::Kind::System, "Movement", "Aphelyon", options);
        CHECK(Has(rendered.source, c.spelling));
    }
}

TEST_CASE("ClassTemplates::Render System normalizes stale choices and invalid typed options", "[editor]")
{
    STATIC_REQUIRE(ClassTemplates::kSystemPhaseChoiceCount == 3);
    STATIC_REQUIRE(ClassTemplates::kSystemRoleChoiceCount == 3);
    CHECK(std::string_view(ClassTemplates::SystemPhaseChoiceLabel(0)) == "Fixed Update");
    CHECK(std::string_view(ClassTemplates::SystemPhaseChoiceLabel(1)) == "Update");
    CHECK(std::string_view(ClassTemplates::SystemPhaseChoiceLabel(2)) == "Render");
    CHECK(std::string_view(ClassTemplates::SystemPhaseChoiceLabel(99)) == "Fixed Update");
    CHECK(std::string_view(ClassTemplates::SystemRoleChoiceLabel(0)) == "Both");
    CHECK(std::string_view(ClassTemplates::SystemRoleChoiceLabel(1)) == "Server");
    CHECK(std::string_view(ClassTemplates::SystemRoleChoiceLabel(2)) == "Client");
    CHECK(std::string_view(ClassTemplates::SystemRoleChoiceLabel(-1)) == "Both");

    const auto choices = ClassTemplates::SystemOptionsForChoiceIndices(-1, 99);
    CHECK(choices.phase == Arcane::SystemPhase::FixedUpdate);
    CHECK(choices.role == Arcane::RoleMask::Both);

    ClassTemplates::SystemOptions invalid;
    invalid.phase = static_cast<Arcane::SystemPhase>(255);
    invalid.role  = static_cast<Arcane::RoleMask>(0);
    const auto normalized = ClassTemplates::Render(
        ClassTemplates::Kind::System, "Movement", "Aphelyon", invalid);
    CHECK(Has(normalized.source, "Arcane::SystemPhase::FixedUpdate"));
    CHECK(Has(normalized.source, "Arcane::RoleMask::Both"));
}

TEST_CASE("ClassTemplates::Render PlainClass: a class in the project namespace with its own .cpp", "[editor]")
{
    const ClassTemplates::Rendered r =
        ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "Inventory", "Aphelyon");

    CHECK(r.headerName == "Inventory.hpp");
    CHECK(r.sourceName == "Inventory.cpp");
    CHECK(Has(r.header, "#pragma once"));
    CHECK(Has(r.header, "namespace Aphelyon"));
    CHECK(Has(r.header, "class Inventory"));
    CHECK(Has(r.source, "#include \"Inventory.hpp\""));
    CHECK(Has(r.source, "namespace Aphelyon"));
    CHECK_FALSE(Has(r.header, "{{"));
    CHECK_FALSE(Has(r.source, "{{"));
}

TEST_CASE("ClassTemplates::Render derives a legal namespace from any project name", "[editor]")
{
    // A manifest name is free text ("My Game", "2D Test"); the namespace it
    // becomes must be an identifier.
    const ClassTemplates::Rendered spaced =
        ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "Thing", "My Game");
    CHECK(Has(spaced.header, "namespace My_Game"));

    const ClassTemplates::Rendered digit =
        ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "Thing", "2D Test");
    CHECK(Has(digit.header, "namespace _2D_Test"));

    const ClassTemplates::Rendered empty =
        ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "Thing", "");
    CHECK(Has(empty.header, "namespace Game"));   // the fallback when the name yields nothing
}

TEST_CASE("ClassTemplates::KindLabel names every kind", "[editor]")
{
    CHECK(std::string(ClassTemplates::KindLabel(ClassTemplates::Kind::Component)) == "Component");
    CHECK(std::string(ClassTemplates::KindLabel(ClassTemplates::Kind::System)) == "System");
    CHECK(std::string(ClassTemplates::KindLabel(ClassTemplates::Kind::PlainClass)) == "Plain class");
    CHECK(static_cast<int>(ClassTemplates::Kind::Count) == 3);
}
