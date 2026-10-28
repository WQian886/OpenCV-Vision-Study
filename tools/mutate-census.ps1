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

  Four rules this harness learned the hard way -- the first two runs
  on a real runner failed, and BOTH times the reason was in the
  harness, not in the gate:

    1. THE COPY MUST BE VERIFIED BEFORE IT IS BELIEVED.
       A checker pointed at a tree that is not the tree measures air
       and reports a clean sheet
       (notes/atomic/at-a-checker-pointed-at-the-wrong-tree-still-says-ok.md).
       So the harness runs the checker on the PRISTINE copy first and
       demands the same summary line as the real tree.  If the two
       differ, the harness says "THE COPY IS NOT THE TREE" and stops,
       instead of reporting a verdict about the wrong tree.

    2. THE HOST MUST BE A POWERSHELL HOST.
       Asking the process for its own path is not enough: PowerShell
       can be started by a launcher, and then the path is the LAUNCHER
       -- which cannot run a .ps1 at all.  Measured: the harness ran
       "dotnet.exe -NoProfile -File checker.ps1", every mutation came
       out RED with zero reds, and the harness said nothing about it.

    3. A MUTATION THAT DID NOT HAPPEN MUST BE LOUD.
       Every replacement asserts that it replaced EXACTLY ONE thing.
       A mutation that silently did nothing is a green light for a
       broken gate.

    4. A PATH IS NOT A STRING.
       The copy is made of RELATIVE paths, and a relative path used to
       be computed by slicing the root's length off an absolute one.
       On the runner the root was the 8.3 short form ("RUNNER~1") and
       the provider expanded it ("runneradmin") -- three characters
       apart -- so every relative path came out as "ine\notes\...",
       the tail of "pristine".  The copy was 142 files in the wrong
       place.  Rule 1 is what caught it: the fidelity gate refused to
       measure the copy and said WHY.

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
# The PROVIDER's path, not the string we were handed.  On a Windows
# runner $env:TEMP is the 8.3 short form ("C:\Users\RUNNER~1\...") and
# the provider expands it ("C:\Users\runneradmin\...") -- two strings,
# one directory.  A relative path sliced out of the wrong one starts
# three characters into the middle of a name.
$RepoRoot = (Get-Item -LiteralPath $RepoRoot -ErrorAction Stop).FullName.TrimEnd([char]'\', [char]'/')
if (-not $Work) {
    $Work = Join-Path $env:TEMP ('gn-opencv-mutate-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
}

function Ch([int]$cp) { return [string][char]$cp }

$GE    = Ch 0x4E2A   # "a (measure word)"
$ZHANG = Ch 0x7AE0   # "chapter"
$JIE   = Ch 0x8282   # "section"
$ZHI   = Ch 0x6307   # "point"
$ZHEN  = Ch 0x9488   # "needle"
$KUAN  = Ch 0x5BBD   # "wide"
$SHOU  = Ch 0x677E   # "loose"

$UTF8 = New-Object System.Text.UTF8Encoding($false)

function Read-Text([string]$p) { return [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8) }
function Write-Text([string]$p, [string]$t) { [System.IO.File]::WriteAllText($p, $t, $UTF8) }

# ------------------------------------------------------------------
# 1. The host that runs the checker
# ------------------------------------------------------------------
#
# PSHOME is the directory the engine lives in, and it holds the host
# executable.  This is the one place the harness can be sure of: the
# process path can be a launcher (see the header).
function Get-PsHost {
    foreach ($n in @('pwsh.exe', 'powershell.exe')) {
        $p = Join-Path $PSHOME $n
        if (Test-Path -LiteralPath $p) { return $p }
    }
    throw ("no PowerShell host found under PSHOME (" + $PSHOME + ")." +
           " The harness runs the checker in a child process, and without a host it measures nothing.")
}

$HostExe = Get-PsHost
$verText = (& $HostExe -NoProfile -Command '$PSVersionTable.PSVersion.ToString()')
if ($LASTEXITCODE -ne 0 -or (($verText | Out-String) -notmatch '\d+\.\d+')) {
    throw ("the executable at " + $HostExe + " did not answer as PowerShell." +
           " It said: " + (($verText | Out-String).Trim()))
}
Write-Host ("   host: {0} (PowerShell {1})" -f $HostExe, (($verText | Out-String).Trim()))

# ------------------------------------------------------------------
# 2. The files that make the tree
# ------------------------------------------------------------------
#
# The relative path of a file under the tree.
#
# NOT by slicing a length off an absolute path: the string we were
# handed and the enumerated paths can be two different strings for the
# same directory.  MEASURED on a Windows runner: $env:TEMP is the 8.3
# short form ("C:\Users\RUNNER~1\..."), the provider expands it
# ("C:\Users\runneradmin\..."), and the slice landed three characters
# into the middle of a name -- every relative path came out as
# "ine\notes\..." (the tail of "pristine").
#
# So the base is the provider's own answer, and a file that is not
# under it is a loud error: a wrong relative path copies files into
# the wrong place, and the gate then measures a tree nobody is
# looking at.
function Get-Rel([string]$base, [string]$full) {
    if (-not $full.StartsWith($base + [char]'\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ("'" + $full + "' is not under the tree '" + $base + "'")
    }
    return $full.Substring($base.Length + 1)
}

# The same two sets the checker needs: every markdown file (build/ and
# .git/ excluded -- build products are not documentation) and every
# script under tools/ (the checker dot-sources its own library).
function Get-TreeFiles([string]$root) {
    # Where the PROVIDER says the tree is, not the string we were
    # handed -- see Get-Rel above.
    $base = (Get-Item -LiteralPath $root -ErrorAction Stop).FullName.TrimEnd([char]'\', [char]'/')
    $out = New-Object System.Collections.ArrayList
    foreach ($f in (Get-ChildItem -Path $root -Recurse -File -Filter *.md)) {
        $rel = Get-Rel $base $f.FullName
        if ($rel -match '(^|[\\/])(build|\.git)([\\/]|$)') { continue }
        [void]$out.Add([pscustomobject]@{ Rel = $rel; Full = $f.FullName })
    }
    $tools = Join-Path $root 'tools'
    if (Test-Path -LiteralPath $tools) {
        foreach ($f in (Get-ChildItem -Path $tools -Recurse -File -Filter *.ps1)) {
            $rel = Get-Rel $base $f.FullName
            if ($rel -match '(^|[\\/])(build|\.git)([\\/]|$)') { continue }
            [void]$out.Add([pscustomobject]@{ Rel = $rel; Full = $f.FullName })
        }
    }
    return ,@($out)
}

# The copy is made with the .NET file API, not with Copy-Item: the
# provider parses paths, and a path is not a wildcard, a list, or a
# pattern.  The copy is a copy.
function Copy-Tree([string]$root, [string]$dest, $files) {
    if (Test-Path -LiteralPath $dest) { Remove-Item -Recurse -Force $dest }
    [void][System.IO.Directory]::CreateDirectory($dest)
    foreach ($e in $files) {
        $target = $dest + '\' + $e.Rel
        $parent = $target.Substring(0, $target.LastIndexOf('\'))
        [void][System.IO.Directory]::CreateDirectory($parent)
        [System.IO.File]::Copy($e.Full, $target, $true)
    }
}

# ------------------------------------------------------------------
# 3. Running the checker
# ------------------------------------------------------------------
function Invoke-Checker([string]$exe, [string]$checker, [string]$root) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $exe -NoProfile -ExecutionPolicy Bypass -File $checker -RepoRoot $root 2>&1
    }
    finally {
        $ErrorActionPreference = $prev
    }
    $text = ($out | Out-String)
    $m = [regex]::Match($text, '(?m)^SECTIONS OK: .*$')
    return [pscustomobject]@{
        Text      = $text
        Exit      = $LASTEXITCODE
        Reds      = ([regex]::Matches($text, '(?m)^\s+FAIL: ')).Count
        PtrReds   = ([regex]::Matches($text, '(?m)^\s+FAIL: .*not in this tree')).Count
        Ok        = ($text -match 'SECTIONS OK')
        Summary   = $(if ($m.Success) { $m.Value.Trim() } else { '' })
        SawCensus = ($text -match 'markdown files in scope')
    }
}

# ------------------------------------------------------------------
# 4. The mutations
# ------------------------------------------------------------------
#
# Every replacement asserts that it replaced EXACTLY ONE thing.  A
# mutation that did not happen must be loud: a green light for a
# broken gate is the failure this project keeps paying for.
function Replace-ExactlyOnce([string]$path, [string]$old, [string]$new, [string]$what) {
    $t = Read-Text $path
    $n = ([regex]::Matches($t, [regex]::Escape($old))).Count
    if ($n -ne 1) {
        throw ("mutation " + $what + ": expected exactly one '" + $old + "' in " + $path + ", found " + $n)
    }
    Write-Text $path ($t.Replace($old, $new))
}

function Apply-Mutation([string]$Which, [string]$dest) {
    $readme = $dest + '\README.md'
    $note = $dest + '\notes\notes_SectionPointers_2026-10.md'
    $checker = $dest + '\tools\check-sections.ps1'
    $docscan = $dest + '\tools\lib\DocScan.ps1'
    switch ($Which) {
        'A' {
            # The quoted pointer count in the root README drifts by one.
            Replace-ExactlyOnce $readme ('185 ' + $GE + $ZHANG + $JIE + $ZHI + $ZHEN) ('184 ' + $GE + $ZHANG + $JIE + $ZHI + $ZHEN) 'A'
        }
        'B' {
            # The loose-rule count in the note's rule table drifts by one.
            #
            # The number it drifts FROM is the number the documents
            # quote at the moment -- so this anchor follows the docs.
            # When the docs move (and they did: the loose rule now
            # catches 18 of 37, not 20), this line has to move with
            # them, or the mutation matches nothing.
            #
            # And that is not a nuisance: "found 0" is LOUD.  A
            # mutation that quietly did nothing is a green light for
            # a broken gate, and this harness refuses to report one.
            Replace-ExactlyOnce $note ('| ' + $KUAN + $SHOU + ' | **18** |') ('| ' + $KUAN + $SHOU + ' | **17** |') 'B'
        }
        'C' {
            # The CHECKER'S OWN header drifts: the checker quotes the
            # census in its own comment, and that comment is a claim too.
            Replace-ExactlyOnce $checker '185 section pointers' '186 section pointers' 'C'
        }
        'D' {
            # THE RULER IS BROKEN -- and it has TWO blades.
            #
            # The census is checked two ways: a BLOCK claim compares a
            # list of numbers, a LINE claim compares the numbers in one
            # line.  Breaking only one of them leaves the other one
            # standing, and the harness then reads "the ruler caught
            # it" when it did not: measured on the runner, mutation D
            # left five block-claim failures behind.
            #
            # Both comparisons are broken here, and both replacements
            # are asserted.
            Replace-ExactlyOnce $docscan 'if ($nums[$g].Value -ne [int]$claim.Expect[$g]) {' 'if ($false) {' 'D (block comparison)'
            Replace-ExactlyOnce $docscan 'if ($got -ne $want) {' 'if ($false) {' 'D (line comparison)'
        }
        default { throw ("unknown mutation: " + $Which) }
    }
}

# ------------------------------------------------------------------
# 5. The gate: is the copy the tree?
# ------------------------------------------------------------------
$files = Get-TreeFiles $RepoRoot
Write-Host ("   the tree: {0} files (markdown + tools)" -f $files.Count)

$real = Invoke-Checker $HostExe (Join-Path $RepoRoot 'tools\check-sections.ps1') $RepoRoot
if (-not $real.Ok -or -not $real.SawCensus) {
    Write-Host "THE REAL TREE IS NOT GREEN -- a harness cannot measure a gate on a tree that is already broken" -ForegroundColor Red
    Write-Host ("   the real tree said: " + $real.Summary) -ForegroundColor Red
    exit 1
}
Write-Host ("   the real tree: " + $real.Summary)

$pristine = Join-Path $Work 'pristine'
Copy-Tree $RepoRoot $pristine $files

# The file sets have to be the same set.  A copy that lost a file, or
# gained one, or put one somewhere else, is not the tree.
$copyFiles = Get-TreeFiles $pristine
$srcRel = @{}
foreach ($e in $files) { $srcRel[$e.Rel] = $true }
$dstRel = @{}
foreach ($e in $copyFiles) { $dstRel[$e.Rel] = $true }
$missing = @($srcRel.Keys | Where-Object { -not $dstRel.ContainsKey($_) } | Sort-Object)
$extra = @($dstRel.Keys | Where-Object { -not $srcRel.ContainsKey($_) } | Sort-Object)
if ($missing.Count -gt 0 -or $extra.Count -gt 0) {
    Write-Host "THE COPY IS NOT THE TREE: the copy does not hold the same files" -ForegroundColor Red
    Write-Host ("   missing from the copy ({0}): {1}" -f $missing.Count, (($missing | Select-Object -First 5) -join ', ')) -ForegroundColor Red
    Write-Host ("   extra in the copy ({0}): {1}" -f $extra.Count, (($extra | Select-Object -First 5) -join ', ')) -ForegroundColor Red
    exit 1
}

$copy = Invoke-Checker $HostExe (Join-Path $pristine 'tools\check-sections.ps1') $pristine
if (-not $copy.Ok -or -not $copy.SawCensus) {
    Write-Host "THE COPY IS NOT THE TREE: the checker does not see the same tree in the copy" -ForegroundColor Red
    Write-Host ("   the real tree said: " + $real.Summary) -ForegroundColor Red
    Write-Host ("   the copy said:     " + $copy.Summary) -ForegroundColor Red
    exit 1
}
if ($copy.Summary -ne $real.Summary) {
    Write-Host "THE COPY IS NOT THE TREE: the copy measures something else" -ForegroundColor Red
    Write-Host ("   the real tree said: " + $real.Summary) -ForegroundColor Red
    Write-Host ("   the copy said:     " + $copy.Summary) -ForegroundColor Red
    exit 1
}
Write-Host ("   the copy says the same: " + $copy.Summary)

# ------------------------------------------------------------------
# 6. Run the mutations
# ------------------------------------------------------------------
$results = @()
foreach ($m in ($Mutation -split ',')) {
    $m = $m.Trim()
    if ($m -eq '') { continue }

    # A fresh copy per mutation: no reset, no leftovers, and a name
    # with no comma in it -- a path is not a list.
    $dest = Join-Path $Work ('mut-' + $m)
    Copy-Tree $RepoRoot $dest $files
    Apply-Mutation $m $dest

    $r = Invoke-Checker $HostExe (Join-Path $dest 'tools\check-sections.ps1') $dest

    Write-Host ("=== MUTATION {0} ===" -f $m)
    Write-Host ("  reds: {0} (of them unresolvable pointers: {1})" -f $r.Reds, $r.PtrReds)
    Write-Host ("  verdict: {0}" -f $(if ($r.Ok) { 'GREEN' } else { 'RED' }))
    if ($r.Summary -ne '') { Write-Host ("  summary: " + $r.Summary) }
    foreach ($l in (($r.Text -split "`r?`n") | Where-Object { $_ -match 'FAIL: ' } | Select-Object -First 6)) {
        Write-Host ("  " + $l.Trim())
    }

    if (-not $r.SawCensus) {
        Write-Host ("  UNEXPECTED: mutation " + $m + ": the checker did not report a census at all (it crashed?)") -ForegroundColor Red
        $results += $false
        continue
    }

    if ($m -eq 'D') {
        # A broken ruler reports NOTHING: no failures at all, and it
        # still says the gate passed.  Anything else means the ruler
        # was not broken, and the whole proof collapses.
        $ok = ($r.Reds -eq 0) -and $r.Ok
        if (-not $ok) {
            Write-Host ("  UNEXPECTED: mutation D should report ZERO failures (the ruler is blind), but it reported {0} and the verdict was {1}" -f $r.Reds, $(if ($r.Ok) { 'GREEN' } else { 'RED' })) -ForegroundColor Red
        }
        $results += $ok
    }
    else {
        # A drifting number must be CAUGHT, and it must be caught by
        # the number comparison -- not by a broken tree.  A pointer
        # that cannot be checked means the copy is not the tree, and
        # that is a failure of the harness, not a success of the gate.
        $ok = ($r.Reds -ge 1) -and ($r.PtrReds -eq 0)
        if (-not $ok) {
            Write-Host ("  UNEXPECTED: mutation " + $m + " should be caught by the number comparison (at least one failure, and no unresolvable pointer), but there were {0} failures and {1} unresolvable pointers" -f $r.Reds, $r.PtrReds) -ForegroundColor Red
        }
        $results += $ok
    }
}

if (Test-Path -LiteralPath $Work) { Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue }

if (@($results | Where-Object { -not $_ }).Count -gt 0) {
    Write-Host "MUTATION HARNESS FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "MUTATION HARNESS OK: every mutation came out the way it had to" -ForegroundColor Green
exit 0
