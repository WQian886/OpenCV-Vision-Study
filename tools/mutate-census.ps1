#requires -Version 5.1
<#
  Mutation harness for the CENSUS half of tools/check-sections.ps1 --
  the part that checks the numbers the documents quote.

  Why this file is in the repository:
  the census numbers in the docs (how many pointers, how many resolve,
  how many break) used to come from a ONE-OFF experiment whose script
  lived under build/probe/ -- a directory .gitignore blocks entirely.
  Nobody could recompute the number, so the number was hearsay, and
  hearsay drifts (notes/atomic/at-a-number-nobody-can-recompute-is-hearsay.md).
  A script that measures something has to be where people can run it.

  Every mutation runs on a COPY of the tree: the mutation is a
  deliberate break, and the real tree must not carry it.

  Pure ASCII, no BOM: the Chinese it needs is built from code points.

    A  a quoted number drifts (the pointer count in the root README)
    B  a quoted number drifts (the loose-rule count in the note)
    C  the CHECKER'S OWN header drifts
    D  the ruler is broken (the comparison always passes)

  A, B and C must go RED.  D must go GREEN -- if the ruler is broken,
  the mutations stop being caught, and that is what proves the ruler
  is what caught them.

  Usage:
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\mutate-census.ps1 -Mutation A
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\mutate-census.ps1 -Mutation A,B,C,D
#>

[CmdletBinding()]
param(
    [string]$Mutation = 'A',
    [string]$RepoRoot,
    [string]$Work
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $RepoRoot) { $RepoRoot = (Split-Path -Parent $PSScriptRoot) }
if (-not $Work)     { $Work = Join-Path $env:TEMP ('gn-opencv-mutate-' + [guid]::NewGuid().ToString('N').Substring(0, 8)) }

function Ch([int]$cp) { return [string][char]$cp }

$GE   = Ch 0x4E2A   # "a (measure word)"
$ZHANG = Ch 0x7AE0  # "chapter"
$JIE  = Ch 0x8282   # "section"
$ZHI  = Ch 0x6307   # "point"
$ZHEN = Ch 0x9488   # "needle"
$KUAN = Ch 0x5BBD   # "wide"
$SHOU = Ch 0x677E   # "loose"

$UTF8 = New-Object System.Text.UTF8Encoding($false)

function Read-Text([string]$p) { return [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8) }
function Write-Text([string]$p, [string]$t) { [System.IO.File]::WriteAllText($p, $t, $UTF8) }

$dest = Join-Path $Work $Mutation
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force -Path $dest | Out-Null

# The checker is run with the SAME host this script runs under, so the
# harness measures the gate in the environment the gate actually runs
# in (Windows PowerShell 5.1 locally, PowerShell 7 on the runner).
$self = (Get-Process -Id $PID).Path
if (-not $self) { $self = 'powershell' }

foreach ($f in (Get-ChildItem -Path $RepoRoot -Recurse -File -Filter *.md)) {
    $rel = $f.FullName.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/')
    if ($rel -match '(^|[\\/])(build|\.git)([\\/]|$)') { continue }
    $target = Join-Path $dest $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -Force $f.FullName $target
}
foreach ($f in (Get-ChildItem -Path (Join-Path $RepoRoot 'tools') -Recurse -File -Filter *.ps1)) {
    $rel = $f.FullName.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/')
    $target = Join-Path $dest $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -Force $f.FullName $target
}

$checker = Join-Path $dest 'tools\check-sections.ps1'
$readme = Join-Path $dest 'README.md'
$note = Join-Path $dest 'notes\notes_SectionPointers_2026-10.md'
$docscan = Join-Path $dest 'tools\lib\DocScan.ps1'

