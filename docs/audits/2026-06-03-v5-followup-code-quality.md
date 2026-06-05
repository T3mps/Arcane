# v5 Follow-up Audit: Code Quality + Complexity

**Date:** 2026-06-03 (same-day successor to v4)
**Auditor:** v5 (post-v4 20-commit remediation arc)
**Scope:** Verify v4 code-quality remediation; surface drift introduced by the 20-commit v4 arc; classify new debt.
**Baseline:** `2026-06-03-v4-followup-code-quality.md` (v4 prior).
**Method:** Direct verification of e30ea9e (M-V4-1/M-V4-2/M-V4-4/L-V4-1) and d65dbdf (L-V4-2/L-V4-11/L-V4-7), plus full re-read of `AccountServer.hpp`, `Account.hpp`, `GachaHandlers.hpp` (HandlePull + HandleMultiPull), `QuestHandlers.hpp` (HandleClaimQuestReward), `LruCache.hpp`, `OutboxRelay.hpp`, `ServiceClient.hpp`, `SessionCache.hpp`, `TcpServerBase.hpp`, all three service `main.cpp` files; grep sweeps for dead variables, mojibake, IWYU debt, audit-tag density.

## Verdict

**Clean.** Every v4 code-quality item the remediation arc claimed is materially closed in code (trust-but-verified, not just commit-message-trusted). The five `<algorithm>/<chrono>/<mutex>/<unordered_set>/<vector>` dead includes on `AccountServer.hpp` are gone (e30ea9e:42-50 deleted, replaced with a documented-pruning comment block); `SaveAccountToRepository` is gone (e30ea9e:324-327 deleted, tombstone comment in its place); the orphan "Idle Account Eviction" section divider is gone (e30ea9e); `<vector>` is now a direct include in `Account.hpp:12` (e30ea9e:7-12); `claimed_at_streak_day` has the disambiguation doc-comment (d65dbdf:28-37); the literal "TODO" in `Account.hpp:556` is lowercased to "to-do" (d65dbdf). LruCache's `Put` fresh-insert path is `try_emplace` (d87b457). All five v4-claimed Medium items and the v4 Lows close cleanly. Two genuinely-new code-quality concerns surfaced, both Medium-low: a dead-variable in `Server/Account/src/main.cpp` (`portOverride` set but never read — Auth's main.cpp uses it, Account's drops the override on the floor and defaults to 7771) and a tombstone-of-a-tombstone in `AccountServer.hpp:326-346` (M-V4-2's deletion comment now sits immediately above L3-2026-06-02's ConvertAccountToData tombstone, with zero live code between them — the misleading-adjacency L-Q3 v3 first flagged got worse, not better). The biggest carry-forward concern is the same `HandlePull` / `HandleMultiPull` ~80% duplication at 708 LOC (M-V4-3 carry-forward); no drift between the two from the v4 arc's edits, but the deferred-refactor token persists. No criticals, no highs.

## CRITICAL

*None.*

## HIGH

*None.*

## MEDIUM

### M-V5-1. `Server/Account/src/main.cpp` — `portOverride` is a dead variable

**File:** `Server/Account/src/main.cpp:99, 111`
**Status:** NEW (drift between Account and Auth main.cpp)

```cpp
uint16_t port = 7771;
bool portOverride = false;     // <-- set but never read
...
port = static_cast<uint16_t>(std::stoi(argv[++i]));
portOverride = true;            // <-- set but never read
```

Verified by grep: `portOverride` appears only at the declaration site (99) and the inside-the-if-branch assignment (111). Account's main never reads it. The construction at line 231-232 passes `port` unconditionally:

```cpp
Aphelyon::AccountServer server(port, /*authInternalPort=*/7770,
                               /*internalPort=*/7773, dbConn);
```

Compare to `Server/Auth/src/main.cpp:132-135`, where `portOverride` is actually read to choose between the CLI arg and `protocol.GetSettings().defaultPort`:

```cpp
if (!portOverride)
    port = static_cast<uint16_t>(protocol.GetSettings().defaultPort);
```

So Account either:
- (a) should drop `portOverride` and the default `7771` is the single source of truth, or
- (b) should mirror Auth's pattern and let `protocol.json`'s `defaultPort` win when no CLI override.

