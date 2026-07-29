import { describe, it, expect } from "vitest";
import {
  isCompatible, filterProjects, coverFor, sortProjects, nextSort,
  projectKey, projectDir, projectNameError, projectPathPreview, resolveEngine,
  engineChipText, engineChipTitle, compatibilityNote, missingNote, menuItemsFor,
  type SortKey,
} from "./format";

describe("isCompatible", () => {
  // Exact parity with the inline rule this replaces (+page.svelte:139):
  //   !selectedEngine || p.engineAbi === 0 || p.engineAbi === selectedEngine.engineAbi
  it("matching abi is compatible", () => {
    expect(isCompatible(7, 7)).toBe(true);
  });
  it("differing abi is not compatible", () => {
    expect(isCompatible(5, 7)).toBe(false);
  });
  it("unknown project abi (0) is treated as compatible", () => {
    // 0 means the manifest did not state one -- we cannot prove a conflict,
    // so we must not brand it broken.
    expect(isCompatible(0, 7)).toBe(true);
  });
  it("no engine selected is compatible", () => {
    // Nothing to conflict with; the UI disables launching separately.
    expect(isCompatible(5, null)).toBe(true);
  });
  it("unknown abi with no engine is compatible", () => {
    expect(isCompatible(0, null)).toBe(true);
  });
});

describe("filterProjects", () => {
  const items = [
    { name: "Aphelyon", path: "D:/dev/starworks/Gacha/Aphelyon.arcproj" },
    { name: "SampleProject", path: "D:/dev/starworks/Arcane/Samples/SampleProject" },
    { name: "OldPrototype", path: "D:/dev/archive/OldPrototype.arcproj" },
  ];
  it("empty query returns everything", () => {
    expect(filterProjects(items, "")).toHaveLength(3);
  });
  it("whitespace-only query returns everything", () => {
    expect(filterProjects(items, "   ")).toHaveLength(3);
  });
  it("matches name case-insensitively", () => {
    expect(filterProjects(items, "aphel").map((i) => i.name)).toEqual(["Aphelyon"]);
  });
  it("matches path as well as name", () => {
    // Typing a folder you remember must find the project.
    expect(filterProjects(items, "archive").map((i) => i.name)).toEqual(["OldPrototype"]);
  });
  it("no match returns empty", () => {
    expect(filterProjects(items, "zzz")).toEqual([]);
  });
  it("does not mutate the input array", () => {
    const copy = [...items];
    filterProjects(items, "aphel");
    expect(items).toEqual(copy);
  });
});

describe("sortProjects", () => {
  // Recency-ordered input, the shape the Rust recents Vec arrives in.
  const p = (name: string, opened: string, abi: number, favorite = false, engine = "E") => ({
    name, lastOpenedUtc: opened, engineAbi: abi, favorite, engine,
  });
  const byEngine = (x: { engine: string }) => x.engine;

  it("favorites sit above the rest whatever the sort", () => {
    const items = [p("B", "300", 2), p("A", "200", 1), p("Z", "100", 3, true)];
    for (const key of ["opened", "name", "abi", "engine"] as SortKey[]) {
      expect(sortProjects(items, key, false, byEngine)[0].name).toBe("Z");
    }
  });
  it("opened compares numerically, not lexicographically", () => {
    // "999" > "1000" as strings -- the trap this test pins.
    const items = [p("Old", "999", 1), p("New", "1000", 1)];
    expect(sortProjects(items, "opened", true, byEngine).map((i) => i.name))
      .toEqual(["New", "Old"]);
  });
  it("a never-opened stamp sorts as oldest", () => {
    const items = [p("Never", "never", 1), p("Once", "50", 1)];
    expect(sortProjects(items, "opened", true, byEngine)[0].name).toBe("Once");
  });
  it("name sorts case-insensitively both directions", () => {
    const items = [p("banana", "1", 1), p("Apple", "2", 1)];
    expect(sortProjects(items, "name", false, byEngine).map((i) => i.name))
      .toEqual(["Apple", "banana"]);
    expect(sortProjects(items, "name", true, byEngine).map((i) => i.name))
      .toEqual(["banana", "Apple"]);
  });
  it("abi sorts numerically", () => {
    const items = [p("Ten", "1", 10), p("Two", "2", 2)];
    expect(sortProjects(items, "abi", false, byEngine).map((i) => i.name))
      .toEqual(["Two", "Ten"]);
  });
  it("engine sorts by the injected label", () => {
    const items = [
      p("A", "1", 1, false, "Zeta build"),
      p("B", "2", 1, false, "Alpha build"),
    ];
    expect(sortProjects(items, "engine", false, byEngine).map((i) => i.name))
      .toEqual(["B", "A"]);
  });
  it("ties keep their incoming recency order", () => {
    const items = [p("First", "9", 5), p("Second", "8", 5)];
    expect(sortProjects(items, "abi", false, byEngine).map((i) => i.name))
      .toEqual(["First", "Second"]);
  });
  it("does not mutate the input array", () => {
    const items = [p("B", "1", 1), p("A", "2", 2)];
    const copy = [...items];
    sortProjects(items, "name", false, byEngine);
    expect(items).toEqual(copy);
  });
});

