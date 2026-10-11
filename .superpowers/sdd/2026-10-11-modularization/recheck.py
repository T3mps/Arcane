# Re-run the sweep against the edited P1 tree with no masked inversion sites.
import sys, importlib.util
from pathlib import Path
here = Path(__file__).parent
spec = importlib.util.spec_from_file_location("sweep", here / "sweep.py"); s = importlib.util.module_from_spec(spec); spec.loader.exec_module(s)
base_owners = s.owners
def owners(path):
    p = s.rel(path)
    if p == "ArcaneCore/src/ArcaneCore.cpp": return ["ArcaneCore"]
    if p == "ArcaneClient/src/Arcane/Base/Api.hpp": return ["ArcaneClient"]   # retired in P2
    if p.startswith("ArcaneCore/src/Arcane/Base/Engine."): return ["ArcaneCore"]
    if p.startswith(("ArcaneCore/src/Arcane/Scene/EngineRoster", "ArcaneCore/src/Arcane/Scene/EngineResourceTypes")): return ["ArcaneEngine"]
    if p.startswith("ArcaneCore/src/Arcane/Scene/PhysicsDebugGeometry."): return ["ArcanePhysics2D"]
    if p.startswith("ArcaneClient/src/Arcane/Host/VerifyHooks."): return ["ArcaneVerify"]
    return base_owners(path)
s.owners = owners
REMOVED = set()  # P1 must remove each inversion in source.
base_includes = s.includes
def includes(path):
    for n, d, inc in base_includes(path):
        if inc == "Arcane/Base/Api.hpp" and s.rel(path).startswith("ArcaneClient/"): continue   # ARC_API retired in P2
        yield n, d, inc
s.includes = includes
C = "ArcaneCore"
s.DECLARED.update({
    "ArcaneScene": {C, "ArcaneAssets", "ArcaneSim"},
    "ArcanePhysics2D": {C, "ArcaneScene"},
    "ArcaneEngine": {C, "ArcaneScene", "ArcanePhysics2D", "ArcaneSim", "ArcaneAssets", "ArcaneNet"},
    "ArcaneRender": {C, "ArcaneScene", "ArcaneAssets", "ArcaneWindow", "ArcanePhysics2D"},
    "ArcaneImGui": {C, "ArcaneRender", "ImGui", "ArcaneInput", "ArcaneSim", "ArcaneWindow", "ArcaneClient"},
    "ArcaneClient": {C, "ArcaneEngine", "ArcaneRender", "ArcaneInput", "ArcaneAudio", "ArcaneWindow", "ArcaneAssets", "ArcaneScene", "ArcaneSim"},
    "ArcaneVerify": {C, "ArcaneAssets", "ArcaneClient", "ArcaneRender"},
    "ArcaneEdit": {C, "ArcaneScene"},
})
s.OUT = here / "sweep-p1.md"
s.main()
report = s.OUT.read_text(encoding="utf-8").split("## NETMODE CHECK")[0]
for section in ("UNASSIGNED", "MISSING DEP", "TYPE VIOLATIONS", "CYCLES"):
    content = report.split("## " + section + "\n", 1)[1].split("## ", 1)[0]
    if content.strip().splitlines()[0] != "None.":
        raise SystemExit(f"P1 sweep still has {section.lower()} findings")
report = report.replace("/sweep.py`", "/recheck.py`")
report += """## P1 NOTES

`GameModule.hpp` still has a direct `imgui.h` include under `ARC_GAME_HAS_IMGUI`;
this textual sweep reports it under ArcaneEngine. Its conditional is enabled for
in-tree game modules in P1 and becomes closure driven in P4. The new
ArcaneImGui -> ArcaneClient dependency is the HostOverlay implementation edge.
The `Arcane/Base/Api.hpp` include suppression is retained for its P2 retirement.

## VERDICT

UNASSIGNED none; MISSING DEP none; TYPE VIOLATIONS none; CYCLES none.
"""
s.OUT.write_text(report, encoding="utf-8")
