# Local patches

`cgltf.h` is vendored at the `v1.15` tag
(`bbeb5b0b070ddacddac6852fb72143eb68454937`) plus one hand-applied patch:

## jkuhlmann/cgltf#293 -- "Fix integer overflow in accessor validation"

**CVE-2026-32845.** `cgltf_validate`'s dense- and sparse-accessor size checks
compute `offset + stride * count` (and `offset + component_size * count`)
without checking for `size_t` overflow. A crafted glTF with a large
`byteOffset`/`byteStride`/`count` can wrap the required-size computation to a
small value, pass validation, and let a subsequent read walk past the end of
the buffer -- the exact bug `cgltf_validate` exists to catch (spec s4.5).

PR #293 (https://github.com/jkuhlmann/cgltf/pull/293, opened by zeux,
head commit `8211a9f12a729e7c2998bcc68f43c2ed2e5462d9`) fixes it but is
**unmerged** as of the `v1.15` tag and the current `jkuhlmann/cgltf` `master`
(`85cd62382dfea638278962690cf515023f33ed00`, verified by grepping master's
`cgltf.h` for `cgltf_calc_required_size` -- absent). Applied here by hand,
verbatim except for one line dropped (see below).

Applied hunk (in `cgltf_validate` and the new helper immediately after
`cgltf_calc_index_bound`):

- Added `cgltf_calc_required_size(offset, stride, count)`: returns `SIZE_MAX`
  (which then fails the size check) instead of wrapping when
  `offset + stride * count` would overflow `size_t`.
- Dense accessor path: `req_size` now goes through
  `cgltf_calc_required_size`, with the trailing `+ element_size` also
  overflow-checked.
- Sparse accessor path: `indices_req_size` and `values_req_size` now go
  through `cgltf_calc_required_size`.
- Added `CGLTF_ASSERT_IF(accessor->count == 0, ...)` and
  `CGLTF_ASSERT_IF(sparse->count == 0, ...)` -- the PR's companion fix for a
  benign underflow when `count == 0` (the dense-accessor path computes
  `accessor->count - 1`, which underflows to `SIZE_MAX` on an unsigned
  `cgltf_size` if `count` is 0).
- `data->accessors[i].component_type` / `.type` reads in the two pre-existing
  asserts were tightened to the already-local `accessor->` pointer (no
  behavior change, carried over from the PR diff for a clean apply).

**Not applied:** the PR's third hunk trims one line of trailing whitespace
in `cgltf_parse_json_diffuse_transmission` (unrelated formatting noise, not
part of the security fix) -- left out as out of scope for this patch.

Re-verify on any future cgltf bump: `grep -n cgltf_calc_required_size
ThirdParty/cgltf/cgltf.h` should find the helper; if a fresh upstream pull
already contains it (PR #293 merged), drop this file and the
`ARCANE LOCAL PATCH` comment in `cgltf.h`.
