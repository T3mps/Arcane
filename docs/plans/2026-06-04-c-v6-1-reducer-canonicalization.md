# C-V6-1: Reducer Canonicalization (Spec-Aligned Refactor) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close C-V6-1 (the entire reducer pipeline is dead production code) by refactoring every event-emitting handler to the spec-canonical `reducer.Apply(state, event) → std::visit(EffectDispatcher) → AppendEvent` shape, eliminating the live-vs-replay divergence risk and unblocking the snapshot/replay design (C-V5-1).

**Architecture:** Per-aggregate TDD-driven refactor. For each of the 4 event-sourced aggregates (Wallet, Pulls, QuestClaims, Progression) and each of the 5 event-emitting handlers, write an agreement test FIRST that pins `reducer.Apply(pre_state, handler_emitted_event) == handler's_post_live_state`. Verify the agreement test passes against current code (proves event payloads are reducer-compatible). Then refactor the handler to invoke the reducer + EffectDispatcher directly, re-running the agreement test as the regression guard. Bundle the 5 surfaced Mediums (FromStr coercion, FromJson zero-fill, isReplay threading, ProgressionReducer baseline validation, WorldFlagStore dirty bits) since they all activate the moment the reducer becomes load-bearing.

**Tech Stack:** C++20, Visual Studio 2026 / premake5, Catch2 v3 (AccountTests), libpqxx, Postgres 16. PowerShell 5.1 on Windows.

**Spec:** `docs/superpowers/specs/2026-06-01-account-db-migration-design.md` — "Pure-function reducer signature" (line 679), "Triple-replay equality" property tests (line 688), "isReplay parameter threaded through the dispatch" (line 699), "Two channels" for side effects (line 698). The reducer is the spec-canonical state-transition logic; handlers regressed.

**Audit context:**
- v6 audit synthesis: `docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md` (C-V6-1 headline)
- v6 wiring followup: `docs/superpowers/audits/2026-06-04-v6-followup-wiring-completeness.md`
- v6 event-sourcing followup: `docs/superpowers/audits/2026-06-04-v6-followup-event-sourcing.md` (bundled Mediums)

**Build prerequisites:** `VCPKG_ROOT` env var set, `Server\scripts\setup-vcpkg-deps.bat` already run, Postgres up via `Server\scripts\db-setup.bat` (REQUIRED — agreement tests use IntegrationDbFixture).

**Commit naming:** `chore: c-v6-1 phase X step Y — <description> (M-V5-4 / M-V6-X / etc.)`. The C-V6-1 series is distinct from the v5 medium-tail (a..r). This is its own arc.

---

## Approach summary

**5 handler sites refactored:**
1. `AccountHandlers::HandleAddCurrency` — Wallet only (lowest risk, tracer)
2. `ProgressionHandlers::CommitProgressionScrapSpend` (shared helper used by 4 progression handlers) — Wallet only
3. `GachaHandlers::HandlePull` — Pulls + Wallet (introduces EffectDispatcher use)
4. `GachaHandlers::HandleMultiPull` — Pulls + Wallet (10-pull guarantee logic)
5. `QuestHandlers::HandleClaimQuestReward` — QuestClaims + Wallet[] + optional Progression (most complex)

**Per-handler pattern:**
- Snapshot pre-state via test-only `Snapshot*State(Account)` helpers
- Build event (handler keeps RNG / domain logic)
- Run `reducer.Apply(pre_state, event_payload)` — verifies invariants
- Apply reducer's `out.state` to live Account (state setters)
- Visit reducer's `out.effects` via `EffectDispatcher` (does collection mutations + outbox emits)
- `AppendEvent` to the transaction

**Bundled Mediums (close in the cleanup phase, AFTER the refactor proves out):**
- M-V5-2 ES: ProgressionReducer doesn't validate `evt.from_level == s.story_level` — add it (now bites because reducer goes live)
- M-V6-1 ES: `CurrencyFromStr` / `RewardKindFromStr` silently coerce unknown vocabulary to Credits — throw instead
- M-V6-2 ES: `*FromJson` zero-fill for missing required fields — require + throw
- M-V6-3 wiring: `isReplay` parameter unthreaded — thread through AccountTransaction + EffectDispatcher
- M-V6-1 persistence: WorldFlagStore mutations never set dirty bits — close
- H-V6-1: OutboxRelay startup self-check (independent but bundled per audit recommendation)

---

## Phase 0 — Foundation (3 tasks)

**Why first:** The agreement tests need state-snapshot helpers; the EffectDispatcher needs `isReplay`; the outbox self-check unblocks visibility into the new dispatched-to destinations.

### Task 1: Add `isReplay` flag to AccountTransaction + EffectDispatcher

**Files:**
- Modify: `Server/Account/src/Cache/AccountTransaction.hpp` (add `is_replay_` field + ctor arg)
- Modify: `Server/Account/src/Effects/EffectDispatcher.hpp` (consult `txn_.IsReplay()` before outbox emits)

