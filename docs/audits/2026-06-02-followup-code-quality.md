# Follow-up Audit: Code Quality + Complexity

**Date:** 2026-06-02
**Auditor:** #7 of 8 (follow-up)
**Scope:** Verify the L1/L3/L5/L6/L7/L8/L13 + M-class fixes hold; assess responsibility/complexity creep introduced by the triage sweep.
**Baseline:** `2026-06-02-server-persistence-audit.md` (which closed C1–C7, every H, every M, and L1/L3/L5/L6/L7/L8/L13).

## Summary

The L-class cleanup sweep (commit `8e19666`) and M-class triage (commits `d3416a6`, `c172606`, `70e0650`, `7899997`, `dcd9331`, `c3ed91a`, `cb59ae9`, `cd4e37a`, `40871dd`, `bdf0f7c`, `4065818`, `90a5f89`, `8b2c77f`) land cleanly — every reviewed fix is in the code and matches the audit description. The new `IdempotencyKey` / `InternalRpcAuth` helpers are small, focused, and re-used at the right call sites. The previously-deferred L10/L11/L12 refactors did not regress: `QuestHandlers.hpp` grew from 827 to 847 lines and `AccountServer.hpp` from ~504 to 616 lines — both still within the same flagged complexity band, just slightly worse. The two genuinely-new code-quality concerns introduced by the sweep are (1) the C7 Memento adds a deep-copy snapshot cost to every `Begin()` even on read-only handlers, partly undermining M2's lazy-acquisition perf win; and (2) `Account::Snapshot` is a parallel duplicate of the `Account` member layout with no compile-time enforcement that future fields stay in sync. Neither is critical; both are real maintenance burdens.

## Critical
*None.*

## High
*None.*

## Medium

### M-Q1. `Account::Snapshot` is a parallel duplicate of `Account`'s mutable surface with no compile-time enforcement
**Files:** `Server/Account/src/Account.hpp:73-152`
**Status:** NEW (introduced by C7 fix)

The C7 Memento adds three parallel surfaces for the same data:

1. `struct Snapshot` — 24 fields (line 73-98)
2. `CaptureSnapshot()` — 24 assignments (line 100-126)
3. `RestoreFrom(Snapshot&&)` — 24 moves (line 128-152)

Adding a new mutable field to `Account` requires touching all three places plus adding a private member; **nothing in the type system catches the omission**. The Snapshot field naming also drops the `m_` prefix used by Account (e.g. `Snapshot::loginStreak` vs `Account::m_loginStreak`), so a future `m_xxx → m_yyy` rename in Account silently leaves Snapshot's field name divergent. The struct also spells the map types out inline rather than reusing the `WeaponEquipmentMap` / `GearEquipmentMap` / `MaterialInventoryMap` `using` declarations defined later in the class (line 281, 309-311) — the inline note at line 68-72 acknowledges this is a forward-declaration ordering issue but accepts the type-drift risk.

```cpp
// Account.hpp:86-88 — typed by hand, not via the using declarations
std::unordered_map<std::string, aphelyon::UuidV7::ValueType>          weaponEquipment;
std::unordered_map<std::string,
    std::unordered_map<uint8_t, aphelyon::UuidV7::ValueType>>         gearEquipment;
```

**Fix (deferred, not blocking):** either (a) move the `using` declarations above the Snapshot struct so it can name `WeaponEquipmentMap` etc., (b) generate Capture/Restore from an X-macro list of fields, or (c) accept the duplication and add a `static_assert(sizeof(Account) == X)` tripwire that fails noisily when fields are added. (c) is the cheapest; (a) is the cleanest.

### M-Q2. C7 snapshot deep-copy partially undermines M2's lazy-lease win
**Files:** `Server/Account/src/AccountTransaction.hpp:38-52`, `Server/Account/src/Account.hpp:100-126`
**Status:** NEW (interaction between C7 and M2)

