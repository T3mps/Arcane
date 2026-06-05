# v3 Follow-up Audit: Code Quality + Complexity

**Date:** 2026-06-03
**Auditor:** #7 of 8 (v3, third pass)
**Scope:** Verify the v2 M-class triage closes cleanly and that the M-V2-2/M-V2-3/M-V2-4/M-V2-10 refactors don't introduce new structural debt.
**Baseline:** `2026-06-02-followup-code-quality.md` (v2 — flagged M-Q1/M-Q2/M-Q3 + L-Q1..L-Q8).

## Summary

The v2 deferred-refactor cluster landed cleanly. **`AccountServer.hpp` is now 345 lines** (was 683 at v2 baseline, audit baseline ~504) — the L10/M-Q3 complaint is closed and inverted: the file is now *smaller* than the original v1 audit baseline. The three extractions (`AccountHydrator` 93, `AccountCache` 264, `InternalRpcHandlers` 199) all have crisp single responsibilities and minimal cross-coupling. The M-V2-2 X-macro collapses the three Snapshot mirror surfaces into a single macro-driven list; the rngState/collectionState "indirect" fields are flagged in-line so future authors won't miss them. The M-V2-4 HandleClaimQuestReward split reads top-to-bottom for the first time since the audit campaign began — 120 lines of orchestration, two helpers that each fit on one screen. **LruCache** is a small, well-scoped reusable type that retroactively justifies a v2 finding (M-V2-10) by way of being applicable to future bounded caches (SessionManager is the obvious next consumer).

The remaining concerns are all small. The biggest one is the **dead-include drift in `AccountServer.hpp`** (eight includes that no longer have call sites in this file) — minor in isolation, but symptomatic of the M-V2-3 extraction not running an IWYU pass at the end. **Three v2 items did not get touched**: the dead `<algorithm>` include (L-Q1) is still there, the dead `SaveAccountToRepository` method (L-Q2) is still there, and the `<vector>` IWYU violation in `Account.hpp` (L-Q3) is still there. These are surely-not-blocking but trivially closable.

Two genuinely-new code-quality concerns surfaced from the refactor batch: (1) `AccountCache`'s `LockFor`/`InsertIfAbsent`/`UpdateCachedPasswordHash` escape-hatch trio has a contract that's documented but **structurally unenforceable** — the methods take their own internal mutex but don't assert (or accept a token witnessing) that the caller holds the stripe lock; and (2) `InternalRpcHandlers` includes `<chrono>` that nothing in the file uses (probably copied over from the original lambda site that called `std::chrono::system_clock`).

## Critical

*None.*

## High

*None.*

## Medium

### M-Q3-1. `AccountCache` escape-hatch contract is documented but unenforced

**Files:** `Server/Account/src/AccountCache.hpp:218-250`
**Status:** NEW (introduced by M-V2-3 step 2/3)

The `LockFor` / `InsertIfAbsent` / `UpdateCachedPasswordHash` trio carries this contract in its doc comment:

```cpp
// LockFor / InsertIfAbsent /
// UpdateCachedPasswordHash are escape hatches for handlers
// (VerifyCredentials) that need fine-grained cache access — the
// stripe lock returned by LockFor must be held across any matching
// InsertIfAbsent / UpdateCachedPasswordHash call.
```

The two mutator methods both acquire `m_mapMutex` internally, but neither verifies the caller holds the stripe lock for the relevant playerId. A future caller (or refactor) that forgets to take `LockFor(playerId)` first will compile and run — and at low concurrency may even pass tests — but loses the invariant that motivated H-V2-3 (the re-read under stripe lock). Specifically: a concurrent `GetLockedAccount` between an unguarded `LoadByUsername` and `InsertIfAbsent` can see the OLD password hash, and the `H-V2-3` v2 fix was precisely designed to close that race window.

Today there's exactly one caller (`InternalRpcHandlers::HandleVerifyCredentials`) and it does follow the protocol. So this is a structural-debt finding, not a live bug. Options:

