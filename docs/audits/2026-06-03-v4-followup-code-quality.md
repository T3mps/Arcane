# v4 Follow-up Audit: Code Quality + Complexity

**Date:** 2026-06-03
**Auditor:** v4 (post-v3 remediation arc, single-dimension sweep)
**Scope:** Verify v3 remediation in code quality dimension; surface drift introduced by the 26-commit v3 arc; classify any new debt.
**Baseline:** `2026-06-03-v3-followup-code-quality.md` (v3) and `2026-06-03-server-persistence-audit-v3.md` (synthesis).
**Method:** Direct read of `AccountServer.hpp`, `AccountCache.hpp`, `Account.hpp`, `GachaHandlers.hpp` (HandlePull + HandleMultiPull), `QuestHandlers.hpp` (HandleClaimQuestReward + helpers), `LruCache.hpp`; grep sweeps for dead code, magic numbers, TODO markers, dup patterns.

## Verdict

The v3 arc closed the v3 dimension cleanly on the new-finding axis — H-V3-7/8/9/10/11 fixes are visible (`~AccountServer() override { Stop(); }`, `[[nodiscard]]` on `LockFor`, `try_emplace` in `InsertIfAbsent` and `LruCache::Put`, mutable `Peek` + explicit `Touch` in `LruCache`, function-local-static dummy hash via L-Q3-1 chrono drop). The Snapshot X-macro and AccountCache extraction holds. The biggest carry-forward concern is **M-Q3-2 is only partially closed**: 5 of the 8 dead `<algorithm>/<chrono>/<mutex>/<unordered_set>/<vector>` includes flagged in v3 are still present in `AccountServer.hpp:42-50` despite the M-Q3-2 fix comment at the top of the file claiming the cleanup happened. The other carry-forwards are M-Q3-3 (HandlePull/HandleMultiPull duplication, explicitly deferred — verified ~80% overlap is unchanged, still the largest cleanup target in the service) and the unchanged Low cluster L-Q3-2/3/4/6/8/9/10/11. No criticals, no highs.

## CRITICAL

*None.*

## HIGH

*None.*

## MEDIUM

### M-V4-1. M-Q3-2 dead-include cleanup landed only partially

**File:** `Server/Account/src/AccountServer.hpp:42-50`
**Status:** CARRY-FORWARD from v3 M-Q3-2; partially regressed by the M-Q3-2 fix comment claim.

The v3 M-Q3-2 fix comment at the top of `AccountServer.hpp:3-6` reads:

> "Audit M-Q3-2 (2026-06-03): pared back dead includes after M-V2-3 extracted AccountHydrator / AccountCache / InternalRpcHandlers. AccountData, TickQuests, StripedMutex are now owned by those extracted units; AccountServer.hpp doesn't reference any of them."

The three **named** includes (`AccountData.hpp`, `TickQuests.hpp`, `StripedMutex.hpp`) are indeed gone. But v3 flagged ten dead includes; the standard-library ones from `<algorithm>` through `<vector>` at lines 42, 43, 46, 49, 50 are still present and still unused:

```cpp
#include <algorithm>       // line 42 — flagged in v2 L-Q1, v3 L-Q3-4 & M-Q3-2; still here
#include <chrono>          // line 43 — flagged in v3 M-Q3-2; still here
#include <mutex>           // line 46 — flagged in v3 M-Q3-2; still here
#include <unordered_set>   // line 49 — flagged in v3 M-Q3-2; still here
#include <vector>          // line 50 — flagged in v3 M-Q3-2; still here
```

I verified by grep: zero in-file references to `std::sort`, `std::find`, `std::chrono::`, `std::mutex`, `std::lock_guard`, `std::unique_lock`, `std::unordered_set`, `std::vector` in `AccountServer.hpp`. The only `std::` calls present are `std::move(dbConn)` (ctor init) and `std::getenv`. The transitive-include path keeps the build green; the contract still lies.

**Fix (mechanical, ~5 LOC):** delete lines 42, 43, 46, 49, 50. Same single-commit cleanup that v3 recommended.

### M-V4-2. `SaveAccountToRepository` is still the dead method v3 L-Q3-4 flagged

**File:** `Server/Account/src/AccountServer.hpp:324-327`
**Status:** CARRY-FORWARD from v3 L-Q3-4 (which was already v2 L-Q2); promoted to M because the v3 cycle explicitly named it in the "trivial / one commit" triage list and it didn't land.

```cpp
void SaveAccountToRepository(Account& account)
{
    m_repository.Save(account);
}
```

Verified zero callers via grep (`Grep` against the full `Server/` tree returns only the declaration site). Sitting right next to the `(Audit L3 2026-06-02 — ConvertAccountToData removed.)` tombstone at line 329-332; together they form exactly the misleading adjacency v3 called out — reader sees the tombstone, assumes the file is clean, fails to notice the still-dead method one block away.

