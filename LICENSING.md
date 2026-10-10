# Arcane licensing, in plain English

Arcane is **source-available** under the **Starworks Source License**: the
Business Source License 1.1 with the Starworks parameters in [`LICENSE`](LICENSE).
This page explains what that means. If anything here and `LICENSE` disagree,
`LICENSE` is what counts.

## You can

- Read, build, modify and fork Arcane's source.
- Make games and interactive applications with Arcane, **including commercial
  ones**, and sell them. No royalty, no fee.
- Ship Arcane's compiled engine inside your game, and run servers for it.
- Write and share plugins, modules and extensions for Arcane, free or paid.
- Use Arcane inside your studio for development, testing and evaluation.

## You must

- **Credit Arcane** in every game or application you distribute, in its
  credits, an about screen, or a similar place players can reach:

  > Made with the Arcane Engine. Arcane (c) Ethan Temprovich / Starworks.

  A splash screen or logo is optional.
- Keep `LICENSE` with any copy of Arcane's source you pass on, modified or not.

## You cannot

- Offer Arcane, or a modified version of it, **as an engine product**: a game
  engine, editor, SDK, toolset or runtime that other people use to build or run
  their own games, apps or content. That applies whether it is free or paid,
  downloadable or hosted.

If you want to do something this does not allow, a separate commercial license
may be available: contact licensing@starworks.dev.

## Every version becomes MIT after four years

Each released version of Arcane converts automatically to the **MIT License**
four years after it is first published. After that, that version has no
restrictions beyond MIT's.

## Older versions stay MIT

Arcane was published under the MIT License before this license took effect.
Every revision before the commit that added this license (`5c120859`,
2026-10-10) remains available under MIT; this license applies from that commit
onward.

## Arcane's libraries are MIT

Astra (ECS), Manifold2D (2D physics) and Mosaic (job system) are separate
projects under the MIT License. Their own repositories and the copies vendored
under `ThirdParty/` keep that license.

## Third-party code

Arcane includes third-party libraries under their own licenses. See
[`NOTICE.md`](NOTICE.md).

## Contributing

Contributions to Arcane need a one-time Contributor License Agreement; see
[`CONTRIBUTING.md`](CONTRIBUTING.md).
