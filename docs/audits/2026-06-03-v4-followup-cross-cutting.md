# Aphelyon Server — v4 Follow-up: Cross-Cutting / Integration / End-to-End

**Date:** 2026-06-03 (same day, ~hours after the v3 remediation arc landed)
**Auditor:** Cross-cutting / integration lens (v4 sweep verifying v3 fixes + finding new drift)
**Method:** Walked the env-var matrix across the three `main.cpp` files; verified the H-V3-14(a/b) envelope-version + startup probe wiring; cross-checked event_type emission against reducer dispatch and golden-file vocabulary; re-read the M-V2-2 / M-V3-2 X-macro Snapshot guarantees; traced the Auth↔Account, Auth↔Combat ProbeStartupPeers timing and failure modes; spot-checked the L-V3-5 (quest-token secret), L-V3-1 (claim partition dedup → H-V3-12), and L-V3-6 (cursor reload) carry-forwards.

**Required reading verified:** v2 synthesis, v3 synthesis, v3 cross-cutting per-dimension followup, full `git log f46d496..HEAD` (26 commits — H-V3 batch through TC-V3-1 fixture).

---

## Verdict

The v3 remediation arc closed cleanly in code for the cross-cutting surface — H-V3-14(a) envelope-version field landed in both `ServiceClient` (sender) and `ServiceEndpoint` (receiver) with a distinctive `"unsupported_envelope_version"` error, and H-V3-14(b) wired a `ProbeStartupPeers()` hook into all three services with peer-Ping auto-registration on every `ServiceEndpoint`. The env-var matrix is consistent across `Auth/Account/Combat` `main.cpp`. **But the H-V3-14 fix has a load-bearing implementation defect**: `ServiceClient::Probe` collapses every non-`Unreachable` outcome into `ProbeOutcome::UnknownError`, so the `AuthFailed` and `EnvelopeVersionMismatch` enum members are dead — the entire point of distinguishing secret-mismatch from version-skew from network-down is defeated at the call site. Beyond that, the M-V3-2 X-macro convention remains comment-only, L-V3-3 (story_xp_gained golden fixture) is still missing, and `event_type` is still unconstrained TEXT in the schema. System health: **B+** (steady from v3) — no Criticals, but H-V4-1 sits exactly on the seam v3 thought it had closed.

---

## CRITICAL (C-V4-N)

*(none — v4 surface)*

---

## HIGH (H-V4-N)

### H-V4-1. `Probe` collapses every wire-error into `UnknownError` — defeats H-V3-14(b)'s observability promise
**File:** `Server/Common/src/Net/ServiceClient.hpp:258-294` (`Probe`), `:296-307` (`ProbeOutcomeName`)
**Status:** NEW — H-V3-14(b) completeness defect.

The H-V3-14(a) commit (`da38a09`) added a distinctive `"unsupported_envelope_version"` error response with `{"expected_v": N}` so ops can triage version-skew apart from MAC mismatch. H-V3-14(b) (`bfbcf2c`) then added `ProbeOutcome { Ok, Unreachable, AuthFailed, EnvelopeVersionMismatch, UnknownError }` and wired `ProbeStartupPeers` into Auth/Account/Combat. **But `Probe` itself never returns `AuthFailed` or `EnvelopeVersionMismatch`:**

```cpp
// ServiceClient.hpp:282-291
// WireError — inspect the response body. The server's error
// response is a Json wrapped by FormatRpcMessage; on
// WireError ServiceClient returned an empty .value, so we
// can't introspect further from this side. [...]
// Report UnknownError unless a future iteration extends
// RpcCallResult with the server's error code.
return ProbeOutcome::UnknownError;
```

The comment explicitly acknowledges the defect: `RpcCallResult` doesn't carry the server's error string, so the receiver-side distinction (`"unsupported_envelope_version"` vs `"Authentication failed"`) is dropped on the floor before `Probe` ever sees it. The two enum members are dead code, and `ProbeOutcomeName(EnvelopeVersionMismatch)` returns `"envelope_version_mismatch"` only for unit-test consumers of the stringifier.