**Fix:** delete lines 324-327. Trivial.

### M-V4-3. HandlePull / HandleMultiPull duplication still the largest cleanup target

**Files:** `Server/Account/src/Handlers/GachaHandlers.hpp:56-305` (HandlePull, 250 lines), `:307-639` (HandleMultiPull, 333 lines).
**Status:** CARRY-FORWARD from v3 M-Q3-3; explicitly deferred.

LOC unchanged from v3 (verified by Read + wc -l of GachaHandlers.hpp = 708 lines total vs v3's reported 710). The C-V3-2 fix (move `Begin()` above GetPity/GetGuarantee) landed in both handlers symmetrically and did not introduce new drift between them; the duplication remains shape-for-shape parallel. The five structurally-identical phases are still present in both handlers:

1. Strict parse + clientKey extraction (lines 60-65 / 310-316).
2. Lock + idempotency check + cache lookup (66-89 / 318-336).
3. Rate limit + banner / slot / pity-config / wallet-affordance validation (91-115 / 338-362).
4. Begin txn + snapshot pre-state + RNG/pity/wallet capture (117-141 / 364-401).
5. Pull execution + collection mutation + event building + commit (143-302 / 403-636).

The duplication is acknowledged design debt deferred from v3, NOT a new finding. Keeping the entry so v5 has a stable carry-forward token.

**Fix (deferred):** when the next M-class cycle happens, extract `PullEngine::ExecuteSinglePull` + `PullEngine::ExecuteBundle` along the M-V2-4 helper pattern. Estimated reduction: ~280 LOC.

### M-V4-4. `Account.hpp` still missing `<vector>` direct include

**File:** `Server/Account/src/State/Account.hpp:3-19`
**Status:** CARRY-FORWARD from v2 L-Q3 / v3 L-Q3-4.

Account.hpp uses `std::vector<ProgressionConfig::StoryProgression::LevelReward>` at the line v3 cited (around 363); the include list (lines 3-19) has `<array>`, `<cstdint>`, `<ctime>`, `<string>`, `<unordered_map>` but no `<vector>`. Works only because `ProgressionConfig.hpp` transitively includes it; fragile to upstream pruning. Same IWYU debt v3 flagged.

**Fix:** add `#include <vector>` to the standard-library include block. One line.

## LOW

### L-V4-1. `AccountServer.hpp:299-303` carries two adjacent empty section dividers

**File:** `Server/Account/src/AccountServer.hpp:298-303`
**Status:** CARRY-FORWARD from v3 L-Q3-7.

```cpp
// ============================================================================
// Idle Account Eviction
// ============================================================================

// ============================================================================
// Account Access (two-lock protocol: stripe → map)
// ============================================================================
```

The "Idle Account Eviction" section header has zero content beneath it (the body moved to `AccountCache::CleanupIdleAccounts` per M-V2-3). v3 flagged this; not closed. Misleading reader hint.

**Fix:** delete lines 298-301.

### L-V4-2. `claimed_at_streak_day` field name still ambiguous

**File:** `Server/Account/src/Events/QuestClaimEvents.hpp:28, 57, 88`
**Status:** CARRY-FORWARD from v2 L-V2-13 / v3 L-Q3-8.

`claimed_at_streak_day` stores a streak *count* (an int counter, not a calendar day). v3 noted; not closed. Test fixture at `tests/events/v1_quest_reward_claimed.json` and golden test at `tests/GoldenFile/SchemaMigrationTest.cpp:104` reinforce the name — making the rename a slightly larger sweep (4 sites in the tree per grep). Still trivially closable.

**Fix:** rename to `streak_count_at_claim` across the 5 hit sites OR add a one-line doc comment on the field declaration: `// streak count at time of claim; NOT a calendar day index`.

### L-V4-3. `LruCache::LruKey()` still has no production consumer

**File:** `Server/Common/src/Util/LruCache.hpp:176-180`
**Status:** CARRY-FORWARD from v3 L-Q3-3 / L-Q3-6.

Diagnostic accessor exercised only in `LruCacheTest.cpp`. v3 flagged; not closed. Either delete (YAGNI) or add an in-comment "not currently consumed, diagnostic-only" tag. Leaving as-is is fine; tracking so the entry doesn't fall through future audit cracks.

### L-V4-4. `ParseJsonStrict` boilerplate at 11 sites; `FindIdempotency` ladder at 8 sites — unchanged

**Files:** spread across `GachaHandlers.hpp` (4 ParseJsonStrict), `ProgressionHandlers.hpp` (8), `QuestHandlers.hpp` (7), `AccountHandlers.hpp` (5).
**Status:** CARRY-FORWARD from v2 L-Q7 / v3 L-Q3-9.

Pattern count verified by grep (counts above are call-site counts including idempotency-lookup tri-state block). v3 said defer until a real drift case surfaces; no drift case surfaced this cycle, so defer remains correct. Still defensible. The shape is grep-friendly which has real value while audits are hot.

### L-V4-5. `rewardKindFor` lambda still duplicates `DispatchCurrencyReward`'s currency mapping

**File:** `Server/Account/src/Handlers/QuestHandlers.hpp:430-438` (lambda) vs `DispatchCurrencyReward` (separately defined in QuestHandlers as well).
**Status:** CARRY-FORWARD from v3 L-Q3-10.

Two parallel if-else chains over the 5 currency string IDs. Adding a 6th currency means editing both. v3 deferred until a 6th currency lands. No new currency added this cycle. Defer remains correct.

### L-V4-6. AccountCache escape-hatch contract still documented-not-enforced

**File:** `Server/Account/src/Cache/AccountCache.hpp:233-291`
**Status:** CARRY-FORWARD from v3 M-Q3-1 / H-V3-8; H-V3-8 closed *the [[nodiscard]] half only*.

H-V3-8 added `[[nodiscard]]` to `LockFor`, closing the discarded-return-value misuse vector. The deeper structural concern from v3 M-Q3-1 — that `InsertIfAbsent` / `UpdateCachedPasswordHash` take their own `m_mapMutex` internally but don't verify the caller holds the matching stripe lock — is unchanged. The H-V3-8 commit was visibly scoped to `[[nodiscard]]` only (per its v3 entry "H-V3-8 only addressed LockFor [[nodiscard]]"). The contract is enforceable by a `StripeLockToken` type-witness parameter but the v3 cycle deliberately did not invest there.

Demoting from M to L because the only caller (`InternalRpcHandlers::HandleVerifyCredentials`) does follow the protocol and no second consumer has emerged. Track as drift surface for the next consumer.

### L-V4-7. M-V2-2 X-macro vs inline member-decl spelling still mixed

**File:** `Server/Account/src/State/Account.hpp` (member-decl block vs X-macro)
**Status:** CARRY-FORWARD from v3 L-Q3-2 (OBSERVATION).

The X-macro at lines ~96-116 uses type aliases (`PityTrackerMap`, `GuaranteeTrackerMap`, `PartyArray`, etc.); the member declarations still spell several inline (`std::unordered_map<std::string, PityTracker>`, `std::array<std::string, 4>`). Cosmetic; X-macro and decls stay in sync via the `using` aliases. Same observation v3 made; same trivial fix.

### L-V4-8. Snapshot doc-comment ↔ Rollback location asymmetry

**File:** `Server/Account/src/State/Account.hpp:82-84` (Snapshot doc) vs `AccountTransaction.hpp` (Rollback's stale-mark)
**Status:** CARRY-FORWARD from v3 L-Q3-3.

The `m_stale` exclusion lives on the Snapshot doc-comment but the actual post-restore stale mark fires from `Rollback`. v3 flagged for symmetry; unchanged.

### L-V4-9. `AccountCache.hpp` mojibake from box-drawing characters

**File:** `Server/Account/src/Cache/AccountCache.hpp` (multiple sites — line 3, 9, 16, 17, 19, 137, 139, 199, 233, 239)
**Status:** NEW (low impact).

The comment lines containing what should be Unicode em-dash `—` and box-drawing characters render in my Read tool output as `â€”`, `â€"`, etc. — i.e. UTF-8 bytes are being interpreted as Latin-1 somewhere in the toolchain (editor save, or original source encoding mis-set). Same pattern appears in `AccountServer.hpp` ctor comments, `GachaHandlers.hpp` section dividers (lines 129, 134, 195, 200, 532, 542), `QuestHandlers.hpp`, and others. The C++ compiler treats the bytes as raw chars in comments so it builds fine; the rendered text is non-ASCII garbage.

Not a code defect, but a code-quality / readability concern that will compound: every future audit / code review will see the same garbled comments. Future replacements via `sed`/`Edit` will need to match the byte sequence the tool wrote, not the intended em-dash.

**Fix (defer):** set the editor / VS save encoding to UTF-8-with-BOM (or UTF-8 no BOM but with an `.editorconfig` `charset = utf-8` directive); then a one-time `iconv` or PowerShell `-Encoding utf8` resave fixes the existing files. Until then, future audits should be aware that string-match edits on these comment blocks need to round-trip the mojibake.

### L-V4-10. Audit-trail comment density getting heavy in AccountCache

**File:** `Server/Account/src/Cache/AccountCache.hpp`
**Status:** OBSERVATION.

AccountCache.hpp at 305 lines has 11 "Audit" tags (3.6%), vs v3's reported 1.9%. The increase comes from V3-L2, V3-L3, V3-L4, M-V3-1, H-V3-8, H-V3-9 batch (all genuinely load-bearing — each one annotates a subtle invariant). Density is high but signal/noise still healthy. Mention only so future cycles know to start watching this file — if a v5 cycle adds another 4 audit tags here, consider migrating some to a sibling `AccountCache_DESIGN.md` rather than expanding the in-line comment surface.

### L-V4-11. `Account.hpp:550` literal "TODO" in a comment

**File:** `Server/Account/src/State/Account.hpp:550`
**Status:** NEW (low impact; observation).

```
// existing setters. (Audit L5 2026-06-02 — retired the stale "Phase 7
// TODO" note that this hadn't been done yet.)
```

The word `TODO` appears inside a quoted retrospective referring to a *previously deleted* TODO note. Grep tools that count `TODO` markers will flag this site as a live TODO; it is not — it's an audit tombstone. A reader doing a TODO sweep gets a false positive.

**Fix:** lowercase the literal: `... retired the stale "Phase 7 to-do" note ...` or `"Phase 7 deferred-work" note`. One-character edit avoids the false-positive sweep hit.

## Verified Closed from v3

These v3 items show clear, verifiable fixes in the current code state:

- **H-V3-7** — `~AccountServer() override { Stop(); }` at `AccountServer.hpp:126` joins the detached endpoint threads before m_cache is destroyed. UAF window closed.
- **H-V3-8** (partial) — `[[nodiscard]]` decorator at `AccountCache.hpp:247`. The discarded-return misuse vector is closed. Deeper "caller must hold stripe lock" contract is *not* compiler-enforced (see L-V4-6).
- **H-V3-9** — `try_emplace` at `AccountCache.hpp:273` (InsertIfAbsent) and at `LruCache.hpp:129` (Put). Empty-default-then-move-assign window is closed.
- **H-V3-10** — Function-local-static dummy PBKDF2 hash moved into `HandleVerifyCredentials` (per AccountServer.hpp:106-110 commentary). Startup ordering coupling gone.
- **H-V3-11** — Mutable `Peek` + explicit `Touch` at `LruCache.hpp:86-103`. RateLimiter inversion fixable at the consumer.
- **L-Q3-1** — `<chrono>` dead include in `InternalRpcHandlers.hpp` dropped (verified by grep — no `<chrono>` line, no `std::chrono::` usage).
- **L-Q3-7** / **L-Q3-11** — stale comment scrubs visible in AccountServer.hpp ctor block; M-V2-3 explanations refined (lines 69-80).
- **L-V3-3** / **L-V3-5** — event-sourcing observation comments added per the docs+chore commit.
- **L-V3-6** — snapshot density forward-link added (per the d87b457 commit).
- **C-V3-2** — Begin() moved above GetPity/GetGuarantee in both HandlePull (line 124) and HandleMultiPull (line 369). Lazy-mutating accessor window closed.
- **C-V3-1** — `ProgressionReducer::ProgressionState::difficulty_tier = 0` alignment (per the 8c53a34 cross-file traceability commit).
- **H-V3-2** — `pg_advisory_xact_lock` moved into `EnsureOpen`.
- **H-V3-4 / H-V3-5 / H-V3-6** — outbox sweeper + idempotency_cache sweeper + wallet CHECK constraints landed.
- **H-V3-13** — CommonTests / AuthTests / CombatTests premake projects added.
- **H-V3-14** — `ServiceClient::Call → RpcCallResult` + envelope version field + startup canary probe.

## Suggested triage order

**Trivial sweep (single commit, ~15 LOC of deletions):**
1. **M-V4-1** — drop 5 dead std-lib includes from AccountServer.hpp.
2. **M-V4-2** — drop dead SaveAccountToRepository (4 lines).
3. **L-V4-1** — drop empty "Idle Account Eviction" section divider (4 lines).
4. **M-V4-4** — add `#include <vector>` to Account.hpp (1 line).
5. **L-V4-11** — lowercase the literal "TODO" in Account.hpp:550 tombstone.

**Defer to next M-class cycle:**
6. **M-V4-3** — HandlePull/HandleMultiPull extraction (the big one — ~280 LOC reduction available).
7. **L-V4-6** — StripeLockToken witness type once a second `LockFor` consumer lands.
8. **L-V4-9** — UTF-8 re-encoding sweep for mojibake.

**Observations only:** L-V4-2, L-V4-3, L-V4-4, L-V4-5, L-V4-7, L-V4-8, L-V4-10.
