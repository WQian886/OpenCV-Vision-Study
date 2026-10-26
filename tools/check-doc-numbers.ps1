<#
.SYNOPSIS
    Check that the numbers quoted in the documentation still match reality.

.DESCRIPTION
    Debt 11 (notes/notes_W7_Knob_Audit_2026-10.md, section 10) says the audit
    has to follow the code. This script is the machine-checked half of that:
    it compares every CURRENT number quoted in the docs against the number
    that was just measured.

    Three things make it worth having:

      1. It checks the PER-SUITE table, not just the totals. A total can be
         right while the distribution is wrong (one suite +5, another -5),
         and the total looks perfectly normal. That is exactly the shape of
         the drift this project has already been bitten by twice.

      2. It does NOT hardcode the expected numbers. The expected numbers are
         read FROM the documentation; the measured numbers come from the build
         that just ran and from the tree itself. If the docs are stale, this
         fails -- and that is the whole point.

         So a claim's expected value is never written twice: the docs hold it
         once, and this script only knows WHERE to look. That is the
         difference between "a second copy that drifts" and "a check that
         holds the copy still".

      3. It refuses to guess. A claim that is not found is a FAILURE, not a
         skip. A rule that stops firing and reports nothing is the failure
         this project keeps paying for.

    Claims are anchored on ASCII tokens so this file can stay pure ASCII
    (PowerShell 5.1 reads a no-BOM .ps1 as ANSI/GBK; Chinese in here would
    be mangled, and a mangled pattern silently stops matching). Where the
    surrounding text is Chinese, the pattern steps over it with \D+.

    Some documents carry a CURRENT snapshot AND a HISTORICAL snapshot of the
    same shape. Those claims carry a scope ('After'): the lines from that
    marker to the next heading. Without it the checker would either flag
    the historical record or ignore both.

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's folder.

.PARAMETER PerSuiteFile
    The per-suite counts written by ci.ps1 step 3b.
    One line per suite: "<project>|<suite>|<assertions>".

