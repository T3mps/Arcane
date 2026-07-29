<script module lang="ts">
  // Stroke icons in Lucide's 24x24 geometry, matching the vocabulary the Arcane
  // Editor already uses (Arcane/ArcaneEditor's IconsLucide.h).
  //
  // These replace Unicode geometric glyphs (U+25A3, U+25C8, U+2699, U+25A6,
  // U+2630). Those could not be sized to match the text: a glyph occupies only
  // part of its em box, so a 13px glyph renders far smaller than 13px of text
  // beside it, and it falls back to whatever symbol font the system happens to
  // have -- which is neither the UI font nor consistent between machines.
  //
  // Path data only, no <rect>/<circle>, so one loop renders every icon.
  export type IconName =
    "folder" | "box" | "layers" | "gear" | "grid" | "list" | "x" | "ellipsis" | "star"
    | "plus" | "chevron-down" | "check" | "arrow-up" | "arrow-down" | "search";

  const PATHS: Record<IconName, string[]> = {
    folder: [
      "M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z",
    ],
    box: [
      "M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16Z",
      "m3.3 7 8.7 5 8.7-5",
      "M12 22V12",
    ],
    // Stacked layers, not a second box: Engines already owns the box, and two
    // near-identical silhouettes in one nav is worse than no icon at all.
    layers: [
      "m12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83Z",
      "m6.08 9.5-3.5 1.6a1 1 0 0 0 0 1.81l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9a1 1 0 0 0 0-1.83l-3.5-1.59",
      "m6.08 14.5-3.5 1.6a1 1 0 0 0 0 1.81l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9a1 1 0 0 0 0-1.83l-3.5-1.59",
    ],
    gear: [
      "M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z",
      "M15 12a3 3 0 1 1-6 0 3 3 0 0 1 6 0",
    ],
    grid: [
      "M3 3h7v7H3z",
      "M14 3h7v7h-7z",
      "M14 14h7v7h-7z",
      "M3 14h7v7H3z",
    ],
    list: [
      "M3 6h.01", "M3 12h.01", "M3 18h.01",
      "M8 6h13", "M8 12h13", "M8 18h13",
    ],
    // Dismissal. Distinct from WindowChrome's close mark, which is drawn to the
    // native 12px caption metric rather than in this 24-unit geometry -- the
    // window control has to match Windows, an in-app control has to match the
    // other in-app icons.
    x: ["M18 6 6 18", "m6 6 12 12"],
    // Favorite. The filled state is CSS (`fill: currentColor` on the button's
    // `.on` class), so one path serves both.
    star: [
      "M11.525 2.295a.53.53 0 0 1 .95 0l2.31 4.679a2.123 2.123 0 0 0 1.595 1.16l5.166.756a.53.53 0 0 1 .294.904l-3.736 3.638a2.123 2.123 0 0 0-.611 1.878l.882 5.14a.53.53 0 0 1-.771.56l-4.618-2.428a2.122 2.122 0 0 0-1.973 0L6.396 21.01a.53.53 0 0 1-.77-.56l.881-5.139a2.122 2.122 0 0 0-.611-1.879L2.16 9.795a.53.53 0 0 1 .294-.906l5.165-.755a2.122 2.122 0 0 0 1.597-1.16z",
    ],
    // Lucide draws MoreHorizontal as three circles; these are zero-length
    // strokes with the round linecap this file already sets, which renders the
    // same three dots through the one path loop.
    ellipsis: ["M5 12h.01", "M12 12h.01", "M19 12h.01"],
    // The toolbar pair (Unity Hub's grammar): plus marks the primary create,
    // chevron-down marks a button that opens a menu instead of acting.
    plus: ["M5 12h14", "M12 5v14"],
    "chevron-down": ["m6 9 6 6 6-6"],
    // The current choice in a pick-one Dropdown menu.
    check: ["M20 6 9 17l-5-5"],
    // Sort DIRECTION. Full arrows, not chevrons or triangles: the sorter's
    // trigger already wears a chevron-down for "opens a menu", and a second
    // downward mark beside it meaning "descending" read as a stutter (visual
    // review 2026-07-29) -- an arrow is a different word.
    "arrow-up": ["m5 12 7-7 7 7", "M12 19V5"],
    "arrow-down": ["M12 5v14", "m19 12-7 7-7-7"],
    // Lucide's Search, with the lens circle as a two-arc path (this file's
    // one loop draws paths only, no <circle> elements).
    search: ["M11 3a8 8 0 1 0 0 16 8 8 0 1 0 0-16", "m21 21-4.35-4.35"],
  };
</script>

<script lang="ts">
  // aria-hidden always: every call site is a control that already carries its
  // own accessible name, so announcing the icon would just duplicate it.
  let { name, size = 18 }: { name: IconName; size?: number } = $props();
</script>

<svg class="icon" width={size} height={size} viewBox="0 0 24 24" fill="none"
     stroke="currentColor" stroke-width="2" stroke-linecap="round"
     stroke-linejoin="round" aria-hidden="true">
  {#each PATHS[name] as d}<path {d} />{/each}
</svg>

<style>
  /* flex:none so a shrinking flex row never squashes the icon, and block so it
     does not sit on the text baseline with descender space under it. */
  .icon { display: block; flex: none; }
</style>
