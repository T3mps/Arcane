#include <catch2/catch_test_macros.hpp>
#include "App/ModalErrorQueue.hpp"

using namespace Arcane::Editor;

TEST_CASE("errors display FIFO, one at a time", "[editor][modal]")
{
    ModalErrorQueue q;
    CHECK(q.Front() == nullptr);
    q.Push("Scene Error", "bad file");
    q.Push("Play in Separate Window Failed", "no exe");
    REQUIRE(q.Front() != nullptr);
    CHECK(q.Front()->title == "Scene Error");
    q.Pop();
    REQUIRE(q.Front() != nullptr);
    CHECK(q.Front()->title == "Play in Separate Window Failed");
    CHECK(q.Front()->message == "no exe");
    q.Pop();
    CHECK(q.Front() == nullptr);
}

TEST_CASE("Clear drops everything (project switch)", "[editor][modal]")
{
    ModalErrorQueue q;
    q.Push("Open Project Failed", "stale");
    q.Clear();
    CHECK(q.Front() == nullptr);
}