describe("nextSort", () => {
  it("clicking the active column flips direction", () => {
    expect(nextSort("name", false, "name")).toEqual({ key: "name", desc: true });
    expect(nextSort("name", true, "name")).toEqual({ key: "name", desc: false });
  });
  it("a new column starts at its natural direction", () => {
    // Opened: newest first. Everything else: ascending.
    expect(nextSort("name", true, "opened")).toEqual({ key: "opened", desc: true });
    expect(nextSort("opened", true, "name")).toEqual({ key: "name", desc: false });
    expect(nextSort("opened", true, "abi")).toEqual({ key: "abi", desc: false });
    expect(nextSort("opened", true, "engine")).toEqual({ key: "engine", desc: false });
  });
});

describe("coverFor", () => {
  it("monogram is the first alphanumeric character, uppercased", () => {
    expect(coverFor("aphelyon", "x").monogram).toBe("A");
    expect(coverFor("  sample", "x").monogram).toBe("S");
    expect(coverFor("3d-demo", "x").monogram).toBe("3");
  });
  it("falls back to ? when there is no alphanumeric character", () => {
    expect(coverFor("---", "x").monogram).toBe("?");
    expect(coverFor("", "x").monogram).toBe("?");
  });
  it("accepts non-Latin letters as the monogram", () => {
    expect(coverFor("守护者", "x").monogram).toBe("守");
    expect(coverFor("éclair", "x").monogram).toBe("É");
  });
  it("still falls back for a name with no letter or digit", () => {
    expect(coverFor("--- ***", "x").monogram).toBe("?");
  });
  it("angle is deterministic for the same path", () => {
    // THE requirement: a card must not change appearance between launches.
    expect(coverFor("A", "D:/one").angle).toBe(coverFor("A", "D:/one").angle);
  });
  it("angle differs for different paths", () => {
    expect(coverFor("A", "D:/one").angle).not.toBe(coverFor("A", "D:/two").angle);
  });
  it("angle stays inside the intended band", () => {
    for (const p of ["D:/a", "D:/b", "D:/c", "D:/dev/x/y", "", "//?/UNC"]) {
      const a = coverFor("N", p).angle;
      expect(a).toBeGreaterThanOrEqual(100);
      expect(a).toBeLessThanOrEqual(200);
    }
  });
  it("angle does not depend on the name", () => {
    expect(coverFor("Zebra", "D:/one").angle).toBe(coverFor("Apple", "D:/one").angle);
  });
  it("a project keeps its cover when recorded as a manifest instead of a folder", () => {
    // Switching the Open dialog to pick the .arcproj re-records existing
    // projects by file path. Their cards must not change appearance.
    expect(coverFor("MyGame", "D:/Games/MyGame/MyGame.arcproj").angle)
      .toBe(coverFor("MyGame", "D:/Games/MyGame").angle);
  });
});

describe("projectDir", () => {
  it("drops the manifest, leaving the folder", () => {
    expect(projectDir("D:\\Games\\MyGame\\MyGame.arcproj")).toBe("D:\\Games\\MyGame");
    expect(projectDir("D:/Games/MyGame/MyGame.arcproj")).toBe("D:/Games/MyGame");
  });
  it("keeps the original spelling, unlike projectKey", () => {
    // projectKey lowercases and rewrites separators because it is an identity.
    // This value is displayed, so it must be the path the user actually has.
    expect(projectDir("D:\\Games\\MyGame\\MyGame.arcproj")).toBe("D:\\Games\\MyGame");
  });
  it("leaves a folder-shaped entry alone", () => {
    // Entries recorded before the dialog asked for a .arcproj are folders.
    expect(projectDir("D:/Games/MyGame")).toBe("D:/Games/MyGame");
  });
  it("ignores a trailing separator", () => {
    expect(projectDir("D:/Games/MyGame/")).toBe("D:/Games/MyGame");
  });
  it("ignores the extension's case", () => {
    expect(projectDir("D:/G/MyGame.ARCPROJ")).toBe("D:/G");
  });
  it("does not strip a folder that merely ends in .arcproj-like text", () => {
    expect(projectDir("D:/Games/MyGame.arcprojects")).toBe("D:/Games/MyGame.arcprojects");
  });
  it("keeps a bare manifest name when there is no folder to show", () => {
    // Returning "" here would render an empty path line, which reads as a bug.
    expect(projectDir("MyGame.arcproj")).toBe("MyGame.arcproj");
  });
});

