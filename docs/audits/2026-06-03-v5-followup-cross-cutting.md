# Aphelyon Server — v5 Follow-up: Cross-Cutting / Integration / End-to-End

**Date:** 2026-06-03 (same-day successor to v4, after the 20-commit v4 remediation arc)
**Auditor:** Cross-cutting / integration lens (v5 sweep verifying v4 fixes + finding new drift)
**Method:** Walked the v4 commits cross-cutting-relevant (`d8fd1c7`, `6357419`, `b2f6049`, `25a81c1`, `388c0dd`, plus the L-cluster `d65dbdf`) against the live source. Verified the M-V4-4 retries=15/delayMs=2000 propagation at all three `ProbeStartupPeers` call sites. Cross-walked emitted `event_type` strings vs. the `Server/Account/tests/events/v1_*.json` fixture set. Re-checked the M-V4-1 X-macro convention for any new static_assert or round-trip test. Compared the C-V4-2 `Probe` classification logic against the M-V4-9 `SessionCache::Validate` classification logic against ServiceEndpoint's actual error-string vocabulary. Inspected ProtocolLoader cache-miss handling and `kInvalidMsgId` propagation.

**Required reading verified:** v4 synthesis, v4 cross-cutting per-dimension followup, v3 cross-cutting per-dimension followup, full `git log 6483b01..HEAD` (20 commits — C-V4-1 OutboxRelay wire through L-cluster cleanups).

---

## Verdict

The v4 remediation arc closed cleanly in code for the cross-cutting surface:

- **M-V4-4 (probe budget)** is uniform across all three sites — `AuthServer.hpp:115`, `AccountServer.hpp:188`, `CombatServer.hpp:78` all use `Probe(retries=15, delayMs=2000)` with matching audit-ref comments. ✓
- **M-V4-2 (golden fixtures)** closed completely — all 6 emitted event_types (`pull_performed`, `credits_spent`, `credits_added`, `quest_reward_claimed`, `story_level_advanced`, `story_xp_gained`) now have on-disk fixtures + `SchemaMigrationTest` cases. ✓
- **C-V4-2 (Probe classification)** closed — `ServiceClient::Probe` (`ServiceClient.hpp:294-305`) inspects `rpc.value` on the Ok path and routes the two specific error strings to `AuthFailed` / `EnvelopeVersionMismatch`. ✓
- **M-V4-5 idempotency (type-safe key extractor)** closed — `IdempotencyKey::ExtractClientKey` (`IdempotencyKey.hpp:89`) replaces 8 hand-rolled `request.value("idempotency_key", "")` sites. ✓
- **H-V4-1 (Begin above TickQuests::Apply in HandleClaimQuestReward)** closed — `QuestHandlers.hpp:591` opens the txn, `:593` calls `TickQuests::Apply`. ✓

But the v4 fixes for `Probe` (C-V4-2) and `SessionCache::Validate` (M-V4-9) **diverged in classification approach** while sharing the same hard-coded error-string vocabulary at three independent sites. That's the v5 headline finding — **H-V5-1**. Two carry-forwards from v4 remain: **M-V4-1 (X-macro convention is still comment-only)** and **L-V4-3 (Probe before protocol-ID cache in Auth)** plus **L-V4-4 (envelope_v / v / expected_v naming inconsistency)**. Plus a new structural finding: **M-V5-2** — `events.schema_version` is the same silent vocabulary-drift surface that M-V4-3 flagged for `event_type`, just one column over. No new Criticals.

System-level health: **A− (improved from B+).** v4 remediation closed both Criticals and 10 Highs; the remaining cross-cutting surface is one promotable High (the multi-site error-string vocabulary) and two carry-forward Mediums.

---

## CRITICAL (C-V5-N)

*(none — v5 surface)*

---

## HIGH (H-V5-N)

