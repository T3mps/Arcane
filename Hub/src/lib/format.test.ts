import { describe, it, expect } from "vitest";
import { isCompatible, filterProjects, coverFor } from "./format";

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
});