.PARAMETER GitExe
    Path to git.exe. Needed to measure the tree. Auto-detected when omitted.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/check-doc-numbers.ps1 `
        -PerSuiteFile build/ci/assertions_by_suite.txt
#>
param(
    [string]$RepoRoot = "",
    [string]$PerSuiteFile = "",
    [string]$GitExe = ""
)

# NOTE: this file is deliberately pure ASCII, no BOM.
# Windows PowerShell 5.1 parses a .ps1 without a BOM as ANSI, so any
# non-ASCII byte in here turns into mojibake (or worse, a parse error).
# All commentary lives in the notes/ folder, which is UTF-8.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrEmpty($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
if ([string]::IsNullOrEmpty($PerSuiteFile)) {
    $PerSuiteFile = Join-Path $RepoRoot 'build\ci\assertions_by_suite.txt'
}

$Failures = New-Object System.Collections.ArrayList

function Fail([string]$msg) {
    [void]$Failures.Add($msg)
    Write-Host "   FAIL: $msg" -ForegroundColor Red
}

# Read a doc as UTF-8. Get-Content without -Encoding reads ANSI/GBK under
# PowerShell 5.1, which mangles the Chinese text and, worse, can swallow
# line endings (see notes/atomic/at-the-decoder-eats-the-newline.md).
function Get-DocLines([string]$rel) {
    $p = Join-Path $RepoRoot $rel
    if (-not (Test-Path $p)) {
        Fail "document not found: $rel"
        return @()
    }
    $t = [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8)
    return ,($t -split "`r?`n")
}

# "41,330" -> 41330
function Parse-Num([string]$s) {
    return [int](($s -replace ',', '').Trim())
}

function Find-Git([string]$given) {
    if (-not [string]::IsNullOrEmpty($given)) {
        if (-not (Test-Path $given)) { return $null }
        return $given
    }
    $c = Get-Command git -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @('C:/Program Files/Git/cmd/git.exe', 'C:/Tools/git/cmd/git.exe')) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

# ------------------------------------------------------------------
# 1. Read the measured counts
# ------------------------------------------------------------------
if (-not (Test-Path $PerSuiteFile)) {
    Write-Host "FAILED: per-suite assertion counts not found: $PerSuiteFile" -ForegroundColor Red
    Write-Host "   This file is produced by tools/ci.ps1 step 3b."
    exit 1
}

$Measured = @{}
foreach ($line in (Get-Content -LiteralPath $PerSuiteFile)) {
    $l = $line.Trim()
    if ($l -eq '' -or $l.StartsWith('#')) { continue }
    $parts = $l -split '\|'
    if ($parts.Count -ne 3) {
        Fail "malformed line in $PerSuiteFile : $l"
        continue
    }
    $proj = $parts[0].Trim()
    $suite = $parts[1].Trim()
    $n = 0
    if (-not [int]::TryParse($parts[2].Trim(), [ref]$n)) {
        Fail "malformed assertion count in $PerSuiteFile : $l"
        continue
    }
    if (-not $Measured.ContainsKey($proj)) { $Measured[$proj] = @{} }
    $Measured[$proj][$suite] = $n
}

if ($Measured.Count -eq 0) {
    Write-Host "FAILED: no per-suite counts in $PerSuiteFile" -ForegroundColor Red
    Write-Host "   A count of zero would be worse than no count at all."
    exit 1
}

$W7Proj = 'W7_VisionMasterLite'
$W6Proj = 'W6_PCB_DefectProject'
foreach ($p in @($W7Proj, $W6Proj)) {
    if (-not $Measured.ContainsKey($p)) {
        Fail "the per-suite file has no entry for project '$p'"
    }
}
if ($Failures.Count -gt 0) { Write-Host ""; Write-Host "DOC NUMBERS FAILED" -ForegroundColor Red; exit 1 }

$W7 = $Measured[$W7Proj]
$W6 = $Measured[$W6Proj]
$W7Suites = $W7.Count
$W6Suites = $W6.Count
$W7Asserts = ($W7.Values | Measure-Object -Sum).Sum
$W6Asserts = ($W6.Values | Measure-Object -Sum).Sum
$AllSuites = $W7Suites + $W6Suites
$AllAsserts = $W7Asserts + $W6Asserts

Write-Host ("   measured (build): {0} {1} suites / {2} assertions" -f $W7Proj, $W7Suites, $W7Asserts)
Write-Host ("   measured (build): {0} {1} suites / {2} assertions" -f $W6Proj, $W6Suites, $W6Asserts)
Write-Host ("   measured (build): TOTAL {0} suites / {1} assertions" -f $AllSuites, $AllAsserts)

# ------------------------------------------------------------------
# 2. Measure the tree itself
# ------------------------------------------------------------------
# These are the numbers that are quoted in the docs but are NOT produced by
# the test run: how much C++ there is, how big the notes are.
#
# Both are measured the way the README says to measure them, so that the
# "this number is reproducible" claim in the README is itself checked.
$Git = Find-Git $GitExe
if (-not $Git) {
    Fail "git not found -- cannot measure the tree (pass -GitExe)"
}

$CppFiles = 0
$CppLines = 0
$NoteFiles = 0
$AtomicFiles = 0
$NoteBytes = 0

if ($Git) {
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 0
    Push-Location $RepoRoot
    try {
        # EXACTLY the command the README documents. If the README's
        # reproducibility claim is wrong, this is where it shows up.
        $cpp = @(& $Git ls-files '*.cpp' '*.h')
        $CppFiles = $cpp.Count
        foreach ($f in $cpp) {
            $CppLines += (Get-Content -Encoding UTF8 -LiteralPath $f).Count
        }
    }
    finally { Pop-Location }
    $ErrorActionPreference = 'Stop'
}

# The notes are measured from the WORKING TREE, not from the git index.
#
# Two reasons, and the second one is the important one:
#   * the index can be stale (it holds what was last staged, not what is on
#     disk right now), and measuring a stale object is the exact disease
#     this project keeps paying for;
#   * a byte count depends on the line endings, and the working tree may
#     have CRLF where a fresh clone has LF. So the bytes are normalised to
#     LF first -- that makes the number identical on every machine.
$noteDir = Join-Path $RepoRoot 'notes'
if (Test-Path $noteDir) {
    foreach ($f in (Get-ChildItem -Path $noteDir -Recurse -File -Filter *.md)) {
        $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
        $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
        $start = 0
        if ($hasBom) { $start = 3 }
        $text = [System.Text.Encoding]::UTF8.GetString($bytes, $start, $bytes.Length - $start)
        $norm = $text.Replace("`r`n", "`n")
        $NoteBytes += [System.Text.Encoding]::UTF8.GetByteCount($norm)
        if ($hasBom) { $NoteBytes += 3 }
        if ($f.DirectoryName -like (Join-Path $noteDir 'atomic')) { $AtomicFiles++ } else { $NoteFiles++ }
    }
}