### H-V5-1. ServiceEndpoint error vocabulary is hard-coded at THREE independent sites; Probe and SessionCache classify it differently
**Files:**
- `Server/Common/src/Net/ServiceEndpoint.hpp:203` (`"unsupported_envelope_version"` producer)
- `Server/Common/src/Net/ServiceEndpoint.hpp:219` (`"Authentication failed"` producer)
- `Server/Common/src/Net/ServiceEndpoint.hpp:246` (`"Unknown method: <name>"` producer)
- `Server/Common/src/Net/ServiceEndpoint.hpp:279` (`"Internal error: <what>"` producer)
- `Server/Common/src/Net/ServiceClient.hpp:300-303` (Probe consumer — exact match on first two only)
- `Server/Common/src/Net/SessionCache.hpp:148` (SessionCache consumer — `contains("error")` catchall)

**Source:** new finding — cross-service drift introduced by the C-V4-2 + M-V4-9 paired fixes diverging in approach.

**Status:** NEW — H severity for the vocab-drift surface; the latent classification bug for "Unknown method" is the proximate hazard.

The C-V4-2 fix (commit `25a81c1`) made `Probe` inspect `rpc.value` and route two specific error strings:

```cpp
// ServiceClient.hpp:300-303 — exact-string match
const auto err = rpc.value.value("error", std::string{});
if (err == "unsupported_envelope_version") return ProbeOutcome::EnvelopeVersionMismatch;
if (err == "Authentication failed")        return ProbeOutcome::AuthFailed;
if (!err.empty())                          return ProbeOutcome::UnknownError;
```

The M-V4-9 fix (commit `388c0dd`) made `SessionCache::Validate` treat any error-bearing envelope as integrity loss:

```cpp
// SessionCache.hpp:148-156 — any-error catchall
if (rpc.value.contains("error"))
{
    LOG_AUTH_WARN("SessionCache: Auth RPC returned envelope error: {}", ...);
    NoteAuthLost();
    SessionInfo info; info.valid = false;
    return info;
}
```

ServiceEndpoint actually emits **four** distinct error envelopes (lines 203, 219, 246, 279). Probe classifies two of them specifically and collapses the others into `UnknownError`; SessionCache treats all four identically as `NoteAuthLost`. Two latent classification bugs result:

1. **If Auth's `AuthorizeToken` handler is unregistered** (deployment regression or a refactor that drops `m_internalRpcHandlers.Register()`), the response is `{"error": "Unknown method: AuthorizeToken"}` — ServiceEndpoint.hpp:246. `SessionCache` calls `NoteAuthLost` (signals "Auth integrity broken") when the actual condition is "Auth is up but doesn't know how to validate sessions." Ops sees "Auth lost" telemetry; the playbook says check the secret / network / cert — none of which is the cause. The condition is invisible until someone reads logs and notices `Unknown method: AuthorizeToken`.

2. **The error-string vocabulary is comment-coupled, not code-coupled.** The two literal strings (`"unsupported_envelope_version"`, `"Authentication failed"`) appear in 4 source locations: producer (ServiceEndpoint:203/219), Probe consumer (ServiceClient:301-302), SessionCache documentation (SessionCache.hpp:139-140, in comment only). A typo in any one of the consumers silently degrades the classification — e.g., a renaming of `"Authentication failed"` → `"authentication_failed"` in ServiceEndpoint will keep Probe's `Ok` branch matching anything, but reclassify auth failure as `UnknownError`. The golden-file `EnvelopeShapeTest.cpp` pins the envelope shape but not the error strings.

**Fix:** introduce a `Net/ServiceEnvelopeErrors.hpp` header with `constexpr std::string_view kErrUnsupportedEnvelopeVersion = "unsupported_envelope_version"; ...` for all four ServiceEndpoint error strings. Producer + both consumers reference the constants. Then unify the consumer classification: extend `RpcCallResult` with an `envelopeError` enum populated from the error string (already what v4 cross-cutting `H-V4-1` recommended), and have both `Probe` and `SessionCache` switch on that enum rather than re-matching strings. This also fixes the L-V4-1 contract gap on `wireError()`.

