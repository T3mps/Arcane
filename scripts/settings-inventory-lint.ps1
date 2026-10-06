# Settings inventory lint (settings arc S5-1). Exit 0 = every SETTING row obeys
# the Reconciliation (R1-R4) and spec s16.11 (rules L1-L9); exit 1 = violations (listed);
# exit 3 = -RequireFrozen and the FROZEN marker is absent.
param(
    [string]$Inventory = "docs/superpowers/audits/2026-10-03-settings-inventory.md",
    [switch]$FixScope,
    [switch]$RequireFrozen,
    [string]$EmitFrozen
)
$ErrorActionPreference = 'Stop'
$lines = [System.Collections.Generic.List[string]](Get-Content -LiteralPath $Inventory -Encoding UTF8)

if ($RequireFrozen -and -not ($lines | Where-Object { $_ -match '^\*\*FROZEN ' })) {   # the S5-2 status line
    Write-Host "inventory is not FROZEN: S5-2 has not run"; exit 3
}

$rows = @(); $part = 0; $section = ''
for ($i = 0; $i -lt $lines.Count; $i++) {
    $l = $lines[$i]
    if ($l -match '^## Part (\d)') { $part = [int]$Matches[1]; continue }
    if ($l -match '^### (.+)$') { $section = $Matches[1]; continue }
    if ($part -eq 0 -or -not $l.StartsWith('|')) { continue }
    $inner = $l.Trim().Substring(1, $l.Trim().Length - 2)
    $c = [regex]::Split($inner, '(?<!\\)\|') | ForEach-Object { $_.Trim() }
    if ($c.Count -lt 12 -or $c[0] -eq 'file:line' -or $c[0] -match '^-+$') { continue }
    $rows += [pscustomobject]@{ Index=$i; Line=$i+1; Part=$part; Section=$section; File=$c[0]; Symbol=$c[1];
        Value=$c[2]; Verdict=$c[3]; Name=$c[4]; Struct=$c[5]; Aud=$c[6]; Scope=$c[7]; Apply=$c[8];
        Range=$c[9]; Det=$c[10]; Why=$c[11]; Cells=$c }
}
$setting = @($rows | Where-Object { $_.Verdict -like 'SETTING*' })
$errors = [System.Collections.Generic.List[string]]::new()
function Fail($r, $rule, $msg) { $errors.Add(("{0}:{1}: {2} {3} [{4}]" -f $Inventory, $r.Line, $rule, $msg, $r.Name)) }

# L1 scope vocabulary (R2). -FixScope maps Parts 1-2 "Preferences": theme/fonts/keys/layout -> Pref-M, else Pref-P.
foreach ($r in $setting) {
    if ($r.Scope -in @('Pref-M','Pref-P','Project')) { continue }
    if ($FixScope -and $r.Scope -eq 'Preferences') {
        $new = if ($r.Name -match '^editor\.(theme|ui|keys|layout)\.') { 'Pref-M' } else { 'Pref-P' }
        $cells = $r.Cells.Clone(); $cells[7] = $new
        $lines[$r.Index] = '| ' + ($cells -join ' | ') + ' |'
        continue
    }
    Fail $r 'L1' "scope '$($r.Scope)' is not Pref-M/Pref-P/Project"
}
# L2 retired spellings (R1 "Was" column).
$retired = '^splash\.','^assets\.thumbnail\.','^editor\.assets\.(thumbnail|meshThumbFov)','^editor\.viewport\.snap\.',
           '^editor\.viewport\.gizmo','^editor\.viewport\.axisColor','^editor\.viewport\.grid\.axis',
           '^editor\.selection\.(outline|hover)Color','^render\.outline\.\w*Color','^editor\.play\.maxSimDtSeconds',
           '^editor\.shader\.compileDebounceSeconds','^render\.window\.minimizedSleepMs','^editor\.perf\.idleSleepMs',
           '^diagnostics\.reporter\.(copyFlashSeconds|logTailLines)','^editor\.ui\.copyFlashSeconds',
           '^editor\.crash\.logTailLines','^editor\.preview\.light(Direction|Ambient|\{)','^editor\.console\.bufferLines',
           'log\.mosaicLevel','^jobs\.shaderCompileThreads.*JobSettings'
