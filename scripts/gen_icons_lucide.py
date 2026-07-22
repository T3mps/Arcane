# Generate Arcane/ArcaneEditor/src/IconsLucide.h from the lucide codepoints map.
# Run from the repo root: python Arcane/scripts/gen_icons_lucide.py
import json, re, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root
CP   = os.path.join(ROOT, "Arcane", "data", "font", "lucide", "codepoints.json")
OUT  = os.path.join(ROOT, "Arcane", "ArcaneEditor", "src", "IconsLucide.h")

cps = json.load(open(CP, encoding="utf-8"))
values = [int(v) for v in cps.values()]
lo, hi = min(values), max(values)

lines = [
    "#pragma once",
    "// GENERATED from Arcane/data/font/lucide/codepoints.json by",
    "// Arcane/scripts/gen_icons_lucide.py -- do not edit by hand.",
    "// Lucide icon-font codepoint macros for ImGui (raw UTF-8 byte-escape narrow literals).",
    "",
    "#define ICON_LC_MIN 0x%04Xu" % lo,
    "#define ICON_LC_MAX 0x%04Xu" % hi,
    "",
]
for name in sorted(cps):
    cp    = int(cps[name])
    macro = "ICON_LC_" + re.sub(r"[^0-9A-Za-z]", "_", name).upper()
    utf8  = "".join("\\x%02X" % b for b in chr(cp).encode("utf-8"))
    lines.append('#define %s "%s"' % (macro, utf8))
lines.append("")

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines))
print("wrote", OUT, "(%d icons, U+%04X..U+%04X)" % (len(cps), lo, hi))
