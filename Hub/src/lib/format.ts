// The Hub's only branching logic, kept out of markup so it can be unit-tested.
// Everything else in src/lib is presentational.

/**
 * Whether `projectAbi` will open under `engineAbi`.
 *
 * Exact parity with the rule this replaces. Two permissive cases are
 * deliberate: abi 0 means the manifest did not state one (we cannot prove a
 * conflict, so we must not brand it broken), and a null engine means nothing
 * is selected to conflict with -- the UI gates launching separately.
 */
export function isCompatible(projectAbi: number, engineAbi: number | null): boolean {
  if (engineAbi === null) return true;
  if (projectAbi === 0) return true;
  return projectAbi === engineAbi;
}

/** Case-insensitive substring over BOTH name and path; blank query passes all. */
export function filterProjects<T extends { name: string; path: string }>(
  items: T[],
  query: string,
): T[] {
  const q = query.trim().toLowerCase();
  if (!q) return items.slice();
  return items.filter(
    (i) => i.name.toLowerCase().includes(q) || i.path.toLowerCase().includes(q),
  );
}

export type Cover = { monogram: string; angle: number };

/**
 * Cover art inputs for a project card: a monogram from the NAME and a gradient
 * angle from the PATH.
 *
 * Keyed on path, not name, because the path is the identity (two projects may
 * share a display name) and because it must be stable: a card that changed
 * appearance between launches would read as a bug. FNV-1a is used purely
 * because it is short, deterministic, and dependency-free -- no hash quality
 * is required here.
 */
export function coverFor(name: string, path: string): Cover {
  const ch = [...name].find((c) => /[a-z0-9]/i.test(c));
  const monogram = ch ? ch.toUpperCase() : "?";

  let h = 0x811c9dc5;
  for (let i = 0; i < path.length; i++) {
    h ^= path.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  // 100..200 degrees keeps every gradient diagonal and on-brand; a full 0..360
  // sweep would put some covers in flat vertical/horizontal bands.
  const angle = 100 + (h % 101);
  return { monogram, angle };
}