The minimum-viable closure: extract the 4 strings to constants and add a 5-line classifier function (`ClassifyEnvelopeError(const Json& rpcValue) -> EnvelopeErrorKind`) that both `Probe` and `SessionCache` call. ~30 LOC, one new header.

---

## MEDIUM (M-V5-N)

### M-V5-1. Snapshot X-macro convention is still comment-only (carry-forward from M-V4-1 / M-V3-2)
**Files:** `Server/Account/src/State/Account.hpp:117-172` (X-macro + Capture/Restore + indirect block)
**Status:** CARRY-FORWARD third time; v4 added no test and no static_assert.

The convention "every mutable Account field is in the X-macro or in the 2-entry indirect block" remains enforced by reviewer attention only. The X-macro list (`Account.hpp:117-137`) now covers 20 direct fields; `rngState` + `collectionState` are the 2 indirect fields covered by manual code at `:159-160` and `:170-171`. No new field has been added in the v4 arc that would have exercised the gap, but the same future-engineer hazard from v3-M-V3-2 remains: adding `std::vector<UnlockedTitle> m_titles + SetTitle()` without an X-macro entry silently disables Rollback for that field. C1 stale-flag eviction still rescues across the cache boundary, so the surface is narrow but real.

**Fix:** the prior recommendations stand. A single `[c7-fields]` Catch2 test that uses the X-macro to expand into a round-trip walk over every direct field is the cheapest closure (~80 LOC). Alternative: a `static_assert(sizeof(Snapshot) == kExpectedSnapshotSize)` tripwire where `kExpectedSnapshotSize` is a hand-maintained constant. Both raise the convention-vs-enforcement visibility floor.

### M-V5-2. `events.schema_version` is unconstrained INT — same silent vocabulary-drift surface as M-V4-3's event_type
**Files:**
- `Server/Account/schema.sql:300` (`schema_version INT NOT NULL DEFAULT 1` — no CHECK)
- `Server/Account/src/Events/Event.hpp:32` (`int schema_version = 1` — emit default never overridden)
- `Server/Account/src/Db/EventStore.hpp:171` (load path reads the column)
- `Server/Account/src/Reducers/*.hpp` (5 reducers — none reference `schema_version`)

**Status:** NEW — symmetric to M-V4-3 (event_type vocabulary), discovered while verifying the M-V4-2 golden-fixture closure.

