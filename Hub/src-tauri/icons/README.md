# Hub application icons

All of these come from `Arcane/data/images/arcane_logo.png` — the same source
the editor's icon is built from, so the Hub and `ArcaneEditor.exe` show one mark
in Explorer, the taskbar and Alt-Tab.

`icon.ico` is a **byte-for-byte copy** of `Arcane/ArcaneEditor/resources/arcane.ico`
rather than a fresh render of the logo. It is the file Windows shows for the
executable, and copying it means the two binaries carry the identical icon
instead of two lookalikes that could drift apart the next time either is
regenerated. It is multi-resolution (256/128/64/48/32/16).

`tauri.conf.json` references four of these (`32x32.png`, `128x128.png`,
`128x128@2x.png`, `icon.ico`). The `Square*Logo.png` and `StoreLogo.png` files
are for the MSIX/AppX bundle target, which this app does not build (`targets`
is `["nsis"]`) — they are kept in step anyway so the folder does not end up
half Arcane and half Tauri placeholder.

## Regenerating

The logo is 550x550, and every PNG target is smaller, so all of them are
downsamples — nothing here is upscaled. Do **not** run `tauri icon`: it wants a
1024x1024 source and would upscale the logo to get there.

```python
from PIL import Image
import pathlib, shutil

src = Image.open("data/images/arcane_logo.png").convert("RGBA")
out = pathlib.Path("Hub/src-tauri/icons")
sizes = {
    "32x32.png": 32, "128x128.png": 128, "128x128@2x.png": 256, "icon.png": 512,
    "StoreLogo.png": 50,
    **{f"Square{n}x{n}Logo.png": n for n in (30, 44, 71, 89, 107, 142, 150, 284, 310)},
}
for name, n in sizes.items():
    src.resize((n, n), Image.LANCZOS).save(out / name, "PNG")
shutil.copyfile("ArcaneEditor/resources/arcane.ico", out / "icon.ico")
```

Run it from `Arcane/`.

**After changing `icon.ico`, touch `src-tauri/build.rs` before rebuilding.**
`tauri_build::build()` writes an `out/resource.rc` that names the `.ico` by
absolute path and compiles it with `rc.exe`, but it does not emit a
`rerun-if-changed` for the icon — so cargo sees no reason to re-run the build
script and links the previously compiled `resource.lib`, silently keeping the
old icon in a build that otherwise looks completely fresh.

`cargo clean -p arcane_hub` does **not** fix this. It was tried: it removed
3.4 GB and the very next build still produced the old icon, because it leaves
`bin-int/hub-cargo/release/build/arcane_hub-*/out/` in place. Check that
directory's `resource.lib` timestamp if an icon change seems not to take.

The in-app titlebar mark is separate — that is `Hub/static/logo.png`, a copy of
the same source loaded by `WindowChrome.svelte`.
