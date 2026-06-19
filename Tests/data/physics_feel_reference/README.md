# physics_feel_reference

Archived **Lua-engine "feel-reference" traces** for the eventual Aphelyon client
port. These are validated in **Phase B**, not now.

## What this is (and is NOT)

The shipping client runs the **Lua** physics engine
(`Client/src/physics/*.lua`). The C++ `Arcane::Physics` core (Physics v2) is the
engine the client will eventually port to. To make that port *feel* the same to
players, we want a record of how the Lua overworld behaves on representative
scenarios -- slide paths along walls, query (raycast / shapecast / LOS) results,
the shape of a settle -- so the C++ port can be cross-checked against the
**experience** the live game already has.

These traces are:

- **A Phase-B reference, captured from the Lua engine.** They describe what the
  current (shipping) overworld feels like.
- **NOT a C++ correctness gate.** They are not loaded by any `[physics]` test.
  The C++ invariant gate is `Arcane/Tests/src/PhysicsInvariantsTest.cpp`
  (`[physics][invariant]`) plus the analytic V2 tests (PhysicsShapesV2 /
  PhysicsGjkV2 / PhysicsManifoldV2 / PhysicsRotation / PhysicsQueryRotation).
- **NOT the retired M6 oracle.** The old bit-match oracle
  (`physics_oracle/*.json`) was an M6 *porting scaffold* and was deleted in
  Physics v2 Task T8. Physics v2 re-baselined its gate to physics INVARIANTS +
  hand-derived analytic values + fresh goldens, so a bit-for-bit Lua match is no
  longer the contract. Feel-reference traces are a *Phase-B comparison aid*, not
  a pass/fail assertion on the C++ side.

## Format

JSON, one file per scenario group, numbers emitted with `%.17g` (full f64
round-trip). Each scenario records its inputs (scene geometry, the moving
shape + its sweep, the query) and the Lua engine's outputs (slide/contact path
samples, query hits). The exact schema is defined by the capture program (see
below) and is self-describing per scenario.

## How these are produced

`Client/src/tests/physics_oracle_capture/main.lua` is the capture program. It
runs under Love2D (LuaJIT) and writes its JSON output into THIS directory:

```
ThirdParty/love2d/lovec.exe Client/src/tests/physics_oracle_capture
```

> NOTE (T8): the capture program was retargeted from the deleted
> `physics_oracle/` dir to here and re-documented as emitting Phase-B
> feel-reference traces. It was **not** re-run at retarget time (no Love2D in the
> T8 environment); the captured fixtures are (re)generated in Phase B when the
> overworld port work begins. Until then this directory may be empty.

## When these get used

Phase B (the overworld / client port milestone). When the C++ `Arcane::Physics`
core drives the overworld, these traces are the side-by-side reference for
"does the port feel like the game players already know" -- compared with a
generous tolerance and human judgement, **not** asserted bit-for-bit.