foreach ($r in $rows) { foreach ($p in $retired) { if ($r.Name -match $p) { Fail $r 'L2' "retired spelling (R1); use the canonical name" } } }
# L3 apply vocabulary: the first word is Live / NextWorld / Restart; "Live (world rebuild)" is NextWorld (R2).
foreach ($r in $setting) {
    if ($r.Apply -notmatch '^(Live|NextWorld|Restart)\b' -or $r.Apply -match 'world rebuild') { Fail $r 'L3' "apply '$($r.Apply)'" }
}
# Proposed-cvar cell -> concrete names. Cells use shorthand: "a.b.x / .y" and "a.b.width / height" continue
# the previous name's category, "a.{b,c}d" lists alternatives, ", " separates names, and a trailing
# "(...)" is an annotation ("(exists)", "(see ...)"). Wildcards ("a.b.*", "a.b.node*") are kept as written.
function RowNames([string]$cell) {
    $out = [System.Collections.Generic.List[string]]::new(); $prev = $null
    foreach ($raw in ($cell -split '\s*/\s*|,\s+')) {
        $p = ($raw -replace '\s*\(.*$','').Trim()
        if (-not $p) { continue }
        if ($prev -and $p.StartsWith('.')) { $p = ($prev -replace '\.[^.]+$','') + $p }
        elseif ($prev -and $p -match '^[a-z]\w*$') { $p = ($prev -replace '\.[^.]+$','') + '.' + $p }
        if ($p -notmatch '^[a-z][\w.\{\},*]*\.[\w{},*]+$') { continue }
        $prev = $p
        $queue = [System.Collections.Generic.Queue[string]]::new(); $queue.Enqueue($p)
        while ($queue.Count) {
            $n = $queue.Dequeue()
            if ($n -match '^(.*?)\{([^{}]*)\}(.*)$') {
                $pre = $Matches[1]; $alts = $Matches[2] -split ','; $post = $Matches[3]
                foreach ($alt in $alts) { $queue.Enqueue($pre + $alt + $post) }
            } else { $out.Add($n) }
        }
    }
    return ,$out
}
# A cvar's category is its name minus the last segment; 'a.b.*' is category a.b.
function CategoryOf([string]$name) { if ($name -match '\.\*$') { $name -replace '\.\*$','' } else { $name -replace '\.[^.]+$','' } }

# L4 struct per category, L8 category per struct (ARC_SETTINGS registers <category>.<field>, so a
# field name cannot contain a dot: the struct<->category map is one-to-one), L6 retired struct names.
$byCategory = @{}; $byStruct = @{}
foreach ($r in $setting) {
    foreach ($name in (RowNames $r.Name)) {
        $cat = CategoryOf $name
        if ($byCategory.ContainsKey($cat) -and $byCategory[$cat] -ne $r.Struct) { Fail $r 'L4' "category '$cat' has structs '$($byCategory[$cat])' and '$($r.Struct)'" }
        else { $byCategory[$cat] = $r.Struct }
        if ($byStruct.ContainsKey($r.Struct) -and $byStruct[$r.Struct] -ne $cat) { Fail $r 'L8' "struct '$($r.Struct)' spans categories '$($byStruct[$r.Struct])' and '$cat'" }
        else { $byStruct[$r.Struct] = $cat }
    }
    if ($r.Struct -in @('JobSettings','GridSettings','GizmoSettings','OutlineSettings','AssetImportSettings')) { Fail $r 'L6' "retired struct name '$($r.Struct)'" }
}
# L5 one name per value: a name on two SETTING rows agrees on struct/audience/scope/apply/det.
$byName = @{}
foreach ($r in $setting) { foreach ($name in (RowNames $r.Name)) { if (-not $byName.ContainsKey($name)) { $byName[$name] = [System.Collections.Generic.List[object]]::new() }; $byName[$name].Add($r) } }
foreach ($name in ($byName.Keys | Sort-Object)) {
    $group = $byName[$name]; if ($group.Count -lt 2) { continue }
    $sig = $group | ForEach-Object { "$($_.Struct)|$($_.Aud)|$($_.Scope)|$(($_.Apply -split ' ')[0])|$($_.Det)" } | Sort-Object -Unique
    if (@($sig).Count -gt 1) { Fail $group[0] 'L5' ("'$name' has conflicting metadata: " + ($sig -join ' vs ')) }
}
# L7 R2 policies.
foreach ($r in $rows) {
    if ($r.Name -match '^diagnostics\.(installCrashHandler|hangWatchdog)$' -and ($r.Aud -notmatch 'Dev' -or $r.Why -notmatch 'command-line only')) { Fail $r 'L7' 'R2: Dev, never archived, command-line only' }
    if ($r.Symbol -match 'console pattern' -and $r.Verdict -ne 'CONSTANT') { Fail $r 'L7' 'R2: log.pattern is CONSTANT' }
    if ($r.Name -match '^input\.(pressThreshold|holdSeconds|tapSeconds|deadzone)' -and $r.Det -ne 'Y') { Fail $r 'L7' 'R2: input thresholds are det Y' }
    if ($r.Name -eq 'physics.gravity' -and $r.Apply -notmatch '^NextWorld') { Fail $r 'L7' 'R2: gravity is NextWorld' }
    if ($r.Symbol -match 'RECV_CHUNK_SIZE|kInitialResidentSlots|spill copy chunk|writer reserve|read buffer|kChunkFrames|BootStage weights' -and $r.Verdict -ne 'CONSTANT') { Fail $r 'L7' 'R2: capacity hint is CONSTANT' }
}
# L9 spec s16.11: an "Editor Dev" px metric is DERIVED (editor.ui.scale x base), not its own cvar.
# Exempt by name (kept SETTING because the value is not UI chrome; S5-1 fix round 1 and the user's S5-2 review):
# render-target extents and texel counts (fallbackExtent, previewCheckerCell), px of a LOD/grid fade
# (grid fade, minorTargetPx), zoom-scaled canvas-space geometry (editor.graph.* node/pin metrics, the shader
# chain layout, the asset graph node geometry) and the asset row/ref thumbnails (S6-28 / S6-32 / S6-34 convert
# them as cvars, read as Ui::Px(setting) except the render-target ones). A row is exempt when EVERY name it expands to is listed.
$l9KeptSetting = @('editor.viewport.fallbackExtent','editor.graph.grid.minorTargetPx','editor.graph.nodeHeaderGap',
                   'editor.ui.assetRowThumbPx','editor.ui.assetRefThumbPx',
                   'editor.viewport.grid.fadeInPx','editor.viewport.grid.fadeFullPx','editor.shader.previewCheckerCell',
                   'editor.graph.pinRing.width','editor.graph.pinRing.outerGap','editor.graph.pinRing.outerWidth',
                   'editor.graph.nodePadding','editor.graph.passNameFieldWidth','editor.graph.paramNameFieldWidth','editor.graph.swizzleFieldWidth','editor.graph.constPinNeutralWidth1','editor.graph.constPinNeutralWidth2','editor.graph.constPinNeutralWidth4','editor.graph.constFloatWidth','editor.graph.constFloat2Width','editor.graph.constFloat4Width','editor.graph.constParamRangeWidth',
                   'editor.shader.chainLayout.originX','editor.shader.chainLayout.originY','editor.shader.chainLayout.pitchX',
                   'editor.shader.chainLayout.sceneOffsetX','editor.shader.chainLayout.sceneOffsetY','editor.shader.passThumbPx',
                   'editor.assetGraph.node.minWidth','editor.assetGraph.node.maxWidth','editor.assetGraph.node.headerHeight',
                   'editor.assetGraph.node.accentBarWidth','editor.assetGraph.node.padding','editor.assetGraph.pinRadius',
                   'editor.assetGraph.overflowWireThickness','editor.assetGraph.labelPad')
