// Asset-manager arc (Plan 2 Task 3): AssetReferenceIndex -- the editor's
// pure, engine-facade-free inverted-reference index over ListAssetReferences
// answers (spec s9.1). Headless: no registry, no temp dirs, no filesystem at
// all -- every guid below is a Guid::FromString literal, and every "refs
// answer" is hand-built via the local Refs() helper. Task 4's real caller
// feeds this from AssetPanelModel's rebuild; this file drives the unit
// directly instead.
//
// The two UE-derived disciplines Update's doc comment enforces (see
// AssetReferenceIndex.hpp) are the whole point of these cases: "re-walk
// removes old edges before re-adding" and "tombstones for referenced-but-
// gone targets" are each pinned by name below, and are designed to go red
// if Update's five-step order ever gets reordered.

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetReferenceIndex.hpp"

#include <optional>
#include <vector>

using namespace Arcane::Editor;

namespace
{
    // Guid::FromString literal -> Guid, REQUIRE-gated so a typo'd hex
    // literal fails loudly at the call site instead of silently
    // constructing a nil/garbage guid.
    Arcane::Guid ParseGuid(const char* s)
    {
        const auto g = Arcane::Guid::FromString(s);
        REQUIRE(g.has_value());
        return *g;
    }

    // Tiny local helper matching the brief's shorthand: wraps a braced list
    // of AssetRef into the optional<vector<AssetRef>> Update expects.
    std::optional<std::vector<Arcane::AssetRef>> Refs(std::vector<Arcane::AssetRef> refs)
    {
        return refs;
    }
}

TEST_CASE("AssetReferenceIndex builds forward, inverted and inbound counts", "[editor]")
{
    const auto A = ParseGuid("7e5d0001-0001-4001-8001-00000000000a");
    const auto B = ParseGuid("7e5d0001-0001-4001-8001-00000000000b");
    const auto C = ParseGuid("7e5d0001-0001-4001-8001-00000000000c");

    AssetReferenceIndex idx;
    idx.Update(A, true, Refs({ { B, Arcane::AssetRefKind::References } }));
    idx.Update(C, true, Refs({ { B, Arcane::AssetRefKind::DerivesFrom } }));
    idx.Update(B, true, Refs({}));

    CHECK(idx.InboundCount(B) == 2);   // both edge kinds count
    REQUIRE(idx.Find(B));
    CHECK(idx.Find(B)->inbound == std::vector<Arcane::Guid>{ A, C });   // sorted
}

TEST_CASE("AssetReferenceIndex re-walk removes old edges before re-adding (UE discipline)", "[editor]")
{
    // A->B, then A re-walks to ->C. Without the removal pass B keeps A
    // forever (the classic incremental-index corruption, spec s9.1).
    const auto A = ParseGuid("7e5d0002-0001-4001-8001-00000000000a");
    const auto B = ParseGuid("7e5d0002-0001-4001-8001-00000000000b");
    const auto C = ParseGuid("7e5d0002-0001-4001-8001-00000000000c");

    AssetReferenceIndex idx;
    idx.Update(A, true, Refs({ { B, Arcane::AssetRefKind::References } }));
    idx.Update(B, true, Refs({}));
    idx.Update(A, true, Refs({ { C, Arcane::AssetRefKind::References } }));

    CHECK(idx.InboundCount(B) == 0);
    CHECK(idx.InboundCount(C) == 1);
}

TEST_CASE("AssetReferenceIndex re-walk with identical refs is idempotent", "[editor]")
{
    // A->B twice -> B.inbound holds A exactly once.
    const auto A = ParseGuid("7e5d0003-0001-4001-8001-00000000000a");
    const auto B = ParseGuid("7e5d0003-0001-4001-8001-00000000000b");

    AssetReferenceIndex idx;
    idx.Update(B, true, Refs({}));
    idx.Update(A, true, Refs({ { B, Arcane::AssetRefKind::References } }));
    idx.Update(A, true, Refs({ { B, Arcane::AssetRefKind::References } }));

    CHECK(idx.InboundCount(B) == 1);
    REQUIRE(idx.Find(B));
    CHECK(idx.Find(B)->inbound == std::vector<Arcane::Guid>{ A });
}

