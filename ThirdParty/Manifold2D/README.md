# Manifold2D

Manifold2D is Starworks' standalone 2D physics + geometry library -- one of
three independently-usable components in the Starworks stack (Astra = ECS,
Manifold2D = 2D physics, Arcane = full engine). A consumer that wants only 2D
physics can take Manifold2D on its own, without Arcane. The library is laid
out as `include/Manifold2D/` (public headers, all under the flat
`namespace Manifold2D` for `Core/` primitives and `Manifold2D::Physics` /
`Manifold2D::Geometry` for the solver and its geometry kernel) plus `src/`
(implementation `.cpp`, not on the public include path). Threading is an
injected seam: `Manifold2D::IWorkScheduler` is a `ParallelFor`-only interface
the host supplies its own task-scheduler adapter for; Manifold2D itself
creates no threads, and `SerialWorkScheduler` (runs each range inline as
worker 0) is the deterministic default when no scheduler is injected. The
library has zero external dependencies -- standard library only.
