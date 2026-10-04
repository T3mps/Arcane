# One-shot: ARCANE_* C++ macros -> ARC_* (user decision 2026-10-03). Env vars keep ARCANE_.
# Kept as the record of Arcane S0-1; deleted in the commit after it.
import re, sys, pathlib
ENV = {"ARCANE_SDK","ARCANE_BUILD_MACHINE","ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE","ARCANE_IDE_DESK",
       "ARCANE_IDE_DESK_FILE","ARCANE_DIAG_DESK","ARCANE_BUILD_DESK","ARCANE_THUMBS_BLESS",
       "ARCANE_TEST_CAPTURE_DIR","ARCANE_RESAVE_SCENES","ARCANE_RECORD_TRAJECTORY","ARCANE_SHADER_DIR",
       "ARCANE_TP","ARCANE_BIN"}
EXPLICIT = {"ARCANE_DEBUG":"ARC_BUILD_DEBUG","ARCANE_RELEASE":"ARC_BUILD_RELEASE","ARCANE_DIST":"ARC_BUILD_DIST",
            "ARCANE_BUILD_DLL":"ARC_API_EXPORTS","ARCANE_CORE_BUILD_DLL":"ARC_CORE_API_EXPORTS"}
TOKEN = re.compile(r"\bARCANE_[A-Z0-9_]+")
DATED = re.compile(r"^\d{4}-\d{2}-\d{2}")
def repl(m):
    t = m.group(0)
    if t in ENV: return t
    return EXPLICIT.get(t, "ARC_" + t[len("ARCANE_"):])
args = sys.argv[1:]
allow_wip = "--include-wip" in args          # the user's yes (S0-1 carry): rename the TestComponent* spellings too
roots = [pathlib.Path(p) for p in args if p != "--include-wip"]
exts = {".cpp",".hpp",".h",".inl",".lua",".hlsl",".hlsli",".md",".json"}
changed = 0
for root in roots:
    files = [root] if root.is_file() else [p for p in root.rglob("*") if p.is_file() and p.suffix in exts]
    for p in files:
        if any(part in ("ThirdParty","bin","bin-int","out",".git",".superpowers","node_modules") for part in p.parts): continue
        # Dated documents (specs, plans, audits, research, records) keep their historical text.
        if "docs" in p.parts and (DATED.match(p.name) or any(s in p.parts for s in ("specs","plans","audits","research"))): continue
        if (p.name.startswith("TestComponent") and not allow_wip) or "Content" in p.parts:
            if TOKEN.search(p.read_bytes().decode("utf-8", "replace")):
                print("SKIPPED (user WIP):", p)
            continue
        raw = p.read_bytes()
        text = raw.decode("utf-8")
        new = TOKEN.sub(repl, text)
        if new != text:
            p.write_bytes(new.encode("utf-8")); changed += 1; print(p)
print("files changed:", changed)
