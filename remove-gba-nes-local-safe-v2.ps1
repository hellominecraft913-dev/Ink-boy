[CmdletBinding()]
param(
    # Preview is the default. Pass -Apply only after reviewing the plan.
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'

function Find-RepositoryRoot {
    $starts = @()
    if ($PSScriptRoot) { $starts += $PSScriptRoot }
    $starts += (Get-Location).Path

    foreach ($start in ($starts | Select-Object -Unique)) {
        try { $cursor = (Resolve-Path -LiteralPath $start).Path }
        catch { continue }
        while ($cursor) {
            if ((Test-Path -LiteralPath (Join-Path $cursor 'platformio.ini') -PathType Leaf) -and
                (Test-Path -LiteralPath (Join-Path $cursor 'src/activities/Activity.cpp') -PathType Leaf)) {
                $top = & git -C $cursor rev-parse --show-toplevel 2>$null
                if ($LASTEXITCODE -eq 0 -and $top) {
                    return [System.IO.Path]::GetFullPath(($top | Select-Object -First 1).Trim())
                }
            }
            $parent = Split-Path -Parent $cursor
            if (-not $parent -or $parent -eq $cursor) { break }
            $cursor = $parent
        }
    }
    throw 'Could not find the Ink-boy Git checkout. Run this from the repository root or keep this script in the repository.'
}

function Get-NewlineStyle([string]$Text) {
    if ($Text.Contains("`r`n")) { return "`r`n" }
    return "`n"
}

function Test-OuterParentheses([string]$Expression) {
    if ($Expression.Length -lt 2 -or $Expression[0] -ne '(' -or $Expression[$Expression.Length - 1] -ne ')') {
        return $false
    }
    $depth = 0
    for ($i = 0; $i -lt $Expression.Length; $i++) {
        if ($Expression[$i] -eq '(') { $depth += 1 }
        elseif ($Expression[$i] -eq ')') { $depth -= 1 }
        if ($depth -eq 0 -and $i -lt ($Expression.Length - 1)) { return $false }
        if ($depth -lt 0) { return $false }
    }
    return ($depth -eq 0)
}

function Get-EmulatorMacroForDirective([string]$Line) {
    # Support the common local variants: #if CROSSINK_AGNES,
    #if defined(CROSSINK_AGNES) && CROSSINK_AGNES, outer parentheses,
    #and #ifdef CROSSINK_AGNES. Do not match #ifndef fallback definitions.
    if ($Line -match '^\s*#\s*ifdef\s+(CROSSINK_(?:AGNES|GBA))\s*(?://.*)?$') {
        return $Matches[1]
    }
    if ($Line -notmatch '^\s*#\s*if\s+(.+?)\s*$') { return $null }

    $expr = $Matches[1] -replace '\s*//.*$', ''
    $compact = $expr -replace '\s+', ''
    $compact = $compact -replace '(?<!defined)\((CROSSINK_(?:AGNES|GBA))\)', '$1'
    while (Test-OuterParentheses $compact) {
        $compact = $compact.Substring(1, $compact.Length - 2)
        $compact = $compact -replace '(?<!defined)\((CROSSINK_(?:AGNES|GBA))\)', '$1'
    }

    foreach ($macro in @('CROSSINK_AGNES', 'CROSSINK_GBA')) {
        $escaped = [regex]::Escape($macro)
        if ($compact -match "^$escaped$") { return $macro }
        if ($compact -match "^defined\($escaped\)$") { return $macro }
        if ($compact -match "^defined\($escaped\)&&$escaped(?:==1)?$") { return $macro }
        if ($compact -match "^$escaped&&defined\($escaped\)(?:==1)?$") { return $macro }
    }
    return $null
}

function Remove-EmulatorConditionalBlocks([string]$Text) {
    $newline = Get-NewlineStyle $Text
    $lines = [regex]::Split($Text, "\r\n|\n|\r")
    $output = New-Object 'System.Collections.Generic.List[string]'
    $counts = @{ CROSSINK_AGNES = 0; CROSSINK_GBA = 0 }
    $skipDepth = 0

    foreach ($line in $lines) {
        if ($skipDepth -gt 0) {
            if ($line -match '^\s*#\s*(if|ifdef|ifndef)\b') { $skipDepth++ }
            elseif ($line -match '^\s*#\s*endif\b') { $skipDepth-- }
            continue
        }

        $macro = Get-EmulatorMacroForDirective $line
        if ($macro) {
            $counts[$macro]++
            $skipDepth = 1
            continue
        }
        $output.Add($line) | Out-Null
    }

    if ($skipDepth -ne 0) {
        throw 'Found an unterminated preprocessor block while preparing edits; no files have been changed.'
    }

    return [pscustomobject]@{
        Text = [string]::Join($newline, $output.ToArray())
        Counts = $counts
    }
}

function Save-Backup([string]$Root, [string]$RelativePath, [string]$BackupRoot) {
    $source = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $source)) { return }
    $destination = Join-Path $BackupRoot $RelativePath
    $destinationParent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
    $item = Get-Item -LiteralPath $source -Force
    if ($item.PSIsContainer) {
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        Get-ChildItem -LiteralPath $source -Force | Copy-Item -Destination $destination -Recurse -Force
    } else {
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
}

