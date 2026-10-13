# Golden manifests

Signed fixtures for the Hub's manifest tests (distribution spec s10; phase 3).
Verify them at `now = 2026-10-12T00:00:00Z` with highest serial seen = 5,
trusting `tests/keys/test-manifest-1.pub` (and `-2` where noted).

| File | Expected |
|---|---|
| `valid` | Verifies. Serial 5. Stable 0.3.1 (security), preview 0.4.0-preview.2 (min Hub 0.3.0), 0.2.0 yanked with a reason. |
| `older` | Rollback: serial 4 is below 5. |
| `expired` | Expired (2026-09-29). |
| `unknown-fields` | Verifies. Unknown fields are ignored. |
| `rotated` | Signed with key 2. Bad signature against key 1 alone; verifies against [key 1, key 2]. |
| `tampered` | An artifact hash was changed after signing. Bad signature. |
| `schema2` | Signature fine, schema 2: "unsupported schema, update the Hub". |

Regenerate with `ARCANE_BLESS_GOLDENS=1 cargo test --test golden_test -- --test-threads=1`
(one thread: the rehearsal-key test reads the files the bless writes) after
a deliberate schema or operations change. The test checks that each `.json`
is current, then checks every verdict above against the committed files.
Signatures carry randomness, so a re-bless rewrites every `.minisig`. The
files are byte-exact (`.gitattributes`: `-text`).
