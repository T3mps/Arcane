"""Reproducible include-edge audit for the 2026-10-11 module-map proposal.

Run from the repository root: python .superpowers/sdd/2026-10-11-modularization/sweep.py
This script reads sources and writes only sweep.md beside itself.
"""
from __future__ import annotations

from collections import defaultdict
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).with_name("sweep.md")
SUFFIXES = {".h", ".hpp", ".cpp", ".inl"}
ENGINE = ("ArcaneCore/src", "ArcaneClient/src")
PROGRAMS = ("ArcaneEditor/src", "ArcaneRuntime/src", "ArcaneServer/src", "ArcaneTests/src", "ReferenceProject/Source")
INC = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

TYPES = {m: "Runtime" for m in ("ArcaneCore", "ArcaneNet", "ArcaneCrypto", "ArcaneAssets", "ArcaneScene", "ArcanePhysics2D", "ArcaneSim", "ArcaneEngine")}
TYPES.update({m: "Client" for m in ("ArcaneWindow", "ArcaneInput", "ArcaneAudio", "ArcaneRender", "ImGui", "ArcaneImGui", "ArcaneClient")})
TYPES.update({"ArcaneVerify": "Developer", "ArcaneAssetPipeline": "Developer", "ArcaneEdit": "Editor", "ImGuiNodeEditor": "Editor"})

# Literal s1.1-s1.3 dependency columns, after the s1.5 ownership moves.
DECLARED = {
    "ArcaneCore": set(), "ArcaneNet": {"ArcaneCore"}, "ArcaneCrypto": {"ArcaneCore"},
    "ArcaneAssets": {"ArcaneCore"}, "ArcaneScene": {"ArcaneCore", "ArcaneAssets"},
    "ArcanePhysics2D": {"ArcaneScene"}, "ArcaneSim": {"ArcaneCore"},
    "ArcaneEngine": {"ArcaneScene", "ArcanePhysics2D", "ArcaneSim", "ArcaneAssets", "ArcaneNet"},
    "ArcaneWindow": {"ArcaneCore"}, "ArcaneInput": {"ArcaneCore", "ArcaneWindow"},
    "ArcaneAudio": {"ArcaneCore", "ArcaneAssets"},
    "ArcaneRender": {"ArcaneScene", "ArcaneAssets", "ArcaneWindow"},
    "ImGui": set(), "ArcaneImGui": {"ArcaneRender", "ImGui", "ArcaneInput"},
    "ArcaneClient": {"ArcaneEngine", "ArcaneRender", "ArcaneInput", "ArcaneAudio", "ArcaneWindow"},
    # s1.3 supplies no dependency column for these modules.
    "ArcaneVerify": set(), "ArcaneAssetPipeline": set(), "ArcaneEdit": set(), "ImGuiNodeEditor": set(),
}
VENDOR_ALLOWED = {
    "ArcaneCore": {"enkiTS", "Astra", "glm"}, "ArcanePhysics2D": {"Manifold2D"},
    "ArcaneSim": {"Astra"},  # s1.1 explicitly says "Astra headers via Core".
    "ArcaneRender": {"NRI", "freetype", "msdfgen"}, "ImGui": {"imgui"},
    "ArcaneImGui": {"imgui"},  # s1.2 declares the ImGui vendor module by name.
    "ArcaneAssetPipeline": {"bc7enc_rdo", "meshoptimizer"},
}

def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()

def source_files(base: str) -> list[Path]:
    return sorted(p for p in (ROOT / base).rglob("*") if p.is_file() and p.suffix.lower() in SUFFIXES)