$root = Find-RepositoryRoot
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

$sourceTargets = @(
    'src/activities/Activity.cpp',
    'src/activities/ActivityManager.h',
    'src/activities/ActivityManager.cpp',
    'src/activities/home/FileBrowserActivity.cpp'
)
$updates = [ordered]@{}
$removedCounts = [ordered]@{}
foreach ($relative in $sourceTargets) {
    $file = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required source file is missing: $relative. No files have been changed."
    }
    $original = [System.IO.File]::ReadAllText($file, [System.Text.Encoding]::UTF8)
    $result = Remove-EmulatorConditionalBlocks $original
    $updates[$relative] = $result.Text
    $removedCounts[$relative] = $result.Counts
}

# Guard against an unseen local conditional style or an unguarded integration:
# verify routing / factory symbols are absent from the proposed edited contents.
$checks = @(
    [pscustomobject]@{ Path='src/activities/Activity.cpp'; Pattern='goToNES\s*\(|goToGBA\s*\(|checkFileExtension\s*\(\s*path\s*,\s*"\.(nes|gba)"' },
    [pscustomobject]@{ Path='src/activities/ActivityManager.h'; Pattern='goToNES|goToGBA' },
    [pscustomobject]@{ Path='src/activities/ActivityManager.cpp'; Pattern='NESActivity|GbaActivity|goToNES|goToGBA' },
    [pscustomobject]@{ Path='src/activities/home/FileBrowserActivity.cpp'; Pattern='checkFileExtension\s*\(\s*filename\s*,\s*"\.(nes|gba)"' }
)
foreach ($check in $checks) {
    if ([regex]::IsMatch([string]$updates[$check.Path], $check.Pattern)) {
        throw "Could not safely remove all GBA/NES integration references from $($check.Path). No files have been changed. This local file uses an unexpected structure; send me the relevant section."
    }
}

# Remove only the emulator build flags. Already-removed flags are okay.
$iniPath = Join-Path $root 'platformio.ini'
if (-not (Test-Path -LiteralPath $iniPath -PathType Leaf)) {
    throw 'platformio.ini is missing. No files have been changed.'
}
$iniOriginal = [System.IO.File]::ReadAllText($iniPath, [System.Text.Encoding]::UTF8)
$iniNewline = Get-NewlineStyle $iniOriginal
$iniLines = [regex]::Split($iniOriginal, "\r\n|\n|\r")
$iniOutput = New-Object 'System.Collections.Generic.List[string]'
$flagCounts = @{ CROSSINK_AGNES = 0; CROSSINK_GBA = 0 }
foreach ($line in $iniLines) {
    if ($line -match '^\s*-DCROSSINK_(AGNES|GBA)=1\s*(?:;.*)?$') {
        $flagCounts[$Matches[1]]++
        continue
    }
    $iniOutput.Add($line) | Out-Null
}
$iniUpdated = [string]::Join($iniNewline, $iniOutput.ToArray())
if ($iniUpdated -match '(?m)^\s*-DCROSSINK_(AGNES|GBA)=1\b') {
    throw 'Could not remove all GBA/NES build flags cleanly. No files have been changed.'
}
$updates['platformio.ini'] = $iniUpdated

