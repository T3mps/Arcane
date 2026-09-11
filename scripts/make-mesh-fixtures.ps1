<#
    F2c Task 2: the checked-in glTF fixture corpus's generator (spec s9).

    This script is the SOURCE OF TRUTH for ArcaneTests/data/gltf/*. The committed
    binaries are its output, never hand-typed. Every vertex/index/matrix number
    used below is a LITERAL in this file -- nothing is base64-typed by hand and
    nothing is fetched from anywhere.

    DETERMINISM CLAUSE: this script takes no input, reads no clock, and emits no
    randomness, so two runs -- on this machine or any other -- produce
    byte-identical files. That is the corpus's whole value as a determinism
    fixture (spec s9's byte-identity test); Step 5 of the Task 2 brief proves it
    via `git status --porcelain` before and after a re-run. Encoding is pinned to
    UTF-8 with NO byte-order mark and LF-only line endings for every text file
    ([System.Text.UTF8Encoding]::new($false) + an explicit CR-strip), and every
    JSON object handed to ConvertTo-Json is an [ordered] hashtable/dictionary so
    key order -- and therefore the serialized bytes -- cannot drift between runs
    or hosts (PowerShell's ConvertTo-Json over a plain @{} hashtable has
    UNSTABLE key order; [ordered]@{} is mandatory throughout this file).

    THE CORPUS (eleven fixtures, thirteen files -- nested.gltf ships with a
    sibling nested.bin, external_bin.glb with a sibling external_bin.bin):

      single.glb          1 mesh, 1 primitive, a unit quad (4 verts / 2 tris),
                           material "SingleMat". The happy path (spec s5.3
                           determinism).

      multi.glb            1 mesh, 3 primitives: three unit quads in a strip
                           (x [0,1], [1,2], [2,3], z [0,1], y=0, all facing +y),
                           4 verts each = kMultiGlbRawVertexCount = 12 RAW
                           vertices. The two shared edges (x=1 between quad A/B,
                           x=2 between quad B/C) duplicate 2 verts each, so a
                           meshoptimizer vertex-remap dedupes to 8 UNIQUE
                           vertices (strictly below the raw count) -- proving
                           the dedup pass actually fires. For that dedup to
                           fire, the duplicated corners must be BIT-IDENTICAL in
                           every attribute: this fixture carries POSITION and
                           NORMAL only and DELIBERATELY HAS NO TEXCOORD_0 AT ALL
                           (the chosen option -- simpler than proving two
                           primitives' UVs are perfectly continuous across a
                           shared edge; the importer's missing-UV fallback of
                           (0,0) is itself bit-identical across primitives, so
                           it does not defeat the dedup). Materials, in
                           primitive order, are "Metal", "Metal", "Paint" --
                           A1's dedup pin: sections 0 and 1 name the SAME
                           material and must share one slotIndex; section 2
                           names a different material and gets its own slot.

      nested.gltf +        A 2-level node hierarchy sharing an EXTERNAL buffer
      nested.bin           via "uri":"nested.bin" (exercises spec s5.4's
                           external-buffer cook-key hashing -- editing the .bin
                           must move the cook key). Parent node: translation
                           (1,0,3), rotation identity, scale (0.5,1,0.5).
                           Child node: translation (0,0,0), rotation identity,
                           scale (2,1,2), holding a unit quad in the XZ plane
                           at y=0 (local corners x in [0,1], z in [0,1]).
                           Composing parent-TRS x child-TRS bakes the quad to
                           EXACTLY x in [1,2], z in [3,4] (y stays 0, a
                           zero-thickness AABB on that axis) -- an unbaked
                           import would leave the quad at the local origin
                           instead. These four numbers (x min 1, x max 2, z min
                           3, z max 4) are the fixture's baked-AABB contract.

      mirrored.glb         One node, "scale":[-1,1,1] (a genuinely negative
                           determinant), TWO primitives in this fixed ORDER:
                             section[0]: a quad at y=+1 facing +y, WITH an
                               authored NORMAL accessor -- (0,1,0) at all four
                               corners, consistent with CCW winding *before*
                               the mirror (so the post-import winding-flip
                               predicate is non-vacuous: the authored normal
                               only agrees with the geometric one if the
                               importer actually flips the triangle winding for
                               the negative-determinant node).
                             section[1]: a quad at y=-1, POSITION ONLY -- NO
                               NORMAL accessor -- wound (pre-mirror) to face
                               -y, i.e. away from the whole-mesh centroid
                               (below it, facing down/outward). This is the
                               primitive whose flat normal must be DERIVED from
                               geometry, and its corner order was chosen so
                               that a flat normal taken from the PRE-FLIP
                               corner order on the MIRRORED positions points
                               INTO the mesh (toward the centroid) -- the exact
                               UE-divergent bug (T7 step 2) the centroid-
                               outward predicate exists to catch. A
                               flip-correct importer produces the opposite
                               (outward/-y) normal and passes.
                           Task 7 indexes sections[1] as "the no-NORMAL
                           primitive" -- this order is binding, not incidental.

      embedded_tex.glb     1 primitive (a unit quad, POSITION + NORMAL +
                           TEXCOORD_0), material "EmbeddedTexMat" with a
                           baseColorTexture pointing at an embedded 2x2 PNG
                           living in the GLB's own BIN chunk (image
                           "name":"albedo"). The PNG is hand-built byte-for-
                           byte in this script (PNG signature + IHDR/IDAT/IEND
                           chunks, a single STORED/uncompressed zlib deflate
                           block, CRC-32 and Adler-32 computed in pure
                           PowerShell) rather than rendered through any host
                           imaging library, so its bytes cannot drift with the
                           machine's GDI+/.NET version. Pixels (row-major,
                           top-to-bottom): red, green / blue, white.

      degenerate.glb       1 primitive, 3 triangles over a unit-quad POSITION
                           accessor (4 verts): triangle 0 (indices 0,1,2) is
                           GOOD; triangles 1 and 2 (0,2,2 and 0,3,3) each repeat
                           a corner index -- structurally valid (cgltf_validate
                           passes; no index exceeds the accessor's count) but
                           geometrically degenerate (zero area). A2 part 2: the
                           importer must WARN and DROP degenerate triangles,
                           never refuse the whole primitive over them. The GOOD
                           triangle's index triple lives at byte offset 676
                           WITHIN THE .glb FILE (see
                           $DegenerateGoodTriangleByteOffset below, which this
                           script derives analytically from the GLB layout and
                           asserts against on every run) -- Task 6's
                           all-degenerate regression test patches those exact
                           bytes to {0,0,0} and re-imports, expecting the
                           nothing-drawable refusal (spec s4.5).

      empty.glb            A structurally valid glTF with "meshes":[] and no
                           geometry at all -- spec s4.5's nothing-drawable
                           refusal path (refuse loudly, never a silent empty
                           artifact).

      bad_sparse.glb       A unit quad (POSITION count=4) whose POSITION
                           accessor carries a "sparse" override with count=5 --
                           already invalid, since a sparse count must never
                           exceed the accessor it patches -- backed by an
                           indices bufferView sized for only 2 unsigned-short
                           entries (4 bytes) and a values bufferView sized for
                           only 1 VEC3 (12 bytes), though 5 sparse entries
                           require 10 and 60 bytes respectively. This is A
                           TRUNCATED/OVERFLOWING INDEX BOUND, NOT A WORKING
                           EXPLOIT (spec s9/s4.5's own words) -- it exists to
                           prove `cgltf_validate` (the CVE-2026-32845-hardened
                           path, ThirdParty/cgltf/PATCHES.md) is called on
                           every import and actually rejects the file; cgltf
                           itself is patched to fail this closed rather than
                           read past the buffer. `cgltf_parse` still succeeds
                           on this file (it does no bounds checking) --
                           `cgltf_validate` is the mandatory second pass that
                           must fail it.

      requires_draco.gltf  A standalone .gltf (buffer embedded as a base64 data
                           URI, no companion .bin) that is OTHERWISE a valid
                           single quad -- the same shape as single.glb -- with
                           top-level "extensionsUsed"/"extensionsRequired":
                           ["KHR_draco_mesh_compression"]. A2 part 1: the
                           importer must refuse loudly because it does not
                           implement that extension, purely by reading
                           extensionsRequired (no real Draco-compressed payload
                           is needed to exercise this path).

      external_bin.glb +   (Final-review fix I1, 2026-09-11.) A GLB whose
      external_bin.bin     buffers[0] is the BIN chunk (POSITION + NORMAL of a
                           unit quad) and buffers[1] is {"uri":"external_bin.bin"}
                           carrying the index accessor alone (6 u16 indices, 2
                           triangles), material "ExternalBinMat". The GLB
                           container only requires buffer 0 to be the BIN chunk;
                           any further buffer may reference an external file
                           exactly like a .gltf's -- legal, and the pipeline
                           hashes such a file as .glb ++ .bin. The client's
                           reader used to assume "a .glb carries no external
                           buffers" and refused this file with HashMismatch
                           forever; the [assets] facade case cooks it through
                           CookSession and resolves it through
                           Assets::MeshArtifactFor to pin the fix.

      nonindexed.glb       (Final-review fix I2, 2026-09-11.) One triangle-mode
                           primitive with POSITION ONLY and NO `indices`
                           accessor -- 6 vertices drawn in order as 2 triangles
                           (the implicit identity index buffer glTF defines for
                           a non-indexed primitive), material "Flat". Legal and
                           common; the importer used to `continue` on a null
                           index accessor, refusing this file with the wrong
                           reason ("every triangle is degenerate"). The
                           [pipeline] geometry case asserts it imports with 6
                           indices, one section and no warnings.

    Re-run: `powershell -ExecutionPolicy Bypass -File scripts\make-mesh-fixtures.ps1 -Force`
    from the repo root. Without -Force the script refuses to overwrite an
    existing fixture.
#>

[CmdletBinding()]
param(
    [string]$OutDir = (Join-Path $PSScriptRoot "..\ArcaneTests\data\gltf"),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$OutDir = [System.IO.Path]::GetFullPath($OutDir)

# The GOOD triangle in degenerate.glb (indices 0,1,2, the first 6 bytes of its
# index bufferView) lives at this byte offset within the finished .glb file.
# Derived analytically below (12-byte GLB header + 8-byte JSON chunk header +
# the padded JSON chunk's length + 8-byte BIN chunk header + the index
# bufferView's byteOffset within the BIN buffer) and asserted against this
# literal so a future edit to the JSON shape fails loudly instead of silently
# moving Task 6's patch target. Task 6 patches bytes
# [kDegenerateGoodTriangleByteOffset, +6) to {0,0,0,0,0,0} (three u16 zeros)
# and re-imports, expecting the nothing-drawable refusal.
$DegenerateGoodTriangleByteOffset = 676

# --------------------------------------------------------------------------
# Low-level byte / JSON helpers
# --------------------------------------------------------------------------

# NOTE on a PowerShell gotcha that recurs throughout this file: a function that
# `return`s a [byte[]] gets its array UNROLLED onto the pipeline unless the
# return is comma-guarded (`return ,$array`); the caller then receives a boxed
# Object[] instead of a byte[], which fails hard against anything typed
# [byte[]] (e.g. List[byte]::AddRange). Every helper below that hands back a
# byte array uses `return ,$x`; every call site that immediately feeds such a
# result into a strongly-typed sink also casts `[byte[]](...)` defensively.

function Pad-To4 {
    param([byte[]]$Bytes, [byte]$PadByte)
    $list = [System.Collections.Generic.List[byte]]::new($Bytes)
    while (($list.Count % 4) -ne 0) { $list.Add($PadByte) }
    return ,($list.ToArray())
}

function Write-GltfText {
    param([string]$Path, $JsonObject)
    $json = $JsonObject | ConvertTo-Json -Depth 20 -Compress
    $json = ($json -replace "`r`n", "`n") -replace "`r", "`n"
    $json = $json + "`n"
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
    Write-Host "Wrote $Path ($($json.Length) chars)"
}

function Write-Glb {
    param([string]$Path, $JsonObject, [byte[]]$BinBytes)

    $json = $JsonObject | ConvertTo-Json -Depth 20 -Compress
    $json = ($json -replace "`r`n", "`n") -replace "`r", "`n"
    [byte[]]$jsonBytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    [byte[]]$jsonPadded = Pad-To4 $jsonBytes 0x20   # JSON chunks pad with spaces

    $chunks = [System.Collections.Generic.List[byte]]::new()
    $chunks.AddRange([System.Text.Encoding]::ASCII.GetBytes("glTF"))          # magic
    $chunks.AddRange([System.BitConverter]::GetBytes([uint32]2))              # version
    $totalLengthIndex = $chunks.Count
    $chunks.AddRange([byte[]](0, 0, 0, 0))                                    # totalLength placeholder

    # JSON chunk: chunkLength (u32 LE) + chunkType 0x4E4F534A ("JSON") + payload.
    $chunks.AddRange([System.BitConverter]::GetBytes([uint32]$jsonPadded.Length))
    $chunks.AddRange([System.Text.Encoding]::ASCII.GetBytes("JSON"))
    $chunks.AddRange($jsonPadded)

    if ($null -ne $BinBytes -and $BinBytes.Length -gt 0) {
        [byte[]]$binPadded = Pad-To4 $BinBytes 0x00   # BIN chunks pad with zeros
        # chunkType 0x004E4942 ("BIN" + a trailing NUL).
        $chunks.AddRange([System.BitConverter]::GetBytes([uint32]$binPadded.Length))
        $chunks.AddRange([byte[]](0x42, 0x49, 0x4E, 0x00))
        $chunks.AddRange($binPadded)
    }

    $totalLength = $chunks.Count
    [byte[]]$totalBytes = [System.BitConverter]::GetBytes([uint32]$totalLength)
    for ($i = 0; $i -lt 4; $i++) { $chunks[$totalLengthIndex + $i] = $totalBytes[$i] }

    $finalBytes = $chunks.ToArray()
    [System.IO.File]::WriteAllBytes($Path, $finalBytes)
    Write-Host "Wrote $Path ($($finalBytes.Length) bytes)"
    return ,$finalBytes
}

# --------------------------------------------------------------------------
# Hand-rolled PNG encoder (embedded_tex.glb's 2x2 albedo) -- CRC-32 and
# Adler-32 implemented from the bare algorithms so the PNG's bytes depend on
# nothing but this script (no System.Drawing/GDI+, whose encoder output can
# vary by host .NET/OS patch level).
# --------------------------------------------------------------------------

function Get-Crc32Table {
    $table = New-Object 'System.Int64[]' 256
    for ($n = 0; $n -lt 256; $n++) {
        [int64]$c = $n
        for ($k = 0; $k -lt 8; $k++) {
            if (($c -band 1) -ne 0) { $c = (0xEDB88320L -bxor ($c -shr 1)) -band 0xFFFFFFFFL }
            else { $c = ($c -shr 1) -band 0xFFFFFFFFL }
        }
        $table[$n] = $c
    }
    return ,$table
}
$Script:Crc32Table = Get-Crc32Table

function Get-Crc32 {
    param([byte[]]$Bytes)
    [int64]$crc = 0xFFFFFFFFL
    foreach ($byte in $Bytes) {
        $idx = ($crc -bxor [int64]$byte) -band 0xFFL
        $crc = ($Script:Crc32Table[$idx] -bxor ($crc -shr 8)) -band 0xFFFFFFFFL
    }
    return ($crc -bxor 0xFFFFFFFFL) -band 0xFFFFFFFFL
}

function Get-Adler32 {
    param([byte[]]$Bytes)
    [int64]$MOD = 65521
    [int64]$a = 1
    [int64]$b = 0
    foreach ($byte in $Bytes) {
        $a = ($a + $byte) % $MOD
        $b = ($b + $a) % $MOD
    }
    return (($b -shl 16) -bor $a) -band 0xFFFFFFFFL
}

function Get-BEU32Bytes {
    param([int64]$Value)
    $b = [byte[]]::new(4)
    $b[0] = [byte](($Value -shr 24) -band 0xFF)
    $b[1] = [byte](($Value -shr 16) -band 0xFF)
    $b[2] = [byte](($Value -shr 8) -band 0xFF)
    $b[3] = [byte]($Value -band 0xFF)
    return ,$b
}

function New-PngChunk {
    param([string]$Type, [byte[]]$Data)
    $list = [System.Collections.Generic.List[byte]]::new()
    $list.AddRange([byte[]](Get-BEU32Bytes $Data.Length))
    $typeBytes = [System.Text.Encoding]::ASCII.GetBytes($Type)
    $list.AddRange($typeBytes)
    $list.AddRange($Data)
    $crcInput = [System.Collections.Generic.List[byte]]::new()
    $crcInput.AddRange($typeBytes)
    $crcInput.AddRange($Data)
    $crc = Get-Crc32 ($crcInput.ToArray())
    $list.AddRange([byte[]](Get-BEU32Bytes $crc))
    return ,($list.ToArray())
}

function New-Png2x2 {
    # Fixed RGBA pixels, row-major top-to-bottom: red, green / blue, white.
    $pixels = @(
        @(255, 0, 0, 255), @(0, 255, 0, 255),
        @(0, 0, 255, 255), @(255, 255, 255, 255)
    )
    $raw = [System.Collections.Generic.List[byte]]::new()
    $raw.Add(0); $raw.AddRange([byte[]]$pixels[0]); $raw.AddRange([byte[]]$pixels[1])   # filter=None, row 0
    $raw.Add(0); $raw.AddRange([byte[]]$pixels[2]); $raw.AddRange([byte[]]$pixels[3])   # filter=None, row 1
    $rawBytes = $raw.ToArray()

    # zlib stream: 2-byte header + ONE final STORED deflate block + Adler-32 trailer.
    $deflate = [System.Collections.Generic.List[byte]]::new()
    $deflate.Add(0x01)   # BFINAL=1, BTYPE=00 (stored)
    $len = $rawBytes.Length
    $deflate.Add([byte]($len -band 0xFF)); $deflate.Add([byte](($len -shr 8) -band 0xFF))
    $nlen = (-bnot $len) -band 0xFFFF
    $deflate.Add([byte]($nlen -band 0xFF)); $deflate.Add([byte](($nlen -shr 8) -band 0xFF))
    $deflate.AddRange($rawBytes)

    $zlib = [System.Collections.Generic.List[byte]]::new()
    $zlib.Add(0x78); $zlib.Add(0x9C)
    $zlib.AddRange($deflate.ToArray())
    $zlib.AddRange([byte[]](Get-BEU32Bytes (Get-Adler32 $rawBytes)))

    $sig = [byte[]](137, 80, 78, 71, 13, 10, 26, 10)
    $ihdr = [System.Collections.Generic.List[byte]]::new()
    $ihdr.AddRange([byte[]](Get-BEU32Bytes 2))   # width
    $ihdr.AddRange([byte[]](Get-BEU32Bytes 2))   # height
    $ihdr.Add(8); $ihdr.Add(6); $ihdr.Add(0); $ihdr.Add(0); $ihdr.Add(0)   # 8-bit RGBA, no interlace

    $png = [System.Collections.Generic.List[byte]]::new()
    $png.AddRange($sig)
    $png.AddRange([byte[]](New-PngChunk "IHDR" ($ihdr.ToArray())))
    $png.AddRange([byte[]](New-PngChunk "IDAT" ($zlib.ToArray())))
    $png.AddRange([byte[]](New-PngChunk "IEND" ([byte[]]@())))
    return ,($png.ToArray())
}

# --------------------------------------------------------------------------
# Mesh-buffer builder: accumulates a single BIN payload plus its bufferViews
# and accessors as fixtures append attributes/indices to it.
# --------------------------------------------------------------------------

function New-MeshBuilder {
    [PSCustomObject]@{
        Bin         = [System.Collections.Generic.List[byte]]::new()
        BufferViews = [System.Collections.Generic.List[object]]::new()
        Accessors   = [System.Collections.Generic.List[object]]::new()
    }
}

function Add-Padding {
    param($List, [int]$Alignment)
    while (($List.Count % $Alignment) -ne 0) { $List.Add(0) }
}

function Add-F32Array {
    param($List, [double[]]$Values)
    foreach ($v in $Values) { $List.AddRange([System.BitConverter]::GetBytes([float]$v)) }
}

function Add-U16Array {
    param($List, [uint16[]]$Values)
    foreach ($v in $Values) { $List.AddRange([System.BitConverter]::GetBytes([uint16]$v)) }
}

function Add-PositionAccessor {
    param($Builder, $Positions)
    Add-Padding $Builder.Bin 4
    $offset = $Builder.Bin.Count
    foreach ($p in $Positions) { Add-F32Array $Builder.Bin $p }
    $byteLength = $Builder.Bin.Count - $offset
    $bvIndex = $Builder.BufferViews.Count
    $Builder.BufferViews.Add([ordered]@{ buffer = 0; byteOffset = $offset; byteLength = $byteLength; target = 34962 })

    $minX = $Positions[0][0]; $minY = $Positions[0][1]; $minZ = $Positions[0][2]
    $maxX = $Positions[0][0]; $maxY = $Positions[0][1]; $maxZ = $Positions[0][2]
    foreach ($p in $Positions) {
        if ($p[0] -lt $minX) { $minX = $p[0] }; if ($p[0] -gt $maxX) { $maxX = $p[0] }
        if ($p[1] -lt $minY) { $minY = $p[1] }; if ($p[1] -gt $maxY) { $maxY = $p[1] }
        if ($p[2] -lt $minZ) { $minZ = $p[2] }; if ($p[2] -gt $maxZ) { $maxZ = $p[2] }
    }

    $accIndex = $Builder.Accessors.Count
    $Builder.Accessors.Add([ordered]@{
        bufferView   = $bvIndex
        componentType = 5126   # FLOAT
        count        = $Positions.Count
        type         = "VEC3"
        min          = @([double]$minX, [double]$minY, [double]$minZ)
        max          = @([double]$maxX, [double]$maxY, [double]$maxZ)
    })
    return $accIndex
}

function Add-NormalAccessor {
    param($Builder, $Normals)
    Add-Padding $Builder.Bin 4
    $offset = $Builder.Bin.Count
    foreach ($n in $Normals) { Add-F32Array $Builder.Bin $n }
    $byteLength = $Builder.Bin.Count - $offset
    $bvIndex = $Builder.BufferViews.Count
    $Builder.BufferViews.Add([ordered]@{ buffer = 0; byteOffset = $offset; byteLength = $byteLength; target = 34962 })
    $accIndex = $Builder.Accessors.Count
    $Builder.Accessors.Add([ordered]@{ bufferView = $bvIndex; componentType = 5126; count = $Normals.Count; type = "VEC3" })
    return $accIndex
}

function Add-TexCoordAccessor {
    param($Builder, $UVs)
    Add-Padding $Builder.Bin 4
    $offset = $Builder.Bin.Count
    foreach ($uv in $UVs) { Add-F32Array $Builder.Bin $uv }
    $byteLength = $Builder.Bin.Count - $offset
    $bvIndex = $Builder.BufferViews.Count
    $Builder.BufferViews.Add([ordered]@{ buffer = 0; byteOffset = $offset; byteLength = $byteLength; target = 34962 })
    $accIndex = $Builder.Accessors.Count
    $Builder.Accessors.Add([ordered]@{ bufferView = $bvIndex; componentType = 5126; count = $UVs.Count; type = "VEC2" })
    return $accIndex
}

function Add-IndexAccessorU16 {
    param($Builder, [uint16[]]$Indices)
    Add-Padding $Builder.Bin 2
    $offset = $Builder.Bin.Count
    Add-U16Array $Builder.Bin $Indices
    $byteLength = $Builder.Bin.Count - $offset
    $bvIndex = $Builder.BufferViews.Count
    $Builder.BufferViews.Add([ordered]@{ buffer = 0; byteOffset = $offset; byteLength = $byteLength; target = 34963 })

    $minV = $Indices[0]; $maxV = $Indices[0]
    foreach ($ix in $Indices) { if ($ix -lt $minV) { $minV = $ix }; if ($ix -gt $maxV) { $maxV = $ix } }

    $accIndex = $Builder.Accessors.Count
    $Builder.Accessors.Add([ordered]@{ bufferView = $bvIndex; componentType = 5123; count = $Indices.Count; type = "SCALAR"; min = @([int]$minV); max = @([int]$maxV) })
    return $accIndex
}

function Add-RawBufferView {
    param($Builder, [byte[]]$Data)
    Add-Padding $Builder.Bin 4
    $offset = $Builder.Bin.Count
    $Builder.Bin.AddRange($Data)
    $byteLength = $Builder.Bin.Count - $offset
    $bvIndex = $Builder.BufferViews.Count
    $Builder.BufferViews.Add([ordered]@{ buffer = 0; byteOffset = $offset; byteLength = $byteLength })
    return $bvIndex
}

function New-AssetBlock {
    [ordered]@{ version = "2.0"; generator = "arcane-mesh-fixtures" }
}

# --------------------------------------------------------------------------
# Fixture builders
# --------------------------------------------------------------------------

function Build-SingleGlb {
    $b = New-MeshBuilder
    $positions = @(@(0.0, 0.0, 0.0), @(1.0, 0.0, 0.0), @(1.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $normals   = @(@(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0))
    $posAcc = Add-PositionAccessor $b $positions
    $nrmAcc = Add-NormalAccessor $b $normals
    $idxAcc = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    $json = [ordered]@{
        asset     = (New-AssetBlock)
        materials = @([ordered]@{ name = "SingleMat" })
        meshes    = @([ordered]@{
            name       = "SingleMesh"
            primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc; NORMAL = $nrmAcc }
                indices    = $idxAcc
                material   = 0
                mode       = 4
            })
        })
        nodes       = @([ordered]@{ name = "SingleNode"; mesh = 0 })
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @([ordered]@{ byteLength = $b.Bin.Count })
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }
    Write-Glb (Join-Path $OutDir "single.glb") $json ($b.Bin.ToArray()) | Out-Null
}

function Build-MultiGlb {
    $b = New-MeshBuilder

    # Quad A: x [0,1], z [0,1], y=0 -- ordered v0,v1,v2,v3 so cross(v1-v0,v2-v0) = +y. Metal.
    $posA = @(@(0.0, 0.0, 0.0), @(0.0, 0.0, 1.0), @(1.0, 0.0, 1.0), @(1.0, 0.0, 0.0))
    $nrmA = @(@(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $posAccA = Add-PositionAccessor $b $posA
    $nrmAccA = Add-NormalAccessor $b $nrmA
    $idxAccA = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    # Quad B: x [1,2] -- its x=1 edge (v0,v1) is bit-identical to Quad A's x=1 edge (v3,v2). Metal.
    $posB = @(@(1.0, 0.0, 0.0), @(1.0, 0.0, 1.0), @(2.0, 0.0, 1.0), @(2.0, 0.0, 0.0))
    $nrmB = @(@(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $posAccB = Add-PositionAccessor $b $posB
    $nrmAccB = Add-NormalAccessor $b $nrmB
    $idxAccB = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    # Quad C: x [2,3] -- its x=2 edge (v0,v1) is bit-identical to Quad B's x=2 edge (v3,v2). Paint.
    $posC = @(@(2.0, 0.0, 0.0), @(2.0, 0.0, 1.0), @(3.0, 0.0, 1.0), @(3.0, 0.0, 0.0))
    $nrmC = @(@(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $posAccC = Add-PositionAccessor $b $posC
    $nrmAccC = Add-NormalAccessor $b $nrmC
    $idxAccC = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    # kMultiGlbRawVertexCount = 12 (3 primitives x 4 verts); unique after a
    # meshoptimizer remap = 8 (the x=1 and x=2 edges each dedupe 2 verts).
    $json = [ordered]@{
        asset     = (New-AssetBlock)
        materials = @([ordered]@{ name = "Metal" }, [ordered]@{ name = "Paint" })
        meshes    = @([ordered]@{
            name       = "MultiMesh"
            primitives = @(
                [ordered]@{ attributes = [ordered]@{ POSITION = $posAccA; NORMAL = $nrmAccA }; indices = $idxAccA; material = 0; mode = 4 },
                [ordered]@{ attributes = [ordered]@{ POSITION = $posAccB; NORMAL = $nrmAccB }; indices = $idxAccB; material = 0; mode = 4 },
                [ordered]@{ attributes = [ordered]@{ POSITION = $posAccC; NORMAL = $nrmAccC }; indices = $idxAccC; material = 1; mode = 4 }
            )
        })
        nodes       = @([ordered]@{ name = "MultiNode"; mesh = 0 })
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @([ordered]@{ byteLength = $b.Bin.Count })
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }
    Write-Glb (Join-Path $OutDir "multi.glb") $json ($b.Bin.ToArray()) | Out-Null
}

function Build-Nested {
    $b = New-MeshBuilder
    # Child-local unit quad in the XZ plane at y=0, same +y-normal winding as multi.glb's quads.
    $positions = @(@(0.0, 0.0, 0.0), @(0.0, 0.0, 1.0), @(1.0, 0.0, 1.0), @(1.0, 0.0, 0.0))
    $normals   = @(@(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $posAcc = Add-PositionAccessor $b $positions
    $nrmAcc = Add-NormalAccessor $b $normals
    $idxAcc = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    # Parent-TRS x child-TRS bakes to exactly x in [1,2], z in [3,4] (y stays 0):
    #   childScale (2,1,2) . local -> x,z in [0,2]
    #   parentScale (0.5,1,0.5) . that -> x,z in [0,1]
    #   parentTranslate (1,0,3) . that -> x in [1,2], z in [3,4]
    $json = [ordered]@{
        asset  = (New-AssetBlock)
        meshes = @([ordered]@{
            name       = "NestedChildMesh"
            primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc; NORMAL = $nrmAcc }
                indices    = $idxAcc
                mode       = 4
            })
        })
        nodes  = @(
            [ordered]@{
                name        = "NestedParent"
                translation = @(1.0, 0.0, 3.0)
                rotation    = @(0.0, 0.0, 0.0, 1.0)
                scale       = @(0.5, 1.0, 0.5)
                children    = @(1)
            },
            [ordered]@{
                name        = "NestedChild"
                translation = @(0.0, 0.0, 0.0)
                rotation    = @(0.0, 0.0, 0.0, 1.0)
                scale       = @(2.0, 1.0, 2.0)
                mesh        = 0
            }
        )
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @([ordered]@{ uri = "nested.bin"; byteLength = $b.Bin.Count })
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }

    Write-GltfText (Join-Path $OutDir "nested.gltf") $json
    $binBytes = $b.Bin.ToArray()
    [System.IO.File]::WriteAllBytes((Join-Path $OutDir "nested.bin"), $binBytes)
    Write-Host "Wrote $(Join-Path $OutDir 'nested.bin') ($($binBytes.Length) bytes)"
}

function Build-Mirrored {
    $b = New-MeshBuilder

    # section[0]: y=+1 quad WITH authored NORMAL (0,1,0), CCW pre-mirror.
    $pos0 = @(@(-0.5, 1.0, -0.5), @(-0.5, 1.0, 0.5), @(0.5, 1.0, 0.5), @(0.5, 1.0, -0.5))
    $nrm0 = @(@(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $posAcc0 = Add-PositionAccessor $b $pos0
    $nrmAcc0 = Add-NormalAccessor $b $nrm0
    $idxAcc0 = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    # section[1]: y=-1 quad, POSITION ONLY -- no NORMAL. Wound (pre-mirror) to face -y
    # (below the whole-mesh centroid, facing outward/down): a flat normal taken from
    # this corner order on the MIRRORED positions, without a winding-flip correction,
    # points INTO the mesh -- the failure mode T7's centroid-outward predicate exists
    # to catch.
    $pos1 = @(@(-0.5, -1.0, -0.5), @(0.5, -1.0, -0.5), @(0.5, -1.0, 0.5), @(-0.5, -1.0, 0.5))
    $posAcc1 = Add-PositionAccessor $b $pos1
    $idxAcc1 = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    $json = [ordered]@{
        asset  = (New-AssetBlock)
        meshes = @([ordered]@{
            name       = "MirroredMesh"
            primitives = @(
                [ordered]@{ attributes = [ordered]@{ POSITION = $posAcc0; NORMAL = $nrmAcc0 }; indices = $idxAcc0; mode = 4 },
                [ordered]@{ attributes = [ordered]@{ POSITION = $posAcc1 }; indices = $idxAcc1; mode = 4 }
            )
        })
        nodes  = @([ordered]@{
            name        = "MirroredNode"
            translation = @(0.0, 0.0, 0.0)
            rotation    = @(0.0, 0.0, 0.0, 1.0)
            scale       = @(-1.0, 1.0, 1.0)
            mesh        = 0
        })
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @([ordered]@{ byteLength = $b.Bin.Count })
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }
    Write-Glb (Join-Path $OutDir "mirrored.glb") $json ($b.Bin.ToArray()) | Out-Null
}

function Build-EmbeddedTex {
    $b = New-MeshBuilder
    $positions = @(@(0.0, 0.0, 0.0), @(1.0, 0.0, 0.0), @(1.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $normals   = @(@(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0))
    $uvs       = @(@(0.0, 0.0), @(1.0, 0.0), @(1.0, 1.0), @(0.0, 1.0))
    $posAcc = Add-PositionAccessor $b $positions
    $nrmAcc = Add-NormalAccessor $b $normals
    $uvAcc  = Add-TexCoordAccessor $b $uvs
    $idxAcc = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    $pngBytes = New-Png2x2
    $imgBv = Add-RawBufferView $b $pngBytes

    $json = [ordered]@{
        asset     = (New-AssetBlock)
        images    = @([ordered]@{ name = "albedo"; mimeType = "image/png"; bufferView = $imgBv })
        textures  = @([ordered]@{ source = 0 })
        materials = @([ordered]@{
            name                 = "EmbeddedTexMat"
            pbrMetallicRoughness = [ordered]@{ baseColorTexture = [ordered]@{ index = 0 } }
        })
        meshes    = @([ordered]@{
            name       = "EmbeddedTexMesh"
            primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc; NORMAL = $nrmAcc; TEXCOORD_0 = $uvAcc }
                indices    = $idxAcc
                material   = 0
                mode       = 4
            })
        })
        nodes       = @([ordered]@{ name = "EmbeddedTexNode"; mesh = 0 })
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @([ordered]@{ byteLength = $b.Bin.Count })
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }
    Write-Glb (Join-Path $OutDir "embedded_tex.glb") $json ($b.Bin.ToArray()) | Out-Null
}

function Build-Degenerate {
    $b = New-MeshBuilder
    $positions = @(@(0.0, 0.0, 0.0), @(1.0, 0.0, 0.0), @(1.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $posAcc = Add-PositionAccessor $b $positions

    # Triangle 0 (GOOD): 0,1,2 -- three distinct corners.
    # Triangle 1 (degenerate): 0,2,2 -- repeats corner 2.
    # Triangle 2 (degenerate): 0,3,3 -- repeats corner 3.
    $goodTriangleBinOffset = $b.Bin.Count   # the index bufferView starts here (2-byte aligned already)
    $idxAcc = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 2, 0, 3, 3))

    $json = [ordered]@{
        asset  = (New-AssetBlock)
        meshes = @([ordered]@{
            name       = "DegenerateMesh"
            primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc }
                indices    = $idxAcc
                mode       = 4
            })
        })
        nodes       = @([ordered]@{ name = "DegenerateNode"; mesh = 0 })
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @([ordered]@{ byteLength = $b.Bin.Count })
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }

    $jsonText = $json | ConvertTo-Json -Depth 20 -Compress
    $jsonText = ($jsonText -replace "`r`n", "`n") -replace "`r", "`n"
    [byte[]]$jsonBytes = [System.Text.Encoding]::UTF8.GetBytes($jsonText)
    [byte[]]$jsonPadded = Pad-To4 $jsonBytes 0x20

    # 12-byte GLB header + 8-byte JSON chunk header + padded JSON + 8-byte BIN
    # chunk header + the index bufferView's byteOffset within the BIN buffer.
    $computedOffset = 12 + 8 + $jsonPadded.Length + 8 + $goodTriangleBinOffset
    if ($computedOffset -ne $DegenerateGoodTriangleByteOffset) {
        throw "degenerate.glb good-triangle byte offset drifted: computed $computedOffset, " +
              "header/contract says $DegenerateGoodTriangleByteOffset. Update " +
              '$DegenerateGoodTriangleByteOffset at the top of this script AND the header ' +
              "comment -- Task 6's patch test depends on this exact number."
    }

    Write-Glb (Join-Path $OutDir "degenerate.glb") $json ($b.Bin.ToArray()) | Out-Null
    Write-Host "degenerate.glb good-triangle byte offset: $computedOffset"
}

function Build-Empty {
    $json = [ordered]@{
        asset  = (New-AssetBlock)
        meshes = @()
        scenes = @([ordered]@{ nodes = @() })
        scene  = 0
    }
    Write-Glb (Join-Path $OutDir "empty.glb") $json $null | Out-Null
}

function Build-BadSparse {
    $b = New-MeshBuilder
    $positions = @(@(0.0, 0.0, 0.0), @(1.0, 0.0, 0.0), @(1.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $posAcc = Add-PositionAccessor $b $positions
    $idxAcc = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    # The bad sparse override: count=5 against accessor.count=4 (already invalid --
    # sparse.count must never exceed the accessor it patches). Its indices bufferView
    # holds only 2 unsigned-short entries (4 bytes; 5 need 10) and its values
    # bufferView holds only 1 VEC3 (12 bytes; 5 need 60) -- both undersized for the
    # declared count. cgltf_validate's indices_req_size/values_req_size checks (the
    # CVE-2026-32845-hardened path) catch this via a plain size comparison, with no
    # dependency on cgltf_load_buffers having run. A truncated/overflowing index
    # bound, not a working exploit (spec s9/s4.5).
    $sparseIndexList = [System.Collections.Generic.List[byte]]::new()
    Add-U16Array $sparseIndexList ([uint16[]](4, 5))   # values already past accessor.count=4
    $sparseIndicesBv = Add-RawBufferView $b ($sparseIndexList.ToArray())

    $sparseValueList = [System.Collections.Generic.List[byte]]::new()
    Add-F32Array $sparseValueList ([double[]](9.0, 9.0, 9.0))
    $sparseValuesBv = Add-RawBufferView $b ($sparseValueList.ToArray())

    $b.Accessors[$posAcc]["sparse"] = [ordered]@{
        count   = 5
        indices = [ordered]@{ bufferView = $sparseIndicesBv; componentType = 5123 }
        values  = [ordered]@{ bufferView = $sparseValuesBv }
    }

    $json = [ordered]@{
        asset  = (New-AssetBlock)
        meshes = @([ordered]@{
            name       = "BadSparseMesh"
            primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc }
                indices    = $idxAcc
                mode       = 4
            })
        })
        nodes       = @([ordered]@{ name = "BadSparseNode"; mesh = 0 })
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @([ordered]@{ byteLength = $b.Bin.Count })
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }
    Write-Glb (Join-Path $OutDir "bad_sparse.glb") $json ($b.Bin.ToArray()) | Out-Null
}

function Build-RequiresDraco {
    $b = New-MeshBuilder
    $positions = @(@(0.0, 0.0, 0.0), @(1.0, 0.0, 0.0), @(1.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $normals   = @(@(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0))
    $posAcc = Add-PositionAccessor $b $positions
    $nrmAcc = Add-NormalAccessor $b $normals
    $idxAcc = Add-IndexAccessorU16 $b ([uint16[]](0, 1, 2, 0, 2, 3))

    $binBytes = $b.Bin.ToArray()
    $dataUri = "data:application/octet-stream;base64," + [Convert]::ToBase64String($binBytes)

    $json = [ordered]@{
        asset               = (New-AssetBlock)
        extensionsUsed      = @("KHR_draco_mesh_compression")
        extensionsRequired  = @("KHR_draco_mesh_compression")
        materials           = @([ordered]@{ name = "DracoMat" })
        meshes              = @([ordered]@{
            name       = "RequiresDracoMesh"
            primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc; NORMAL = $nrmAcc }
                indices    = $idxAcc
                material   = 0
                mode       = 4
            })
        })
        nodes       = @([ordered]@{ name = "RequiresDracoNode"; mesh = 0 })
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @([ordered]@{ uri = $dataUri; byteLength = $binBytes.Length })
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }
    Write-GltfText (Join-Path $OutDir "requires_draco.gltf") $json
}

function Build-ExternalBin {
    # buffer 0: the GLB's own BIN chunk -- POSITION + NORMAL of a unit quad in the
    # XY plane facing +z (same geometry as single.glb).
    $b = New-MeshBuilder
    $positions = @(@(0.0, 0.0, 0.0), @(1.0, 0.0, 0.0), @(1.0, 1.0, 0.0), @(0.0, 1.0, 0.0))
    $normals   = @(@(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0), @(0.0, 0.0, 1.0))
    $posAcc = Add-PositionAccessor $b $positions
    $nrmAcc = Add-NormalAccessor $b $normals

    # buffer 1: the EXTERNAL external_bin.bin, carrying the index accessor ALONE.
    # Built through a second mesh builder (whose helpers hard-code buffer 0), then
    # re-homed: its one bufferView is pointed at buffer 1 and appended after the
    # BIN chunk's own views, and its one accessor is appended after the BIN
    # chunk's own accessors with its bufferView index rewritten to match.
    $ext = New-MeshBuilder
    Add-IndexAccessorU16 $ext ([uint16[]](0, 1, 2, 0, 2, 3)) | Out-Null
    $extView = $ext.BufferViews[0]
    $extView["buffer"] = 1
    $idxView = $b.BufferViews.Count
    $b.BufferViews.Add($extView)
    $extAccessor = $ext.Accessors[0]
    $extAccessor["bufferView"] = $idxView
    $idxAcc = $b.Accessors.Count
    $b.Accessors.Add($extAccessor)

    $json = [ordered]@{
        asset     = (New-AssetBlock)
        materials = @([ordered]@{ name = "ExternalBinMat" })
        meshes    = @([ordered]@{
            name       = "ExternalBinMesh"
            primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc; NORMAL = $nrmAcc }
                indices    = $idxAcc
                material   = 0
                mode       = 4
            })
        })
        nodes       = @([ordered]@{ name = "ExternalBinNode"; mesh = 0 })
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @(
            [ordered]@{ byteLength = $b.Bin.Count },
            [ordered]@{ uri = "external_bin.bin"; byteLength = $ext.Bin.Count }
        )
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }
    Write-Glb (Join-Path $OutDir "external_bin.glb") $json ($b.Bin.ToArray()) | Out-Null
    $extBytes = $ext.Bin.ToArray()
    [System.IO.File]::WriteAllBytes((Join-Path $OutDir "external_bin.bin"), $extBytes)
    Write-Host "Wrote $(Join-Path $OutDir 'external_bin.bin') ($($extBytes.Length) bytes)"
}

function Build-NonIndexed {
    # Six POSITIONs drawn in order as two triangles -- a unit quad in the XY plane
    # (facing +z), each triangle spelled out corner by corner. NO `indices` key on
    # the primitive: glTF's implicit identity index buffer.
    $b = New-MeshBuilder
    $positions = @(
        @(0.0, 0.0, 0.0), @(1.0, 0.0, 0.0), @(1.0, 1.0, 0.0),   # triangle 0
        @(0.0, 0.0, 0.0), @(1.0, 1.0, 0.0), @(0.0, 1.0, 0.0)    # triangle 1
    )
    $posAcc = Add-PositionAccessor $b $positions

    $json = [ordered]@{
        asset     = (New-AssetBlock)
        materials = @([ordered]@{ name = "Flat" })
        meshes    = @([ordered]@{
            name       = "NonIndexedMesh"
            primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc }
                material   = 0
                mode       = 4
            })
        })
        nodes       = @([ordered]@{ name = "NonIndexedNode"; mesh = 0 })
        scenes      = @([ordered]@{ nodes = @(0) })
        scene       = 0
        buffers     = @([ordered]@{ byteLength = $b.Bin.Count })
        bufferViews = @($b.BufferViews)
        accessors   = @($b.Accessors)
    }
    Write-Glb (Join-Path $OutDir "nonindexed.glb") $json ($b.Bin.ToArray()) | Out-Null
}

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

$expectedFiles = @(
    "single.glb", "multi.glb", "nested.gltf", "nested.bin",
    "mirrored.glb", "embedded_tex.glb", "degenerate.glb",
    "empty.glb", "bad_sparse.glb", "requires_draco.gltf",
    "external_bin.glb", "external_bin.bin", "nonindexed.glb"
)

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

if (-not $Force) {
    foreach ($f in $expectedFiles) {
        if (Test-Path (Join-Path $OutDir $f)) {
            throw "Fixture '$f' already exists in $OutDir -- pass -Force to overwrite."
        }
    }
}

Write-Host "Generating glTF fixture corpus into $OutDir"
Build-SingleGlb
Build-MultiGlb
Build-Nested
Build-Mirrored
Build-EmbeddedTex
Build-Degenerate
Build-Empty
Build-BadSparse
Build-RequiresDraco
Build-ExternalBin
Build-NonIndexed
Write-Host "Done -- $($expectedFiles.Count) fixture files generated."