$NoteKB = [int][math]::Round($NoteBytes / 1024)

Write-Host ("   measured (tree) : {0} C++ files / {1} lines" -f $CppFiles, $CppLines)
Write-Host ("   measured (tree) : {0} notes + {1} atomic cards / {2} KB" -f $NoteFiles, $AtomicFiles, $NoteKB)

# ------------------------------------------------------------------
# 3. The strong check: the per-suite table
# ------------------------------------------------------------------
# This is the one that catches "the total is right but the distribution
# is wrong". The table in W7_VisionMasterLite/README.md is the single
# place where every suite's count is written down.
Write-Host ""
Write-Host "   -- per-suite table (W7_VisionMasterLite/README.md)"

$w7Readme = Get-DocLines 'W7_VisionMasterLite/README.md'
$DocRows = @{}
foreach ($line in $w7Readme) {
    $m = [regex]::Match($line, '^\|\s*`([A-Za-z0-9_]+)`\s*\|\s*(\d+)\s*\|')
    if ($m.Success) { $DocRows[$m.Groups[1].Value] = [int]$m.Groups[2].Value }
}

if ($DocRows.Count -eq 0) {
    Fail ("the per-suite table in W7_VisionMasterLite/README.md was not found." +
          "`n   The checker's pattern no longer matches the table -- update it here." +
          "`n   A table that cannot be read is not a table that can be checked.")
}
else {
    if ($DocRows.Count -ne $W7Suites) {
        Fail ("the per-suite table lists {0} suites, reality has {1}" -f $DocRows.Count, $W7Suites)
    }
    foreach ($name in ($W7.Keys | Sort-Object)) {
        if (-not $DocRows.ContainsKey($name)) {
            Fail ("the per-suite table has no row for suite '$name' (reality: {0} assertions)" -f $W7[$name])
        }
        elseif ($DocRows[$name] -ne $W7[$name]) {
            Fail ("suite '$name': the table says {0} assertions, reality says {1}" -f $DocRows[$name], $W7[$name])
        }
    }
    foreach ($name in ($DocRows.Keys | Sort-Object)) {
        if (-not $W7.ContainsKey($name)) {
            Fail "the per-suite table lists a suite '$name' that does not exist"
        }
    }
    $docSum = ($DocRows.Values | Measure-Object -Sum).Sum
    if ($docSum -ne $W7Asserts) {
        Fail ("the per-suite table adds up to {0}, reality says {1}" -f $docSum, $W7Asserts)
    }
    if ($Failures.Count -eq 0) {
        Write-Host ("   {0} suites, every row matches, sum = {1}" -f $DocRows.Count, $docSum)
    }
}

# ------------------------------------------------------------------
# 4. The numbers quoted in prose
# ------------------------------------------------------------------
# Each claim names a place in a document and the numbers it must carry.
# The EXPECTED values come from the measurements above, never from here.
#
# The patterns step over Chinese text with \D+ and anchor on ASCII tokens.
# \D+ between two numbers is safe: it cannot swallow a digit, so the two
# captured numbers are still the two numbers that are actually written.
Write-Host ""
Write-Host "   -- numbers quoted in the docs"

function Test-Claim($claim, $lines) {
    # Optional scope: 'After' is an ASCII regex that locates a section.
    # Only the lines from there to the next markdown heading are considered.
    #
    # This exists because some documents carry a CURRENT snapshot AND a
    # HISTORICAL snapshot of the same shape. A checker that cannot tell
    # them apart either flags the historical one (a false alarm on a
    # deliberate record) or ignores both (no check at all).
    $scope = $lines
    if ($claim.ContainsKey('After')) {
        $start = -1
        for ($i = $lines.Count - 1; $i -ge 0; $i--) {
            if ($lines[$i] -match $claim.After) { $start = $i; break }
        }
        if ($start -lt 0) {
            return ("the section marker was NOT FOUND -- the anchor in" +
                    " tools/check-doc-numbers.ps1 has to be updated. It is deliberately" +
                    " not skipped: a checker that stops recognising its own docs and" +
                    " says nothing is the failure this project keeps paying for.")
        }
        $end = $lines.Count
        for ($i = $start + 1; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^#{1,6}\s') { $end = $i; break }
        }
        if ($end -le $start) { $end = $start + 1 }
        $scope = $lines[$start..($end - 1)]
    }

    $hits = @()
    foreach ($line in $scope) {
        $m = [regex]::Match($line, $claim.Pattern)
        if ($m.Success) { $hits += ,$m }
    }
    if ($hits.Count -eq 0) {
        return ("the claim was NOT FOUND -- the wording changed, so the anchor in" +
                " tools/check-doc-numbers.ps1 has to be updated. It is deliberately" +
                " not skipped: a checker that stops recognising its own docs and" +
                " says nothing is the failure this project keeps paying for.")
    }
    $bad = @()
    foreach ($m in $hits) {
        for ($g = 1; $g -le $claim.Expect.Count; $g++) {
            $got = Parse-Num $m.Groups[$g].Value
            $want = [int]$claim.Expect[$g - 1]
            if ($got -ne $want) {
                $bad += ("group {0} is {1}, reality says {2}" -f $g, $got, $want)
            }
        }
    }
    if ($bad.Count -gt 0) { return ($bad -join '; ') }
    return $null
}