TEST_CASE("AssetReferenceIndex keeps last-known-good on nullopt (spec s3.2)", "[editor]")
{
    // A->B; Update(A, true, nullopt) -> InboundCount(B) still 1; A's
    // outbound intact.
    const auto A = ParseGuid("7e5d0004-0001-4001-8001-00000000000a");
    const auto B = ParseGuid("7e5d0004-0001-4001-8001-00000000000b");

    AssetReferenceIndex idx;
    idx.Update(B, true, Refs({}));
    idx.Update(A, true, Refs({ { B, Arcane::AssetRefKind::References } }));

    idx.Update(A, true, std::nullopt);

    CHECK(idx.InboundCount(B) == 1);
    REQUIRE(idx.Find(A));
    REQUIRE(idx.Find(A)->outbound.size() == 1);
    CHECK(idx.Find(A)->outbound[0].target == B);
    CHECK(idx.Find(A)->outbound[0].kind == Arcane::AssetRefKind::References);
}

TEST_CASE("AssetReferenceIndex tombstones an unresolvable target and reports it dangling", "[editor]")
{
    // A -> M (M never walked): Find(M)->exists == false, inbound {A};
    // DanglingTargets() == {M}.
    const auto A = ParseGuid("7e5d0005-0001-4001-8001-00000000000a");
    const auto M = ParseGuid("7e5d0005-0001-4001-8001-0000000000dd");

    AssetReferenceIndex idx;
    idx.Update(A, true, Refs({ { M, Arcane::AssetRefKind::References } }));

    const AssetReferenceIndex::Node* node = idx.Find(M);
    REQUIRE(node);
    CHECK_FALSE(node->exists);
    CHECK(node->inbound == std::vector<Arcane::Guid>{ A });
    CHECK(idx.DanglingTargets() == std::vector<Arcane::Guid>{ M });
}

TEST_CASE("AssetReferenceIndex garbage-collects a tombstone when its last referencer lets go", "[editor]")
{
    // ...then Update(A, true, Refs({})) -> Find(M) == nullptr.
    const auto A = ParseGuid("7e5d0006-0001-4001-8001-00000000000a");
    const auto M = ParseGuid("7e5d0006-0001-4001-8001-0000000000dd");

    AssetReferenceIndex idx;
    idx.Update(A, true, Refs({ { M, Arcane::AssetRefKind::References } }));
    REQUIRE(idx.Find(M));

    idx.Update(A, true, Refs({}));

    CHECK(idx.Find(M) == nullptr);
    CHECK(idx.InboundCount(M) == 0);
    CHECK(idx.DanglingTargets().empty());
}

TEST_CASE("AssetReferenceIndex tombstones a deleted asset that is still referenced", "[editor]")
{
    // A->B (both exist); Update(B, false, nullopt) -> B is a tombstone
    // (exists=false, inbound {A}), DanglingTargets() == {B}; its outbound
    // edges were removed. Then Update(A, true, Refs({})) erases it.
    const auto A = ParseGuid("7e5d0007-0001-4001-8001-00000000000a");
    const auto B = ParseGuid("7e5d0007-0001-4001-8001-00000000000b");
    const auto C = ParseGuid("7e5d0007-0001-4001-8001-00000000000c");

    AssetReferenceIndex idx;
    idx.Update(C, true, Refs({}));
    idx.Update(B, true, Refs({ { C, Arcane::AssetRefKind::References } }));
    idx.Update(A, true, Refs({ { B, Arcane::AssetRefKind::References } }));

    idx.Update(B, false, std::nullopt);

    const AssetReferenceIndex::Node* bNode = idx.Find(B);
    REQUIRE(bNode);
    CHECK_FALSE(bNode->exists);
    CHECK(bNode->inbound == std::vector<Arcane::Guid>{ A });
    CHECK(bNode->outbound.empty());
    CHECK(idx.DanglingTargets() == std::vector<Arcane::Guid>{ B });
    CHECK(idx.InboundCount(C) == 0);   // B's own outbound edge into C was removed

    idx.Update(A, true, Refs({}));

    CHECK(idx.Find(B) == nullptr);
}

TEST_CASE("AssetReferenceIndex: a never-parsed asset contributes nothing", "[editor]")
{
    // Update(A, true, nullopt) on a fresh A -> A exists, zero outbound, and
    // no inbound appears anywhere (spec s3.2's "contributes nothing"
    // sentence).
    const auto A = ParseGuid("7e5d0008-0001-4001-8001-00000000000a");
    const auto B = ParseGuid("7e5d0008-0001-4001-8001-00000000000b");

    AssetReferenceIndex idx;
    idx.Update(A, true, std::nullopt);

    const AssetReferenceIndex::Node* node = idx.Find(A);
    REQUIRE(node);
    CHECK(node->exists);
    CHECK(node->outbound.empty());
    CHECK(node->inbound.empty());
    CHECK(idx.InboundCount(B) == 0);
    CHECK(idx.DanglingTargets().empty());
    CHECK(idx.NodeCount() == 1);
}