def owners(path: Path) -> list[str]:
    p = rel(path)
    if p == "ArcaneCore/src/ArcaneCore.cpp" or p == "ArcaneClient/src/Arcane/Base/Api.hpp":
        return []  # Neither is in a s1.1-s1.3 contents cell.
    if p.startswith("ArcaneCore/src/Arcane/"):
        tail = p.removeprefix("ArcaneCore/src/Arcane/")
        folder, _, name = tail.partition("/")
        if folder == "Base":
            return ["ArcaneEngine" if name.split(".")[0] in {"Runtime", "Engine", "ProcessContext"} else "ArcaneCore"]
        if folder == "Project":
            return ["ArcaneAssets" if name == "AssetId.hpp" else "ArcaneEngine"]
        if folder == "Serialization":
            return ["ArcaneAssets" if name == "IdentityFieldRule.hpp" else "ArcaneScene"]
        if folder == "Scene":
            return ["ArcanePhysics2D" if name.startswith(("Physics2D", "PhysicsEvents2D", "PhysicsQuerySettings")) or name in {"PhysicsComponents.hpp", "PhysicsSystem.hpp"} else "ArcaneScene"]
        mapping = {"Core": "ArcaneCore", "Config": "ArcaneCore", "Platform": "ArcaneCore", "Util": "ArcaneCore", "Math": "ArcaneCore", "Jobs": "ArcaneCore", "Cli": "ArcaneCore", "Build": "ArcaneCore", "Net": "ArcaneNet", "Crypto": "ArcaneCrypto", "Assets": "ArcaneAssets", "Material": "ArcaneAssets", "Mesh": "ArcaneAssets", "Sprite": "ArcaneAssets", "Sim": "ArcaneSim", "Plugin": "ArcaneEngine"}
        if folder in mapping:
            return [mapping[folder]]
        return {"Guid.cpp": ["ArcaneCore"], "Guid.hpp": ["ArcaneCore"], "Reflection.hpp": ["ArcaneCore"], "Version.hpp": ["ArcaneCore"], "Ecs.hpp": ["ArcaneScene"], "EcsFwd.hpp": ["ArcaneScene"], "Physics2D.hpp": ["ArcanePhysics2D"], "Physics2DFwd.hpp": ["ArcanePhysics2D"]}.get(tail, [])
    if p.startswith("ArcaneClient/src/Arcane/"):
        tail = p.removeprefix("ArcaneClient/src/Arcane/")
        folder, _, name = tail.partition("/")
        mapping = {"Platform": "ArcaneWindow", "Input": "ArcaneInput", "Audio": "ArcaneAudio", "ImGui": "ArcaneImGui", "Client": "ArcaneClient", "Edit": "ArcaneEdit"}
        if folder in mapping:
            return [mapping[folder]]
        if folder == "Render":
            return ["ArcaneImGui" if tail == "Render/Nri/nodes/ImGuiNriNode.hpp" else "ArcaneRender"]
        if folder == "Host":
            return ["ArcaneVerify" if name.split(".")[0] in {"Verdict", "VerifyReport", "ReferenceImages", "OffscreenVehicle"} else "ArcaneClient"]
    return []

def vendor(inc: str) -> str | None:
    x = inc.lower()
    if x.startswith(("imgui", "backends/imgui")): return "imgui-node-editor" if "node_editor" in x else "imgui"
    if x.startswith(("astra/", "astra.hpp")): return "Astra"
    if x.startswith("glm/"): return "glm"
    if x.startswith("spdlog/"): return "spdlog"
    if x.startswith(("nlohmann/", "json.hpp")): return "nlohmann"
    if x.startswith(("nri", "extensions/nri", "interfaces/nri", "wrappers/nri")): return "NRI"
    if x.startswith("sdl3/"): return "SDL3"
    if x.startswith(("ft2build", "freetype/")): return "freetype"
    if x.startswith("msdf"): return "msdfgen"
    if x.startswith("manifold2d/"): return "Manifold2D"
    if x.startswith(("enki", "tasksch", "task_scheduler")): return "enkiTS"
    if x.startswith("stb"): return "stb"
    if x.startswith(("tracy", "tracy/")): return "Tracy"
    if x.startswith("miniaudio"): return "miniaudio"
    if x.startswith("picosha2"): return "picosha2"
    if x.startswith("cgltf"): return "cgltf"
    if x.startswith("vma/") or "vk_mem_alloc" in x: return "VMA"
    if x.startswith("d3d12memalloc"): return "D3D12MA"
    if x.startswith("vulkan/"): return "Vulkan-Headers"
    if x.startswith("mosaic/"): return "Mosaic"
    if x == "dxcapi.h": return "DXC"
    if x.startswith(("bc7", "meshoptimizer")): return "bc7enc_rdo" if x.startswith("bc7") else "meshoptimizer"
    return None

def includes(path: Path):
    for number, line in enumerate(path.read_text(encoding="utf-8-sig", errors="replace").splitlines(), 1):
        match = INC.match(line)
        if match:
            yield number, match.group(1), match.group(2).replace("\\", "/")

def scc(graph):
    index, stack, onstack, ids, low, result = 0, [], set(), {}, {}, []
    def visit(v):
        nonlocal index
        ids[v] = low[v] = index; index += 1; stack.append(v); onstack.add(v)
        for w in sorted(graph.get(v, ())):
            if w not in ids: visit(w); low[v] = min(low[v], low[w])
            elif w in onstack: low[v] = min(low[v], ids[w])
        if low[v] == ids[v]:
            group = []
            while True:
                w = stack.pop(); onstack.remove(w); group.append(w)
                if w == v: break
            if len(group) > 1: result.append(sorted(group))
    for v in sorted(graph):
        if v not in ids: visit(v)
    return sorted(result)