1. **Token witness** — `LockFor` returns a `StripeLock` type that wraps `std::unique_lock<std::mutex>`; `InsertIfAbsent` / `UpdateCachedPasswordHash` accept a `const StripeLock&` parameter so the type system enforces the protocol. Cost: ~15 LOC in the type wrapper, every call site gains an explicit lock arg.
2. **Move the escape hatch out** — the only consumer is `InternalRpcHandlers::HandleVerifyCredentials`. If `AccountCache` grew a single `VerifyAndCache(playerId, password, hashCheck)` method that took the stripe lock internally, the escape hatch goes away. Cost: pushes domain-specific logic into `AccountCache`, which is currently storage-only.
3. **Assert/log on entry** — `InsertIfAbsent` could check `m_playerLocks.IsHeldByCurrentThread(playerId)` (would require `StripedMutex` to expose this, doesn't today). Cost: API expansion on `StripedMutex`.

**Recommendation:** Option (1) when one of the v3-future audit recommendations adds another `LockFor` caller; until then the doc-comment contract is acceptable.

### M-Q3-2. `AccountServer.hpp` carries 8 dead `#include` lines

**File:** `Server/Account/src/AccountServer.hpp:4, 10, 15, 21, 22, 41, 42, 45, 48`
**Status:** NEW (M-V2-3 extraction moved usages out; includes weren't pruned)

After the M-V2-3 step-by-step split, the following includes have zero in-file usage:

```cpp
#include "AccountData.hpp"          // line  4 — only mentioned in the L3 tombstone comment
#include "GachaConfig.hpp"          // line 10 — moved with handlers; no call sites here
#include "TickQuests.hpp"           // line 15 — moved into AccountHydrator
#include "Json.hpp"                 // line 21 — only handlers parse JSON now
#include "StripedMutex.hpp"         // line 22 — moved into AccountCache
#include <algorithm>                // line 41 — L-Q1 carry-over from v2 (still dead)
#include <chrono>                   // line 42 — moved into AccountCache
#include <mutex>                    // line 45 — moved into AccountCache
#include <unordered_set>            // line 48 — m_pendingCleanup moved into AccountCache
#include <vector>                   // line 49 — moved into AccountCache
```

Verified by `Grep`: zero in-file references to anything these provide. Each transitively comes back via one of the handler headers, so removal won't break the build — but the contract has rotted. Future authors reading the include list to understand "what does AccountServer depend on?" get a misleading answer.

**Fix (mechanical):** delete the dead lines + L-Q1's `<algorithm>` and L-Q2's `SaveAccountToRepository` body in one cleanup pass. The cluster of dead state is small but the audit-trail comments around them are now stale (L3's tombstone, the `// (Audit L3 2026-06-02 — ConvertAccountToData removed)` block at line 289-292 — keep the tombstone, but the dead include + dead helper around it both go).

## Low / Observation

### L-Q3-1. `InternalRpcHandlers` includes `<chrono>` that nothing in the file uses

**File:** `Server/Account/src/InternalRpcHandlers.hpp:34`
**Status:** NEW

Verified by `Grep`: zero references to `std::chrono`, `system_clock`, `steady_clock`, `seconds`, or `milliseconds`. Probably copied over from the original lambda location that used `std::chrono::system_clock` to bump `last_login`; that path now goes through `m_repository.BumpLastLogin(...)` which encapsulates the time call. Drop the line.

### L-Q3-2. M-V2-2 X-macro: minor style drift between alias use sites

**File:** `Server/Account/src/Account.hpp:96-116, 477-494`
**Status:** OBSERVATION

The X-macro at line 96 references `PityTrackerMap`, `GuaranteeTrackerMap`, `SlotPityDataMap`, `PartyArray`, etc. — uniformly aliased. The member declarations at line 477-494 still spell three of these inline:

```cpp
std::unordered_map<std::string, PityTracker>     m_pityBySlot;
std::unordered_map<std::string, GuaranteeTracker> m_guaranteeBySlot;
std::unordered_map<std::string, SlotPityData>     m_rawPity;
// ...
std::array<std::string, 4> m_party = {"", "", "", ""};
```

vs. the macro's:

```cpp
X(PityTrackerMap,         pityBySlot,       m_pityBySlot)
X(GuaranteeTrackerMap,    guaranteeBySlot,  m_guaranteeBySlot)
X(SlotPityDataMap,        rawPity,          m_rawPity)
X(PartyArray,             party,            m_party)
```

The types are equivalent so the X-macro `Type` arg is correct, but a reader looking at the member-decl block sees the un-aliased form. If a future engineer renames `PityTracker` they have to update both sites (member decl + macro types stay coherent via the `using` alias, but the inline expansion at line 477 won't pick up the alias change). Minor.

**Fix (cosmetic):** rewrite the inline member declarations to use the aliases (e.g. `PityTrackerMap m_pityBySlot;`). Affects 4 lines, zero behavior change.

### L-Q3-3. Snapshot doc-comment mentions `m_stale` exclusion but post-restore stale mark is in `Rollback`, not `RestoreFrom`

**Files:** `Server/Account/src/Account.hpp:82-84`, `Server/Account/src/AccountTransaction.hpp:357`
**Status:** OBSERVATION

The doc comment at Account.hpp:82-84 says:

```
m_stale is NOT captured either: Rollback explicitly sets it after
the restore as belt-and-suspenders, so the next GetLockedAccount
reloads from DB even if the restore itself somehow drifts.
```

True for `Rollback`, but the comment lives on `Account::Snapshot` whose docstring frames it as "what's captured and what isn't." A reader who jumps to `RestoreFrom` (where they'd reasonably look for "what does restore touch") doesn't see the m_stale clarification. Either move the m_stale note to `RestoreFrom`'s body comment too, or leave it but note: "see Rollback for the post-restore stale mark."

### L-Q3-4. L-Q1, L-Q2, L-Q3 from v2 still open

**Files:** v2 audit, lines 71-114
**Status:** VERIFIED-OPEN (v2 carry-over)

- **L-Q1** — `#include <algorithm>` at `AccountServer.hpp:41` still dead (now bundled with M-Q3-2 above).
- **L-Q2** — `SaveAccountToRepository` at `AccountServer.hpp:284-287` still dead, no callers, only a tombstone-shaped comment marks the L3 ConvertAccountToData removal next to it.
- **L-Q3** — `Account.hpp` still missing direct `#include <vector>` despite using `std::vector` at line 363 (`std::vector<ProgressionConfig::StoryProgression::LevelReward> granted`). Pulled in transitively via `ProgressionConfig.hpp`; fragile.

All three are one-line edits. Closing in a single `chore(account): code-quality micro-sweep` commit costs ~10 LOC of deletion + 1 LOC of insertion.

### L-Q3-5. `LruCache::Put` copies the key twice on a fresh insert

**File:** `Server/Common/src/LruCache.hpp:81-99`
**Status:** OBSERVATION

`Put(const K& key, V value)` calls `m_list.emplace_front(key, std::move(value))` (copy 1) then `m_map[key] = m_list.begin()` (copy 2). For `std::string` IP-shaped keys the cost is sub-microsecond and irrelevant to the actual `RateLimiter` workload, but the LruCache is positioned as reusable — adding an rvalue overload `Put(K&& key, V value)` or making `Put` take K by value-then-move would let future consumers with heavier keys (e.g. UUID byte vectors) avoid the double-copy. Pure forward-looking observation; the current consumer doesn't care.

### L-Q3-6. `LruCache::LruKey()` has no consumer

**File:** `Server/Common/src/LruCache.hpp:144-148`
**Status:** OBSERVATION

The diagnostic accessor is exercised in `LruCacheTest.cpp` but no production code reads it. Either delete (YAGNI), or leave for the moment with a comment marking it "diagnostic, not currently consumed" so a future reader knows it's not load-bearing for any path.

### L-Q3-7. `AccountServer.hpp` retains an empty section divider

**File:** `Server/Account/src/AccountServer.hpp:258-261`
**Status:** OBSERVATION

```cpp
// ============================================================================
// Idle Account Eviction
// ============================================================================

// ============================================================================
// Account Access (two-lock protocol: stripe → map)
// ============================================================================
```

After the M-V2-3 step 2/3 split, the "Idle Account Eviction" section has zero content (the body moved to `AccountCache::CleanupIdleAccounts`). Dropping the header would tighten the file by 4 lines and remove a misleading hint that there's a method below.

### L-Q3-8. v2 L-Q5 (`claimed_at_streak_day` naming) still open

**File:** `Server/Account/src/events/QuestClaimEvents.hpp:28`
**Status:** VERIFIED-OPEN (v2 carry-over)

The naming nit from the v2 audit (`claimed_at_streak_day` storing a count, not a calendar day) is unaddressed. Low priority; either rename to `streak_count_at_claim` or extend the M16 comment to disambiguate.

### L-Q3-9. v2 L-Q7 deduplication pattern unchanged

**Files:** 8 idempotency-lookup sites, 11 ParseJsonStrict sites
**Status:** VERIFIED-OPEN (v2 carry-over)

v2's L-Q7 flagged the 8-call repetition of the `IdempotencyKey::Scoped` + tri-state `Hit/Miss/Error` block. The M-V2-* refactors did not address this; the pattern repeats verbatim across `HandlePull`, `HandleMultiPull`, `HandleAddCurrency`, `HandleLevelCharacter`, `HandleAscendCharacter`, `HandleLevelWeapon`, `HandleAscendWeapon`, `HandleClaimQuestReward`. Same with `ParseJsonStrict` — 11 sites of the 3-line `auto parsed = ... if (!parsed) return ... Json& request = *parsed;` block.

Still defensible to defer until a real drift case occurs; preserving the grep-friendly structure has value while the audit campaign is hot. If the count goes any higher (or a future audit flags a drifted message), factor it. Pattern that would work: a `IdempotentDispatch(prefix, clientKey, msgId, label, accountId, fn)` higher-order helper.

### L-Q3-10. `BuildClaimEvents`'s `rewardKindFor` helper is a local lambda where a static could live

**File:** `Server/Account/src/QuestHandlers.hpp:396-404`
**Status:** OBSERVATION

The inline lambda at line 396 maps currency-id strings to `RewardKind`. It's an exact mirror of `DispatchCurrencyReward`'s if-else chain plus the `Material` fallback for `xp`. Two minor concerns:

1. **Drift surface** — adding a new currency requires updating `DispatchCurrencyReward` (line 331-339) AND `rewardKindFor` (line 396-404). Same shape, different signatures.
2. **`xp` returning `Material`** — `Material` is the catch-all for unknown currency ids, AND it's what `xp` (which isn't a wallet currency at all) maps to. The comment "includes 'xp' — handled separately downstream" rescues this, but a future reader sees the `Material` mapping firing on `xp` and may rewire it without realizing the downstream filter at line 406 (`if (reward.currencyId == "xp") continue`) already excludes xp from the event-rewards list.

**Fix (deferred):** when a new currency lands, both sites get touched together; otherwise the dual-mapping is fine. If/when consolidating, a `Currency` enum-key-aware helper (e.g. `WalletEvents::QuestRewardKindFor(Currency)` keyed by the existing enum) would collapse both sites onto the same primitive.

### L-Q3-11. `AccountServer.hpp` line 281 ctor comment mentions "soon-to-be-loaded m_questLoader"

**File:** `Server/Account/src/AccountServer.hpp:63-67`
**Status:** OBSERVATION (mostly a wording nit)

```cpp
// Audit M-V2-3 step 2/3 — the cache binds refs to the two
// members above (m_repository) and the soon-to-be-loaded
// m_questLoader.
```

Tiny wording issue: "soon-to-be-loaded" is ambiguous between "constructed" and "populated with data via Load()." The actual semantic is that `m_questLoader` is *constructed* before `m_cache` (declaration order) but *populated* only later (in `InitializeQuests`). A reader unfamiliar with C++ member init order might assume the comment claims `m_cache` binds an uninitialized reference, which would be a bug. Reword to "the soon-to-be-populated m_questLoader (declared after m_cache in the member layout — m_cache's ref-bind sees the constructed-but-empty loader; InitializeQuests fills it below)."

## Verified Closed

- **M-Q1** (v2) — `Account::Snapshot` X-macro is in place at `Account.hpp:96-116`. CaptureSnapshot's reads and RestoreFrom's writes both expand from the same macro list. The 24-field three-place duplication is now a single 20-line macro. Indirect fields (rngState, collectionState) are listed in the Snapshot struct's comment and handled explicitly in both Capture and Restore so a future "indirect" field joins them visibly.
- **M-Q2** (v2) — H-V2-1's concern (eager snapshot capture defeats M2's lazy-lease win) is acknowledged in `AccountTransaction.hpp:30-52` doc; the snapshot remains eager because C7-A requires it to happen BEFORE handler mutations, which forces it at `Begin()` time. The perf wart on read-only handlers is acknowledged at design level but accepted because correctness (C7-A) takes priority. Snapshot is a deep copy but the only "read-only" handlers that pay this cost are `HandleGetState`, `HandleGetQuestState`, `HandleGetCharacterStats`, `HandleGetInventory`, `HandleGetBanners` — and three of those don't open transactions at all (no `m_ctx.repository->Begin(account)` call).
- **M-Q3** (v2) — `AccountServer.hpp` is now **345 lines** (was 616 at v2 baseline, 504 at original audit baseline). The M-V2-3 split sequence produced three crisply-scoped extractions: `AccountHydrator` (93 lines, pure factory), `AccountCache` (264 lines, in-memory hash + stripe locks + eviction), `InternalRpcHandlers` (199 lines, three named RPC methods). Each fits on roughly two screens. The previously-monolithic file is now genuinely smaller than the original audit baseline.
- **M-V2-2** — single-source-of-truth X-macro in `Account.hpp:96-116`. Verified: every mutable member field is either in the X-macro (19 fields) or the indirect list (rngState, collectionState) or explicitly excluded by the doc comment (m_id, m_accountId, m_publicUid hydrate-only identity; m_stale belt-and-suspenders). The macro expands correctly into the struct declaration, CaptureSnapshot, and RestoreFrom. No field is missing; no field is in the macro that shouldn't be.
- **M-V2-3** — three crisply-scoped headers; the stripe-lock contract documented inside `AccountCache` matches `InternalRpcHandlers::HandleVerifyCredentials`'s call shape. Minimal cross-coupling: `AccountHydrator` is a pure factory (no state); `AccountCache` owns nothing it doesn't need; `InternalRpcHandlers` is wired by ref-bind in the constructor and Register() called explicitly. M-V2-3 verified closed; see M-Q3-1 above for the structural-debt note on the escape-hatch contract.
- **M-V2-4** — `HandleClaimQuestReward` body shrank from ~310 to ~120 lines (lines 486-641). The two helpers `ApplyClaimRewards` (line 345-376) and `BuildClaimEvents` (line 384-484) each fit on roughly one screen and have clear contracts. The main handler now reads top-to-bottom as: parse → validate → idempotency check → state checks → snapshot pre-state → open txn → mutate quest → apply rewards → streak bridge → tier bridge → propagate → build response → build events → commit. No phase was missed for extraction; the remaining inlines (streak bridge, tier bridge, PropagateClaim+UnlockEligibleQuests calls) are each ≤10 lines and don't warrant their own helper.
- **M-V2-10** — `LruCache<K,V,Hash>` is a clean, well-documented reusable bounded-cache type. The Get/Peek distinction is exactly what `RateLimiter::Allow` / `GetCooldownRemaining` need (touch vs. no-touch). EraseIf handles the 10-min idle cleanup sweep. Put auto-evicts at cap, closing the M-V2-10 attacker scenario. Tests at `Server/Account/tests/LruCacheTest.cpp` exercise the API surface + two scenario tests (attacker spray, legit-user-survives). Test count grew 162→442 assertions.
- **M-V2-11** — `InternalRpcAuth::GetSharedSecret()` is now static-local cached (`InternalRpcAuth.hpp:53-66`); reads the env var ONCE at first access. C++11+ thread-safe static-local initialization closes the per-RPC `std::getenv` syscall and the thread-safety nit.

## Long handlers remaining

Per-handler line counts after M-V2-4:

```
HandlePull               (GachaHandlers.hpp:56-305)         ~250 lines
HandleMultiPull          (GachaHandlers.hpp:307-641)        ~335 lines  ← longest in service
HandleAddCurrency        (AccountHandlers.hpp:298-412)      ~115 lines
HandleClaimQuestReward   (QuestHandlers.hpp:486-641)        ~120 lines  (post-M-V2-4)
HandleSetParty           (AccountHandlers.hpp:170-296)      ~127 lines
HandleReportQuestProgress (QuestHandlers.hpp:166-292)       ~127 lines
HandleLevelCharacter      (ProgressionHandlers.hpp:121-214)  ~94 lines
HandleAscendCharacter     (ProgressionHandlers.hpp:216-303)  ~88 lines
HandleLevelWeapon         (ProgressionHandlers.hpp:305-393)  ~89 lines
HandleAscendWeapon        (ProgressionHandlers.hpp:395-483)  ~89 lines
```

**`HandleMultiPull` is now the longest in the service.** Three phases stand out as extraction candidates by the same M-V2-4 logic:

1. **The 10-pull guarantee upgrade + pity recompute** (lines 436-474, ~40 lines) — orthogonal to the main pull loop and easily testable in isolation.
2. **Per-pull collection mutation + resonance bookkeeping** (lines 478-513, ~36 lines) — mirrors `HandlePull`'s lines 169-193 almost verbatim; the duplication between Pull and MultiPull is the most painful in the service.
3. **Build pull + wallet events** (lines 549-609, ~60 lines) — paired with the equivalent block in `HandlePull` (lines 195-268), this is the most obvious deduplication target.

A `MultiPullEngine` / `PullEvents` helper layer would collapse Pull and MultiPull substantially — the audit calls this out as the next M-class extraction target. Defer for now; behavior is correct and the helpers are visible. **Tracking as M-Q3-3 below.**

### M-Q3-3. `HandlePull` / `HandleMultiPull` duplication is the next extraction target

**Files:** `Server/Account/src/GachaHandlers.hpp:56-641`
**Status:** OBSERVATION (deferred refactor, lower priority than this v3 cycle)

The two handlers share ~80% of their event-building logic, collection-mutation logic, and idempotency-cache logic. M-V2-4 successfully demonstrated the helper-extraction pattern on `HandleClaimQuestReward`; applying the same pattern here would shrink GachaHandlers.hpp from 710 lines to ~400 and eliminate a category of "fix Pull, forget MultiPull" bugs (which we've already shipped once — H9's deterministic key handling required parallel edits in both handlers).

## Duplication audit

Cross-handler duplication state:

- **Idempotency lookup ladder** — 8 sites, identical 7-line block. (v2 L-Q7, this cycle L-Q3-9.) Not a bug; pattern is grep-friendly while audit campaign is hot.
- **`ParseJsonStrict` boilerplate** — 11 sites, identical 3-line block. Same shape. Not yet a bug.
- **`HandlePull` / `HandleMultiPull` event-build** — most painful duplication in service. M-Q3-3 above.
- **`ProgressionHandlers` action-pair duplication** — `HandleLevelCharacter` / `HandleAscendCharacter` and `HandleLevelWeapon` / `HandleAscendWeapon` are byte-identical at the structural level (pre-check → idempotency check → entity-lookup → mutation → `CommitProgressionScrapSpend` call). The 4 handlers add up to ~360 LOC; a generic `HandleEntityProgression<EntityType>` template could collapse to ~150. Deferred — type-genericity costs readability, and these four handlers are stable.
- **`DispatchCurrencyReward` vs `rewardKindFor`** — L-Q3-10 above. Small surface, low drift risk.
- **NEW from M-V2-3** — none. The split was clean.
- **NEW from M-V2-4** — none. The two helpers don't duplicate anything else in the service.

The M16/M17/M18 currency dispatch dedup from v2 holds; `Wallet::GetBy(Currency)` / `AddBy(Currency, int)` are used by both `HandleAddCurrency` and `EffectDispatcher::operator()(GrantCurrencyEffect)` and no third implementation has re-emerged.

## Comment / audit-trail quality

Audit-trail comment density (`grep -c "[Aa]udit\b"`):
- `AccountServer.hpp`: 19 references in 345 lines (5.5%)
- `AccountCache.hpp`: 5 references in 264 lines (1.9%)
- `AccountHydrator.hpp`: 1 reference in 93 lines (1.1%)
- `InternalRpcHandlers.hpp`: 6 references in 199 lines (3.0%)

Signal-to-noise feels healthy. No block is grossly over-commented — the densest cluster is the `AccountServer` ctor where five separate audits influenced the construction order, and the comments there are load-bearing (member-init-order interaction with `m_dummyPasswordHash` and `m_cache`'s ref-bind).

One block that has drifted: the `// (Audit L3 2026-06-02 — ConvertAccountToData removed.)` tombstone at AccountServer.hpp:289-292 sits next to the still-living `SaveAccountToRepository` (L-Q2). A reader sees the tombstone and assumes the file is clean — but the line directly above the tombstone is the still-dead method. Closing L-Q2 fixes the misleading adjacency.

## Header hygiene

| File | Includes used? | Notes |
|---|---|---|
| `AccountServer.hpp` | 8 dead includes | M-Q3-2 above |
| `AccountHydrator.hpp` | All used | clean |
| `AccountCache.hpp` | All used | clean (uses `<utility>` for `std::pair` returns, `<unordered_set>` for `m_pendingCleanup`, etc.) |
| `InternalRpcHandlers.hpp` | `<chrono>` dead | L-Q3-1 above |
| `LruCache.hpp` | All used | clean |
| `Account.hpp` | Missing `<vector>` | L-Q3 carry-over from v2 (L-Q3-4) |

The three new files (AccountHydrator/AccountCache/InternalRpcHandlers/LruCache) are mostly tight. The legacy `AccountServer.hpp` and `Account.hpp` are where the IWYU debt lives.

## Const-correctness sweep

- `AccountCache::Peek` is not present — the cache exposes only mutating accessors (`GetLockedAccount` bumps `m_lastAccess`). Read-only access through the cache layer isn't a current use case; if/when it lands (e.g. an admin/metrics endpoint that wants to inspect a cached account without bumping access time), a `Peek(playerId) const` mirror would fit. Not blocking.
- `LruCache::Peek` correctly diverges from `Get` — Peek does NOT touch the LRU position. RateLimiter's `GetCooldownRemaining` uses Peek; `Allow` uses Get. This distinction is correct and tested.
- The Account class has no const/non-const accessor pair mismatches that I could find — the read-only `GetWallet() const`, `GetCollection() const`, `GetQuestStates() const`, `GetWorldFlags() const`, etc. all have const mirrors. The dirty-mark setters are explicitly non-const.
- One asymmetry: `HandlerContext::getLockedAccount` returns `LockedAccountRef` (which holds a non-const `Account*`). A const variant would be useful for read-only handlers (`HandleGetState`, `HandleGetInventory`) — they currently get a mutable ref and only call const methods. Not a bug; potential improvement.

## LOC trajectory

`scripts/count-loc.ps1` baseline + current:

```
Audit baseline (v1 commit 62e43d3):       Code 86,893  Data 60,466
v2 baseline (commit 8e19666):             Code 87,021  Data 60,466
v3 current   (commit d163b07):            Code 87,290  Data 60,466
```

Server delta over the v3 cycle: **+269 LOC** net.
- `AccountServer.hpp`: 616 → 345 (-271 LOC)
- `AccountHydrator.hpp`: 0 → 93 (+93)
- `AccountCache.hpp`: 0 → 264 (+264)
- `InternalRpcHandlers.hpp`: 0 → 199 (+199)
- `LruCache.hpp`: 0 → 177 (+177)
- `RateLimiter.hpp`: ~210 → 186 (-24)
- `LruCacheTest.cpp`: 0 → 193 (+193, in tests)
- `QuestHandlers.hpp`: 847 → 914 (+67 from helper signatures + helper bodies overhead)
- `Account.hpp`: 533 → 533 (X-macro substitution was roughly neutral)

The growth is structurally beneficial: extractions traded one large file for four small ones, and an inline RateLimiter cache became a reusable, tested type. The v3 cycle is a net win even though Server LOC ticked up.

## Suggested triage order

**Trivial (one commit, ~30 LOC of edits):**
1. **L-Q3-1** — drop dead `<chrono>` in `InternalRpcHandlers.hpp`.
2. **M-Q3-2** — drop 8 dead includes in `AccountServer.hpp`.
3. **L-Q3-4** — close v2 L-Q1 (`<algorithm>`), L-Q2 (`SaveAccountToRepository`), L-Q3 (`<vector>` IWYU).
4. **L-Q3-7** — drop empty "Idle Account Eviction" section divider.

All four trivials roll into a single `chore(account): include + dead-code micro-sweep` commit.

**Defer until next M-class cycle:**
5. **M-Q3-1** — token-witness for `AccountCache` escape hatches when a second consumer lands.
6. **M-Q3-3** — `HandlePull` / `HandleMultiPull` extraction. Pattern proven by M-V2-4; targets the most painful duplication in the service.

**Observations only (no action):**
7. L-Q3-2, L-Q3-3, L-Q3-5, L-Q3-6, L-Q3-8, L-Q3-9, L-Q3-10, L-Q3-11.

## Verdict

The v2 cycle's structural-debt cluster is closed. **AccountServer.hpp dropped from 683 → 345 lines** through three crisply-scoped extractions; **the Snapshot drift surface is collapsed to a single X-macro**; **HandleClaimQuestReward is half its previous length and reads top-to-bottom**; **LruCache is a small, well-tested, reusable type** that retroactively justifies its existence. The Account service's code-quality posture is materially better than at v2 baseline.

No critical or high concerns in this dimension for v3. The Medium findings are structural-debt observations (escape-hatch contract, dead include cluster) rather than live bugs; the Lows are all one-line edits clustered into a single micro-sweep commit.