The fixture JSONs pin `"schema_version": 1` and the read-side reads the column, but no reducer dispatches on it. Today this is fine — every event is at v1. The first time a payload migrates (e.g., `wallet::CurrencyDelta` adds a `currency_token: string` field for in-game-tokenized currencies, bumping that event type's `schema_version` to 2), the reducer must learn to dispatch. There's no infrastructure to do that — the field is dead weight on the write path and dead weight on the read path until the first migration, at which point it becomes load-bearing with zero scaffold.

Compounding: the `SchemaMigrationTest` cases pin v1 by hard-coding `REQUIRE(j["schema_version"].get<int>() == 1)`. A v2-emitting handler would need a new fixture file, a new test case, and a reducer migration helper — and the test infra has no shape for any of that.

**Fix:** either (a) drop the column from the schema and the struct (it's actively misleading), or (b) add a `ReducerCommon::ApplyMigrations(event)` shim that's called before every reducer dispatch — even as a no-op stub today, it pins the contract so a future v2 has a clear extension point. Option (b) preserves the original intent (the schema designer specifically reserved the column); option (a) eliminates the dead-code surface today. ADR-worthy.

### M-V5-3. Two-of-four ServiceEndpoint error envelopes have no consumer-side classification
**Files:** `Server/Common/src/Net/ServiceEndpoint.hpp:246` (`"Unknown method: ..."`), `:279` (`"Internal error: ..."`)
**Status:** NEW — partial overlap with H-V5-1, separated because the fix here is narrower.

`Probe` collapses both into `UnknownError`. `SessionCache` collapses both into `NoteAuthLost`. Neither classification is correct for these — `Unknown method` is a deployment regression (handler-registration drift), `Internal error` is a server-side handler exception (bug). Today, an Auth.exe rebuild that accidentally drops `AuthorizeToken` registration shows up as "Auth lost" in `SessionCache` telemetry — wrong observability shape. An Account-side handler that throws on a malformed Auth response shows up the same way.

The `Internal error` case also smuggles the exception's `what()` payload into the error-string body. If an attacker can shape a payload to make a handler throw with a controlled `what()`, they can write arbitrary text to ops logs. Not a security exploit (the secret-mismatch path catches the MAC mismatch before dispatch), but the `Internal error` shape is information-leaky.

**Fix:** define a 4-value `EnvelopeErrorKind { VersionMismatch, AuthFailed, UnknownMethod, HandlerError }`, return it on both consumer sites, classify all four meaningfully. Folds into H-V5-1's larger refactor.

### M-V5-4. ProtocolLoader returns `kInvalidMsgId` (0) on unknown name with no startup validation
**Files:**
- `Server/Common/src/Net/Protocol.hpp:207-211` (`Id` returns `kInvalidMsgId` on miss)
- `Server/Auth/src/AuthServer.hpp:134-145` (12 IDs cached in `OnStarted`, none validated)
- `Server/Account/src/Handlers/*.hpp` (similar pattern across 4 handler classes)
- `Server/Common/src/Net/TcpServerBase.hpp:272/278` (looked up on every CreateErrorResponse / CreateSessionExpiredResponse)
**Status:** NEW — silent failure surface on protocol.json drift.

A typo in `proto.Id("Lgin")` returns `0`. Every subsequent send uses message-id 0 on the wire. The client's protocol.json wouldn't recognize id 0; the client drops the message or routes through its fallback dispatch. No assertion fires; no startup log line. The same applies to `ProtocolLoader::Instance().Id("ErrorResponse")` in TcpServerBase — if the message name is ever renamed in protocol.json and the C++ string isn't updated, error responses go out as id 0.

`CreateErrorResponse` and `CreateSessionExpiredResponse` also hit `ProtocolLoader::Instance().Id(...)` on every invocation rather than caching — a perf surface (hash-map lookup per send), and the silent-failure surface re-fires every send instead of once at startup.

**Fix:** wrap the cached-ID assignment in a helper `MsgId RequireId(ProtocolLoader& p, const char* name)` that `throw`s on `kInvalidMsgId`. Use it everywhere `Id(...)` is cached. For the TcpServerBase hot path, cache the two IDs as class-level statics in `Initialize()` or at first-use. ~10 LOC + one helper.

### M-V5-5. Probe topology still unidirectional (M-V4-5 carry-forward)
**Files:** `Server/Auth/src/AuthServer.hpp:581` (only `m_accountClient`), `Server/Account/src/AccountServer.hpp:386` (only `m_authClient`), `Server/Combat/src/CombatServer.hpp:109` (only `m_authClient`)
**Status:** CARRY-FORWARD from M-V4-5 cross-cutting — no remediation, still a future-extension hazard, not a bug today.

Today's matrix is complete for the existing RPC dependencies. The moment Account→Combat or Auth→Combat RPCs are added (e.g., matchmaking notifies Auth, abandon-match dispatches to Account), the probe matrix is silently incomplete — the new `ServiceClient` member would need a paired probe in the owning service's `ProbeStartupPeers`, and there's no enforcement. The Combat stub's `OnProcessMessage` still returns "not yet implemented" (L-V4-7 carry-forward), so this is genuinely future work.

**Fix:** project convention: every `ServiceClient` member with a `Probe` call. A `[probe-topology]` integration test that walks the constructor of each *Server class and asserts every `ServiceClient` member is matched by a `Probe` invocation in `ProbeStartupPeers` would mechanize the convention.

---

## LOW (L-V5-N)

### L-V5-1. `envelope_v` / `v` / `expected_v` naming triple still drifts (carry-forward from L-V4-4)
**Files:** `Server/Common/src/Net/ServiceEndpoint.hpp:43` (`"envelope_v"`), `:194` (`"v"`), `:204` (`"expected_v"`), `Server/Common/src/Net/InternalRpcAuth.hpp:152` (`"v"`)
The same concept named three ways on the wire. v4 commented no change. Consider canonicalizing to `"envelope_v"` everywhere or `"v"` everywhere; the 3-byte-per-RPC cost of the long form is negligible.

### L-V5-2. `OnStarted` still runs ProbeStartupPeers BEFORE protocol-ID caching in Auth (carry-forward from L-V4-3)
**File:** `Server/Auth/src/AuthServer.hpp:130-145`
```cpp
void OnStarted() override
{
    ProbeStartupPeers();   // L132 — might Stop() in Release
    auto& proto = ProtocolLoader::Instance();
    m_idRegister = proto.Id("Register");   // L134
    ...
}
```
A Release probe failure calls `Stop()` and the protocol IDs are never cached. `Stop()` should be safe with un-cached IDs (it doesn't send anything), but a refactor that adds an outgoing message during shutdown would surface a latent bug. Cheap, order-of-operations rearrange: cache IDs first (cheap, deterministic), then probe peers (network-dependent).

### L-V5-3. Probe-fail WARN line still has no rate-limit / dedup (carry-forward from L-V4-8)
**Files:** `Server/Auth/src/AuthServer.hpp:126`, `Server/Account/src/AccountServer.hpp:199`, `Server/Combat/src/CombatServer.hpp:89`
Debug builds continue past probe failure; the WARN line fires on every dev session where the startup order races. After a week of dev work, the log line is noise — the operator gestalt becomes "ignore the orange line." Consider a one-line "ok-after-N-retries" INFO so ops can grep for either outcome. Net-zero noise reduction (you still log once), but the success case becomes greppable.

### L-V5-4. `L-V4-1 wireError contract` — SessionCache still routes WireError through extension path
**File:** `Server/Common/src/Net/RpcCallResult.hpp` (or wherever `RpcCallResult` is defined), `SessionCache.hpp:125-135`
The L-V4-1 observation flagged that `wireError()` is documented "security-relevant — fail closed" but `SessionCache::Validate` was suspected of routing it through the same H6 extension path as `Unreachable`. Verified in v5: lines 125-135 do log a louder WARN for `wireError`, but the actual session-cache behavior is identical (`NoteAuthLost`, return invalid). That's correct fail-closed behavior — but the L-V4-1 ask was for the docstring to specify the caller contract. Still missing. Tiny doc edit; ~3 lines.

### L-V5-5. Ping handler shadow-late-wins still undocumented runtime behavior (carry-forward from L-V4-2)
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:31-45`
The auto-registered `Ping` runs in the ctor; a subclass `RegisterMethod("Ping", ...)` would overwrite. Carried forward from v4. The d65dbdf cleanup commit closed L-V4-2 code-quality (renamed an unrelated streak field) but not L-V4-2 cross-cutting. Rename to `_Probe` to reserve the namespace, or add a runtime check that `Register("Ping", ...)` from outside the ServiceEndpoint ctor throws or warns.

### L-V5-6. Combat `OnProcessMessage` stub means every cross-cutting check is untested against Combat handlers (carry-forward from L-V4-7)
**File:** `Server/Combat/src/CombatServer.hpp:61-66`
When Combat lands real handlers, every v1–v4 finding that targeted Account's RPC layer must be re-audited against Combat's implementation. The probe wire IS exercised (Combat probes Auth at startup) but the cross-cutting handler patterns (`Begin()` above mutation, `IdempotencyKey::ExtractClientKey`, stripe-lock-per-player) all require re-validation when Combat handlers materialize.

### L-V5-7. `events.schema_version` column also dead-weight from the fixture-vs-emit perspective
**File:** `Server/Account/src/Events/Event.hpp:32` (default `= 1`, never overridden), `Server/Account/tests/events/v1_*.json` (all pin v1)
The default initialization at line 32 means every emitted Event ships v1; the column is set by the EventStore append path but the value is hardcoded by the struct default. There's no constructor argument, no per-event-type override mechanism, no `kCurrentVersion` constant per payload type. The fixture set could just as easily omit the field — the test would still pass because the load-path reads the column verbatim. This is the read-side cousin of M-V5-2 and should be addressed in the same change.

### L-V5-8. `IdempotencyKey::ExtractClientKey` WARN logs on non-string type but `LOG_DATA_WARN` may be sampled in Release
**File:** `Server/Common/src/Idempotency/IdempotencyKey.hpp:101`
The M-V4-5 fix logs `it->type_name()` (nlohmann's enum stringifier) when a client SDK sends a non-string idempotency_key. Useful signal — but `LOG_DATA_WARN` doesn't include `accountId` or `playerId` (the function only has the JSON request). A misbehaving client SDK in production would generate noise with no traceability. The signature could take an optional `std::string_view playerId` for log enrichment. ~3 LOC.

---

## Verified Closed from v4

Cross-cutting items the v4 followup flagged and v4-batch commits closed:

- **C-V4-2** — `Probe` inspects `rpc.value` on Ok path. Distinct outcomes for `"unsupported_envelope_version"` / `"Authentication failed"` / catchall `UnknownError`. ✓ Closed in code at `ServiceClient.hpp:294-305`. **But** the consumer-side approach diverged from `SessionCache::Validate`'s approach in the M-V4-9 fix — see H-V5-1 for the drift.
- **M-V4-2 cross-cutting** (golden fixture vocabulary) — `credits_added` and `story_xp_gained` fixtures landed at `Server/Account/tests/events/v1_credits_added.json` + `v1_story_xp_gained.json`, plus two new `SchemaMigrationTest` cases at `Server/Account/tests/GoldenFile/SchemaMigrationTest.cpp:138-181`. The 6 emitted event_types are now 100% covered. ✓ Closed.
- **M-V4-4 cross-cutting** (probe budget) — All three sites at `Probe(retries=15, delayMs=2000)` = 30s. ✓ Closed.
- **M-V4-5 idempotency** (type-safe key extractor) — `IdempotencyKey::ExtractClientKey` introduced at `IdempotencyKey.hpp:89`; 8 handler sites migrated. ✓ Closed.
- **M-V4-9 networking** (SessionCache classify) — `SessionCache.hpp:148-156` inspects `rpc.value.contains("error")` and routes to `NoteAuthLost`. ✓ Closed in code, **but** drifted from C-V4-2's approach — see H-V5-1.
- **H-V4-1 event-sourcing** (Begin above TickQuests in HandleClaimQuestReward) — `QuestHandlers.hpp:591` opens the txn, `:593` calls `TickQuests::Apply`. C7-A invariant preserved across all 4 handlers using TickQuests. ✓ Closed (verified by grep — 4 sites at QuestHandlers.hpp:99/244/591/734 all show Begin BEFORE TickQuests::Apply).

**Open from v4 carrying into v5:**
- M-V4-1 cross-cutting (X-macro static_assert / round-trip test) → M-V5-1
- L-V4-3 (Probe before ID cache in Auth) → L-V5-2
- L-V4-4 (envelope_v naming triple) → L-V5-1
- L-V4-8 (probe-fail WARN dedup) → L-V5-3
- L-V4-1 (wireError contract docstring) → L-V5-4
- L-V4-2 cross-cutting (Ping shadow-late-wins) → L-V5-5
- L-V4-7 (Combat stub re-audit reminder) → L-V5-6

---

## Cross-Service Env-Var Matrix (re-verified clean)

| Variable | Auth | Account | Combat | Release-fatal? |
|---|---|---|---|---|
| `APHELYON_INTERNAL_SECRET` | ✓ | ✓ | ✓ | all 3 |
| `APHELYON_QUEST_TOKEN_SECRET` | — | ✓ | — | Account |
| `APHELYON_DB_CONNECTION` | — | optional | — | no |

No drift from v4.

---

## End-to-End Probe Topology (v5 verification)

```
  Auth (port 7777, internal 7770)
   |
   | OnStarted → ProbeStartupPeers → m_accountClient.Probe(15, 2000) → Account:7773/Ping
   v
  Account (port 7771, internal 7773)
   |
   | OnStarted → ProbeStartupPeers → m_authClient.Probe(15, 2000) → Auth:7770/Ping
   v
  Combat (port 7772, internal NONE)
   |
   | OnStarted → ProbeStartupPeers → m_authClient.Probe(15, 2000) → Auth:7770/Ping
```

All three sites use **identical** retries=15 / delayMs=2000 / 30s budget (M-V4-4 closure). `Ping` handler auto-registered in `ServiceEndpoint` ctor returns `{"pong": epochSec, "envelope_v": APHELYON_ENVELOPE_VERSION}`. MAC + envelope-v + secret + handler-presence all exercised in a single round trip. ✓ Correct.

The race window from M-V4-4 should now be closed for the canonical Docker-cold-boot case (~25s observed vs. 30s budget).

---

## v4-Remediation-Introduced Cross-Service Drift

This audit specifically looked for new drift introduced by the v4 fixes. Two paired fixes diverged:

| Fix | Approach | Sites |
|---|---|---|
| C-V4-2 (`Probe`) | exact-string match on `"unsupported_envelope_version"` / `"Authentication failed"` → specific outcomes; any other error → `UnknownError` | `ServiceClient.hpp:300-303` |
| M-V4-9 (`SessionCache::Validate`) | `rpc.value.contains("error")` → all error envelopes → `NoteAuthLost` + invalid | `SessionCache.hpp:148-156` |

The producer (`ServiceEndpoint`) emits four distinct error envelopes; two consumers classify them three different ways. The error strings themselves are hard-coded at three independent sites without a shared constant. See H-V5-1 for fix.

---

## Suggested triage order

**This week:**
1. **H-V5-1** — Extract the 4 ServiceEndpoint error strings to `Net/ServiceEnvelopeErrors.hpp` constants; introduce `EnvelopeErrorKind` enum on `RpcCallResult`; unify Probe + SessionCache classification. ~30 LOC. Closes H-V5-1 + M-V5-3 + L-V5-4.
2. **M-V5-1** — Add `[c7-fields]` round-trip test that walks the X-macro. ~80 LOC. Triple carry-forward.
3. **M-V5-4** — `RequireId` helper that throws on `kInvalidMsgId`; replace cached-ID sites. ~20 LOC.

**Before launch:**
4. **M-V5-2** + L-V5-7 — Either drop `events.schema_version` or add reducer migration shim. ADR-worthy.
5. **M-V5-5** — `[probe-topology]` integration test asserting every `ServiceClient` member has a matching probe call.

**Eventually:**
6. L-V5-1 through L-V5-8 — naming, log dedup, Combat stub re-audit reminder.

---

## System-Level Health Verdict

**A− (improved from B+).** The v4 arc closed both Criticals (OutboxRelay wire-in, Probe classification) and the 7 cross-cutting Highs/Mediums it targeted. The remaining surface is one promotable High (the error-vocabulary drift introduced by the paired C-V4-2 / M-V4-9 fixes), four carry-forward Mediums, and a thin layer of Lows. No new Criticals. No architectural surprises. The pattern v3 noted ("test-coverage backlog is the systemic risk") continues to apply at M-V5-1 — but the v4 arc materially improved direct test coverage for AccountCache / AccountHydrator / InternalRpcHandlers / AccountRepository round-trip / AddCurrency e2e / idempotency machinery / C-V3-2 regression. The audit-vs-fix ratio is now positive: 20 commits in v3→v4, ~10 of them test commits, against a v5 surface that is structurally smaller than v4's.
