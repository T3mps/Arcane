// Dockspace modal gate ([editor][modal]): at most one dockspace-level modal draws
// per frame, errors first. CPU-only.
#include <catch2/catch_test_macros.hpp>

#include "App/ModalGate.hpp"
#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>
#include <string>

TEST_CASE("errors beat unsaved, unsaved beats create, none is none", "[editor][modal]")
{
    using Arcane::Editor::ModalAllowed;
    using Arcane::Editor::ModalKind;
    CHECK(ModalAllowed(true, true, true) == ModalKind::Error);
    CHECK(ModalAllowed(true, false, false) == ModalKind::Error);
    CHECK(ModalAllowed(false, true, true) == ModalKind::UnsavedScene);
    CHECK(ModalAllowed(false, false, true) == ModalKind::CreateAsset);
    CHECK(ModalAllowed(false, false, false) == ModalKind::None);
}

TEST_CASE("DrawModals opens the unsaved popup only behind the gate", "[editor][modal]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();

    const std::size_t fn = src.find("void EditorApp::DrawModals(");
    REQUIRE(fn != std::string::npos);
    const std::size_t gate = src.find("ModalAllowed(", fn);
    REQUIRE(gate != std::string::npos);
    const std::size_t open = src.find("ImGui::OpenPopup(\"Unsaved Scene\")", fn);
    REQUIRE(open != std::string::npos);
    CHECK(open > gate);
    const std::size_t branch = src.find("allowed == Arcane::Editor::ModalKind::UnsavedScene", fn);
    REQUIRE(branch != std::string::npos);
    CHECK(branch < open);
    CHECK(src.find("editor.modal.probe") != std::string::npos);
}