M2's read-mostly-handler perf rationale (line 28-35) is that `HandleGetQuestState`, `HandleReportQuestProgress` no-op tick, `HandleSetParty` with nothing dirty, etc. avoid paying a pool lease + connection check-out cost. But every `AccountTransaction` ctor still calls `CaptureSnapshot()` (line 49) — a deep copy of the wallet, RNG state, three pity maps, CollectionState (owned characters + weapons + gear + substats), QuestStateStore (potentially hundreds of quests + their objectives), WorldFlagStore, materials map, party array, weapon equipment, gear equipment (nested map). For a mid-late player this is dozens of KB of allocations and map-rehashes per RPC — including the read-only RPCs M2 was supposed to make cheap.

The C7 commit message describes the Memento choice as "zero handler changes / no regression risk on RNG/pity/wallet" — true for correctness, but the perf characterization didn't account for the read-mostly handlers. The original C7 alternative ("buffer mutations in a transaction-local struct and apply to `Account` only after `Commit()` succeeds") would have skipped the snapshot entirely for read handlers.

**Fix (deferred):** either lazily capture the snapshot on first mutating buffer op (mirror M2's `EnsureOpen` pattern: defer Capture until `AppendEvent` / a setter signals mutation), or skip Capture entirely when the handler signals "no mutation expected" via a `BeginRead()` factory. The first approach is cleaner — requires plumbing dirty-state tracking through the ctor → first-mutation point.

### M-Q3. `AccountServer.hpp` line count grew during M-class triage (audit L10 regressed)
**Files:** `Server/Account/src/AccountServer.hpp` (616 lines, was ~504 at audit baseline `8b2c77f`)
**Status:** VERIFIED-OPEN (worse)

L10 flagged AccountServer.hpp at ~504 lines mixing 5 responsibilities (lifecycle, session validation, message dispatch, internal RPC endpoint setup, idle eviction, account hydration). The deferred-refactor commit message at `8e19666` notes "L10/L11/L12 are larger refactors deferred for separate commits." Since then:

- **M5 quest-token-secret env-var path** (+25 lines in `InitializeQuests`, line 358-380)
- **M1 OnStopped two-phase pattern** (+25 lines line 135-182)
- **L8 OnStopped save-failure logging** (+10 lines line 165-181)
- **L13 ctor invariant + comment** (+5 lines line 71-75)
- **H1 stripe-lock VerifyCredentials** (+15 lines line 225-235)
- **C5 lazy PBKDF2 rehash** (+15 lines line 237-254)
- **C1 stale-flag check in GetLockedAccount** (+15 lines line 451-462)
- **H4 BumpLastLogin** (+5 lines line 218-223)

Net effect: same 5 responsibilities, ~110 more lines of inline complexity, none of which were extracted. The `SetupInternalEndpoint()` method alone is now 113 lines (line 205-320) handling three distinct RPC verbs.

**Fix (deferred refactor):** split as L10 originally proposed — pull `LoadAccountFromData` + `m_accounts` + `GetLockedAccount` + `CleanupIdleAccounts` into an `AccountCache` class; pull `SetupInternalEndpoint`'s three method bodies into an `AccountInternalEndpoints.hpp` analogous to the public handler split.

## Low / Observation

### L-Q1. Unused `<algorithm>` include in `AccountServer.hpp`
**File:** `Server/Account/src/AccountServer.hpp:34`
**Status:** NEW (dead since L3 removed `ConvertAccountToData`)

`L3` deleted `ConvertAccountToData`. That function was the only user of `<algorithm>` in this file (the baseline `git show 8b2c77f` confirms no other `std::sort` / `std::find` / `std::transform` etc. in the file). The include can be dropped.

```cpp
// Line 34 — no std::algorithm uses remain after L3
#include <algorithm>
```

The only `std::` from `<utility>` actually used is `std::move`, which `<utility>` (transitively pulled by `<memory>` line 37) provides.

**Fix:** delete the `#include <algorithm>` line.

### L-Q2. `AccountServer::SaveAccountToRepository` is dead but not deleted
**File:** `Server/Account/src/AccountServer.hpp:565-568`
**Status:** NEW (dead code that L3 missed)

```cpp
void SaveAccountToRepository(Account& account)
{
    m_repository.Save(account);
}
```

`grep -rn "SaveAccountToRepository" Server/` finds zero callers. The only paths that flush an account are `CleanupIdleAccounts` (line 428: `m_repository.Save(*account)` direct) and `OnStopped` (line 169: same direct call). The helper appears to be a leftover from the saveAccount era — same audit-L3 family as `ConvertAccountToData`.

**Fix:** delete the method or tombstone-comment it like the L3 removals.

### L-Q3. `<vector>` not directly included in `Account.hpp` despite use at file scope
**File:** `Server/Account/src/Account.hpp:365`
**Status:** NEW (IWYU violation, currently works transitively)

```cpp
// Account.hpp:365 — uses std::vector
std::vector<ProgressionConfig::StoryProgression::LevelReward> granted;
```

No `#include <vector>` at the top — pulled in transitively via `ProgressionConfig.hpp`. Fragile against an unrelated refactor of `ProgressionConfig.hpp` that drops the vector include.

**Fix:** add `#include <vector>` to Account.hpp's prelude alongside `<array>` / `<unordered_map>`.

### L-Q4. `EffectDispatcher` remains 138 lines of dead scaffolding, no tests
**File:** `Server/Account/src/EffectDispatcher.hpp` (138 lines)
**Status:** VERIFIED-OPEN (L4 intentional retention)

L4 was flagged "kept per project preference." Verified by grep: zero construction sites outside the file itself. H10 (latent version-cursor bug) was fixed inside this dead file. The reducer-purity linter and golden-file tests do not exercise it. The M18 sweep correctly collapsed three currency-dispatch duplicates *through* this file — `operator()(const GrantCurrencyEffect&)` (line 91-137) is the only call site for `Wallet::GetBy`/`AddBy`'s currency-typed API that lives in vestigial code. If a future engineer wires the dispatcher in and the wiring is wrong, the latent failure mode is silent.

**Fix (deferred per project preference):** either commit to wiring it (gate behind a feature flag, then write the integration tests), or accept the maintenance debt and add a `[[deprecated]]` attribute so accidental construction triggers a compiler warning.

### L-Q5. `Snapshot::claimed_at_streak_day` field naming is misleading
**File:** `Server/Account/src/events/QuestClaimEvents.hpp:28`
**Status:** NEW (audit M16 closed but the name remains imprecise)

M16's comment correctly documents the field as intentionally preserved for historical audit queries:

```cpp
// Audit M16 (2026-06-02): the streak day on which the claim happened.
// Intentionally captured at event time for historical audit/queries
int         claimed_at_streak_day;
```

The field actually stores the **streak count** (`account.GetLoginStreak()` at QuestHandlers.hpp:478) — i.e. "how many consecutive days in a row" — not a day-of-epoch or calendar day number. A future analytics query reading `claimed_at_streak_day = 7` will read it as "claimed on day 7 of the calendar" not "claimed when player was on a 7-day streak." The comment also calls it "streak day" which compounds the ambiguity.

**Fix:** rename to `streak_count_at_claim` or `login_streak_at_claim`. Or extend the M16 comment to disambiguate "this is a count of consecutive days, not a calendar day."

### L-Q6. `ServiceClient` / `ServiceEndpoint` call `std::getenv` on every internal RPC
**File:** `Server/Common/src/ServiceClient.hpp:119`, `Server/Common/src/ServiceEndpoint.hpp:162`
**Status:** NEW (introduced by M3)

`InternalRpcAuth::GetSharedSecret()` reads `APHELYON_INTERNAL_SECRET` via `std::getenv` on every call:

```cpp
// ServiceClient.hpp:119 — runs per outbound RPC
const std::string secret = InternalRpcAuth::GetSharedSecret();
```

`std::getenv` is documented as not-thread-safe with `setenv`/`putenv` on POSIX — irrelevant in practice since nothing rewrites the env post-startup, but a perf observation stands. The secret never changes during process lifetime; one cached `static const std::string` inside `InternalRpcAuth` (or read-once in `main.cpp` and threaded through) would eliminate the per-RPC syscall.

**Fix:** cache via `static const std::string` inside `GetSharedSecret()` or add an explicit `Init()` step in main.cpp.

### L-Q7. M11 strict-parse + M7 tri-state patterns repeat verbatim ~9 + ~7 times
**Files:** 9 sites of `ParseJsonStrict` boilerplate; 7 sites of idempotency Hit/Miss/Error
**Status:** NEW pattern emergence (not yet a bug)

The strict-parse boilerplate is 3 lines, repeated 9× (GachaHandlers.hpp:60, 301; AccountHandlers.hpp:175, 303; ProgressionHandlers.hpp:124, 208, 292, 377; QuestHandlers.hpp:291):

```cpp
auto parsed = ParseJsonStrict(payload);
if (!parsed) return m_ctx.createErrorResponse("Invalid request payload");
Json& request = *parsed;
```

The idempotency lookup is 7 lines, repeated 8× (GachaHandlers.hpp:80-89, 318-329; AccountHandlers.hpp:333-342; ProgressionHandlers.hpp:143-155, 227-239, 314-326, 399-411; QuestHandlers.hpp:311-321):

```cpp
const std::string scopedKey = IdempotencyKey::Scoped("<prefix>", clientKey);
if (!scopedKey.empty()) {
    auto lookup = m_ctx.repository->FindIdempotency(account.GetAccountId(), scopedKey);
    if (lookup.status == IdempotencyLookup::Status::Error)
        return m_ctx.createErrorResponse("Cache unavailable, please retry");
    if (lookup.status == IdempotencyLookup::Status::Hit) {
        LOG_DATA_DEBUG("Idempotent <verb> retry: ...");
        return m_ctx.createResponse(<msgId>, lookup.cachedPayload);
    }
}
```

Mechanical duplication — easy to drift. If a future audit changes the error message ("Cache unavailable, please retry"), or the log format, 8 sites need updating. Currently the parameter set is small enough that a `IdempotentDispatch(prefix, clientKey, msgId, label, accountId, fn)` helper would be cleaner.

**Fix (deferred, not blocking):** factor a `IdempotentRpc` helper / lambda passed the action body. Defer until at least one drift case occurs (preserving the simpler grep-visible structure for now is defensible).

### L-Q8. `Snapshot::dirty` round-tripping creates subtle data-flow non-locality
**File:** `Server/Account/src/Account.hpp:97, 124, 151`
**Status:** OBSERVATION

The Snapshot field includes the entire `aphelyon::DirtyState` struct, so on Rollback the dirty bits flip back to the values held at `Begin()` time. For handlers that did `account.SetXxx()` BEFORE `Begin(account)` (e.g. setting dirty flags during read pre-checks), those bits ARE part of the pre-tx snapshot. Today no handler does this — every mutating path opens the transaction before mutating — but the contract isn't documented and a future "set party defaults then begin tx" handler would have those pre-Begin mutations correctly restored by Rollback (i.e. snapshot semantics) which would be surprising if the author thought "Rollback only undoes what happened during the tx."

The behavior is correct (snapshot = state at Begin = what we want to roll back to), but the implicit "Begin's timing matters" rule is undocumented.

**Fix:** one-line contract note in `Account::CaptureSnapshot` documenting that the snapshot is the *Begin-time* state, so any pre-Begin mutations get unwound too.

## Verified Closed

- **L1** — `AccountTransaction::Rollback`'s `tx_->abort()` is now wrapped in try/catch with LOG_DATA_WARN on both `std::exception` and `...` paths. (AccountTransaction.hpp:288-294)
- **L3** — `ConvertAccountToData` removed (tombstone comment at AccountServer.hpp:570-573). `TimestampToTimeT` removed (tombstone at AccountRepository.hpp:334-337). `AccountTransaction::store_`/`pool_` are actively used (`pool_.acquire()` in EnsureOpen line 345; `store_.AppendInTx` line 162).
- **L5** — Phase 7 TODO replaced with the real invariant doc at Account.hpp:519-526.
- **L6** — `TickQuests.hpp:120-124` now correctly notes the FlushQuests stub no longer applies; references audit L6.
- **L7** — `AccountTransaction::Commit` uses `std::optional<std::string>` so null `before`/`after` write SQL NULL not the literal `'null'` (AccountTransaction.hpp:184-200).
- **L8** — `OnStopped` accumulates failed/saved counts, logs per-failure `LOG_DATA_ERROR`, and summarizes at the end (AccountServer.hpp:160-181).
- **L13** — Ctor invariant `if (m_ctx.repository == nullptr) throw std::logic_error(...)` at AccountServer.hpp:74-75. HandlerContext.hpp:71 documents the contract.
- **M2** — `State enum {Pending, Active, Committed, RolledBack}` at line 335 plus `EnsureOpen()` lazy-acquisition at line 341-348. `Lease()` default ctor + move-assign at ConnectionPool.hpp:45-59. State transitions consistent across `Commit`/`Rollback`/dtor (Rollback at line 274-320 handles all three terminal/non-terminal states; dtor at line 59-64 calls Rollback only for Pending/Active).
- **M13** — LoadEventVersions unknown-aggregate path now logs ERROR.
- **M14** — `loadouts.name` dropped; `preset_id > 0` reserved with schema-level comment (`schema.sql:175-194`).
- **M15** — `FlushOwnedGear` sorts gear_substats by `stat_type` for deterministic `slot_idx`.
- **M16** — `claimed_at_streak_day` documented as intentional preservation (see L-Q5 for the naming nit).
- **M17** — `quest_states_active_idx` comment now correctly maps `state IN (1, 2)` to `Available + Active` with full enum table.
- **M18** — `Wallet::GetBy(Currency)` / `AddBy(Currency, int)` exist (Wallet.hpp:140-165); `EffectDispatcher::operator()(GrantCurrencyEffect)` and `AccountHandlers::HandleAddCurrency` both use them. Three duplicate `CurrencyFromStr` implementations collapsed.
- **C7** — Snapshot struct + CaptureSnapshot + RestoreFrom at Account.hpp:73-152. AccountTransaction captures in ctor (line 49) and restores in Rollback (line 308). See M-Q1, M-Q2 for follow-on issues.

## Assurance — invariants the recent edits provide

- **AccountTransaction state machine is consistent across all terminal paths.** `Pending` → `Active` (via EnsureOpen) → `Committed` (via Commit). `Pending` or `Active` → `RolledBack` (via Rollback, including dtor). Double-Commit / double-Rollback are no-ops or throw. Verified by direct read; the test commit (`7899997`) added the [m2] test case proving an empty Commit on a Pending tx never touches the pool.
- **Stale-flag → reload contract is closed.** `GetLockedAccount` checks `IsStale()` and erases from cache (AccountServer.hpp:458-462). Reload happens under the same stripe lock as the next handler. The C7 snapshot is belt-and-suspenders on top of this.
- **Post-commit bookkeeping cannot poison the DB.** AccountTransaction.hpp:247-271 wraps the version-cursor advance in try/catch + MarkStaleForReload, so a thrown exception after the DB commits doesn't propagate as a fake "failed" response.
- **Idempotency tri-state surfaces DB errors instead of silently re-executing.** `IdempotencyLookup::Status::Error` correctly threaded through 8 handler sites; on Error every handler returns `"Cache unavailable, please retry"` instead of falling through.
- **HMAC-authenticated internal RPCs.** `InternalRpcAuth` correctly used in both ServiceClient (sign) and ServiceEndpoint (verify); `IsSecretConfigured()` checked at main.cpp startup in all three services (Auth/Account/Combat).
- **L13 startup invariant catches the structural guarantee at the right time.** Ctor throws `std::logic_error` if `HandlerContext::repository == nullptr` — handlers can dereference unconditionally per the audit's recommendation.

## LOC Trajectory

`scripts/count-loc.ps1` output:

```
Code:    87,021 (Server: 15,946 / Client: 48,086 / Tools: 22,989)
Data:    60,466
Total:  147,487
```

Baseline from the 2026-06-02 audit (commit `62e43d3`, which added count-loc.ps1):
```
Code: 86,893; Data: 60,466
```

Server code grew **+128 LOC** net over the audit-triage commits (15,946 vs ~15,818). Most of this lands in `AccountServer.hpp` (+112) and `AccountTransaction.hpp` (+~80 from C7+M2), partly offset by L3 deletions (-36 ConvertAccountToData, -16 TimestampToTimeT) and M18 deduplication (-~40 across EffectDispatcher + AccountHandlers + Wallet). Client and Data are flat (no Client work in this audit cycle).

Net characterization: the M-class fixes were on net additive — the deferred L10/L11/L12 refactors did not fire, so the file-level complexity flagged in the previous audit is now slightly worse, not better. This is expected and tracked; the deferred work needs to happen before launch.
