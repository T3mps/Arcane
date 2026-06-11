// ArcaneTests runner entry point (Server CommonTests convention).
// Add new test files under Arcane/Tests/src; premake picks them up
// via the wildcard glob.

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