$Claims = @(
    # ---- root README -------------------------------------------------
    @{ Label = 'root README: W6 project bullet'
       File  = 'README.md'
       Pattern = '^\- \*\*W6 PCB.*CSV.*\*\*(\d+)\D+(\d+)\D+\*\*$'
       Expect = @($W6Suites, $W6Asserts) }
    @{ Label = 'root README: W7 project bullet'
       File  = 'README.md'
       Pattern = '^\- \*\*W7 Mini.*?pipeline\.json.*?\D+(\d+)\D+(\d+)\D+'
       Expect = @($W7Suites, $W7Asserts) }
    @{ Label = 'root README: W7 test section'
       File  = 'README.md'
       Pattern = '^(\d+)\D+(\d+)\D+.*QT_QPA_PLATFORM=offscreen'
       Expect = @($W7Suites, $W7Asserts) }
    @{ Label = 'root README: statistics block'
       File  = 'README.md'
       Pattern = '^\- \*\*(\d+)\D+(\d+)\D+\*\*.*W7 (\d+)\D+(\d+).*W6 (\d+)\D+(\d+)'
       Expect = @($AllSuites, $AllAsserts, $W7Suites, $W7Asserts, $W6Suites, $W6Asserts) }
    @{ Label = 'root README: dl_infer halves'
       File  = 'README.md'
       Pattern = '`dl_infer`.*#else.*\*\*\D+(\d+)\s*/\s*(\d+)\D+'
       Expect = @($W7['dl_infer'], $W7['dl_infer_nosdk']) }
    @{ Label = 'root README: camera halves'
       File  = 'README.md'
       Pattern = '`image_source_camera`.*#else.*\*\*\D+(\d+)\s*/\s*(\d+)\D+'
       Expect = @($W7['camera'], $W7['camera_nosdk']) }
    @{ Label = 'root README: C++ size'
       File  = 'README.md'
       Pattern = '(\d+)\D+C\+\+\D+/\s*\*\*([\d,]+)\D+\*\*'
       Expect = @($CppFiles, $CppLines) }
    @{ Label = 'root README: notes volume'
       File  = 'README.md'
       Pattern = '(\d+)\D+\+\s*(\d+)\D+atomic\D+~(\d+) KB'
       Expect = @($NoteFiles, $AtomicFiles, $NoteKB) }

    # ---- W7 README ---------------------------------------------------
    @{ Label = 'W7 README: source tree listing'
       File  = 'W7_VisionMasterLite/README.md'
       Pattern = 'tests/.*#\s*(\d+)\D+\(\s*(\d+)\D+\)'
       Expect = @($W7Suites, $W7Asserts) }
    @{ Label = 'W7 README: test section heading'
       File  = 'W7_VisionMasterLite/README.md'
       Pattern = '^##\s+.*\(.*\D+(\d+)\D+/\s*(\d+)\D+\)'
       Expect = @($W7Asserts, $W7Suites) }

    # ---- close report ------------------------------------------------
    # The close report carries TWO tables of this shape: section 3.2 is
    # the snapshot taken on the day of sign-off, section 3.5 is the live
    # one. Only the live one is a claim about NOW. The scope below is what
    # keeps the historical snapshot from being flagged -- and, more
    # importantly, what keeps it from being silently ignored too.
    @{ Label = 'close report: number table (W7)'
       File  = 'notes/notes_W7_Close_2026-10.md'
       After = '^### 3\.5'
       Pattern = '^\|\s*W7\s*\D+\|\s*\*\*(\d+)\s*/\s*(\d+)\*\*\s*\|'
       Expect = @($W7Suites, $W7Asserts) }
    @{ Label = 'close report: number table (W6)'
       File  = 'notes/notes_W7_Close_2026-10.md'
       After = '^### 3\.5'
       Pattern = '^\|\s*W6\s*\D+\|\s*\*\*(\d+)\s*/\s*(\d+)\*\*\s*\|'
       Expect = @($W6Suites, $W6Asserts) }
    @{ Label = 'close report: number table (whole repo)'
       File  = 'notes/notes_W7_Close_2026-10.md'
       After = '^### 3\.5'
       Pattern = '^\|\s*[^A-Za-z]*\s*\|\s*\*\*(\d+)\s*/\s*(\d+)\*\*\s*\|\s*$'
       Expect = @($AllSuites, $AllAsserts) }
    @{ Label = 'close report: number table (C++ size)'
       File  = 'notes/notes_W7_Close_2026-10.md'
       After = '^### 3\.5'
       Pattern = '^\|\s*C\+\+\s*\D+\|\s*\*\*(\d+)\s*/\s*([\d,]+)\*\*\s*\|'
       Expect = @($CppFiles, $CppLines) }
    @{ Label = 'close report: number table (notes)'
       File  = 'notes/notes_W7_Close_2026-10.md'
       After = '^### 3\.5'
       Pattern = '^\|\s*notes\s*\|\s*\*\*(\d+)\*\*\D+~(\d+) KB'
       Expect = @($NoteFiles, $NoteKB) }
    @{ Label = 'close report: number table (atomic cards)'
       File  = 'notes/notes_W7_Close_2026-10.md'
       After = '^### 3\.5'
       Pattern = '^\|\s*atomic\s*\D+\|\s*\*\*(\d+)\*\*\s*\D+\|'
       Expect = @($AtomicFiles) }
    @{ Label = 'close report: expected CI result'
       File  = 'notes/notes_W7_Close_2026-10.md'
       Pattern = 'ALL CHECKS PASSED\D*\*\*(\d+)\*\*\D+/\s*\*\*(\d+)\*\*'
       Expect = @($AllSuites, $AllAsserts) }
    @{ Label = 'close report: self-check list'
       File  = 'notes/notes_W7_Close_2026-10.md'
       Pattern = '^\- \[x\] \*\*(\d+)\*\*\D+/\s*\*\*(\d+)\*\*'
       Expect = @($AllSuites, $AllAsserts) }

    # ---- knob audit --------------------------------------------------
    @{ Label = 'knob audit: W6 checklist'
       File  = 'notes/notes_W7_Knob_Audit_2026-10.md'
       Pattern = '^\- \[x\]\s*W6\s+(\d+)\D+/\s*(\d+)\D+'
       Expect = @($W6Suites, $W6Asserts) }
    @{ Label = 'knob audit: final numbers'
       File  = 'notes/notes_W7_Knob_Audit_2026-10.md'
       Pattern = 'W7\s+(\d+)\D+/\s*(\d+)\D+.*W6\s+(\d+)\D+/\s*(\d+)\D+'
       Expect = @($W7Suites, $W7Asserts, $W6Suites, $W6Asserts) }
)

$DocCache = @{}
foreach ($claim in $Claims) {
    if (-not $DocCache.ContainsKey($claim.File)) {
        $DocCache[$claim.File] = Get-DocLines $claim.File
    }
    $why = Test-Claim $claim $DocCache[$claim.File]
    if ($null -eq $why) {
        Write-Host ("   OK   {0}" -f $claim.Label)
    }
    else {
        Fail ("{0} [{1}]: {2}" -f $claim.Label, $claim.File, $why)
    }
}

# ------------------------------------------------------------------
# 5. Summary
# ------------------------------------------------------------------
Write-Host ""
if ($Failures.Count -gt 0) {
    Write-Host ("{0} documentation number(s) do not match reality:" -f $Failures.Count) -ForegroundColor Red
    foreach ($f in $Failures) { Write-Host "   - $f" -ForegroundColor Red }
    Write-Host ""
    Write-Host "Either the docs are stale, or the anchors in this script need updating." -ForegroundColor Red
    Write-Host "DOC NUMBERS FAILED" -ForegroundColor Red
    exit 1
}

Write-Host ("DOC NUMBERS OK: {0} claims + the per-suite table agree with reality" -f $Claims.Count) -ForegroundColor Green
exit 0
