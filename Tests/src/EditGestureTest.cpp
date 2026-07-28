#include "EditGesture.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace Arcane::Editor;
using Arcane::TransactionId;

TEST_CASE("EditGesture pure core: EvaluateEnd decision table", "[editor]")
{
    EditGesture::Slots s;
    s.txn  = static_cast<TransactionId>(7);
    s.item = 42;

    SECTION("owner mismatch is inert regardless of flags")
    {
        CHECK(EditGesture::EvaluateEnd(s, 99, true,  true) == EditGesture::EndAction::None);
        CHECK(EditGesture::EvaluateEnd(s, 99, false, true) == EditGesture::EndAction::None);
    }
    SECTION("owner + deactivated-after-edit commits")
    {
        CHECK(EditGesture::EvaluateEnd(s, 42, true, true) == EditGesture::EndAction::Commit);
    }
    SECTION("owner + plain deactivation cancels (a pure click never leaks a step)")
    {
        CHECK(EditGesture::EvaluateEnd(s, 42, false, true) == EditGesture::EndAction::Cancel);
    }
    SECTION("owner + still-active does nothing")
    {
        CHECK(EditGesture::EvaluateEnd(s, 42, false, false) == EditGesture::EndAction::None);
    }
    SECTION("joined gesture (txn None, item parked) still evaluates")
    {
        s.txn = TransactionId::None;
        CHECK(EditGesture::EvaluateEnd(s, 42, true, true) == EditGesture::EndAction::Commit);
    }
}

TEST_CASE("EditGesture pure core: abandonment + stale checks", "[editor]")
{
    EditGesture::Slots s;

    SECTION("cleared slots never close")
    {
        CHECK_FALSE(EditGesture::ShouldCloseAbandoned(s, 0, false));
        CHECK_FALSE(EditGesture::ShouldCloseStaleOnActivate(s, false));
    }
    SECTION("open txn + owner still holds ActiveId -> healthy, no close")
    {
        s.txn = static_cast<TransactionId>(3); s.item = 42;
        CHECK_FALSE(EditGesture::ShouldCloseAbandoned(s, 42, false));
    }
    SECTION("open txn + ActiveId moved on (or nothing active) -> abandoned")
    {
        s.txn = static_cast<TransactionId>(3); s.item = 42;
        CHECK(EditGesture::ShouldCloseAbandoned(s, 0, false));
        CHECK(EditGesture::ShouldCloseAbandoned(s, 7, false));
    }
    SECTION("builder-style joiner: txn None but a command is owed")
    {
        s.item = 42;
        CHECK(EditGesture::ShouldCloseAbandoned(s, 7, true));
        CHECK(EditGesture::ShouldCloseStaleOnActivate(s, true));
    }
    SECTION("open txn -> stale on a fresh activation")
    {
        s.txn = static_cast<TransactionId>(3); s.item = 42;
        CHECK(EditGesture::ShouldCloseStaleOnActivate(s, false));
    }
}
