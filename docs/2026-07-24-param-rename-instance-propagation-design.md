# Assisted param rename: propagating to instance overrides

**Status: BUILT 2026-07-24** (ShaderEditorDocument::BeginParamRename /
PatchParamRename / TranslateOverrideHash + the DocServices::onParamRenamed
seam). One deviation from the sketch below: the local-fix override re-key is
not an immediate `Set` (the OLD template would reject the new name) -- it is a
pending-rename translation applied at PromotePendingInstance, conditional on
the target template actually declaring the new name, which also handles an
undo rolling the template back (reverse translation). Closes the documented
SG-parity wart: instance
overrides key on the param NAME, so renaming a param in a base material orphans
every instance's saved value for it. Unity accepts this because it cannot find
the affected assets; we have GUID identity, so we can.

## Scope decision

Assisted rename is a **graph-tier feature only**. A Param/TextureSample node's
name commit is an explicit, atomic rename event (old → new). Text `//@param`
edits are NOT reliably detectable (a rename is indistinguishable from
delete + add) — text docs keep the documented wart, same as UE/Unity.

## Trigger

The param-name commit handler in `DrawGraphNode` (deactivate-after-edit), with
one guard: the renamed node must have been the **sole declarer** of the old
name. If another node still declares it, the edit is a decl SPLIT, not a
rename — no propagation, no modal.

## Flow

1. **Immediate local fix** (unconditional, even before the modal): re-key the
   live document's own override — `m_instance` override for hash(old) →
   hash(new) — before the rebind rejects and retires it. Today the doc's own
   saved value orphans too; this fixes that for free.
2. **Discovery**: `AssetRegistry::All()` → for each `.arcmat`,
   `LoadMaterialAsset` → walk the parent chain (the `ResolveParentChain`
   shape, cycle-guarded) → collect every instance whose chain reaches this
   base's GUID, at any depth.
3. **Modal**: "Rename 'Speed' → 'Rate' in N instance file(s)?" listing the
   affected names, with **[Rename everywhere] [Just here]**. N == 0 → no
   modal, nothing to do. "Just here" = today's behavior.
4. **Rewrite**: per instance file, re-key the params entry old → new and save
   through `SaveMaterialAsset`. Then per GUID: fire the `onAssetSaved` /
   sprite-cache invalidation seam, and patch any OPEN document for that
   instance in memory via `DocumentHost` (its `m_data.params` + live override
   re-key) — open docs with unsaved edits get patched, never stomped.

## Edge rules

- **Rename onto an existing param name** (merge): an instance holding BOTH
  keeps its existing new-name override; the old entry drops with a console
  warn. The modal wording mentions this.
- **Failures**: an instance file that fails to load or save is skipped with a
  console error; each file write is atomic (existing Save path), no partial
  rewrite of a file.
- **Undo**: the graph rename itself stays one undo step. Cross-FILE
  propagation is not undoable (disk writes to other assets) — the modal is
  the consent gate, same standing as pass-canvas structural edits.
- **Types**: rename never changes the value type; the apply-time type gate
  (`MaterialInstance::Set`) still protects as today.

## Cost

~One short session: the modal, the discovery walk (registry snapshot + chain
walk both exist), Load/Save rewrite, invalidation via the existing
`onAssetSaved` seam, `DocumentHost::ForEach` patch. No engine/DLL changes —
entirely editor-side.