describe("projectKey", () => {
  it("a manifest path and its folder collapse to one key", () => {
    expect(projectKey("D:/Games/MyGame/MyGame.arcproj")).toBe(projectKey("D:/Games/MyGame"));
  });
  it("folds separators and case the way the Rust side dedupes", () => {
    expect(projectKey("D:\\Games\\MyGame")).toBe(projectKey("d:/games/mygame"));
  });
  it("ignores a trailing separator", () => {
    expect(projectKey("D:/Games/MyGame/")).toBe(projectKey("D:/Games/MyGame"));
  });
  it("keeps distinct projects distinct", () => {
    expect(projectKey("D:/Games/A")).not.toBe(projectKey("D:/Games/B"));
  });
  it("a bare manifest with no folder keeps its own name as the key", () => {
    // Guards the empty-string branch: stripping the only component would
    // collapse every such path to "", giving them all one cover.
    expect(projectKey("/MyGame.arcproj")).toBe("/mygame.arcproj");
  });
  it("does not strip a folder that merely ends in .arcproj-like text", () => {
    expect(projectKey("D:/Games/notarcproj")).toBe("d:/games/notarcproj");
  });
});

describe("projectNameError", () => {
  it("accepts ordinary names", () => {
    expect(projectNameError("MyGame")).toBeNull();
    expect(projectNameError("My Game")).toBeNull();
    expect(projectNameError("3d-demo")).toBeNull();
    expect(projectNameError("game_2")).toBeNull();
  });
  it("rejects an empty or whitespace-only name", () => {
    expect(projectNameError("")).toBeTruthy();
    expect(projectNameError("   ")).toBeTruthy();
  });
  it("rejects path separators, which would escape the chosen folder", () => {
    expect(projectNameError("a/b")).toBeTruthy();
    expect(projectNameError("a\\b")).toBeTruthy();
  });
  it("rejects the other characters Windows forbids", () => {
    for (const c of ["<", ">", ":", '"', "|", "?", "*"]) {
      expect(projectNameError(`a${c}b`), `expected "${c}" to be rejected`).toBeTruthy();
    }
  });
  it("rejects a control character", () => {
    expect(projectNameError(`a${String.fromCharCode(7)}b`)).toBeTruthy();
  });
  it("rejects a trailing dot or space, which Windows silently strips", () => {
    // The folder on disk would not match the typed name.
    expect(projectNameError("MyGame.")).toBeTruthy();
    // Note the quoted trailing space survives trim() only inside the string.
    expect(projectNameError("MyGame .")).toBeTruthy();
  });
  it("rejects reserved DOS device names", () => {
    expect(projectNameError("CON")).toBeTruthy();
    expect(projectNameError("nul")).toBeTruthy();
    expect(projectNameError("COM1")).toBeTruthy();
    expect(projectNameError("lpt9.game")).toBeTruthy();
  });
  it("does not reject a name that merely starts with a reserved word", () => {
    expect(projectNameError("Console")).toBeNull();
    expect(projectNameError("Conquest")).toBeNull();
  });
  it("rejects a name long enough to threaten the path limit", () => {
    // The name appears twice in <dir>/<name>/<name>.arcproj.
    expect(projectNameError("a".repeat(65))).toBeTruthy();
    expect(projectNameError("a".repeat(64))).toBeNull();
  });
});

describe("resolveEngine", () => {
  const a = { id: "eng-a", engineAbi: 7 };
  const b = { id: "eng-b", engineAbi: 5 };
  const engines = [a, b];

  it("no pin follows the Hub default", () => {
    expect(resolveEngine(null, engines, a)).toEqual({
      engine: a, pinned: false, dangling: false,
    });
  });
  it("a live pin wins over the default", () => {
    expect(resolveEngine("eng-b", engines, a)).toEqual({
      engine: b, pinned: true, dangling: false,
    });
  });
  it("pinning to what the default already is still counts as pinned", () => {
    // The choice was made explicitly, so changing the default later must not
    // move this project -- the UI has to be able to show that.
    expect(resolveEngine("eng-a", engines, a).pinned).toBe(true);
  });
  it("a pin to an unregistered engine falls back and reports dangling", () => {
    expect(resolveEngine("eng-gone", engines, a)).toEqual({
      engine: a, pinned: false, dangling: true,
    });
  });
  it("no pin and no default resolves to nothing, not a crash", () => {
    expect(resolveEngine(null, [], null)).toEqual({
      engine: null, pinned: false, dangling: false,
    });
  });
  it("a dangling pin with no default still reports dangling", () => {
    expect(resolveEngine("eng-gone", [], null)).toEqual({
      engine: null, pinned: false, dangling: true,
    });
  });
});