function Apply-Mutation([string]$Which) {
    switch ($Which) {
        'A' {
            # The quoted pointer count in the root README drifts by one.
            $old = '179 ' + $GE + $ZHANG + $JIE + $ZHI + $ZHEN
            $new = '178 ' + $GE + $ZHANG + $JIE + $ZHI + $ZHEN
            $t = Read-Text $readme
            if (-not $t.Contains($old)) { throw "mutation A: the quoted pointer count was not found" }
            Write-Text $readme ($t.Replace($old, $new))
        }
        'B' {
            # The loose-rule count in the note's rule table drifts by one.
            $old = '| ' + $KUAN + $SHOU + ' | **20** |'
            $new = '| ' + $KUAN + $SHOU + ' | **19** |'
            $t = Read-Text $note
            if (-not $t.Contains($old)) { throw "mutation B: the rule table row was not found" }
            Write-Text $note ($t.Replace($old, $new))
        }
        'C' {
            # The CHECKER'S OWN header drifts: the checker quotes the
            # census in its own comment, and that comment is a claim too.
            $t = Read-Text $checker
            $old = '179 section pointers'
            $new = '180 section pointers'
            if (-not $t.Contains($old)) { throw "mutation C: the checker's own header was not found" }
            Write-Text $checker ($t.Replace($old, $new))
        }
        'D' {
            # The RULER IS BROKEN: the comparison always passes.
            $t = Read-Text $docscan
            $old = 'if ($got -ne $want) {'
            $new = 'if ($false) {'
            if (-not $t.Contains($old)) { throw "mutation D: the comparison was not found" }
            Write-Text $docscan ($t.Replace($old, $new))
        }
        default { throw "unknown mutation: $Which" }
    }
}

# ------------------------------------------------------------------
# Run the checker on the mutated copy
# ------------------------------------------------------------------
$results = @()
foreach ($m in ($Mutation -split ',')) {
    $m = $m.Trim()
    if ($m -eq '') { continue }
    Apply-Mutation $m

    $out = & $self -NoProfile -ExecutionPolicy Bypass -File $checker -RepoRoot $dest 2>&1
    $text = ($out | Out-String)
    $reds = ([regex]::Matches($text, '(?m)^\s+FAIL: ')).Count
    $ok = ($text -match 'SECTIONS OK')

    Write-Host ("=== MUTATION {0} ===" -f $m)
    Write-Host ("  reds in the census claims: {0}" -f $reds)
    Write-Host ("  verdict: {0}" -f $(if ($ok) { 'GREEN' } else { 'RED' }))
    foreach ($l in ($out | Where-Object { $_ -match 'FAIL: ' })) { Write-Host ("  " + $l) }

    # A, B and C must be RED; D must be GREEN.  A harness that cannot
    # tell the two apart is a harness that proves nothing.
    $want = if ($m -eq 'D') { 'GREEN' } else { 'RED' }
    $gotVerdict = if ($ok) { 'GREEN' } else { 'RED' }
    if ($want -ne $gotVerdict) {
        Write-Host ("  UNEXPECTED: mutation {0} should be {1} but came out {2}" -f $m, $want, $gotVerdict) -ForegroundColor Red
        $results += $false
    } else {
        $results += $true
    }
    if ($m -ne 'D') {
        # each mutation starts from the pristine copy
        if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        foreach ($f in (Get-ChildItem -Path $RepoRoot -Recurse -File -Filter *.md)) {
            $rel = $f.FullName.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/')
            if ($rel -match '(^|[\\/])(build|\.git)([\\/]|$)') { continue }
            $target = Join-Path $dest $rel
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-Item -Force $f.FullName $target
        }
        foreach ($f in (Get-ChildItem -Path (Join-Path $RepoRoot 'tools') -Recurse -File -Filter *.ps1)) {
            $rel = $f.FullName.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/')
            $target = Join-Path $dest $rel
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-Item -Force $f.FullName $target
        }
    }
}

Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue

if (@($results | Where-Object { -not $_ }).Count -gt 0) {
    Write-Host "MUTATION HARNESS FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "MUTATION HARNESS OK: every mutation came out the way it had to" -ForegroundColor Green