# README adjustments are optional and only apply to the specific claims if present.
$readmePath = Join-Path $root 'README.md'
if (Test-Path -LiteralPath $readmePath -PathType Leaf) {
    $readmeOriginal = [System.IO.File]::ReadAllText($readmePath, [System.Text.Encoding]::UTF8)
    $readmeUpdated = $readmeOriginal
    $readmeUpdated = $readmeUpdated -replace '(?i)This is a fork of Crossink with gameboy/gameboy color/Gameboy advance/NES emulators integrated into it', 'This is a fork of CrossInk with Game Boy and Game Boy Color emulators integrated into it'
    $readmeUpdated = $readmeUpdated -replace '(?i)Drop your Gameboy/Gameboy color/Gameboy advance/NES ROM into the /Books folder\.', 'Drop your Game Boy or Game Boy Color ROM into the /Books folder.'
    $readmeUpdated = $readmeUpdated -replace '(?i)Gameboy/Gameboy color/Gameboy advance/NES rom', 'Game Boy or Game Boy Color ROM'
    if ($readmeUpdated -ne $readmeOriginal) { $updates['README.md'] = $readmeUpdated }
}

$deletePaths = @(
    'src/activities/gba',
    'src/activities/nes',
    'lib/GpspGBA',
    'docs/development/agnes-nes-integration.md'
)
foreach ($required in @('src/activities/gba', 'src/activities/nes', 'lib/GpspGBA')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $required))) {
        throw "Expected emulator directory is missing: $required. No files have been changed."
    }
}

Write-Host ''
Write-Host "Repository: $root"
Write-Host 'Planned changes:'
foreach ($path in $updates.Keys) {
    Write-Host "  EDIT   $path"
    if ($removedCounts.Contains($path)) {
        Write-Host "         remove guard blocks: NES=$($removedCounts[$path].CROSSINK_AGNES), GBA=$($removedCounts[$path].CROSSINK_GBA)"
    }
}
foreach ($path in $deletePaths) {
    if (Test-Path -LiteralPath (Join-Path $root $path)) { Write-Host "  DELETE $path" }
}
Write-Host '  KEEP   src/activities/gameboy/GameBoyActivity.cpp unchanged (including the fix discussed in chat)'
Write-Host '  KEEP   Peanut-GB, Walnut-CGB, PSXE, freeink-sdk, and unrelated code'
Write-Host '  BACKUP all targeted files to a timestamped folder under .git before applying'

if (-not $Apply) {
    Write-Host ''
    Write-Host 'Preview only: nothing has been changed.'
    Write-Host 'After reviewing the plan, run this script again with -Apply.'
    exit 0
}

$gitDirText = (& git -C $root rev-parse --git-dir).Trim()
if ($LASTEXITCODE -ne 0 -or -not $gitDirText) {
    throw 'Could not locate the Git metadata directory. No files have been changed.'
}
if ([System.IO.Path]::IsPathRooted($gitDirText)) { $gitDir = $gitDirText }
else { $gitDir = Join-Path $root $gitDirText }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupRoot = Join-Path $gitDir "remove-gba-nes-backup-$stamp"
New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null

try {
    $toBackup = @($updates.Keys) + @($deletePaths)
    foreach ($relative in ($toBackup | Select-Object -Unique)) {
        Save-Backup -Root $root -RelativePath $relative -BackupRoot $backupRoot
    }
    foreach ($relative in $updates.Keys) {
        [System.IO.File]::WriteAllText((Join-Path $root $relative), [string]$updates[$relative], $utf8NoBom)
    }
    foreach ($relative in $deletePaths) {
        $full = Join-Path $root $relative
        if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
    }
} catch {
    Write-Error "The script stopped while applying changes. A backup was saved at: $backupRoot"
    throw
}

Write-Host ''
Write-Host 'Done. Local files were changed; nothing was committed or pushed to GitHub.'
Write-Host "Backup of affected files: $backupRoot"
Write-Host 'Review with: git status --short'
Write-Host 'Then build with: pio run -e x4-pro'