describe("project labels", () => {
  it("an unpinned project is shown as following the default", () => {
    expect(engineChipText("Release", false, false)).toBe("Default: Release");
  });
  it("a pinned project shows its own engine, with no 'default' prefix", () => {
    expect(engineChipText("Debug", true, false)).toBe("Debug");
  });
  it("a dangling pin says so instead of naming the fallback", () => {
    // Naming the fallback would read as if the pin resolved.
    expect(engineChipText("Release", false, true)).toBe("Engine missing");
  });
  it("the tooltip distinguishes all three states", () => {
    const dflt = engineChipTitle("Release", false, false);
    const pin = engineChipTitle("Release", true, false);
    const gone = engineChipTitle("Release", false, true);
    expect(new Set([dflt, pin, gone]).size).toBe(3);
    expect(dflt).toContain("default");
    expect(pin).toContain("Always");
    expect(gone).toContain("no longer registered");
  });

  it("a compatible project shows its path", () => {
    expect(compatibilityNote(true, "D:/g/A.arcproj", 7, "Release", 7))
      .toBe("D:/g/A.arcproj");
  });
  it("an incompatible project NAMES the engine rather than saying 'the selected engine'", () => {
    // Regression: engines are per-project, so "the selected engine" described
    // the sidebar rather than the engine that would actually open this.
    const note = compatibilityNote(false, "D:/g/A.arcproj", 5, "Debug 0.1", 7);
    expect(note).toContain("abi 5");
    expect(note).toContain("Debug 0.1");
    expect(note).toContain("abi 7");
    expect(note).not.toContain("the selected engine");
  });

  it("a missing project names its stale path and both ways out", () => {
    // The copy must point at Locate AND Remove: one repairs the row, the other
    // retires it, and a user staring at a greyed project needs to know both.
    const note = missingNote("D:/g/A.arcproj");
    expect(note).toContain("D:/g/A.arcproj");
    expect(note).toContain("Locate");
    expect(note).toContain("Remove from list");
  });
});

describe("menuItemsFor", () => {
  it("a missing project offers exactly repair-or-retire", () => {
    const kinds = menuItemsFor(true, true).map((i) => i.kind);
    expect(kinds).toEqual(["locate", "forget"]);
  });
  it("a healthy project gets the full vocabulary, delete last and danger", () => {
    const items = menuItemsFor(false, true);
    expect(items.map((i) => i.kind))
      .toEqual(["reveal", "rename", "args", "forget", "delete"]);
    expect(items.at(-1)?.danger).toBe(true);
    expect(items.filter((i) => i.danger)).toHaveLength(1);
  });
  it("remove-from-list opens the below-the-line group in both states", () => {
    for (const missing of [true, false]) {
      const items = menuItemsFor(missing, true);
      expect(items.find((i) => i.kind === "forget")?.sep).toBe(true);
      expect(items.filter((i) => i.sep)).toHaveLength(1);
    }
  });
  it("the delete ellipsis follows the confirmation setting", () => {
    // An ellipsis promises a dialog; with confirmation off there is none.
    const label = (confirm: boolean) =>
      menuItemsFor(false, confirm).find((i) => i.kind === "delete")?.label;
    expect(label(true)).toBe("Delete project…");
    expect(label(false)).toBe("Delete project");
  });
});

describe("projectPathPreview", () => {
  it("shows the manifest the Rust side will actually write", () => {
    // create_project does root = dir.join(name), then writes <name>.arcproj.
    expect(projectPathPreview("D:\\Games", "MyGame")).toBe("D:\\Games\\MyGame\\MyGame.arcproj");
  });
  it("keeps forward slashes when that is what the path uses", () => {
    expect(projectPathPreview("D:/Games", "MyGame")).toBe("D:/Games/MyGame/MyGame.arcproj");
  });
  it("does not double a trailing separator", () => {
    expect(projectPathPreview("D:\\Games\\", "MyGame")).toBe("D:\\Games\\MyGame\\MyGame.arcproj");
  });
  it("treats a bare drive as a Windows path", () => {
    expect(projectPathPreview("D:", "MyGame")).toBe("D:\\MyGame\\MyGame.arcproj");
  });
  it("is empty until both halves are known", () => {
    expect(projectPathPreview("", "MyGame")).toBe("");
    expect(projectPathPreview("D:/Games", "")).toBe("");
    expect(projectPathPreview("D:/Games", "   ")).toBe("");
  });
});