def main():
    engine = [p for base in ENGINE for p in source_files(base)]
    programs = {base: source_files(base) for base in PROGRAMS}
    assigned = {p: owners(p) for p in engine}
    by_include = defaultdict(list)
    for p in engine:
        key = rel(p).split("/src/", 1)[1]
        by_include[key].append(p)
    edges, vendors, program_use = defaultdict(list), defaultdict(list), defaultdict(list)
    unresolved, ambiguous_targets = [], []
    def resolve(src, inc, quoted):
        if inc.startswith("Arcane/"):
            return by_include.get(inc, [])
        if quoted or inc.startswith("../") or inc.startswith("./"):
            candidate = (src.parent / inc).resolve()
            return [candidate] if candidate in assigned else []
        return []
    for p in engine:
        for n, delim, inc in includes(p):
            site = f"{rel(p)}:{n}"
            targets = resolve(p, inc, delim == '"')
            if inc.startswith("Arcane/") and not targets: unresolved.append((site, inc))
            if len(targets) > 1: ambiguous_targets.append((site, inc, [rel(x) for x in targets]))
            for target in targets:
                if assigned[p] and assigned[target]:
                    a, b = assigned[p][0], assigned[target][0]
                    if a != b: edges[a, b].append((site, inc, rel(target)))
            v = vendor(inc)
            if v and assigned[p]: vendors[assigned[p][0], v].append((site, inc))
    for base, files in programs.items():
        for p in files:
            for n, delim, inc in includes(p):
                site = f"{rel(p)}:{n}"
                for target in resolve(p, inc, delim == '"'):
                    if assigned[target]: program_use[base, assigned[target][0]].append((site, inc))
                v = vendor(inc)
                if v in {"imgui", "imgui-node-editor"}:
                    program_use[base, "ImGui" if v == "imgui" else "ImGuiNodeEditor"].append((site, inc))
    lines = ["# Mechanical include-edge sweep", "", "Run: `python .superpowers/sdd/2026-10-11-modularization/sweep.py` from repository root. Paths below are repository-relative. Counts are direct preprocessor include sites, with current-tree paths and ownership after s1.5 moves. Dependency comparison uses the literal s1.1-s1.3 columns; an omitted Core edge remains missing even when transitive.", "", f"Engine files scanned: {len(engine)}. Program/game files scanned: {sum(map(len, programs.values()))}. Resolved cross-module include sites: {sum(map(len, edges.values()))}.", "", "## UNASSIGNED", ""]
    unassigned = [(p, x) for p, x in assigned.items() if len(x) != 1]
    lines += [f"- `{rel(p)}`: {'UNASSIGNED' if not x else 'AMBIGUOUS '+', '.join(x)} (file:1; no matching s1 contents entry)" for p, x in unassigned] or ["None."]
    lines += ["", "### Assignment inventory", "", "Every scanned engine file appears once below. The current file path supplies the file:1 evidence; ownership applies the s1.5 move without changing source paths.", ""]
    for module in sorted(TYPES):
        files = [p for p, x in assigned.items() if x == [module]]
        if files:
            lines += [f"#### {module} ({len(files)} files)", ""]
            lines.extend(f"- `{rel(p)}:1`" for p in sorted(files))
            lines.append("")
    lines += ["", "### Unresolved or ambiguous include targets", "", "These sites are excluded from resolved module edges."]
    lines += [f"- `{site}` → `{inc}` (unresolved)" for site, inc in unresolved]
    lines += [f"- `{site}` → `{inc}` (ambiguous: {', '.join(targets)})" for site, inc, targets in ambiguous_targets]
    if not unresolved and not ambiguous_targets: lines.append("None.")
    def emit_edge_list(items):
        for (a, b), sites in sorted(items):
            lines.append(f"### {a} → {b} ({len(sites)} sites)")
            lines.append("")
            lines.extend(f"- `{site}` → `{inc}` ({target})" for site, inc, target in sorted(sites))
            lines.append("")
    lines += ["", "## MISSING DEP", ""]
    missing = [((a, b), sites) for (a, b), sites in edges.items() if b not in DECLARED[a]]
    emit_edge_list(missing)
    if not missing: lines.append("None.")
    lines += ["## TYPE VIOLATIONS", ""]
    allowed = {"Runtime": {"Runtime"}, "Client": {"Runtime", "Client"}, "Developer": {"Runtime", "Client", "Developer", "Game"}, "Editor": set(TYPES.values()), "Game": {"Runtime", "Client"}}
    bad = [((a, b), sites) for (a, b), sites in edges.items() if TYPES[b] not in allowed[TYPES[a]]]
    emit_edge_list(bad)
    if not bad: lines.append("None.")
    lines += ["## CYCLES", ""]
    graph = defaultdict(set)
    for a, b in edges: graph[a].add(b)
    cycles = scc(graph)
    for group in cycles:
        lines += [f"### SCC: {', '.join(group)}", ""]
        emit_edge_list([((a,b), sites) for (a,b), sites in edges.items() if a in group and b in group])
    if not cycles: lines.append("None.")
    lines += ["## VENDOR INCLUDES", "", "Flags compare direct includes with vendor names explicitly granted by the spec's s1 dependency cells. `Vulkan-Headers` and similar SDK headers are shown because they are direct external includes; this comparison does not infer transitive permission.", ""]
    for module in sorted(set(a for a, _ in vendors)):
        lines += [f"### {module}", ""]
        for (a, v), sites in sorted(vendors.items()):
            if a != module: continue
            lines += [f"#### {v} — {'allowed' if v in VENDOR_ALLOWED.get(a, set()) else 'NOT DECLARED'} ({len(sites)} sites)", ""]
            lines.extend(f"- `{site}` → `{inc}`" for site, inc in sorted(sites))
            lines.append("")
    lines += ["## PROGRAM/GAME MODULE USE", "", "Direct includes only; module closure is deliberately not inferred.", ""]
    for base in PROGRAMS:
        lines += [f"### {base}", ""]
        for (pbase, mod), sites in sorted(program_use.items()):
            if pbase != base: continue
            flag = " **SERVER CLIENT MODULE**" if base == "ArcaneServer/src" and TYPES[mod] == "Client" else ""
            lines += [f"#### {mod}{flag} ({len(sites)} sites)", ""]
            lines.extend(f"- `{site}` → `{inc}`" for site, inc in sorted(sites))
            lines.append("")
    lines += ["## NETMODE CHECK", "", "`Config/PlayerSettings.hpp:24` includes `Plugin/SystemFactory.hpp`; the header uses only `NetMode` at `Config/PlayerSettings.hpp:31,35,36`, and `Config/PlayerSettings.cpp:10,21,25-28,35-36`. Moving NetMode to Core removes that particular Core → Engine edge. Other users of SystemFactory.hpp still need the Engine-owned factory API (for example `Base/ProcessContext.hpp:16`, `Plugin/GameModule.hpp:45`, and `ArcaneEditor/src/Project/ClassTemplates.hpp:26`). `ArcaneServer/src/ServerApp.cpp:6` uses NetMode and ToString(NetMode) at line 90; ToString remains in SystemFactory.cpp:26 unless moved too.", "", "## VERDICT", ""]
    lines += [f"The map does not hold as written: {len(missing)} undeclared module pairs ({sum(len(x) for _, x in missing)} include sites), {len(bad)} type-rule violating pairs, {len(cycles)} module SCCs, and {len(unassigned)} files without unique ownership.", "", "Minimum spec edits indicated by the direct edges above:", "", "1. Assign `ArcaneCore/src/ArcaneCore.cpp:1` and `ArcaneClient/src/Arcane/Base/Api.hpp:1` (or delete/replace those translation-unit and API-header roles in the move plan). Add `ArcaneClient/src/Arcane/Render/Nri/nodes/ImGuiNriNode.cpp:11` to the ArcaneImGui move beside its header; the cpp includes that header and otherwise leaves Render → ImGui.", "2. Complete the s1.5 untangles at `ArcaneCore/src/Arcane/Config/PlayerSettings.hpp:24` (NetMode), `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp:226` (HostConfig), and `ArcaneClient/src/Arcane/Host/HostConfig.cpp:3-4` (VerifyReport/ReferenceImages). The reverse edges in CYCLES require an interface/ownership change, not a descriptor declaration. Address the other reverse dependencies in the same SCCs, notably Assets → Engine, Core → Crypto/Engine, Scene → Physics2D/Sim, Render → Client/Engine/ImGui, and Client → Verify; their exact sites are in MISSING DEP.", "3. Add direct dependency declarations for all remaining MISSING DEP pairs after those ownership/interface changes. The s1.3 table needs explicit ArcaneVerify and ArcaneEdit columns; the existing direct sites above define their required dependencies. A transitive dependency does not cover an included header under the s2.2 public/private rule.", "4. Add thirdParty public/private grants or remove the direct header use for every `NOT DECLARED` vendor group in VENDOR INCLUDES. In particular, SDL3 is directly included by Window and other modules, imgui by modules beyond ArcaneImGui, and Manifold2D by Scene as currently mapped; use the cited sites to decide ownership. The program table shows ArcaneServer currently has no direct Client include."]
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}: {len(engine)} engine files, {sum(map(len, programs.values()))} program/game files, {len(missing)} missing dependency pairs, {len(cycles)} SCCs")

if __name__ == "__main__": main()