Today neither happens. The variable signals an intent that doesn't exist in the code.

**Fix:** delete `bool portOverride = false;` and the `portOverride = true;` line. ~2-line cleanup. Mirroring Auth's `defaultPort` path is a separate decision (would be a behavior change, not a cleanup).

### M-V5-2. `AccountServer.hpp:326-346` — tombstone-of-a-tombstone forms in the persistence section

**File:** `Server/Account/src/AccountServer.hpp:326-346`
**Status:** NEW (worsening of L-Q3 v3 / L-V4-1 carry-forward; M-V4-2's deletion landed but the deletion-comment doubled the misleading-adjacency surface area)

After the M-V4-2 deletion (e30ea9e), the "Persistence" section reads:

```cpp
// ============================================================================
// Persistence
// ============================================================================

// Audit M-V2-3 (2026-06-03): LoadAccountFromData moved to
// AccountHydrator::FromData; ... No state was lost in the extraction —
// the body is identical, just on the other side of a header.
//
// Audit M-V4-2 code-quality (2026-06-03): the SaveAccountToRepository
// shim was a verified-zero-caller wrapper around m_repository.Save
// — direct callers go through AccountTransaction or m_cache. ...
// honest. The neighboring ConvertAccountToData tombstone stays as
// a forward-reading hint.

// (Audit L3 2026-06-02 — ConvertAccountToData removed. ...)

// ============================================================================
// Member Variables
// ============================================================================
```

Three tombstones (M-V2-3 extraction, M-V4-2 SaveAccountToRepository deletion, L3 ConvertAccountToData deletion) stacked under a "Persistence" header with **zero live code beneath the header**. The v3 L-Q3-7 / v4 L-V4-1 observation about misleading section dividers ("reader sees a header, expects a method below, finds nothing") now applies to the **entire persistence section**, not just the one orphan header that e30ea9e fixed.

The v4 fix-comment for M-V4-2 (lines 336-341, 6 comment lines explaining a single 4-line deletion) is itself the kind of comment-debt the audit campaign worries about — the deletion is documented by:
1. The git commit message (e30ea9e),
2. The audit document (this dimension, v4),
3. The grep-able audit tag (`M-V4-2 code-quality`),
4. The inline tombstone (lines 336-341).

Four channels for the same fact. Three would be enough.

**Fix (recommend):** delete the entire "Persistence" section header + the two stacked tombstones at lines 326-346. The git history + audit doc + grep on `M-V4-2` are sufficient. If preserving institutional memory is preferred, collapse all three tombstones into a single 2-line "Persistence orchestration moved to AccountTransaction + AccountCache; see git log for the extraction history" comment. Either way the empty-section surface shrinks.

### M-V5-3. `OutboxRelay.hpp` uses `std::condition_variable` without `<condition_variable>` include

**File:** `Server/Account/src/Db/OutboxRelay.hpp:208`
**Status:** NEW (introduced by C-V4-1's wire-up — the relay grew a CV for the wake-on-Stop path)

```cpp
std::condition_variable wakeCv_;
```

The `wakeCv_` member at line 208 is used at lines 48 (`wakeCv_.notify_all()` in `Stop`), 60 (`Kick`), and 107-108 (`wakeCv_.wait_for(...)` in `Run`). No `#include <condition_variable>` appears in the file's include block (lines 1-14). Compiles green via the transitive include from `<thread>` (line 13) or `<mutex>` (line 10) on MSVC, but the contract is wrong — same IWYU shape v3 M-Q3-2 / v4 M-V4-1 flagged for `AccountServer.hpp` and v4 M-V4-4 closed for `Account.hpp`.

This is a v4-cycle-introduced regression: pre-C-V4-1 the file had no CV. The C-V4-1 fix (f24b3ca, which I don't need to re-read because the diff shape is "instantiate the relay") didn't itself touch OutboxRelay.hpp's includes, but the file's CV usage is now load-bearing for the Stop/join path that v4 needed to wire up.

**Fix:** add `#include <condition_variable>` to the include block. One line.

## LOW / OBSERVATION

### L-V5-1. `HandlePull` / `HandleMultiPull` carry an unused `clientIP` parameter

**File:** `Server/Account/src/Handlers/GachaHandlers.hpp:56, 307`
**Status:** OBSERVATION

Both handler signatures declare `const std::string& clientIP` but neither reads it. The MessageDispatcher hands it down to every handler uniformly; `HandleAddCurrency` is the only handler in the file cluster that actually uses it (for the per-IP rate limiter). The Pull handlers rate-limit on `playerId`, not `clientIP`, so the parameter is a no-op.

Not new — the parameter has always been there for signature uniformity with the dispatcher's contract. Tracked now because the v4 dimension closed the easy code-quality items and this kind of "uniform-signature unused-arg" is the noise floor that surfaces. Either silence with `const std::string& /*clientIP*/` (preferred — keeps the signature uniformity but documents the no-use) or sweep all five handler files at once. The same shape appears in:
- `HandleSetParty` (no clientIP param — already excluded)
- `HandleReportQuestProgress` (no clientIP param — already excluded)
- `HandleCompleteQuest` (no clientIP param — already excluded)
- `HandleClaimQuestReward` (no clientIP param — already excluded)

So only the two GachaHandlers entries and `HandleAddCurrency` use the uniform 3-arg signature. The split is intentional but inconsistent — three handlers take 3 args, the rest take 2. Either pattern is fine; mixing them creates per-handler inconsistency that audit eyes will catch.

**Fix (defer):** when a new audit cycle promotes "noexcept / const-correctness / unused-arg sweep" to a real workstream, comment out the param name in the Pull handlers. One-line edit per site.

### L-V5-2. `Account.hpp` mojibake from prior episodes remains

**File:** `Server/Account/src/State/Account.hpp:554, 556`, `Server/Account/src/AccountServer.hpp:308, 315, 318, 333, 343`, all of `Server/Account/src/Cache/AccountCache.hpp` comment headers, `Server/Account/src/Handlers/GachaHandlers.hpp:129, 195, 200, 390, 395`, `Server/Common/src/Net/TcpServerBase.hpp:205, 229, 239, 258, 281, 288, 346, 391, 493, 532, 572`
**Status:** CARRY-FORWARD from v4 L-V4-9

The em-dash (`—` rendering as `â€"`/`â€”`) and box-drawing characters (rendering as `â”€`/`â—`) remain across the AccountServer / AccountCache / GachaHandlers / TcpServerBase comment surface. The v4 audit flagged this as "deferred — set editor save encoding to UTF-8 then iconv-sweep." No sweep happened in the v4 arc. Each new audit-tag comment the v4 arc added (e30ea9e adds two, d65dbdf adds one, the bracketed `M-V4-X` tags add another five) uses ASCII em-dash variants (`--` or no em-dash), so the **new** comments are clean — the surface keeps growing the clean-comment-percentage but the mojibake floor stays. Pure aesthetic + future-`sed` hazard, no functional impact.

**Fix (defer):** still defer. The mojibake doesn't compound; new comments are clean.

### L-V5-3. Audit-tag density on `AccountServer.hpp` climbed from 5.5% (v3) → 7.0% (v5)

**File:** `Server/Account/src/AccountServer.hpp`
**Status:** OBSERVATION (early-warning signal, no action yet)

Audit-tag counts (grep `[Aa]udit\b`):
- v3 (commit d163b07): 19 references in 345 lines = 5.5%
- v5 (HEAD): 28 references in 398 lines = 7.0%

The +9 tags in the v4 arc:
- `M-V4-1 code-quality` (the dead-include cleanup comment, lines 43-49)
- `M-V4-2 code-quality` (the SaveAccountToRepository tombstone, lines 336-341)
- `C-V4-1` (the OutboxRelay wire-up, lines 71-78 + 360-364)
- `M-V4-4 cross-cutting` (the probe timeout, lines 184-188)

Most are load-bearing — they document non-obvious construction order or deletion-vs-grep collision. The 7% density still feels signal-heavy, not noise-heavy. But the trajectory is up four cycles in a row (v1 baseline 0.5% → v2 2.1% → v3 5.5% → v5 7.0%). The v4 L-V4-10 warned about AccountCache crossing this threshold first; AccountServer is the actual current leader.

The early-warning threshold the v4 doc suggested ("if a v5 cycle adds another 4 audit tags here, consider migrating some to a sibling `_DESIGN.md` rather than expanding the in-line comment surface") was 4 tags in AccountCache — AccountCache held flat (still 11 tags, 305 lines, 3.6%). AccountServer added 9. Watch this file.

**Fix (defer):** if v6 adds another 5 tags here, extract `AccountServer_DESIGN.md` for the higher-level construction-order / extraction-history narrative, leave only the grep-able `Audit <ID>:` markers in code. M-V5-2's collapse recommendation would also reduce 6 of these tags in one stroke.

### L-V5-4. `AccountServer.hpp` line 96 — handler-context construction quietly relies on a four-deep member-init-order chain

**File:** `Server/Account/src/AccountServer.hpp:95-102`
**Status:** OBSERVATION

```cpp
, m_ctx{
    m_database, m_templateDb, m_questLoader, m_questTokenSecret, m_banners,
    m_pullLimiter, m_currencyLimiter,
    ...
}
```

`m_ctx` binds refs to 7 prior members (m_database, m_templateDb, m_questLoader, m_questTokenSecret, m_banners, m_pullLimiter, m_currencyLimiter). The order matters — flipping any two would silently break construction. The member-decl block at lines 352-395 carries the prescribed order but only as a layout convention; the language doesn't enforce that the m_ctx initializer's binding sources are already constructed (it does because they're in earlier declaration positions, but a future reorder of the decl block would silently swap construction order without breaking the initializer-list textually).

Pure shape observation. No action recommended today; flagged as the kind of structural-debt to extract a `HandlerContext::Builder` for if/when a sixth ref-bound dependency lands.

### L-V5-5. v5 carry-forwards from v4 (verified-open)

These v4 entries verified unchanged in the v5 sweep:

- **M-V4-3** (HandlePull / HandleMultiPull duplication, 708 LOC total): `wc -l` confirms 708 LOC. The v4 arc edited both handlers symmetrically (the only diffs from e30ea9e..HEAD touching GachaHandlers.hpp are the `IdempotencyKey::ExtractClientKey` swap at lines 65 + 316 — symmetric, no drift introduced).
- **L-V4-3** (LruCache::LruKey has no production consumer): unchanged. Test-only; v3 L-Q3-6 carry-forward.
- **L-V4-4** (ParseJsonStrict + FindIdempotency ladder boilerplate): unchanged count. Pattern still grep-friendly; no drift case surfaced.
- **L-V4-5** (`rewardKindFor` lambda vs `DispatchCurrencyReward`): unchanged. No new currency landed.
- **L-V4-6** (AccountCache escape-hatch contract documented-not-enforced): unchanged. Single consumer (InternalRpcHandlers::HandleVerifyCredentials).
- **L-V4-7** (X-macro vs inline member-decl spelling mixed in Account.hpp): unchanged. Cosmetic.
- **L-V4-8** (Snapshot doc ↔ Rollback location asymmetry): unchanged.
- **L-V4-10** (AccountCache audit-tag density): unchanged at 11 tags / 305 lines / 3.6%. Threshold *not* crossed; AccountServer.hpp took the lead instead (L-V5-3).

## Verified Closed from v4

- **M-V4-1** — `<algorithm>`, `<chrono>`, `<mutex>`, `<unordered_set>`, `<vector>` dead includes deleted from `AccountServer.hpp:42-50` (e30ea9e). The lines no longer exist; lines 43-49 carry a `M-V4-1 code-quality` audit comment explaining the cleanup. Grep verifies zero `std::sort` / `std::find` / `std::chrono::` / `std::mutex` / `std::lock_guard` / `std::unique_lock` / `std::unordered_set` / `std::vector` usage in the file.
- **M-V4-2** — `SaveAccountToRepository` deleted (e30ea9e). Verified by grep: zero matches for `SaveAccountToRepository` anywhere in `Server/`. The function body that lived at `AccountServer.hpp:324-327` is gone; lines 336-341 carry a tombstone comment (which itself becomes M-V5-2 below).
- **M-V4-3** — explicitly deferred per v4 triage. Verified unchanged at 708 LOC and no drift between Pull/MultiPull from the v4 arc's edits.
- **M-V4-4** — `#include <vector>` added to `Account.hpp:12` (e30ea9e). Verified directly.
- **L-V4-1** — orphan "Idle Account Eviction" section header deleted from `AccountServer.hpp` (e30ea9e). Verified directly — only the "Account Access (two-lock protocol: stripe → map)" header remains at line 314.
- **L-V4-2** — `claimed_at_streak_day` doc-comment added at `QuestClaimEvents.hpp:28-37` (d65dbdf). Rename deferred per the comment (wire-name pinned by v1 fixture + SchemaMigrationTest). Acceptable resolution.
- **L-V4-11** — `Account.hpp:550` lowercased: was "Phase 7 TODO" inside a tombstone, now reads "Phase 7 to-do" (d65dbdf). Grep-for-TODO no longer flags this site.

## v3 → v4 → v5 trajectory summary

The trivial-sweep cluster v4 promised landed cleanly in one batch commit (e30ea9e for 4 items + d65dbdf for 3 items, ~25 LOC of edits across 4 files). The two genuinely new items are both Medium (M-V5-1 dead variable in Account/main.cpp, M-V5-2 tombstone-of-a-tombstone) and both trivially closable (~5 LOC of deletions total). M-V5-3 is a 1-line `#include <condition_variable>` add.

LOC trajectory:
- `AccountServer.hpp`: v3=345 → v4=345 → v5=398. The +53 LOC from v4→v5 is mostly the M-V4-1 explanatory comment block (+7), the M-V4-2 tombstone (+8), the C-V4-1 OutboxRelay wire-up (+12 with comments), the M-V4-4 probe-budget comment (+4), plus the v4 arc's other audit tags. Comment growth, not code growth.
- `GachaHandlers.hpp`: v3=710 → v4=708 → v5=708. Stable.
- `Account.hpp`: v3=533 → v4≈537 → v5=565. Growth split between the M-V4-4 vector-include comment (+5), L-V4-11 lowercase, and unrelated content I didn't audit (the file's grown across v4 from other dimensions).
- `OutboxRelay.hpp`: 0 (v3) → 0 (v4) → 212 (v5, after C-V4-1's wire-up made the class load-bearing). New file participation.

Comment-vs-code growth ratio in `AccountServer.hpp` (v4→v5): +53 LOC, of which ~35 are pure-comment audit explanations and ~18 are the C-V4-1 member-decl + initializer. Code-quality-good (comments document the why), but reinforces L-V5-3 — the audit-tag density is climbing, not flat.

## Suggested triage order

**Trivial sweep (single commit, ~10 LOC of edits):**
1. **M-V5-1** — delete `portOverride` from Account/main.cpp (2 LOC).
2. **M-V5-2** — collapse the persistence-section tombstones (replace ~20 LOC of comment with ~3 LOC, or delete the whole header block entirely).
3. **M-V5-3** — add `#include <condition_variable>` to OutboxRelay.hpp (1 LOC).
4. **L-V5-1** — silence `clientIP` param name in HandlePull / HandleMultiPull (2 LOC).

All four trivials roll into one `chore(server): code-quality micro-sweep (M-V5-1/2/3 + L-V5-1)` commit. Estimated ~8 LOC net change.

**Defer to next M-class cycle:**
5. **M-V4-3** (carry-forward) — HandlePull / HandleMultiPull extraction. Still the largest cleanup target; ~280 LOC reduction available.
6. **L-V5-3** — AccountServer.hpp audit-tag density watch. M-V5-2's collapse buys margin; revisit if v6 adds 5+ more tags.

**Observations only (no action):**
7. L-V5-2 mojibake, L-V5-4 m_ctx member-init chain, L-V5-5 carry-forward verifications.

## Verdict (restated)

Clean. v4's code-quality dimension is genuinely closed. The 20-commit v4 arc introduced **3 Medium / 1 Low** of net-new code-quality debt, all trivially closable in one batch commit, plus the existing M-V4-3 carry-forward. AccountServer.hpp's audit-tag density crossed the v4-flagged early-warning threshold (5.5%→7.0%), but the comment growth is load-bearing not noise — collapsing M-V5-2 reverses the trend by ~6 tags in one stroke. No tracking-the-code-quality-debt-is-now-a-job concerns; the dimension is staying tractable through audit cycles.
