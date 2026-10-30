[CmdletBinding()]
param(
    [string]$DocsRoot = "docs"
)

# ---------------------------------------------------------------------------
# Documentation-system consistency check.
#
# This project is driven by LLMs: every new conversation has to rebuild project
# state from a limited context window, so the documentation is treated as a
# verifiable artifact rather than prose. This enforces docs/00-meta/doc-rules.md:
#
#   1. the repository root may contain exactly one markdown file (README.md)
#   2. every markdown file carries a complete front-matter block
#   3. the front-matter "id" matches the logical path (ordering prefix dropped)
#   4. every document is listed in its parent directory README.md
#   5. every relative markdown link resolves to an existing file
#   6. no single document exceeds the 600-line budget
#
# Exit code 0 = all checks passed; 1 = at least one violation.
#
# ASCII-only on purpose: Windows PowerShell 5.1 reads a .ps1 without a BOM as
# ANSI, and a non-ASCII comment can swallow the following line of code.
# ---------------------------------------------------------------------------

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot

$docsPath = Join-Path $repoRoot $DocsRoot
if (-not (Test-Path -LiteralPath $docsPath -PathType Container)) {
    Write-Host "[docs] FAIL docs root not found: $docsPath" -ForegroundColor Red
    exit 1
}

$problems = New-Object System.Collections.Generic.List[string]
function Add-Problem([string]$Message) { $script:problems.Add($Message) }

# ---- 1) root may contain exactly one markdown file ----------------------
$rootMarkdown = @(Get-ChildItem -LiteralPath $repoRoot -Filter "*.md" -File |
    Select-Object -ExpandProperty Name)
if ($rootMarkdown.Count -ne 1 -or $rootMarkdown[0] -ne "README.md") {
    Add-Problem ("repository root must contain only README.md; found: " + ($rootMarkdown -join ", "))
}

# ---- 2) front-matter / id / size ---------------------------------------
$requiredFields = @("id", "title", "type", "scope", "read-when", "size", "updated")
$validTypes = @("meta", "architecture", "pipeline", "system", "build",
                "validation", "debug", "status", "roadmap")

function Get-FrontMatter([string]$Path) {
    $lines = @(Get-Content -LiteralPath $Path -Encoding UTF8)
    if ($lines.Count -lt 3 -or $lines[0].Trim() -ne "---") { return $null }
    $result = @{}
    for ($i = 1; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line.Trim() -eq "---") { return $result }
        $idx = $line.IndexOf(":")
        if ($idx -gt 0) {
            $result[$line.Substring(0, $idx).Trim()] = $line.Substring($idx + 1).Trim()
        }
    }
    return $null
}

function Get-LogicalId([string]$Relative) {
    $logical = $Relative -replace "\.md$", ""
    if ($logical -eq "README") { return "docs" }
    if ($logical -match "^(.*)/README$") {
        return ($Matches[1] -replace "^\d+-", "")
    }
    $logical = $logical -replace "^\d+-", ""
    return ($logical -replace "/\d+-", "/")
}

$allDocs = @(Get-ChildItem -LiteralPath $docsPath -Recurse -Filter "*.md" -File)
$docInfo = @{}