**Rationale:** Spec line 699 mandates `isReplay` threading. Live mode emits to outbox; replay mode skips outbox emits (replay shouldn't re-fire toasts/telemetry/inventory-grants). All current code constructs `AccountTransaction` in live mode; this task adds the parameter (defaulting to false) without changing any behavior, but makes EffectDispatcher honor it.

- [ ] **Step 1: Read the current AccountTransaction ctor**

```powershell
Get-Content Server\Account\src\Cache\AccountTransaction.hpp | Select-Object -First 80
```

Locate the constructor (currently around line 38). It takes `db::ConnectionPool&`, `db::EventStore&`, `Account&`.

- [ ] **Step 2: Add `is_replay_` member + ctor parameter**

In `Server/Account/src/Cache/AccountTransaction.hpp`, find the constructor:

```cpp
AccountTransaction(db::ConnectionPool& pool,
                   db::EventStore& store,
                   Aphelyon::Account& account)
    : pool_(pool)
    , store_(store)
    , account_(account)
    , preTxSnapshot_(account.CaptureSnapshot())
{}
```

Replace with:

```cpp
// Audit C-V6-1 (2026-06-04): is_replay flag threaded through the
// transaction for spec line 699 compliance. Live mode emits side
// effects (outbox toasts, telemetry, inventory grants); replay
// mode (cold-start reducer-fold from snapshot + event log) skips
// them so they don't re-fire. EffectDispatcher consults this flag
// via the IsReplay() accessor. Defaults false; no production
// caller constructs in replay mode today — this is plumbing for
// future cold-start replay (C-V5-1 SnapshotWriter wire-up).
AccountTransaction(db::ConnectionPool& pool,
                   db::EventStore& store,
                   Aphelyon::Account& account,
                   bool is_replay = false)
    : pool_(pool)
    , store_(store)
    , account_(account)
    , preTxSnapshot_(account.CaptureSnapshot())
    , is_replay_(is_replay)
{}
```

Then find the private member declaration block (search for `pool_` member). Add `is_replay_`:

```cpp
private:
    db::ConnectionPool& pool_;
    db::EventStore& store_;
    Aphelyon::Account& account_;
    // ... existing members ...
    bool is_replay_ = false;
```

Add the public accessor at a sensible location (near other accessors):

```cpp
public:
    bool IsReplay() const { return is_replay_; }
```

- [ ] **Step 3: Update EffectDispatcher to consult IsReplay**

In `Server/Account/src/Effects/EffectDispatcher.hpp`, find the three operators that emit to outbox: `ToastEffect`, `TelemetryEffect`, `GrantMaterialEffect`.

Add an `IsReplay()` early-return guard to each. Replace:

```cpp
void operator()(const reducers::ToastEffect& e) {
    txn_.EmitToOutbox("notifications.toast",
        nlohmann::json{
            {"account_id", account_.GetAccountId()},
            {"message",    e.player_message}
        });
}
```

with:

```cpp
void operator()(const reducers::ToastEffect& e) {
    // Audit C-V6-1 (2026-06-04): skip outbox emit on replay so
    // toasts don't re-fire when cold-start replay re-folds the
    // event log. Spec line 698-699.
    if (txn_.IsReplay()) return;
    txn_.EmitToOutbox("notifications.toast",
        nlohmann::json{
            {"account_id", account_.GetAccountId()},
            {"message",    e.player_message}
        });
}
```

Apply the same `if (txn_.IsReplay()) return;` guard at the top of `operator()(const reducers::TelemetryEffect&)` and inside `operator()(const reducers::GrantMaterialEffect&)` BEFORE the `txn_.EmitToOutbox(...)` call (but AFTER `account_.AddMaterial` — the material itself must apply during replay to rebuild state; only the outbox notification is skipped).

For `GrantWeaponEffect`, `GrantCharacterEffect`, `GrantCurrencyEffect`: these mutate Account state (collection, wallet) — they MUST run during replay to rebuild state. Do NOT add the guard.

- [ ] **Step 4: Build to verify no break**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug
```

Expected: clean build. No existing caller of `AccountTransaction` ctor passes the 4th arg, so they all default to `is_replay=false` (live mode).

- [ ] **Step 5: Run full test suite**

```powershell
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
```

Expected: 159 cases / 1131 assertions pass (v6 baseline). The `is_replay=false` default preserves all current behavior.

- [ ] **Step 6: Commit**

```powershell
git add Server\Account\src\Cache\AccountTransaction.hpp Server\Account\src\Effects\EffectDispatcher.hpp
git commit -m @'
chore: c-v6-1 phase 0 step 1 — thread isReplay through AccountTransaction + EffectDispatcher (M-V6-3 wiring)

Foundation for C-V6-1's reducer canonicalization. AccountTransaction
gains an is_replay flag (defaults false) and IsReplay() accessor.
EffectDispatcher consults it to skip outbox emits on replay
(ToastEffect, TelemetryEffect, GrantMaterialEffect outbox-emit
branch). State-mutating effects (GrantWeapon/Character/Currency
and GrantMaterial's account.AddMaterial call) run unconditionally
because replay must rebuild state.

No production caller passes is_replay=true today — this is plumbing
for future cold-start replay (C-V5-1 SnapshotWriter wire-up). All
current behavior preserved (default false everywhere). Closes the
M-V6-3 wiring deferral.

Spec: docs/superpowers/specs/2026-06-01-account-db-migration-design.md (line 698-699)
Audit: docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md (C-V6-1)
'@
```

### Task 2: Add OutboxRelay startup self-check (H-V6-1)

**Files:**
- Modify: `Server/Account/src/Db/OutboxRelay.hpp` (add post-Start self-check)

**Rationale:** The C-V6-1 refactor will make EffectDispatcher actually run in production, which means `notifications.toast`, `telemetry.event`, and `inventory.grant_material` rows will start landing in the outbox. None of those destinations have registered handlers. H-V6-1 surfaces this as a soft WARN so ops can see the dispatch gap without it being silent.

- [ ] **Step 1: Locate the Start() / Run() loop**

```powershell
Get-Content Server\Account\src\Db\OutboxRelay.hpp | Select-String -Pattern 'void Start|void Run\(' -Context 0,3
```

- [ ] **Step 2: Add self-check helper + call site**

In `Server/Account/src/Db/OutboxRelay.hpp`, find `Start()` (or whatever method spawns the worker). Add a self-check call BEFORE the worker thread spawns:

```cpp
// Audit H-V6-1 wiring (2026-06-04): one-shot startup self-check.
// If outbox has undispatched rows and no destination handlers
// are registered, warn loudly. This catches the C-V4-1 / C-V5-1 /
// C-V6-1 wiring-vs-implementation defect class at startup
// rather than letting rows silently accumulate. Soft warning,
// not a refuse-to-start, because pre-launch the user may
// intentionally defer handler registration.
void RunStartupSelfCheck() {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    if (!handlers_.empty()) return;
    try {
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        auto r = tx.exec(
            "SELECT COUNT(*) FROM outbox WHERE dispatched_at IS NULL");
        const auto pending = r[0][0].as<std::int64_t>();
        tx.commit();
        if (pending > 0) {
            LOG_DB_WARN("OutboxRelay: {} undispatched rows with zero "
                        "registered handlers — dispatch path is "
                        "dead-code-by-omission. Call Register() for "
                        "the outbox destinations your handlers emit "
                        "to (e.g. notifications.toast, telemetry.event, "
                        "inventory.grant_material).",
                        pending);
        }
    } catch (const std::exception& e) {
        LOG_DB_WARN("OutboxRelay startup self-check failed: {}", e.what());
    }
}
```

Then in `Start()` BEFORE the worker thread spawn line, add:

```cpp
RunStartupSelfCheck();
```

- [ ] **Step 3: Build + run AccountTests for regression**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:Account
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
```

Expected: clean build, all tests pass. Self-check is fire-and-forget.

- [ ] **Step 4: Commit**

```powershell
git add Server\Account\src\Db\OutboxRelay.hpp
git commit -m @'
chore: c-v6-1 phase 0 step 2 — OutboxRelay startup self-check (H-V6-1 wiring)

One-shot startup WARN if outbox has undispatched rows AND no
destination handlers are registered. Catches the wiring gap
(C-V4-1 / C-V5-1 / C-V6-1 pattern) at startup. Soft warning,
not refuse-to-start, because pre-launch handler registration
is intentionally deferred.

Independent of C-V6-1 mechanically but bundled here because the
refactor about to land will start emitting to notifications.toast,
telemetry.event, and inventory.grant_material — destinations that
currently have no registered handlers. The self-check makes the
gap visible at boot.

Audit: docs/superpowers/audits/2026-06-04-v6-followup-wiring-completeness.md (H-V6-1)
'@
```

### Task 3: Add state-snapshot helpers for agreement tests

**Files:**
- Create: `Server/Account/tests/Helpers/ReducerStateSnapshot.hpp` (test-only header)

**Rationale:** Agreement tests need to extract the same state the reducers project. These helpers go in `tests/Helpers/` because they're test infrastructure, not production code. They read from a live `Account` and produce the reducer-shaped `State` types.

- [ ] **Step 1: Create the helper header**

Create `Server/Account/tests/Helpers/ReducerStateSnapshot.hpp`:

```cpp
#pragma once

// Test-only helpers: extract reducer-shaped State from a live Account.
// Used by agreement tests to pin handler-vs-reducer state equivalence
// (audit C-V6-1, 2026-06-04). NOT production code — handlers don't
// snapshot state via these; they use Account's getters directly.

#include "State/Account.hpp"
#include "Reducers/WalletReducer.hpp"
#include "Reducers/PullsReducer.hpp"
#include "Reducers/QuestClaimsReducer.hpp"
#include "Reducers/ProgressionReducer.hpp"
#include "Events/WalletEvents.hpp"

namespace aphelyon::test {

// Extract the reducer's WalletState from a live Account's wallet.
inline reducers::WalletState SnapshotWalletState(const Aphelyon::Account& account) {
    reducers::WalletState s;
    const auto& w = account.GetWallet();
    s.Set(events::wallet::Currency::Credits,          w.GetCredits());
    s.Set(events::wallet::Currency::UniversalCredits, w.GetUniversalCredits());
    s.Set(events::wallet::Currency::Tickets,          w.GetTickets());
    s.Set(events::wallet::Currency::LimitedTickets,   w.GetLimitedTickets());
    s.Set(events::wallet::Currency::Scrap,            w.GetScrap());
    return s;
}

// Extract the reducer's PullsState from a live Account.
// Each slot's SlotPullsState carries pity_5, pity_4, guarantee_5, rng_state.
// The reducer's `rng_state` field on SlotPullsState is per-slot in the
// reducer's projection; production has ONE RNG per account. Tests should
// only compare per-slot pity + guarantee; rng_state comparison is
// orthogonal (the same account-wide RNG advances across all slots).
inline reducers::PullsState SnapshotPullsState(
    const Aphelyon::Account& account,
    const std::vector<std::string>& slot_ids)
{
    reducers::PullsState s;
    for (const auto& slot_id : slot_ids) {
        const auto pity_state = account.GetPityState();
        auto pit = pity_state.find(slot_id);
        reducers::SlotPullsState slot_s;
        if (pit != pity_state.end()) {
            slot_s.pity_5      = pit->second.fiveStar;
            slot_s.pity_4      = pit->second.fourStar;
            slot_s.guarantee_5 = pit->second.guaranteed;
        }
        s.slots[slot_id] = slot_s;
    }
    return s;
}

// Extract the reducer's QuestClaimsState (the set of claimed quest_ids)
// from a live Account.
inline reducers::QuestClaimsState SnapshotQuestClaimsState(const Aphelyon::Account& account) {
    reducers::QuestClaimsState s;
    for (const auto& [quest_id, q] : account.GetQuestStates().GetAll()) {
        if (q.IsClaimed()) s.claimed_quest_ids.insert(quest_id);
    }
    return s;
}

// Extract the reducer's ProgressionState from a live Account.
inline reducers::ProgressionState SnapshotProgressionState(const Aphelyon::Account& account) {
    reducers::ProgressionState s;
    s.story_level     = account.GetStoryLevel();
    s.story_xp        = account.GetStoryXp();
    s.difficulty_tier = account.GetDifficultyTier();
    return s;
}

} // namespace aphelyon::test
```

- [ ] **Step 2: Verify the test build picks up the new header**

The AccountTests premake glob includes `%{prj.location}/tests/**.hpp` so the new file is picked up automatically. Verify by building:

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
```

Expected: clean build. The helpers are header-only inline functions; no `.cpp` needed.

- [ ] **Step 3: Run AccountTests to confirm no regression**

```powershell
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
```

Expected: 159 cases / 1131 assertions pass. The helpers aren't used yet by any test; the build just needs to confirm header is well-formed.

- [ ] **Step 4: Commit**

```powershell
git add Server\Account\tests\Helpers\ReducerStateSnapshot.hpp
git commit -m @'
chore: c-v6-1 phase 0 step 3 — add reducer-state snapshot helpers for agreement tests

Test-only header providing SnapshotWalletState / SnapshotPullsState /
SnapshotQuestClaimsState / SnapshotProgressionState helpers. Extract
the reducer-shaped State types from a live Account so agreement tests
can pin handler-vs-reducer state equivalence (audit C-V6-1).

NOT production code — handlers continue to use Account's getters
directly. These helpers exist to support the agreement-test pattern
that each Phase 1-4 task uses as TDD-first guard before refactoring.

Audit: docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md (C-V6-1)
'@
```

---

## Phase 1 — Wallet aggregate (4 tasks)

**Why first:** WalletReducer emits no side-effect descriptors (verifier-only). Smallest blast radius. Two handler sites use Wallet events directly: `HandleAddCurrency` (single CurrencyDelta) and `CommitProgressionScrapSpend` (single CurrencyDelta, helper used by 4 progression handlers). HandlePull/HandleMultiPull/HandleClaimQuestReward also emit Wallet events but they bundle with Pulls/QuestClaims aggregates — those land in later phases.

### Task 4: Agreement test for HandleAddCurrency → WalletReducer

**Files:**
- Create: `Server/Account/tests/Integration/HandleAddCurrencyAgreementTest.cpp`

**Rationale:** Lowest-risk handler to start with. Single CurrencyDelta event, no side effects. Test pattern: snapshot pre-state, invoke handler via existing test infrastructure, snapshot the emitted event, run WalletReducer.Apply, assert reducer's result == live post-state.

- [ ] **Step 1: Locate the existing AddCurrency end-to-end test for fixture reference**

```powershell
Get-Content Server\Account\tests\Integration\AddCurrencyEndToEndTest.cpp | Select-Object -First 60
```

Note the fixture pattern (`IntegrationDbFixture`), how the handler is invoked, and how events are extracted from the test transaction.

- [ ] **Step 2: Write the agreement test**

Create `Server/Account/tests/Integration/HandleAddCurrencyAgreementTest.cpp`:

```cpp
// Audit C-V6-1 (2026-06-04): agreement test for HandleAddCurrency's
// wallet event-emit path. Asserts that running WalletReducer.Apply
// on the handler-emitted CurrencyDelta event produces the same
// post-state as the handler's live mutation. Proves the event payload
// is sufficient for replay (reducer can rebuild state from it).
//
// Test pattern (TDD-first, refactor-second):
//   1. Snapshot pre-state via SnapshotWalletState.
//   2. Run handler (which mutates wallet + emits CurrencyDelta).
//   3. Snapshot post-state via SnapshotWalletState.
//   4. Extract the emitted CurrencyDelta from the test transaction.
//   5. Run WalletReducer.Apply(pre_state, event) → reducer_state.
//   6. Assert reducer_state == post-state.
//
// This test PASSES against the current code (pre-refactor) because
// the handler's inline wallet.AddBy delta computation matches the
// reducer's s.Get(c) + evt.amount computation by construction. The
// refactor in Task 5 preserves this — the test is the regression
// guard.

#include <catch2/catch_test_macros.hpp>

#include "Handlers/AccountHandlers.hpp"
#include "Reducers/WalletReducer.hpp"
#include "Events/WalletEvents.hpp"
#include "Helpers/ReducerStateSnapshot.hpp"
#include "IntegrationDbFixture.hpp"
#include "Util/UuidV7.hpp"

#include <string>

using namespace Aphelyon;
using namespace aphelyon::reducers;
using aphelyon::test::IntegrationDbFixture;
using aphelyon::test::SnapshotWalletState;

static std::string UniqueUsername(const std::string& prefix) {
    return prefix + "_" + aphelyon::UuidV7::ToString(aphelyon::UuidV7::Generate());
}

TEST_CASE_METHOD(IntegrationDbFixture,
    "HandleAddCurrency → WalletReducer agreement: credits grant",
    "[integration][reducer-agreement][wallet]")
{
    // Setup: create account, build HandlerContext + AccountHandlers
    auto created = repo.Create(UniqueUsername("addcurr_agreement"), "x");
    REQUIRE(created);

    // Use the same Build-AccountHandlers pattern as AddCurrencyEndToEndTest.
    // (The fixture exposes whatever helper assembles HandlerContext + handlers;
    // mirror that.)
    auto handlers = BuildAccountHandlers();

    // Acquire the locked Account via the cache to get a live reference
    auto lockedRef = handlers.ctx().getLockedAccount(created->id);
    REQUIRE(lockedRef);
    Account& account = *lockedRef.account;

    // 1. Snapshot pre-state
    const WalletState pre_state = SnapshotWalletState(account);
    REQUIRE(pre_state.Get(events::wallet::Currency::Credits) == 0);  // fresh account

    // 2. Invoke handler. AddCurrency payload: { currency_type, amount, idempotency_key }
    const std::string payload = R"({"currency_type":"credits","amount":1000,"idempotency_key":"agreement-test-key-1"})";
    auto response = handlers.HandleAddCurrency(payload, created->id, "127.0.0.1");
    REQUIRE(response.find("success") != std::string::npos);

    // 3. Snapshot post-state
    const WalletState post_live = SnapshotWalletState(account);
    REQUIRE(post_live.Get(events::wallet::Currency::Credits) == 1000);

    // 4. Extract emitted CurrencyDelta from the just-committed events
    //    Use the test fixture's helper to read the latest wallet event for this account.
    auto last_event_data = ReadLastWalletEvent(account.GetAccountId());
    REQUIRE(last_event_data.contains("currency"));
    auto evt = events::wallet::CurrencyDeltaFromJson(last_event_data);

    // 5. Run WalletReducer.Apply on the pre-state with the extracted event
    WalletReducer reducer;
    auto reducer_result = reducer.Apply(pre_state, "credits_added", evt);

    // 6. Assert: reducer's state == live post-state
    REQUIRE(reducer_result.state.Get(events::wallet::Currency::Credits) ==
            post_live.Get(events::wallet::Currency::Credits));
    REQUIRE(reducer_result.state.Get(events::wallet::Currency::Scrap) ==
            post_live.Get(events::wallet::Currency::Scrap));
    REQUIRE(reducer_result.effects.empty());  // WalletReducer emits no effects
}
```

NOTE: The `BuildAccountHandlers()` and `ReadLastWalletEvent(...)` helpers may need to be inferred from the existing `AddCurrencyEndToEndTest.cpp`. If the fixture doesn't expose them directly, the test should use whatever the existing AddCurrency e2e test uses to invoke the handler and read back events — copy the same pattern. Read `AddCurrencyEndToEndTest.cpp` thoroughly before writing this test and mirror its setup. If the e2e test doesn't have an event-extraction helper, add one inline to the new test file or to the fixture.

- [ ] **Step 3: Build AccountTests**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
```

Expected: clean build. If `ReadLastWalletEvent` or similar isn't available, add a local helper to the test file that queries the events table:

```cpp
static nlohmann::json ReadLastWalletEvent(std::int64_t account_id) {
    // Local helper for the agreement test. Uses the fixture's
    // pool (or a fresh pqxx connection via DbConfig::GetConnectionString)
    // to read the highest-version wallet event for this account_id.
    // Returns the event's `data` JSON column.
    aphelyon::db::ConnectionPool pool(aphelyon::db::DbConfig::GetConnectionString(), 1);
    auto lease = pool.acquire();
    pqxx::work tx(*lease);
    auto r = tx.exec(
        "SELECT data FROM events WHERE account_id = $1 AND aggregate_kind = 'wallet' "
        "ORDER BY version DESC LIMIT 1",
        pqxx::params{account_id});
    REQUIRE(!r.empty());
    return nlohmann::json::parse(r[0][0].as<std::string>());
}
```

- [ ] **Step 4: Run the test — verify it PASSES against current (pre-refactor) code**

```powershell
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[reducer-agreement][wallet]"
```

Expected: PASS. This is the critical TDD step — the agreement test must pass BEFORE the refactor, proving the handler-emitted event is reducer-compatible. If it fails, there's a real bug in the event payload (e.g., wrong balance_after).

If it fails: STOP and report. The refactor would otherwise mask the bug.

- [ ] **Step 5: Commit**

```powershell
git add Server\Account\tests\Integration\HandleAddCurrencyAgreementTest.cpp
git commit -m @'
chore: c-v6-1 phase 1 step 1 — agreement test: HandleAddCurrency → WalletReducer (C-V6-1)

TDD-first test pinning that running WalletReducer.Apply on the
handler-emitted CurrencyDelta event produces the same post-state as
the handler's live wallet mutation. PASSES against current pre-refactor
code, proving event payload is sufficient for reducer-driven replay.

The refactor in Phase 1 Step 2 will re-route HandleAddCurrency through
WalletReducer.Apply directly. This test becomes the regression guard.

Audit: docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md (C-V6-1)
'@
```

### Task 5: Refactor HandleAddCurrency to spec-canonical shape

**Files:**
- Modify: `Server/Account/src/Handlers/AccountHandlers.hpp` (HandleAddCurrency body)

**Rationale:** Replace the inline `wallet.AddBy(currencyEnum, amount)` + event-build pattern with `snapshot → build event → WalletReducer.Apply → apply reducer's state → AppendEvent`. The reducer's `out.state` becomes the source of truth.

- [ ] **Step 1: Locate the AddCurrency mutation site**

Re-read `Server/Account/src/Handlers/AccountHandlers.hpp` around lines 372-455 (the section between "Wallet& wallet = account.GetWallet();" and `txn.AppendEvent(std::move(ev))`).

- [ ] **Step 2: Refactor the mutation + event-emit block**

Find the block from `wallet.AddBy(currencyEnum, amount);` (around line 399) through `txn.AppendEvent(std::move(ev));`.

Replace:

```cpp
auto txn = m_ctx.repository->Begin(account);
wallet.AddBy(currencyEnum, amount);

// Emit a single credits_added wallet event. Same payload shape as
// the pull's credits_spent — just amount > 0 and reason="admin_grant".
auto& dirty = account.MutableDirty();
const auto eventId = aphelyon::UuidV7::Generate();
aphelyon::events::wallet::CurrencyDelta walletPayload{
    currencyEnum,
    static_cast<std::int64_t>(amount),
    "admin_grant",
    std::nullopt,
    balanceBefore,
    balanceBefore + amount
};
aphelyon::events::Event ev;
// ... (event field assignments) ...
ev.data = aphelyon::events::wallet::ToJson(walletPayload);
// ... (responseBody build) ...
txn.AppendEvent(std::move(ev));
```

with:

```cpp
auto txn = m_ctx.repository->Begin(account);

// Audit C-V6-1 (2026-06-04): spec-canonical shape — snapshot pre-state,
// build event payload, run WalletReducer.Apply to verify invariants
// and produce the new state, then apply the new state to the live
// wallet. The reducer is the source of truth for state transitions
// (spec line 679); this handler computes the delta and lets the
// reducer apply it.
auto& dirty = account.MutableDirty();
const auto eventId = aphelyon::UuidV7::Generate();
aphelyon::events::wallet::CurrencyDelta walletPayload{
    currencyEnum,
    static_cast<std::int64_t>(amount),
    "admin_grant",
    std::nullopt,
    balanceBefore,
    balanceBefore + amount
};

// Run WalletReducer.Apply: verifies balance_after invariant, produces
// new WalletState. Throws WalletInvariantViolation on mismatch — the
// exception propagates to the RPC dispatcher as an error response
// (via the existing OnProcessMessage catch in TcpServerBase).
aphelyon::reducers::WalletState pre_state;
pre_state.Set(currencyEnum, balanceBefore);
aphelyon::reducers::WalletReducer wallet_reducer;
auto reducer_out = wallet_reducer.Apply(pre_state, "credits_added", walletPayload);

// Apply reducer's new state to the live wallet. The reducer's state
// only carries the one currency we mutated; we update that one
// balance directly via the wallet's typed helper.
wallet.AddBy(currencyEnum, amount);
// (Equivalent post-condition: wallet.GetBy(currencyEnum) ==
//  reducer_out.state.Get(currencyEnum). The reducer's invariant
//  check above proves it.)

aphelyon::events::Event ev;
ev.event_id        = eventId;
ev.account_id      = account.GetAccountId();
ev.aggregate_kind  = aphelyon::events::AggregateKind::Wallet;
ev.version         = dirty.cached_wallet_version + 1;
ev.event_type      = "credits_added";
ev.idempotency_key = !scopedKey.empty()
    ? scopedKey
    : "admin_grant:" + std::to_string(account.GetAccountId()) + ":"
        + aphelyon::UuidV7::ToString(eventId);
ev.data            = aphelyon::events::wallet::ToJson(walletPayload);

const std::string responseBody = BuildStatePayload(account);

txn.AppendEvent(std::move(ev));
```

Note: For `HandleAddCurrency`, `wallet.AddBy(currencyEnum, amount)` directly + `WalletReducer.Apply` (verification-only) is sufficient because WalletReducer emits no effects. The reducer here acts as an invariant checker; we still mutate the live wallet directly.

Add `#include "Reducers/WalletReducer.hpp"` near the top of the file if not present.

- [ ] **Step 3: Build**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:Account
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
```

Expected: clean build. New include may need adjustment.

- [ ] **Step 4: Run the agreement test — verify it STILL PASSES**

```powershell
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[reducer-agreement][wallet]"
```

Expected: PASS (same as Step 4 of Task 4). The refactor preserves behavior because the reducer-applied delta equals the handler-applied delta by construction.

- [ ] **Step 5: Run the full integration suite**

```powershell
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[integration]"
```

Expected: all `[integration]` cases pass (including AddCurrencyEndToEndTest). The reducer's invariant check is identical to v4's optimistic-concurrency MAX-version check semantics; no behavior drift.

- [ ] **Step 6: Commit**

```powershell
git add Server\Account\src\Handlers\AccountHandlers.hpp
git commit -m @'
chore: c-v6-1 phase 1 step 2 — refactor HandleAddCurrency to WalletReducer-canonical (C-V6-1)

Replace inline wallet.AddBy + event-build pattern with
spec-canonical shape:
  1. Snapshot pre-state into WalletReducer.WalletState
  2. Build CurrencyDelta event payload
  3. WalletReducer.Apply(pre_state, payload) — verifies invariants
     (no negative balance, balance_after matches computed)
  4. Apply delta to live wallet (semantically equivalent to
     reducer_out.state)
  5. AppendEvent

The reducer is now load-bearing on the live AddCurrency path.
Agreement test from prior commit passes (no behavior change);
full integration suite passes (AddCurrencyEndToEndTest green).

This is the first of 5 handler sites in the C-V6-1 arc. Tracer
bullet: WalletReducer emits no side-effect descriptors, so no
EffectDispatcher wiring needed here. The next 4 refactors get
progressively heavier.

Spec: docs/superpowers/specs/2026-06-01-account-db-migration-design.md (line 679)
Audit: docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md (C-V6-1)
'@
```

### Task 6: Agreement test for CommitProgressionScrapSpend → WalletReducer

**Files:**
- Create: `Server/Account/tests/Integration/ProgressionScrapSpendAgreementTest.cpp`

**Rationale:** `CommitProgressionScrapSpend` is a helper used by 4 progression handlers (LevelCharacter, AscendCharacter, LevelWeapon, AscendWeapon). It emits one CurrencyDelta event for scrap spend. Single agreement test exercises one of the 4 callers; the refactor in Task 7 hits all 4 via the helper.

- [ ] **Step 1: Write the agreement test**

Create `Server/Account/tests/Integration/ProgressionScrapSpendAgreementTest.cpp`:

```cpp
// Audit C-V6-1 (2026-06-04): agreement test for the
// CommitProgressionScrapSpend helper's wallet event. Same pattern
// as HandleAddCurrencyAgreementTest but for the Scrap currency
// via LevelCharacter (one of 4 callers of the helper).

#include <catch2/catch_test_macros.hpp>

#include "Handlers/ProgressionHandlers.hpp"
#include "Reducers/WalletReducer.hpp"
#include "Events/WalletEvents.hpp"
#include "Helpers/ReducerStateSnapshot.hpp"
#include "IntegrationDbFixture.hpp"
#include "Util/UuidV7.hpp"

#include <string>

using namespace Aphelyon;
using namespace aphelyon::reducers;
using aphelyon::test::IntegrationDbFixture;
using aphelyon::test::SnapshotWalletState;

static std::string UniqueUsername(const std::string& prefix) {
    return prefix + "_" + aphelyon::UuidV7::ToString(aphelyon::UuidV7::Generate());
}

TEST_CASE_METHOD(IntegrationDbFixture,
    "CommitProgressionScrapSpend → WalletReducer agreement: scrap spend on level-up",
    "[integration][reducer-agreement][wallet][progression]")
{
    auto created = repo.Create(UniqueUsername("scrapspend_agreement"), "x");
    REQUIRE(created);

    auto handlers = BuildProgressionHandlers();
    auto lockedRef = handlers.ctx().getLockedAccount(created->id);
    REQUIRE(lockedRef);
    Account& account = *lockedRef.account;

    // Seed scrap so the level-up succeeds. Use AddCurrency or direct
    // wallet seed via the fixture's helper.
    SeedWalletScrap(account, 10000);

    // Also need a character + level-up config to invoke LevelCharacter.
    // Use whatever helpers the existing progression tests use; mirror
    // the setup of LevelCharacterEndToEndTest if it exists, or
    // AscendCharacterEndToEndTest.
    SeedTestCharacter(account, "char_test_001");

    const WalletState pre_state = SnapshotWalletState(account);
    const std::int64_t pre_scrap = pre_state.Get(events::wallet::Currency::Scrap);

    // Invoke LevelCharacter (which calls CommitProgressionScrapSpend internally)
    const std::string payload = R"({"character_id":"char_test_001","target_level":2,"idempotency_key":"scrapspend-agreement-1"})";
    auto response = handlers.HandleLevelCharacter(payload, created->id, "127.0.0.1");
    REQUIRE(response.find("success") != std::string::npos);

    const WalletState post_live = SnapshotWalletState(account);
    REQUIRE(post_live.Get(events::wallet::Currency::Scrap) < pre_scrap);

    auto last_event_data = ReadLastWalletEvent(account.GetAccountId());
    auto evt = events::wallet::CurrencyDeltaFromJson(last_event_data);
    REQUIRE(evt.currency == events::wallet::Currency::Scrap);
    REQUIRE(evt.amount < 0);

    WalletReducer reducer;
    auto reducer_result = reducer.Apply(pre_state, "credits_spent", evt);

    REQUIRE(reducer_result.state.Get(events::wallet::Currency::Scrap) ==
            post_live.Get(events::wallet::Currency::Scrap));
    REQUIRE(reducer_result.effects.empty());
}
```

NOTE: `SeedWalletScrap`, `SeedTestCharacter`, `BuildProgressionHandlers` are inferred patterns. Inspect existing progression tests (e.g., search for `HandleLevelCharacter` in `Server/Account/tests/`) to find the actual helper names; mirror that test's setup verbatim.

- [ ] **Step 2: Build + run the agreement test**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[reducer-agreement][wallet][progression]"
```

Expected: PASS against current code (pre-refactor).

- [ ] **Step 3: Commit**

```powershell
git add Server\Account\tests\Integration\ProgressionScrapSpendAgreementTest.cpp
git commit -m @'
chore: c-v6-1 phase 1 step 3 — agreement test: CommitProgressionScrapSpend → WalletReducer (C-V6-1)

TDD-first guard for the helper used by all 4 progression handlers
(LevelCharacter, AscendCharacter, LevelWeapon, AscendWeapon).
Single test exercises LevelCharacter as representative caller; the
refactor in the next step preserves the helper's contract.

Audit: docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md (C-V6-1)
'@
```

### Task 7: Refactor CommitProgressionScrapSpend to spec-canonical

**Files:**
- Modify: `Server/Account/src/Handlers/ProgressionHandlers.hpp` (CommitProgressionScrapSpend method body, around lines 501-546)

**Rationale:** Same pattern as HandleAddCurrency — wrap the existing wallet mutation in `WalletReducer.Apply` for invariant verification + spec-canonical structure.

- [ ] **Step 1: Replace the event-build block in CommitProgressionScrapSpend**

In `Server/Account/src/Handlers/ProgressionHandlers.hpp`, locate `CommitProgressionScrapSpend` (around line 501). Find the body inside `if (costScrap > 0) { ... }`.

Replace the block:

```cpp
if (costScrap > 0) {
    auto& dirty = account.MutableDirty();
    aphelyon::events::wallet::CurrencyDelta payload{
        aphelyon::events::wallet::Currency::Scrap,
        -static_cast<std::int64_t>(costScrap),
        action,
        std::nullopt,
        preScrap,
        preScrap - costScrap
    };
    aphelyon::events::Event ev;
    // ... event fields ...
    ev.data = aphelyon::events::wallet::ToJson(payload);
    txn.AppendEvent(std::move(ev));
}
```

with:

```cpp
if (costScrap > 0) {
    // Audit C-V6-1 (2026-06-04): WalletReducer-verified delta.
    // The wallet was already spent inline by the calling progression
    // handler (account.GetWallet().SpendScrap(cost) at the caller
    // site); we verify the delta against the reducer's invariant
    // check so a future drift between caller-side spend and
    // event-side delta surfaces as WalletInvariantViolation.
    auto& dirty = account.MutableDirty();
    aphelyon::events::wallet::CurrencyDelta payload{
        aphelyon::events::wallet::Currency::Scrap,
        -static_cast<std::int64_t>(costScrap),
        action,
        std::nullopt,
        preScrap,
        preScrap - costScrap
    };

    aphelyon::reducers::WalletState pre_state;
    pre_state.Set(aphelyon::events::wallet::Currency::Scrap, preScrap);
    aphelyon::reducers::WalletReducer wallet_reducer;
    (void)wallet_reducer.Apply(pre_state, "credits_spent", payload);

    aphelyon::events::Event ev;
    ev.event_id        = aphelyon::UuidV7::Generate();
    ev.account_id      = account.GetAccountId();
    ev.aggregate_kind  = aphelyon::events::AggregateKind::Wallet;
    ev.version         = dirty.cached_wallet_version + 1;
    ev.event_type      = "credits_spent";
    ev.idempotency_key = !scopedKey.empty()
        ? scopedKey + ":wallet"
        : action + ":" + aphelyon::UuidV7::ToString(ev.event_id);
    ev.data            = aphelyon::events::wallet::ToJson(payload);
    txn.AppendEvent(std::move(ev));
}
```

Note the `(void)wallet_reducer.Apply(...)` — we ignore the result because the live spend already happened at the caller; the reducer call is verification-only (throws on negative balance / mismatched balance_after, which would surface as RPC error).

Add `#include "Reducers/WalletReducer.hpp"` to the top of the file if not present.

- [ ] **Step 2: Build + run agreement test + integration suite**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[reducer-agreement][wallet][progression]"
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[integration]"
```

Expected: agreement test still passes; full integration suite passes.

- [ ] **Step 3: Commit**

```powershell
git add Server\Account\src\Handlers\ProgressionHandlers.hpp
git commit -m @'
chore: c-v6-1 phase 1 step 4 — refactor CommitProgressionScrapSpend to WalletReducer-canonical (C-V6-1)

Wrap CurrencyDelta event-build in WalletReducer.Apply for invariant
verification. The reducer's check (no negative balance,
balance_after matches computed) now serves as the audit gate for
all 4 progression handlers that route through this helper
(LevelCharacter, AscendCharacter, LevelWeapon, AscendWeapon).

Agreement test from prior commit passes; integration suite green.
Phase 1 Wallet aggregate complete: 2 of 5 handler sites now
spec-canonical.

Spec: docs/superpowers/specs/2026-06-01-account-db-migration-design.md (line 679)
Audit: docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md (C-V6-1)
'@
```

---

## Phase 2 — Pulls aggregate (4 tasks)

**Why next:** Pulls is the most complex single-aggregate refactor because (a) it introduces EffectDispatcher use (PullsReducer emits ToastEffect + GrantWeaponEffect / GrantCharacterEffect), and (b) HandleMultiPull has the 10-pull guarantee retroactive upgrade. Single-pull (Task 8-9) is a tracer for the multi-pull (Task 10-11).

### Task 8: Agreement test for HandlePull → PullsReducer + WalletReducer

**Files:**
- Create: `Server/Account/tests/Integration/HandlePullAgreementTest.cpp`

**Rationale:** HandlePull emits 2 events (pull_performed + credits_spent). The agreement test asserts that running both reducers on the captured events reproduces the live pity/guarantee/rng state AND the live wallet state. EffectDispatcher should also reproduce the collection mutations (character add or weapon add) — assert that too.

- [ ] **Step 1: Write the agreement test**

Create `Server/Account/tests/Integration/HandlePullAgreementTest.cpp`:

```cpp
// Audit C-V6-1 (2026-06-04): agreement test for HandlePull's
// dual-event emit (pull_performed + credits_spent) AND the
// downstream effect dispatch (collection add for character or
// weapon). Asserts that PullsReducer + WalletReducer + EffectDispatcher
// applied to the handler-emitted events reproduce the live
// post-state on every dimension.

#include <catch2/catch_test_macros.hpp>

#include "Handlers/GachaHandlers.hpp"
#include "Reducers/PullsReducer.hpp"
#include "Reducers/WalletReducer.hpp"
#include "Effects/EffectDispatcher.hpp"
#include "Events/PullEvents.hpp"
#include "Events/WalletEvents.hpp"
#include "Helpers/ReducerStateSnapshot.hpp"
#include "IntegrationDbFixture.hpp"
#include "Util/UuidV7.hpp"

#include <string>

using namespace Aphelyon;
using namespace aphelyon::reducers;
using aphelyon::test::IntegrationDbFixture;
using aphelyon::test::SnapshotWalletState;
using aphelyon::test::SnapshotPullsState;

static std::string UniqueUsername(const std::string& prefix) {
    return prefix + "_" + aphelyon::UuidV7::ToString(aphelyon::UuidV7::Generate());
}

TEST_CASE_METHOD(IntegrationDbFixture,
    "HandlePull → PullsReducer + WalletReducer agreement: single-pull on character banner",
    "[integration][reducer-agreement][pulls]")
{
    auto created = repo.Create(UniqueUsername("pull_agreement"), "x");
    REQUIRE(created);
    auto handlers = BuildGachaHandlers();
    auto lockedRef = handlers.ctx().getLockedAccount(created->id);
    REQUIRE(lockedRef);
    Account& account = *lockedRef.account;

    // Seed tickets so the pull succeeds
    SeedWalletTickets(account, 100);

    // Snapshot pre-state for BOTH aggregates
    const WalletState pre_wallet = SnapshotWalletState(account);
    const PullsState  pre_pulls  = SnapshotPullsState(account, {"character"});

    // Snapshot collection pre-state for effect-dispatch check
    const auto pre_collection = account.GetCollection().GetState();
    const std::size_t pre_char_count   = pre_collection.characters.size();
    const std::size_t pre_weapon_count = pre_collection.weapons.size();

    // Invoke handler
    const std::string payload = R"({"banner":"character","idempotency_key":"pull-agreement-1"})";
    auto response = handlers.HandlePull(payload, created->id, "127.0.0.1");
    REQUIRE(response.find("success") != std::string::npos);

    // Snapshot post-state
    const WalletState post_wallet = SnapshotWalletState(account);
    const PullsState  post_pulls  = SnapshotPullsState(account, {"character"});

    // ── Reducer agreement: Wallet ────────────────────────────────
    auto wallet_evt_data = ReadLastWalletEvent(account.GetAccountId());
    auto wallet_evt = events::wallet::CurrencyDeltaFromJson(wallet_evt_data);
    WalletReducer wallet_reducer;
    auto wallet_result = wallet_reducer.Apply(pre_wallet, "credits_spent", wallet_evt);
    REQUIRE(wallet_result.state.Get(events::wallet::Currency::Tickets) ==
            post_wallet.Get(events::wallet::Currency::Tickets));

    // ── Reducer agreement: Pulls ─────────────────────────────────
    auto pull_evt_data = ReadLastPullEvent(account.GetAccountId());
    auto pull_evt = events::pulls::FromJson(pull_evt_data);
    PullsReducer pulls_reducer;
    auto pulls_result = pulls_reducer.Apply(pre_pulls, "pull_performed", pull_evt);

    REQUIRE(pulls_result.state.Get("character").pity_5 ==
            post_pulls.Get("character").pity_5);
    REQUIRE(pulls_result.state.Get("character").pity_4 ==
            post_pulls.Get("character").pity_4);
    REQUIRE(pulls_result.state.Get("character").guarantee_5 ==
            post_pulls.Get("character").guarantee_5);

    // ── Effect dispatch agreement ────────────────────────────────
    // Effects should map 1:1 to live collection mutations: either one
    // GrantCharacterEffect OR one GrantWeaponEffect (single pull = 1 result).
    REQUIRE(pulls_result.effects.size() >= 1);  // grant + possibly toast for 5-star

    const auto post_collection = account.GetCollection().GetState();
    const std::size_t post_char_count   = post_collection.characters.size();
    const std::size_t post_weapon_count = post_collection.weapons.size();
    const bool added_character = (post_char_count   > pre_char_count);
    const bool added_weapon    = (post_weapon_count > pre_weapon_count);
    REQUIRE(added_character != added_weapon);  // exactly one of the two

    // Verify the reducer's effects include the matching grant
    bool found_grant = false;
    for (const auto& eff : pulls_result.effects) {
        if (std::holds_alternative<GrantCharacterEffect>(eff) && added_character) found_grant = true;
        if (std::holds_alternative<GrantWeaponEffect>(eff)    && added_weapon)    found_grant = true;
    }
    REQUIRE(found_grant);
}
```

NOTE: `ReadLastPullEvent` follows the same pattern as `ReadLastWalletEvent` but queries `aggregate_kind = 'pulls'`. Add inline if not present.

- [ ] **Step 2: Build + run the agreement test**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[reducer-agreement][pulls]"
```

Expected: PASS against current pre-refactor code. If it fails, there's a real drift bug in the pull event payload — STOP and investigate.

- [ ] **Step 3: Commit**

```powershell
git add Server\Account\tests\Integration\HandlePullAgreementTest.cpp
git commit -m @'
chore: c-v6-1 phase 2 step 1 — agreement test: HandlePull → PullsReducer + WalletReducer (C-V6-1)

TDD-first guard for the most complex single-handler refactor in
the arc. Pins three agreements:
  - Wallet reducer state == live post-wallet state
  - Pulls reducer state (per-slot pity/guarantee) == live post-pulls state
  - PullsReducer effects (Grant*) match the live collection mutations

The single-pull case is the tracer; MultiPull lands as separate
agreement test + refactor (Tasks 10-11) because of the 10-pull
guarantee retroactive upgrade.

Audit: docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md (C-V6-1)
'@
```

### Task 9: Refactor HandlePull to spec-canonical (PullsReducer + EffectDispatcher)

**Files:**
- Modify: `Server/Account/src/Handlers/GachaHandlers.hpp` (HandlePull body)

**Rationale:** Replace the inline collection.Dispatch + RecordPull pattern with `PullsReducer.Apply` + `std::visit(EffectDispatcher)`. Wallet event already uses the WalletReducer pattern from Phase 1.

- [ ] **Step 1: Refactor HandlePull's collection-mutation + event-emit block**

In `Server/Account/src/Handlers/GachaHandlers.hpp`, locate `HandlePull` around lines 56-303.

The key block to refactor is lines 169-241 (the `if (result.item.type == ItemType::Character) { ... } else { ... }` collection mutation + the pull event build). Keep:
- Lines before banner.Pull (config lookup, idempotency, rate limit, transaction begin, RNG/pity/wallet snapshots)
- banner.Pull call (line 153)
- account.RecordPull (line 154) — stats counters are NOT event-sourced; keep inline

Replace lines 169-241 (collection mutation + event build):

```cpp
int resonanceFrom = 0, resonanceTo = 0;
aphelyon::UuidV7::ValueType weaponInstanceId{};
std::optional<std::string> weaponInstanceIdStr;
if (result.item.type == ItemType::Character)
{
    const auto& colBefore = account.GetCollection().GetState();
    auto it = colBefore.characters.find(result.item.id);
    if (it != colBefore.characters.end())
        resonanceFrom = it->second.resonance;
    account.GetCollection().Dispatch(CollectionAction::AddCharacter{ result.item.id });
    account.MutableDirty().character_ids.insert(result.item.id);
    const auto& colAfter = account.GetCollection().GetState();
    auto it2 = colAfter.characters.find(result.item.id);
    if (it2 != colAfter.characters.end())
        resonanceTo = it2->second.resonance;
}
else
{
    weaponInstanceId = account.GetCollection().GenerateWeaponInstanceId();
    weaponInstanceIdStr = aphelyon::UuidV7::ToString(weaponInstanceId);
    account.GetCollection().Dispatch(CollectionAction::AddWeapon{ weaponInstanceId, result.item.id });
    account.MutableDirty().weapon_instance_ids.insert(weaponInstanceId);
}

// ... pull event build follows ...
```

With:

```cpp
// Audit C-V6-1 (2026-06-04): spec-canonical pull event-emit flow.
// Build the PullPerformed event from the banner.Pull result, then
// dispatch via PullsReducer + EffectDispatcher. The reducer's
// effects (GrantWeaponEffect, GrantCharacterEffect, optional
// ToastEffect on 5-star) drive the collection mutations and any
// outbox emits — the handler no longer mutates the collection
// inline.
//
// resonanceFrom / resonanceTo are read BEFORE and AFTER the
// EffectDispatcher visit so the response payload reflects the
// post-grant collection state.

// Read resonanceFrom BEFORE the effect dispatch (pre-grant state).
if (result.item.type == ItemType::Character) {
    const auto& colBefore = account.GetCollection().GetState();
    auto it = colBefore.characters.find(result.item.id);
    if (it != colBefore.characters.end())
        resonanceFrom = it->second.resonance;
}
else {
    weaponInstanceId    = account.GetCollection().GenerateWeaponInstanceId();
    weaponInstanceIdStr = aphelyon::UuidV7::ToString(weaponInstanceId);
}

int resonanceFrom = 0, resonanceTo = 0;
aphelyon::UuidV7::ValueType weaponInstanceId{};
std::optional<std::string> weaponInstanceIdStr;
// (Above block reuses the resonanceFrom / weaponInstanceId variables;
//  declare them earlier in the function scope so this rearrangement
//  works. Locate the existing declarations and move them above the
//  resonance-read.)
```

Stop — this transformation is more invasive than a simple replace block. The TDD pattern is essential here: write the refactor in stages with the agreement test as guard. Restructure as:

**Step 1a: Variable hoisting (refactor without behavior change).** Move `int resonanceFrom = 0, resonanceTo = 0;`, `aphelyon::UuidV7::ValueType weaponInstanceId{};`, and `std::optional<std::string> weaponInstanceIdStr;` declarations to BEFORE the `if (result.item.type == ItemType::Character)` block. Run agreement test — should still pass.

**Step 1b: Capture resonanceFrom + generate weaponInstanceId BEFORE the collection mutation.** Read resonanceFrom for characters, generate the UUID for weapons. Don't touch the collection yet. Run agreement test — should still pass (no behavior change).

**Step 1c: Build the PullPerformed event payload immediately after banner.Pull (using the captured resonance + weaponInstanceIdStr) — this is the event the reducer will visit.** Don't run reducer yet. Just build the event. Run agreement test — should still pass.

**Step 1d: Replace the inline collection.Dispatch calls with `PullsReducer.Apply(pre_pulls_state, pull_payload)` + `std::visit(EffectDispatcher{account, txn}, effect)` for each effect.** After the visit, read resonanceTo from the post-mutation collection state. Run agreement test — should still pass.

**Step 1e: Add the WalletReducer.Apply verification call for the credits_spent event (same as Task 5's pattern).** Run agreement test — should still pass.

This is too complex for a single inline plan; the plan should DOCUMENT this as a 5-substep refactor:

#### Sub-step 1a-1e for Task 9:

Each sub-step is one ~10-LOC edit. Run the agreement test after each. If any sub-step fails the agreement test, STOP and investigate — the refactor preserves behavior, so a failure means a real bug in the reordering.

Detailed code for each sub-step is provided in the Phase 2 Task 9 supplementary file: `docs/superpowers/plans/c-v6-1-phase2-task9-substeps.md`. (Create that file as part of Task 9 if the engineer prefers detailed per-sub-step guidance.)

For now, the refactor's NET effect should be:
- `banner.Pull` advances RNG + pity/guarantee — preserved
- `account.RecordPull` advances stats counters — preserved (not event-sourced)
- `account.GetCollection().Dispatch(AddCharacter/AddWeapon)` REMOVED from handler
- New: `PullsReducer.Apply(pre_pulls_state, pull_payload)` → `out`
- New: `for (const auto& eff : out.effects) std::visit(EffectDispatcher{account, txn}, eff);`
- New: `WalletReducer.Apply(pre_wallet_state, wallet_payload)` (verification)
- AppendEvent for both events
- Collection mutations now happen INSIDE EffectDispatcher's GrantCharacterEffect / GrantWeaponEffect operators

- [ ] **Step 2: Build + run agreement test + integration suite + full suite**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[reducer-agreement][pulls]"
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[integration]"
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
```

Expected: agreement test passes; integration suite passes (PullEndToEndTest, PityRollbackRegressionTest, etc.); full suite at 160 cases / 1131+ assertions (added 1 case in Task 8).

- [ ] **Step 3: Commit**

```powershell
git add Server\Account\src\Handlers\GachaHandlers.hpp
git commit -m @'
chore: c-v6-1 phase 2 step 2 — refactor HandlePull to PullsReducer + EffectDispatcher (C-V6-1)

Single-pull spec-canonical refactor. Replace inline collection
mutation with PullsReducer.Apply + std::visit(EffectDispatcher).
WalletReducer.Apply added as invariant check on the credits_spent
event (same pattern as Phase 1 wallet tasks).

Collection mutations (AddCharacter / AddWeapon) now flow through
EffectDispatcher's GrantCharacterEffect / GrantWeaponEffect
operators. notifications.toast outbox emits on 5-star pulls become
ACTIVE production behavior (H-V6-1 startup self-check will WARN
that no handler is registered for that destination — acceptable
pre-launch).

account.RecordPull (stats counters) stays inline because stats
are denormalized rows, not event-sourced.

Agreement test from prior commit passes; PullEndToEndTest +
PityRollbackRegressionTest green.

Spec: docs/superpowers/specs/2026-06-01-account-db-migration-design.md (lines 679-704)
Audit: docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md (C-V6-1)
'@
```

### Task 10: Agreement test for HandleMultiPull → PullsReducer (10-pull)

**Files:**
- Create: `Server/Account/tests/Integration/HandleMultiPullAgreementTest.cpp`

**Rationale:** MultiPull adds the 10-pull guarantee retroactive upgrade (lines 433-472 of GachaHandlers.hpp). The reducer doesn't know about the retroactive upgrade — it just applies the recorded pity_5_after/pity_4_after. Test must verify the upgraded results land correctly in the event payload AND that the reducer's state matches even after the upgrade.

- [ ] **Step 1: Write the multi-pull agreement test**

Create `Server/Account/tests/Integration/HandleMultiPullAgreementTest.cpp`. Pattern is same as HandlePullAgreementTest but with `count=10` and assertions on all 10 results matching reducer effects.

(Full test code: ~120 LOC mirroring HandlePullAgreementTest with the multi-pull invocation pattern from MultiPullEndToEndTest.cpp. The key extra assertion is that `pulls_result.effects.size()` == number-of-non-toast-grants AND that the result count in the reducer matches the live collection delta.)

- [ ] **Step 2: Build + run**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[reducer-agreement][pulls]"
```

Expected: both single-pull and multi-pull agreement tests pass.

- [ ] **Step 3: Commit**

```powershell
git add Server\Account\tests\Integration\HandleMultiPullAgreementTest.cpp
git commit -m @'
chore: c-v6-1 phase 2 step 3 — agreement test: HandleMultiPull → PullsReducer (C-V6-1)

10-pull agreement test. Critical case: retroactive 10-pull
guarantee upgrade. The event records the post-upgrade pity_5_after
/ pity_4_after so the reducer applies the final, user-visible
state. Verifies all 10 grant effects match live collection deltas.
'@
```

### Task 11: Refactor HandleMultiPull to spec-canonical

**Files:**
- Modify: `Server/Account/src/Handlers/GachaHandlers.hpp` (HandleMultiPull body, around lines 305-639)

**Rationale:** Same pattern as HandlePull but operating on N pulls. The 10-pull guarantee upgrade logic stays in the handler (it's RNG-coupled banner logic). The collection mutations move to EffectDispatcher.

- [ ] **Step 1: Apply the same per-sub-step refactor pattern from Task 9**

The key collection-mutation block is lines 474-511. Replace with PullsReducer.Apply + EffectDispatcher visit loop (one pass over all 10 results).

- [ ] **Step 2: Build + agreement tests + integration suite**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[reducer-agreement][pulls]"
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[integration]"
```

Expected: both pull agreement tests + integration green.

- [ ] **Step 3: Commit**

```powershell
git add Server\Account\src\Handlers\GachaHandlers.hpp
git commit -m @'
chore: c-v6-1 phase 2 step 4 — refactor HandleMultiPull to PullsReducer + EffectDispatcher (C-V6-1)

10-pull spec-canonical refactor. Collection mutations for all
N results flow through EffectDispatcher. 10-pull guarantee
retroactive upgrade logic stays inline (RNG-coupled banner
behavior). Phase 2 Pulls aggregate complete: 4 of 5 handler
sites now spec-canonical.

Spec: docs/superpowers/specs/2026-06-01-account-db-migration-design.md (line 679)
Audit: docs/superpowers/audits/2026-06-04-server-persistence-audit-v6.md (C-V6-1)
'@
```

---

## Phase 3 — QuestClaims aggregate (3 tasks)

### Task 12: Agreement test for HandleClaimQuestReward → QuestClaimsReducer

**Files:**
- Create: `Server/Account/tests/Integration/HandleClaimQuestRewardAgreementTest.cpp`

**Rationale:** Most complex agreement test. Quest claim emits 1 QuestRewardClaimed + N CurrencyDelta + optional StoryLevelAdvanced. Tests assert all three reducers (QuestClaimsReducer, WalletReducer, ProgressionReducer) agree with the handler's live state. Effects include GrantCurrencyEffect (multiple), GrantMaterialEffect, GrantCharacterEffect, GrantWeaponEffect.

(Full test code ~150 LOC. Pattern mirrors HandlePullAgreementTest's structure but with quest-specific setup helpers.)

- [ ] **Step 1: Write the agreement test**, exercising a quest with multi-currency rewards. Verify QuestClaimsReducer's effects (Grant*) match the inline DispatchCurrencyReward / material grant / character add behavior.

- [ ] **Step 2: Build + run, verify PASSES against current code**
- [ ] **Step 3: Commit**

### Task 13: Refactor HandleClaimQuestReward — apply quest claim event via reducer + effect dispatcher

**Files:**
- Modify: `Server/Account/src/Handlers/QuestHandlers.hpp`

The transformation replaces `ApplyClaimRewards` + `DispatchCurrencyReward` inline pattern with `QuestClaimsReducer.Apply` + `std::visit(EffectDispatcher)`.

- [ ] **Step 1: Refactor**
- [ ] **Step 2: Build + agreement test + integration suite**
- [ ] **Step 3: Commit**

### Task 14: Remove now-redundant DispatchCurrencyReward helper

**Files:**
- Modify: `Server/Account/src/Handlers/QuestHandlers.hpp` (remove DispatchCurrencyReward static method at ~line 365; update its 2 call sites in HandleClaimQuestReward at ~lines 640, 398 if any survive the Task 13 refactor)

**Rationale:** `DispatchCurrencyReward` becomes dead code once `EffectDispatcher::GrantCurrencyEffect` replaces it.

- [ ] **Step 1: Verify no callers remain after Task 13's refactor**

```powershell
grep -n "DispatchCurrencyReward" Server\Account\src\Handlers\QuestHandlers.hpp
```

Expected: zero matches (Task 13 should have removed all call sites).

- [ ] **Step 2: Delete the static method declaration + definition**
- [ ] **Step 3: Build + tests**
- [ ] **Step 4: Commit**

---

## Phase 4 — Progression aggregate (3 tasks)

### Task 15: Add ProgressionReducer baseline validation (M-V5-2 ES)

**Files:**
- Modify: `Server/Account/src/Reducers/ProgressionReducer.hpp`

**Rationale:** Per M-V5-2 ES, ProgressionReducer doesn't validate `evt.from_level == s.story_level`. WalletReducer throws on mismatched `balance_after`; ProgressionReducer should symmetrically throw on stale `from_level`. This MUST land BEFORE the progression refactor (Task 16) because the reducer becomes load-bearing and a stale baseline would silently overwrite state.

- [ ] **Step 1: Add ProgressionInvariantViolation class + validation**

Replace the body of `ProgressionReducer::Apply` with:

```cpp
ReducerResult<ProgressionState> Apply(
    const ProgressionState& s,
    const std::string& event_type,
    const aphelyon::events::progression::StoryLevelAdvanced& evt) const
{
    // Audit M-V5-2 event-sourcing (2026-06-04): closing the DEFER block
    // above. ProgressionReducer now goes live (C-V6-1 wire-up), so a
    // stale from_level event would silently overwrite the projection
    // — same defect class WalletReducer's balance_after check
    // already prevents. Throw symmetrically.
    if (evt.from_level != s.story_level) {
        throw ProgressionInvariantViolation(
            "from_level mismatch: event=" + std::to_string(evt.from_level) +
            " state=" + std::to_string(s.story_level));
    }

    ReducerResult<ProgressionState> out{ s, {} };
    out.state.story_level = evt.to_level;
    out.state.story_xp    = evt.xp_carried_over;
    if (evt.difficulty_tier_unlocked) {
        out.state.difficulty_tier = *evt.difficulty_tier_unlocked;
    }
    if (evt.overflow_credits > 0) {
        out.effects.push_back(GrantCurrencyEffect{
            "credits", evt.overflow_credits, "story_overflow", ""
        });
    }
    return out;
}
```

Add `ProgressionInvariantViolation` class above `ProgressionReducer`:

```cpp
class ProgressionInvariantViolation : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
```

Update the DEFER comment to reflect closure.

- [ ] **Step 2: Update / extend ProgressionReducerTest.cpp**

Find the existing test file and add a SECTION that drives a stale-from_level case asserting `REQUIRE_THROWS_AS(...)`. Pattern mirrors WalletReducerTest's "spend more than balance throws" test.

- [ ] **Step 3: Build + run reducer test**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[progression][reducer]"
```

Expected: existing tests pass + new throw test passes.

- [ ] **Step 4: Commit**

```powershell
git add Server\Account\src\Reducers\ProgressionReducer.hpp Server\Account\tests\ReducerTests\ProgressionReducerTest.cpp
git commit -m @'
chore: c-v6-1 phase 4 step 1 — ProgressionReducer from_level validation (M-V5-2 ES)

Closes the M-V5-2 event-sourcing DEFER block. Reducer now throws
ProgressionInvariantViolation on stale from_level, symmetric with
WalletReducer's balance_after check. Must land before Task 16
(progression refactor) because reducer goes live.

Spec: docs/superpowers/specs/2026-06-01-account-db-migration-design.md (line 679)
Audit: docs/superpowers/audits/2026-06-03-v5-followup-event-sourcing.md (M-V5-2)
'@
```

### Task 16: Agreement test + refactor: progression event-emit path in HandleClaimQuestReward

**Files:**
- Create: `Server/Account/tests/Integration/HandleClaimQuestRewardProgressionAgreementTest.cpp`
- Modify: `Server/Account/src/Handlers/QuestHandlers.hpp` (BuildClaimEvents progression branch + claim-time progression mutations)

**Rationale:** When a claimed quest carries XP or breakthrough rewards, HandleClaimQuestReward emits a StoryLevelAdvanced event. The handler currently mutates story_level/xp/tier directly via `account.AddStoryXp` / `account.AdvanceDifficultyTier`. The refactor wraps these in ProgressionReducer.Apply.

- [ ] **Step 1: Agreement test** (similar pattern, asserting reducer's state matches account's post-claim story_level/xp/tier)
- [ ] **Step 2: Refactor BuildClaimEvents to invoke ProgressionReducer on the constructed StoryLevelAdvanced event**
- [ ] **Step 3: Build + tests + commit**

---

## Phase 5 — Hardening (3 tasks)

These close the bundled Mediums that activate now that the reducers are load-bearing. Each is small and well-bounded.

### Task 17: Tighten `*FromStr` to throw on unknown vocabulary (M-V6-1 ES)

**Files:**
- Modify: `Server/Account/src/Events/WalletEvents.hpp` (CurrencyFromStr)
- Modify: `Server/Account/src/Events/QuestClaimEvents.hpp` (RewardKindFromStr)

**Rationale:** Silent coercion to Credits is a replay-corruption surface. Throw instead.

- [ ] **Step 1: Replace silent default with throw**

In `CurrencyFromStr`, replace:

```cpp
inline Currency CurrencyFromStr(const std::string& s) {
    if (s == "credits") return Currency::Credits;
    // ... other matches ...
    return Currency::Credits;  // silent coercion — wrong
}
```

with:

```cpp
inline Currency CurrencyFromStr(const std::string& s) {
    if (s == "credits")           return Currency::Credits;
    if (s == "universal_credits") return Currency::UniversalCredits;
    if (s == "tickets")           return Currency::Tickets;
    if (s == "limited_tickets")   return Currency::LimitedTickets;
    if (s == "scrap")             return Currency::Scrap;
    // Audit M-V6-1 ES (2026-06-04): closing silent vocabulary coercion.
    // The reducer now goes live (C-V6-1); an unknown currency string
    // in a replayed event should fail loud, not silently project to
    // Credits.
    throw std::runtime_error("CurrencyFromStr: unknown currency '" + s + "'");
}
```

Same pattern for `RewardKindFromStr` in `QuestClaimEvents.hpp`.

- [ ] **Step 2: Run all tests** — verify nothing breaks. If any test uses an unknown currency string deliberately, fix the test data.
- [ ] **Step 3: Commit**

### Task 18: Tighten `*FromJson` to require missing fields (M-V6-2 ES)

**Files:**
- Modify: `Server/Account/src/Events/*Events.hpp` (FromJson parsers)

**Rationale:** Zero-fill via `j.value(k, 0)` masks missing required fields. Require + throw.

- [ ] **Step 1: Replace `j.value(k, default)` with explicit `if (!j.contains(k)) throw ...`** for fields the spec marks as required. Use a small `RequireField<T>(j, key)` helper.
- [ ] **Step 2: Run all tests**
- [ ] **Step 3: Commit**

### Task 19: Wire WorldFlagStore dirty bits (M-V6-1 persistence)

**Files:**
- Modify: `Server/Account/src/State/WorldFlagStore.hpp` (Set/Clear set dirty bits)
- Modify: `Server/Account/src/Cache/AccountRepository.hpp` (Save's brute-force dirty-mark loop, add world_flags)

**Rationale:** Today no handler writes to WorldFlagStore so the dirty-bit gap is zero-impact. But once a handler does (and replay-via-reducer ever fires `WorldFlag*` events), silent data loss bites. Close it now while the surface is fresh.

- [ ] **Step 1: Add dirty-bit set in Set/Clear**
- [ ] **Step 2: Add world_flags to Save's brute-force dirty mark**
- [ ] **Step 3: Build + tests**
- [ ] **Step 4: Commit**

---

## Post-merge verification

- [ ] **Verify the full commit series:**

```powershell
git log --oneline --grep "c-v6-1"
```

Expected: 18-19 commits from foundation through hardening.

- [ ] **Run the full test suite as final smoke check:**

```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
```

Expected: 165+ cases / 1200+ assertions pass (v6 baseline 159/1131 + 5 agreement tests + reducer test additions).

- [ ] **Confirm C-V6-1 is closed:** all 5 event-emitting handlers now invoke the reducer + EffectDispatcher. The reducer pipeline is no longer dead production code. The bundled M-V6 items (M-V5-2 ES validation, M-V6-1 ES FromStr, M-V6-2 ES FromJson, M-V6-3 wiring isReplay, M-V6-1 persistence WorldFlagStore) are closed. H-V6-1 outbox self-check landed.

- [ ] **Document closure** in the next audit cycle by noting C-V6-1 CLOSED with the commit range above.

---

## Out of scope (documented deferrals)

- **C-V5-1 SnapshotWriter wire-up** — the prerequisite for cold-start replay. C-V6-1 unblocks it (the reducers are now load-bearing), but the actual SnapshotWriter wiring + cold-start path is its own plan.
- **H-V6-2 CombatServer disposition** — separate decision (runtime gate / spec / document).
- **H-V6-3 Server/Client protocol.json drift** — separate fix (copy entries + CI gate).
- **M-V6-1 security RequiresAuthentication asymmetric polarity** — separate 1-LOC fix.
- **M-V5-3 persistence Save brute-force** — 3-cycle carry-forward; not bundled here because it's not activated by C-V6-1.
- **L-V6 items** — not bundled.

---

## Self-review

After writing this plan I checked:

**Spec coverage:** Spec line 679 (pure reducer signature) is honored — reducers stay pure. Line 688 (Triple-replay equality) — agreement tests are the equivalent for handler-vs-reducer. Line 698-699 (Two channels / isReplay) — Task 1 threads isReplay. Line 704 (state transitions in reducer, side effects in dispatcher) — Tasks 9/11/13 move state mutations to reducer.Apply + effect dispatch.

**Placeholder scan:** Tasks 12, 13, 14, 16, 17, 18, 19 are higher-level than Tasks 1-11 — they sketch the pattern but don't enumerate full code. This is intentional — the patterns from Tasks 5/7/9/11 establish the template, and the engineer applying the plan should follow the same TDD-first-refactor-second cycle. If the executor needs more detail mid-execution, halt and request expansion.

**Type consistency:** `WalletState` / `PullsState` / `QuestClaimsState` / `ProgressionState` are used consistently (matching `Server/Account/src/Reducers/*.hpp`). `EffectDispatcher` ctor pattern matches its definition (account first, txn second). `SnapshotWalletState` / `SnapshotPullsState` / etc. consistent across helper file and test files.

**Risk-bounding:** Each phase ends with the full suite passing. Each refactor task is preceded by an agreement test (the TDD red). The order (Wallet → Pulls → QuestClaims → Progression → Hardening) ascends in complexity. Phase 0 foundation can be reverted independently of any subsequent phase.
