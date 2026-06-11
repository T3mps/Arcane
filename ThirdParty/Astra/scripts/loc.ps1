param(
    [Parameter(Mandatory=$false)]
    [string]$Path = ".",
    
    [Parameter(Mandatory=$false)]
    [string[]]$Include = @("*.ps1", "*.psm1", "*.psd1", "*.py", "*.js", "*.ts", "*.jsx", "*.tsx", 
                           "*.java", "*.cs", "*.cpp", "*.c", "*.h", "*.hpp", "*.go", "*.rb", 
                           "*.php", "*.swift", "*.kt", "*.rs", "*.r", "*.m", "*.mm", "*.scala",
                           "*.pl", "*.sh", "*.bash", "*.zsh", "*.fish", "*.lua", "*.dart",
                           "*.html", "*.htm", "*.css", "*.scss", "*.sass", "*.less",
                           "*.xml", "*.json", "*.yaml", "*.yml", "*.toml", "*.ini", "*.conf",
                           "*.sql", "*.md", "*.markdown", "*.rst", "*.tex"),
    
    [Parameter(Mandatory=$false)]
    [string[]]$Exclude = @("node_modules", "vendor", ".git", ".svn", ".hg", "bin", "obj", 
                           "dist", "build", "target", "__pycache__", ".pytest_cache",
                           "packages", ".vs", ".idea", ".vscode"),
    
    [Parameter(Mandatory=$false)]
    [switch]$IncludeBlankLines = $false,
    
    [Parameter(Mandatory=$false)]
    [switch]$IncludeComments = $true,
    
    [Parameter(Mandatory=$false)]
    [switch]$ShowFiles = $false
)

$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Path = Join-Path (Join-Path $ScriptRoot "..") "include"

# Function to check if a path should be excluded
function Test-ExcludePath {
    param([string]$FilePath)
    
    foreach ($excludePattern in $Exclude) {
        if ($FilePath -like "*\$excludePattern\*" -or $FilePath -like "*/$excludePattern/*") {
            return $true
        }
    }
    return $false
}

# Function to count lines in a file
function Get-FileLineCount {
    param(
        [string]$FilePath,
        [bool]$CountBlankLines,
        [bool]$CountComments
    )
    
    try {
        $lines = Get-Content -Path $FilePath -ErrorAction Stop
        $totalLines = 0
        $extension = [System.IO.Path]::GetExtension($FilePath).ToLower()
        
        foreach ($line in $lines) {
            $trimmedLine = $line.Trim()
            
            # Skip blank lines if requested
            if (-not $CountBlankLines -and [string]::IsNullOrWhiteSpace($trimmedLine)) {
                continue
            }
            
            # Skip comment lines if requested (basic comment detection)
            if (-not $CountComments) {
                # Common single-line comment patterns
                $commentPatterns = @(
                    "^//",      # C-style languages
                    "^#",       # Python, Shell, Ruby, etc.
                    "^--",      # SQL, Lua
                    "^'",       # VB
                    "^%",       # LaTeX
                    "^;",       # INI files
                    "^<!--",    # XML/HTML
                    "^/\*",     # Multi-line comment start
                    "^\*"       # Multi-line comment continuation
                )
                
                $isComment = $false
                foreach ($pattern in $commentPatterns) {
                    if ($trimmedLine -match $pattern) {
                        $isComment = $true
                        break
                    }
                }
                
                if ($isComment) {
                    continue
                }
            }
            
            $totalLines++
        }
        
        return $totalLines
    }
    catch {
        Write-Warning "Error reading file: $FilePath - $_"
        return 0
    }
}

# Main script
Write-Host "Counting lines of code in: $Path" -ForegroundColor Cyan
Write-Host "Include patterns: $($Include -join ', ')" -ForegroundColor Gray
Write-Host "Exclude folders: $($Exclude -join ', ')" -ForegroundColor Gray
Write-Host "Include blank lines: $IncludeBlankLines" -ForegroundColor Gray
Write-Host "Include comments: $IncludeComments" -ForegroundColor Gray
Write-Host ("-" * 60) -ForegroundColor Gray

# Get all matching files
$files = @()
foreach ($pattern in $Include) {
    $matchedFiles = Get-ChildItem -Path $Path -Filter $pattern -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { -not (Test-ExcludePath -FilePath $_.FullName) }
    $files += $matchedFiles
}

# Remove duplicates
$files = $files | Select-Object -Unique

if ($files.Count -eq 0) {
    Write-Host "No matching files found." -ForegroundColor Yellow
    exit
}

# Process files and count lines
$results = @{}
$fileDetails = @()
$totalLOC = 0
$totalFiles = 0

foreach ($file in $files) {
    $lineCount = Get-FileLineCount -FilePath $file.FullName -CountBlankLines $IncludeBlankLines -CountComments $IncludeComments
    
    if ($lineCount -gt 0) {
        $extension = $file.Extension.ToLower()
        if (-not $extension) { $extension = "no extension" }
        
        # Update summary by extension
        if (-not $results.ContainsKey($extension)) {
            $results[$extension] = @{
                Files = 0
                Lines = 0
            }
        }
        
        $results[$extension].Files++
        $results[$extension].Lines += $lineCount
        
        # Store file details
        $relativePath = $file.FullName.Replace($Path, "").TrimStart('\', '/')
        $fileDetails += [PSCustomObject]@{
            Path = $relativePath
            Extension = $extension
            Lines = $lineCount
        }
        
        $totalLOC += $lineCount
        $totalFiles++
    }
}

# Display results
Write-Host "`nSummary by File Type:" -ForegroundColor Green
Write-Host ("-" * 60) -ForegroundColor Gray
Write-Host ("{0,-15} {1,10} {2,15}" -f "Extension", "Files", "Lines") -ForegroundColor Cyan
Write-Host ("-" * 60) -ForegroundColor Gray

# Sort by lines descending
$sortedResults = $results.GetEnumerator() | Sort-Object { $_.Value.Lines } -Descending

foreach ($item in $sortedResults) {
    Write-Host ("{0,-15} {1,10:N0} {2,15:N0}" -f $item.Key, $item.Value.Files, $item.Value.Lines)
}

Write-Host ("-" * 60) -ForegroundColor Gray
Write-Host ("{0,-15} {1,10:N0} {2,15:N0}" -f "TOTAL", $totalFiles, $totalLOC) -ForegroundColor Green

# Show individual files if requested
if ($ShowFiles) {
    Write-Host "`n`nDetailed File List:" -ForegroundColor Green
    Write-Host ("-" * 80) -ForegroundColor Gray
    
    $sortedFiles = $fileDetails | Sort-Object Lines -Descending
    
    foreach ($file in $sortedFiles) {
        Write-Host ("{0,-60} {1,10:N0} lines" -f $file.Path, $file.Lines)
    }
}

# Display top 10 largest files
Write-Host "`n`nTop 10 Largest Files:" -ForegroundColor Green
Write-Host ("-" * 80) -ForegroundColor Gray

$top10 = $fileDetails | Sort-Object Lines -Descending | Select-Object -First 10
foreach ($file in $top10) {
    Write-Host ("{0,-60} {1,10:N0} lines" -f $file.Path, $file.Lines)
}