foreach ($doc in $allDocs) {
    $relative = $doc.FullName.Substring($docsPath.Length + 1).Replace("\", "/")

    # The template intentionally contains placeholder front-matter.
    if ($relative -eq "00-meta/doc-template.md") { continue }

    $fm = Get-FrontMatter $doc.FullName
    if ($null -eq $fm) {
        Add-Problem "missing or malformed front-matter: docs/$relative"
        continue
    }
    foreach ($field in $requiredFields) {
        if (-not $fm.ContainsKey($field) -or [string]::IsNullOrWhiteSpace($fm[$field])) {
            Add-Problem "docs/$relative : missing front-matter field '$field'"
        }
    }
    if ($fm.ContainsKey("type") -and ($validTypes -notcontains $fm["type"])) {
        Add-Problem "docs/$relative : invalid type '$($fm['type'])'"
    }
    if ($fm.ContainsKey("updated") -and ($fm["updated"] -notmatch "^\d{4}-\d{2}-\d{2}$")) {
        Add-Problem "docs/$relative : 'updated' must be YYYY-MM-DD, got '$($fm['updated'])'"
    }
    if ($fm.ContainsKey("id")) {
        $expected = Get-LogicalId $relative
        if ($fm["id"] -ne $expected) {
            Add-Problem "docs/$relative : id '$($fm['id'])' does not match path (expected '$expected')"
        }
    }

    $lineCount = @(Get-Content -LiteralPath $doc.FullName -Encoding UTF8).Count
    if ($lineCount -gt 600) {
        Add-Problem "docs/$relative : $lineCount lines exceeds the 600-line budget, split it"
    }
    $docInfo[$relative] = @{ Path = $doc.FullName; Lines = $lineCount }
}

# ---- 4) every document is indexed by its parent README -----------------
foreach ($relative in $docInfo.Keys) {
    if ($relative -eq "README.md") { continue }

    $leaf = Split-Path -Leaf $relative
    $parent = Split-Path -Parent $relative
    if ([string]::IsNullOrWhiteSpace($parent)) {
        $parentReadme = Join-Path $docsPath "README.md"
        $parentDir = $docsPath
    } else {
        $parentDir = Join-Path $docsPath $parent
        $parentReadme = Join-Path $parentDir "README.md"
        if (-not (Test-Path -LiteralPath $parentReadme)) {
            Add-Problem "docs/$parent/README.md is missing (required as folder index)"
            continue
        }
    }

    $parentText = Get-Content -LiteralPath $parentReadme -Raw -Encoding UTF8

    if ($leaf -eq "README.md") {
        # A folder index is registered by naming the folder in the parent index.
        $categoryName = Split-Path -Leaf $parentDir
        if ($parentText -notmatch [Regex]::Escape($categoryName)) {
            Add-Problem "docs/$relative is not referenced by its parent index (folder '$categoryName')"
        }
        continue
    }

    $stem = $leaf -replace "\.md$", ""
    if ($parentText -notmatch [Regex]::Escape($stem)) {
        Add-Problem "docs/$relative is not referenced by its parent index"
    }
}

# ---- 5) relative links must resolve ------------------------------------
$linkPattern = [Regex]::new('\[[^\]]*\]\(([^)\s]+)\)')

function Test-LinkSet([string]$FilePath, [string]$DisplayPrefix) {
    $dir = Split-Path -Parent $FilePath
    $text = Get-Content -LiteralPath $FilePath -Raw -Encoding UTF8
    foreach ($m in $linkPattern.Matches($text)) {
        $target = $m.Groups[1].Value
        if ($target -match "^[a-zA-Z][a-zA-Z0-9+.-]*:") { continue }
        $target = ($target -split "#")[0]
        if ([string]::IsNullOrWhiteSpace($target)) { continue }
        $resolved = [IO.Path]::GetFullPath((Join-Path $dir $target))
        if (-not (Test-Path -LiteralPath $resolved)) {
            Add-Problem "$DisplayPrefix : broken relative link '$target'"
        }
    }
}

foreach ($relative in $docInfo.Keys) {
    Test-LinkSet -FilePath $docInfo[$relative].Path -DisplayPrefix "docs/$relative"
}
$rootReadmePath = Join-Path $repoRoot "README.md"
if (Test-Path -LiteralPath $rootReadmePath) {
    Test-LinkSet -FilePath $rootReadmePath -DisplayPrefix "README.md"
}

# ---- report ------------------------------------------------------------
if ($problems.Count -gt 0) {
    Write-Host "[docs] FAIL $($problems.Count) problem(s):" -ForegroundColor Red
    foreach ($p in $problems) { Write-Host "  - $p" }
    exit 1
}

$totalLines = ($docInfo.Values | ForEach-Object { $_.Lines } | Measure-Object -Sum).Sum
Write-Host ("[docs] DocCheck PASS | documents={0} lines={1} | rootMarkdown=README.md" -f `
    $docInfo.Count, $totalLines) -ForegroundColor Green
exit 0