#include "EditGesture.hpp"

#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>

using namespace Arcane::Editor;
using Arcane::TransactionId;

namespace
{
    // Minimal ICommand. The close tests below care only WHETHER a step landed:
    // the ICommand contract is that the live edit already happened, so a
    // no-op Undo/Redo is a legitimate command.
    struct MarkerCommand final : public Arcane::ICommand
    {
        void Undo() override {}
        void Redo() override {}
        const char* Label() const override { return "Marker"; }
    };

    // A real CommandStack over an EMPTY registry: these tests never snapshot a
    // component, so `resolve` is never called and the registry exists only to
    // satisfy the ctor's contract. No RegisterSceneComponents (nothing here
    // needs a component type) and deliberately no Arcane::Runtime -- a bare one
    // in a test steals Arcane.dll's TypeContext.
    struct StackFixture
    {
        Astra::Registry      registry;   // Registry.hpp:48, default Config
        Arcane::CommandStack stack{[this]() -> Astra::Registry& { return registry; }};

        // Land a step and undo it, so there IS a redo entry for the close to
        // either clear or preserve. Without this the redo assertions below
        // would pass vacuously.
        void SeedRedo()
        {
            stack.Push(std::make_unique<MarkerCommand>());
            stack.Undo();
        }
    };
}

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

// ClosePending against a REAL stack. These two pin the contract the
// builder-style adopters (ShaderEditorDocument's param + graph gestures,
// SpriteDocument) lean on: the close runs the parked builder exactly once and
// clears the slots either way, and a builder that pushes NOTHING costs
// nothing -- no junk step, and the redo stack survives.
TEST_CASE("EditGesture::ClosePending lands a parked builder's step", "[editor]")
{
    StackFixture f;
    f.SeedRedo();
    REQUIRE(f.stack.CanRedo());
    REQUIRE_FALSE(f.stack.CanUndo());

    EditGesture::GestureState st;
    st.slots.txn  = f.stack.Begin("Edit Thing");
    st.slots.item = 42;
    REQUIRE(st.slots.txn != TransactionId::None);
    st.pendingCommit = [&f] { f.stack.Push(std::make_unique<MarkerCommand>()); };

    EditGesture::ClosePending(f.stack, st);

    CHECK(f.stack.CanUndo());
    // The step wears the TRANSACTION's label (CommandStack.cpp:45), not the
    // pushed command's -- which is exactly why the adopters park their label
    // on the Begin call rather than on the command they build at close.
    CHECK(std::string(f.stack.UndoLabel()) == "Edit Thing");
    CHECK_FALSE(f.stack.CanRedo());        // a landed step clears redo (CommandStack.cpp:70)
    CHECK_FALSE(f.stack.InTransaction());
    CHECK(st.slots.txn == TransactionId::None);
    CHECK(st.slots.item == 0u);
    CHECK_FALSE(static_cast<bool>(st.pendingCommit));   // runs exactly once
}

TEST_CASE("EditGesture::ClosePending drops a no-op gesture, redo intact", "[editor]")
{
    StackFixture f;
    f.SeedRedo();
    REQUIRE(f.stack.CanRedo());

    EditGesture::GestureState st;
    st.slots.txn  = f.stack.Begin("Edit Thing");
    st.slots.item = 42;
    bool ran = false;
    st.pendingCommit = [&ran] { ran = true; };   // builder decided nothing changed

    EditGesture::ClosePending(f.stack, st);

    CHECK(ran);                        // the builder DID run; it chose not to push
    // Nothing was pushed and nothing was snapshotted, so Commit's empty
    // transaction is dropped before it ever reaches the redo clear
    // (CommandStack.cpp:61-62, ahead of :70). These two are the assertions the
    // shader-editor builders' no-op guard depends on: if that drop ever
    // regressed, an unedited close would leave a junk step AND wipe redo, and
    // BOTH of these would fail.
    CHECK_FALSE(f.stack.CanUndo());
    CHECK(f.stack.CanRedo());
    CHECK_FALSE(f.stack.InTransaction());
    CHECK(st.slots.txn == TransactionId::None);
    CHECK(st.slots.item == 0u);
    CHECK_FALSE(static_cast<bool>(st.pendingCommit));
}