foreach ($r in $setting) {
    $allKept = $true; foreach ($n in (RowNames $r.Name)) { if ($n -notin $l9KeptSetting) { $allKept = $false } }
    if ($r.Part -eq 3 -and $r.Aud -match '^Editor Dev$' -and $r.Value -match '\bpx\b' -and $r.Name -notmatch '^editor\.theme\.' -and $r.Name -notmatch '^editor\.layout\.factory\.' -and $r.Name -ne 'editor.thumbnail.size' -and -not $allKept) { Fail $r 'L9' 'px metric must be DERIVED from editor.ui.scale (s16.11)' }
    # The splash and reporter layout px are DERIVED too (base x 1, an ARC_CONSTANT base; S5-1 step 2).
    if ($r.Name -match '^app\.splash\.\w*Px\b|^diagnostics\.reporter\.layout\.') { Fail $r 'L9' 'splash/reporter px metric must be DERIVED (s16.11)' }
}
# L10 (S5-2) every SETTING name is concrete: no wildcard family ('a.b.*', 'a.b.node*'), so settings-frozen-names.txt covers each member.
foreach ($r in $setting) { if ($r.Name -match '\*') { Fail $r 'L10' 'wildcard name: enumerate every member' } }
# Write back UTF-8 without a BOM and with LF endings (the inventory is eol=lf; PS 5.1 Set-Content -Encoding UTF8 adds a BOM).
if ($FixScope) { [System.IO.File]::WriteAllText((Resolve-Path -LiteralPath $Inventory).ProviderPath, (($lines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false)) }
$counts = $rows | Group-Object { ($_.Verdict -split ' ')[0] } | ForEach-Object { "$($_.Name)=$($_.Count)" }
Write-Host ("rows by verdict: " + ($counts -join ', '))
foreach ($n in 1..3) {   # per part, for the Summary table
    $pc = $rows | Where-Object Part -eq $n | Group-Object { ($_.Verdict -split ' ')[0] } | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Count)" }
    Write-Host ("  Part ${n}: " + ($pc -join ', '))
}
if ($errors.Count) { $errors | ForEach-Object { Write-Host $_ }; Write-Host "$($errors.Count) violation(s)"; exit 1 }
# Emit only when clean: a run with violations must not leave a fresh frozen list behind (S5-2).
if ($EmitFrozen) {
    $names = foreach ($r in $setting) { foreach ($n in (RowNames $r.Name)) { if ($n -match '^[a-z]\w*(\.\w+)+$') { $n } } }
    $names = $names | Sort-Object -Unique -CaseSensitive   # concrete names only: wildcard families ('a.b.*', 'a.b.node*') are frozen by their row
    $text = (@('# Frozen SETTING names (S5-2). Generated by scripts/settings-inventory-lint.ps1 -EmitFrozen.') + $names) -join "`n"
    $out = if ([System.IO.Path]::IsPathRooted($EmitFrozen)) { $EmitFrozen } else { Join-Path (Get-Location).ProviderPath $EmitFrozen }
    [System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($out), $text + "`n", [System.Text.UTF8Encoding]::new($false))
    Write-Host "wrote $(@($names).Count) names to $EmitFrozen"
}
Write-Host "inventory lint: clean"; exit 0
