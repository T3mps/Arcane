// ServerApp startup (review round 1, Task 17): a requested --remote whose Start
// fails ends the run through Finish (exit 2, report written), and a --report
// that cannot be written can never leave the exit code at 0. No test builds a
// ServerApp, so this is a source pin; the Servitor launch with an unwritable
// --remote-descriptor is the behavior witness.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace
{
    std::string ReadRepoFile(const char* relative)
    {
        std::ifstream in(Arcane::Test::RepoRoot() / relative);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
}

TEST_CASE("a requested remote Start failure returns Finish, and a failed report cannot exit 0", "[server]")
{
    const auto text = ReadRepoFile("Source/Programs/ArcaneServer/src/ServerApp.cpp");
    const auto start = text.find("if (!m_remote->Start())");
    REQUIRE(start != std::string::npos);
    const auto window = text.substr(start, 180);
    CHECK(window.find("return Finish") != std::string::npos);
    CHECK(window.find("m_remote.reset()") == std::string::npos);
    const auto finish = text.find("ServerApp::Finish");
    REQUIRE(finish != std::string::npos);
    const auto finishBody = text.substr(finish, 500);
    CHECK(finishBody.find("return 1") != std::string::npos);
}