Concrete failure surface: deploy Auth (envelope_v=2) against Account (envelope_v=1). Auth's `ProbeStartupPeers` calls `Probe`. Server responds `{"error":"unsupported_envelope_version", "expected_v":1}`. `RpcCallResult::WireError` returns with empty `.value`. `Probe` returns `UnknownError`. Auth logs `"Auth startup probe of Account FAILED (unknown_error). Refusing to serve traffic."` Ops sees `unknown_error` and reaches for the network/firewall/secret-mismatch troubleshooting playbook instead of the version-skew one. The fix is exactly the v3 cross-cutting M-V3-4 recommendation that was deferred ("extend RpcCallResult with server's error code"); the receiver-side enrichment work was done, but the sender-side decode was not.

**Fix:** extend `RpcCallResult` with an `errorCode` string (or enum) populated on `WireError` from the server's `response["error"]` field. `Probe`'s WireError branch then switches on the marker: `"unsupported_envelope_version"` → `EnvelopeVersionMismatch`, `"Authentication failed"` → `AuthFailed`, anything else → `UnknownError`. Closes the H-V3-14(a+b) loop end-to-end.

---

## MEDIUM (M-V4-N)

### M-V4-1. M-V3-2 (Snapshot X-macro convention) is still comment-only — no static_assert, no round-trip test
**Files:** `Server/Account/src/State/Account.hpp:93-167` (X-macro + Capture/Restore + indirect block)
**Status:** CARRY-FORWARD from M-V3-2 cross-cutting; the v3 commit added richer doc-comments (L-V3-6 event-sourcing carry-forward) but **no test fixture and no static_assert**.

