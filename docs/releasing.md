# Releasing Arcane

How an engine release reaches `downloads.starworks.dev` (design:
`docs/superpowers/specs/2026-10-12-arcane-distribution-design.md`; plan:
`docs/superpowers/plans/2026-10-12-arcane-distribution-phase2-pipeline.md`).

## Cutting a release

1. Set the version in `Arcane/Version.hpp` (`kVersionMajor`, `kVersionMinor`,
   `kVersionPatch`, `kVersionPrerelease`: `""` for stable, `"preview.N"` for
   a preview).
2. Write `docs/releases/<version>.md` (see `docs/releases/README.md`).
3. Merge both to `main`.
4. Tag and push it:

   ```bash
   git tag -a v0.3.1 -m "Arcane 0.3.1"
   git push origin v0.3.1
   ```
5. In Actions → release, approve the `release` environment's jobs (signing,
   then publishing). The run builds, signs, packs, uploads, signs
   `releases.json`, creates the GitHub Release and rebuilds the website.

Everything fails closed. A tag that doesn't match `Version.hpp`, missing
notes, an unpublished CI image, a failed test, signing or signature check, an
object that already exists with different content, or a manifest that doesn't
verify each stops the run before `releases.json` changes. Archives uploaded
before a failure stay and are never overwritten: the job summary lists them,
and re-running the workflow accepts them if they're identical.

`released_at` is when the run started. The archive's LICENSE (phase 1) and the
manifest share it, and it is never rewritten (spec s12).

## Dry runs and rehearsals

Actions → release → Run workflow:
- **dry-run** (any branch): builds a fixture, packs it and prints the manifest
  entry it would publish. No secrets, no upload.
- **rehearsal** (main only): the same fixture, really signed with Artifact
  Signing, published to `downloads.starworks.dev/staging/` with the TEST
  manifest key. Use it after changing the pipeline or the hosting.

## Maintaining the manifest

`releases.json` expires 90 days after it is signed, and Hubs refuse an
expired manifest (installed engines keep working; installs and updates
stop). On the 1st of each month `manifest-admin` re-signs it. **Approve that
run.** Two missed months are still survivable; three are not.

GitHub disables scheduled workflows in a public repository after 60 days
without repository activity. If Arcane goes quiet that long, re-enable
manifest-admin (Actions → manifest-admin → Enable) or run it by hand.

By hand (Actions → manifest-admin → Run workflow, from `main`):
- **resign**: sign again now.
- **yank** (version and reason): marks it yanked. The archives stay, so
  projects pinned to it still install it; the Hub and the website show the
  reason. `released_at` is never touched.
- **init**: writes serial 1. For the first publish to a target only; it
  refuses if a manifest exists.
- **repair**: if `releases.json` and its `.minisig` disagree (a publish failed
  between the two writes, and the run says so), this restores both from the
  newest `history/<serial>.json` pair that verifies. Then re-run whatever
  failed.

Every serial is also stored, immutably, at `history/<serial>.json(.minisig)`:
a record of every manifest ever published.
