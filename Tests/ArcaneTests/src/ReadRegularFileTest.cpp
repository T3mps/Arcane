#include <catch2/catch_test_macros.hpp>
#include <Arcane/IO/ReadRegularFile.hpp>
#include "Helpers/ConstantScan.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::filesystem::path Scratch(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / "arcane-read-regular";
    std::filesystem::create_directories(dir);
    return dir / name;
}

TEST_CASE("ReadRegularFile refuses a directory, a non-regular path, and a file over the cap, and returns an empty vector for an empty file",
          "[io][read]") {
    const auto dir = Scratch("a-directory");
    std::filesystem::create_directory(dir);
    CHECK_FALSE(Arcane::ReadRegularFile(dir).has_value());

    const auto empty = Scratch("empty.bin");
    { std::ofstream out(empty, std::ios::binary); }
    const auto emptyBytes = Arcane::ReadRegularFile(empty);
    REQUIRE(emptyBytes.has_value());
    CHECK(emptyBytes->empty());

    const auto small = Scratch("sixteen.bin");
    {
        std::ofstream out(small, std::ios::binary);
        out << "0123456789abcdef";
    }
    CHECK_FALSE(Arcane::ReadRegularFile(small, 15).has_value());
    const auto exact = Arcane::ReadRegularFile(small, 16);
    REQUIRE(exact.has_value());
    CHECK(exact->size() == 16);

    CHECK_FALSE(Arcane::ReadRegularFile(Scratch("no-such-file")).has_value());

    // Pin: the body sizes with is_regular_file + file_size and never tellg.
    std::ifstream src(Arcane::Test::RepoRoot() / "Source/Runtime/ArcaneCore/src/Arcane/IO/ReadRegularFile.cpp");
    REQUIRE(src.good());
    std::ostringstream ss;
    ss << src.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("is_regular_file") != std::string::npos);
    CHECK(text.find("file_size") != std::string::npos);
    CHECK(text.find("tellg") == std::string::npos);
}