The convention "every mutable Account field must appear in either the X-macro or the two-entry indirect block" is now triple-documented:
- `Account.hpp:60-61` (M-V2-2 note)
- `Account.hpp:93-101` (M-V2-2 X-macro doc)
- `Account.hpp:103-110` (L-V3-6 claim-density note — names HandleClaimQuestReward's 7+ sub-systems)

Enforcement remains: reviewer attention. There is still no:
- `static_assert(sizeof(Snapshot) == compile-time-computed-expectation)` tripwire
- `[c7-coverage]` round-trip test that mutates each X-macro field, rolls back, asserts restored
- Static introspection (e.g., a `BOOST_PP`-style count macro asserting field count vs. a hardcoded `kExpectedSnapshotFields`)

A future engineer adding `std::vector<UnlockedTitle> m_titles + SetTitle()` who forgets the X-macro entry silently regresses Rollback for that field. C1 stale-flag eviction still rescues across the network boundary; the in-handler Memento path becomes a no-op for the new field exactly the way C7-A originally landed. Same defect class as the now-closed C7-A — only the surface differs.

**Fix:** the prior recommendation stands: extend the X-macro 4 ways (decl + capture + restore + **test-walk**). A single `[c7-fields]` Catch2 test that uses the X-macro to mutate→rollback→assert each field is ~80 lines and closes the convention-vs-enforcement gap permanently.

### M-V4-2. Golden-file fixture vocabulary lags emitted event_types (story_xp_gained still uncovered)
**Files:** `Server/Account/tests/events/v1_*.json` (4 fixtures: pull_performed, credits_spent, quest_reward_claimed, story_level_advanced), `Server/Account/src/Handlers/QuestHandlers.hpp:533` (emits `"story_xp_gained"`), `Server/Account/src/Handlers/AccountHandlers.hpp:418` (emits `"credits_added"`)
**Status:** CARRY-FORWARD from L-V3-3 / M-V3-1 cross-cutting; the v3 batch added the inline comment at `QuestHandlers.hpp:523-532` documenting that the reducer ignores `event_type` for the StoryLevelAdvanced case, but did NOT add a `v1_story_xp_gained.json` fixture.

Today the system emits at least 5 event_types in the codepaths I walked:
- `pull_performed` (GachaHandlers) ✓ golden
- `credits_spent` (GachaHandlers x2 + ProgressionHandlers + QuestHandlers + EffectDispatcher) ✓ golden
- `credits_added` (AccountHandlers + QuestHandlers + EffectDispatcher) ✗ no golden
- `quest_reward_claimed` (QuestHandlers) ✓ golden
- `story_level_advanced` (QuestHandlers) ✓ golden
- `story_xp_gained` (QuestHandlers, payload-identical to story_level_advanced) ✗ no golden

`SchemaMigrationTest` cannot detect a payload-shape regression for `credits_added` or `story_xp_gained` because the fixture set is filtered to the 4 names that exist on disk. The fact that `credits_added` and `credits_spent` share the `CreditsDelta` payload makes the credits gap less urgent than the story-XP one (the credits_spent fixture transitively covers credits_added's shape), but the schema doesn't enforce that linkage.

**Fix:** add `v1_credits_added.json` + `v1_story_xp_gained.json` to `Server/Account/tests/events/` and extend `SchemaMigrationTest::TestCases` accordingly. ~20 lines.

### M-V4-3. `events.event_type` column is unconstrained TEXT — silent vocabulary drift surface persists
**File:** `Server/Account/schema.sql` events table
**Status:** CARRY-FORWARD from M-V3-1 cross-cutting; v3 deferred (comment-only fix). The schema still has no `CHECK (event_type IN (...))` or FK to a `event_types` lookup table.

The v3 mitigation was the inline comment at `QuestHandlers.hpp:523-532` documenting that the reducer ignores `event_type`. That works **today** but encodes an invariant ("event_type is diagnostic-only, never a dispatch key") in a single comment site. A future reducer rewrite that DOES dispatch on event_type (e.g., to disambiguate a new `story_xp_gained` payload from `story_level_advanced` when their schemas diverge) must explicitly enumerate every vocab string ever emitted into events table — including any handler still using the dead vocab.

Compounding: any partman partition rolled over without the receiving reducer aware of a new vocab will silently drop fold-state during replay-from-events. The v3 deferral noted this; the v4 verification confirms no schema-level mitigation landed.

**Fix:** the lowest-cost option is a generated `<events/EventTypes.hpp>` header that enumerates every emitted string + a startup check in `AccountServer::OnStarted` that runs `SELECT DISTINCT event_type FROM events WHERE event_type NOT IN (known-set)` and WARN-logs surprises. The schema CHECK is the heavier-but-bulletproof option.

### M-V4-4. Startup probe race: 5 × 2s = 10s budget vs. Account boot ≈ 25s in Debug
**Files:** `Server/Auth/src/AuthServer.hpp:110-125` (Probe with retries=5, delayMs=2000), `Server/Account/src/AccountServer.hpp:170-185` (same), `Server/Combat/src/CombatServer.hpp:73-88` (same)
**Status:** NEW (v4) — startup-ordering correctness gap.

`Probe(retries=5, delayMs=2000)` gives a ~10-second budget for the peer to come up. Empirical: Account.exe Debug startup runs `LoadAccountFromData` over the seeded fixtures (~1-2s), connects the connection pool, primes `ProtocolLoader` + `GachaConfig` + `QuestLoader`, computes the dummy PBKDF2 hash (~150-400ms), spawns the internal RPC endpoint, runs `ProbeStartupPeers` — total well under 10s in a clean dev box but observed at 15-25s on a cold Docker stack startup.

Today's failure modes:
- **Auth boots first** (which is the canonical loop-in-a-box order, per `scripts/start-all.bat`): Auth probes Account → Account isn't listening → 5×2s retries → fails. Auth Debug warns and continues; Auth Release calls `Stop()` and exits. The Release path turns an ordering hiccup into a service crash.
- **Combat boots first**: same Auth-not-listening race; Combat Release `Stop()`s.

The retry budget is fixed at 5 attempts; there's no exponential backoff, no env-var override, and no "wait-until-deadline" knob. The `start-all.bat` script doesn't sequence Auth-then-Account-then-Combat with a readiness gate either.

**Fix:** either (a) extend the retry budget to ~30 attempts × 2s for an effective 60s window, (b) add `--probe-retries N --probe-delay-ms M` CLI flags, or (c) make Probe return `Unreachable` and let `OnStarted` schedule a periodic re-probe instead of crashing the service. Option (c) is the lowest-coupling fix and gives ops a stable observability signal in steady state.

### M-V4-5. Probe is unidirectional — Auth/Combat probe Account/Auth but no reverse probe exists
**Files:** `Server/Auth/src/AuthServer.hpp:110` (probes Account), `Server/Account/src/AccountServer.hpp:172` (probes Auth), `Server/Combat/src/CombatServer.hpp:75` (probes Auth)
**Status:** NEW (v4) — topology observation.

`ServiceEndpoint` auto-registers `Ping` so every endpoint can answer. But the wiring at startup is:
- Auth probes Account (correct — Auth's resume / validate flows are reverse-direction from this naming, but Auth's session-state pushes go Auth→Account.)
- Account probes Auth (Account needs Auth for SessionCache validation pulls.)
- Combat probes Auth (Combat needs Auth for SessionCache.)
- **No service probes itself** — fine (Ping is registered on its own endpoint).
- **Account doesn't probe Combat, Auth doesn't probe Combat** — fine today (no Account→Combat or Auth→Combat RPC). But the moment a new RPC is added (e.g., matchmaking notifies Auth) the probe matrix is silently incomplete.

Not a bug today; documenting so a future engineer adding Account→Combat or Auth→Combat RPCs remembers to extend `ProbeStartupPeers`. The fix is a project-level convention: every `ServiceClient` member should have a matching probe in `ProbeStartupPeers`.

---

## LOW (L-V4-N)

### L-V4-1. `RpcCallResult::wireError()` documented as "security-relevant — fail closed" but `SessionCache` integration still routes wireError through the same H6 extension path as Unreachable
**File:** `Server/Common/src/Net/ServiceClient.hpp:51-59` (RpcCallResult docs)
The H-V3-14 fix introduced `RpcCallStatus { Success, Unreachable, WireError }` and the comment promises wire-failures fail-closed. Verify `SessionCache::Validate` actually distinguishes — quick spot-check (cross-cutting, didn't fully trace): if it doesn't, the v3 M-V3-4 promise is half-delivered. Either way, the docstring on `wireError()` should specify the contract the caller MUST observe.

### L-V4-2. Ping handler shadows-late-wins is undocumented runtime behavior
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:31-45`
The auto-registered `Ping` runs in the ctor; subclass `RegisterMethod("Ping", ...)` calls would overwrite. The comment notes "a shadow would still be safe — handler signature is the same" but a subclass registering Ping with a non-trivial side effect would silently disable the canary. Add `[[maybe_unused]] static_assert(false)` on duplicate registration, or rename the canary method to `_Probe` to reserve the namespace.

### L-V4-3. `OnStarted` runs `ProbeStartupPeers` BEFORE protocol ID caching in Auth
**File:** `Server/Auth/src/AuthServer.hpp:127-145`
```cpp
void OnStarted() override
{
    ProbeStartupPeers();   // L129 — might Stop() in Release
    auto& proto = ProtocolLoader::Instance();
    m_idRegister = proto.Id("Register");   // L131
    ...
}
```
If `ProbeStartupPeers` calls `Stop()` in Release, the protocol IDs are never cached. `Stop()` should be safe with un-cached IDs, but worth a single-line audit: the order should be "cache IDs (cheap) THEN probe peers (network-dependent)" so even a probe failure leaves the cached state consistent.

### L-V4-4. `envelope_v` field name uses short form on wire but `APHELYON_ENVELOPE_VERSION` macro on Cpp side — naming mismatch on review
**Files:** `Server/Common/src/Net/ServiceEndpoint.hpp:43` (`"envelope_v"`), `:174` (`"v"`), `:184` (`"expected_v"`)
The Ping response uses `"envelope_v"`, the request envelope uses `"v"`, and the version-mismatch error uses `"expected_v"`. Three different short-forms for the same concept. The wire is grep-noisy. Consider canonicalizing to `"envelope_v"` everywhere (3-byte cost on every RPC envelope is negligible).

### L-V4-5. `APHELYON_QUEST_TOKEN_SECRET` enforcement matrix verified clean (L-V3-5 closed)
Re-verified `Auth/main.cpp` and `Combat/main.cpp`: neither references `APHELYON_QUEST_TOKEN_SECRET`. Only `Account/main.cpp:171-180` enforces Release-fatal. Matrix is:
| Var | Auth | Account | Combat | Release-fatal |
|---|---|---|---|---|
| `APHELYON_INTERNAL_SECRET` | ✓ | ✓ | ✓ | all 3 |
| `APHELYON_QUEST_TOKEN_SECRET` | — | ✓ | — | Account |
| `APHELYON_DB_CONNECTION` | — | ✓ | — | Account |

No regression from v3.

### L-V4-6. L-V3-6 (cursor reload cycle test) still missing
**File:** `Server/Account/tests/` — no test exercises full mutate→commit→reload→mutate for all 4 aggregates.
v3 carry-forward; no test landed. Mentioned in the v3 cross-cutting closing list but not in any v3-batch commit. The TC-V3-1 commit (`9129a68`) added an integration teardown fixture, which is necessary but not sufficient — the cursor-reload coverage gap is a separate fixture.

### L-V4-7. Combat `OnProcessMessage` is a hardcoded stub that bypasses every cross-cutting check
**File:** `Server/Combat/src/CombatServer.hpp:61-66`
```cpp
std::string OnProcessMessage(...) override {
    return CreateErrorResponse("Combat service not yet implemented");
}
```
Fine for now (Combat is acknowledged stub per OnStarted log), but it means Combat's protocol.json wiring, SessionCache integration, and clientIP propagation are all untested in this surface. When Combat lands real handlers, every v1-v3 audit finding that targeted Account's RPC layer must be re-audited against Combat's implementation. Documenting the carry-forward so v5 doesn't miss it.

### L-V4-8. Probe's 5×2s warning log fires on EVERY Debug start where peer is slow — no rate-limit, no de-dup
**Files:** `AuthServer.hpp:123`, `AccountServer.hpp:183`, `CombatServer.hpp:86`
Debug builds always continue; the WARN line will be present in every dev session log where startup ordering races. After a week of dev, this log line is noise. Consider a one-line "ok-after-N-retries" success log so ops can grep for either outcome.

---

## Verified Closed from v3

Cross-cutting items the v3 followup flagged and v3-batch commits closed:

- **H-V3-14(a)** — Envelope version field added. `ServiceClient` writes `"v": APHELYON_ENVELOPE_VERSION`; `ServiceEndpoint` reads `request.value("v", 0)` and rejects with distinctive `"unsupported_envelope_version"` BEFORE MAC verify. Golden-file shape test added in `EnvelopeShapeTest.cpp`. ✓ Closed in code (sender + receiver), but see H-V4-1 for the `Probe` decode gap.
- **H-V3-14(b)** — Startup canary `Probe` method added to `ServiceClient`. `ServiceEndpoint` auto-registers `"Ping"` in its ctor. All three services (Auth/Account/Combat) wire `ProbeStartupPeers` into `OnStarted`. ✓ Closed in code at the wiring layer; H-V4-1 / M-V4-4 are completeness gaps.
- **M-V3-1 cross-cutting** (event_type vocabulary doc) — `QuestHandlers.hpp:523-532` documents the reducer-ignores-event_type invariant. ✓ Closed as doc-only mitigation; M-V4-3 carries the structural gap forward.
- **L-V3-1 cross-cutting** (claim partition-dedup → H-V3-12) — `0fc8598` documented the partition-local dedup deferral. ✓ Deferred-with-doc.
- **L-V3-5 cross-cutting** (APHELYON_QUEST_TOKEN_SECRET matrix) — Re-verified clean above (L-V4-5). ✓ Closed.
- **M-V2-3** (AccountServer split) — Now `Account/src/AccountServer.hpp:160-203` is concise (lifecycle hooks + ProbeStartupPeers + cache delegate calls). The PascalCase reorg (commit `6775a9b`) tightened the include surface further. ✓ Architectural win compounds.
- **Snapshot X-macro doc enrichment** — `Account.hpp:103-110` now explicitly names HandleClaimQuestReward as the densest snapshot user. ✓ Doc-only; structural gap M-V4-1 unchanged.

**Open from v3 carrying into v4:**
- M-V3-2 cross-cutting (X-macro enforcement) → M-V4-1
- L-V3-3 (story_xp_gained golden) → M-V4-2 (promoted to Medium given the v3 commit explicitly chose to leave it)
- L-V3-6 (cursor reload cycle test) → L-V4-6
- M-V3-1 cross-cutting (event_type schema constraint) → M-V4-3

---

## Cross-Service Env-Var Matrix (re-verified clean)

Identical to v3 cross-cutting Section "Cross-Service Env-Var Matrix" — no drift introduced by the v3-batch commits.

---

## End-to-End Probe Topology (v4 verification)

```
  Auth (port 7777, internal 7770)
   |
   | OnStarted → ProbeStartupPeers → m_accountClient.Probe(5, 2000) → Account:7773/Ping
   v
  Account (port 7771, internal 7773)
   |
   | OnStarted → ProbeStartupPeers → m_authClient.Probe(5, 2000) → Auth:7770/Ping
   v
  Combat (port 7772, internal NONE)
   |
   | OnStarted → ProbeStartupPeers → m_authClient.Probe(5, 2000) → Auth:7770/Ping
```

`Ping` handler auto-registered in `ServiceEndpoint` ctor → returns `{"pong": epochSec, "envelope_v": APHELYON_ENVELOPE_VERSION}`. MAC + envelope-v + secret are all exercised in a single round trip. ✓ Correct.

The race window: Auth/Combat both depend on Auth.exe's internal endpoint being up; Auth's own probe is toward Account, so Auth comes up first in the canonical order. If Account.exe starts before Auth.exe, Account's probe of Auth fails for 10s, then continues in Debug or `Stop()`s in Release. See M-V4-4.

---

## Suggested triage order

**Before next deploy:**
1. **H-V4-1** — Extend `RpcCallResult` with `errorCode` string; decode `"unsupported_envelope_version"` / `"Authentication failed"` markers in `Probe`'s WireError branch. ~30 LOC. Closes the H-V3-14(a+b) loop end-to-end.

**This week:**
2. **M-V4-1** — Add `[c7-fields]` round-trip test that uses the X-macro to walk every Account direct field. ~80 LOC.
3. **M-V4-2** — Add `v1_credits_added.json` + `v1_story_xp_gained.json` golden fixtures + `SchemaMigrationTest` rows. ~20 LOC.
4. **M-V4-4** — Either extend Probe retry budget or convert probe-fail into periodic re-probe instead of `Stop()`. ~10-30 LOC.

**Before launch:**
5. **M-V4-3** — Schema CHECK or runtime warning for unknown event_type values.
6. **M-V4-5** — Convention check: every `ServiceClient` member has a `Probe` call.

**Eventually:**
7. L-V4-1 through L-V4-8 — naming, log dedup, Combat stub re-audit reminder.

---

## System-Level Health Verdict

**B+ (steady).** The v3 remediation arc closed the surface it scoped. H-V4-1 is the one nontrivial new finding — a completeness gap on the v3 batch's headline fix that defeats the observability promise of distinguishing version-skew from MAC failure from network-down. The remaining v4 surface is all carry-forward Mediums and Lows; no Criticals, no architectural surprises. The test-coverage backlog continues to be the systemic risk (M-V4-1, M-V4-2, L-V4-6 are all "no test exists" findings, not "broken code" findings) — same shape as v2 and v3 flagged.
