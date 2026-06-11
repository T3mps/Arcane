# scripts/sync_to_github.ps1 — mirror working copy into the git repo.
# Excludes build artifacts and the destination's .git (robocopy /XD also
# protects excluded dirs from /MIR deletion).
$src = "D:\dev\starworks\Astra"
$dst = "D:\dev\github\Astra"
robocopy $src $dst /MIR /NFL /NDL /NJH `
    /XD .git .claude bin bin-int ide .vs `
    /XF *.user error_list.txt nul Astra.slnx Astra.sln
if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed ($LASTEXITCODE)"; exit 1 }
Write-Host "Synced. Review with: git -C $dst status"
exit 0
