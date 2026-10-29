#requires -Version 5.1
<#
    Mutation harness for the three DOCUMENTATION gates:

      3c  tools/check-doc-numbers.ps1   the numbers the docs quote
      3d  tools/check-links.ps1         the links in the docs
      3e  tools/check-sections.ps1      the section pointers in the docs

    Why this file is in the repository:
    a gate that has only ever been GREEN is a gate nobody knows works.
    The checker could be broken in a way that makes it say OK about
    anything at all -- and the CI log would look exactly the same.  The
    census gate already has a harness of its own (tools/mutate-census.ps1,
    CI step 3f); these three did not, so nothing had ever proved they
    can go red.

    What each mutation does:

      A, B, C   a defect is put into the tree.  The gate MUST go RED,
                and it must say so in the way the mutation was meant to
                be caught (the number comparison / the link resolution /
                the pointer resolution -- not some other failure).

      D         THE RULER IS BROKEN, and the defect is STILL THERE.
                The gate MUST go GREEN, and it must say EXACTLY what it
                says about the PRISTINE tree -- the broken tree in
                front of it is not being measured at all.

    D is the one that carries the proof.  "A, B and C came out red"
    only shows that the checker and the mutation behaved consistently at
    some moment; it does not show that the CHECKER is what made it red.
    D shows that: take the same tree, break the thing that decides
    red, and the redness goes away.  A gate that stays red with a blind
    ruler was never measuring the tree at all.

    Note that D carries the defect WITH it.  A broken ruler on a
    perfect tree is green for a trivial reason (there is nothing to
    catch), and that proves nothing.  The defect has to be there, or
    there is no redness to explain.

    Pure ASCII, no BOM: the Chinese it needs is built from code points.

    Usage:
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\mutate-gates.ps1
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\mutate-gates.ps1 -Gate 3e
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\mutate-gates.ps1 -Mutation A,B,C,D
#>

[CmdletBinding()]
param(
    [string]$Mutation = 'A,B,C,D',
    [string]$RepoRoot,
    [string]$Work,
    [string]$Gate = 'all'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $RepoRoot) { $RepoRoot = (Split-Path -Parent $PSScriptRoot) }
# The PROVIDER's path, not the string we were handed.  On a Windows
# runner $env:TEMP is the 8.3 short form ("C:\Users\RUNNER~1\...") and
# the provider expands it ("C:\Users\runneradmin\...") -- two strings,
# one directory, and a relative path sliced out of the wrong one starts
# three characters into the middle of a name.
$RepoRoot = (Get-Item -LiteralPath $RepoRoot -ErrorAction Stop).FullName.TrimEnd([char]'\', [char]'/')
if (-not $Work) {
    $Work = Join-Path $env:TEMP ('opencv-mutate-gates-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
}

. (Join-Path $PSScriptRoot 'lib\MutHarness.ps1')

# ------------------------------------------------------------------
# The characters the mutations need, from their code points
# ------------------------------------------------------------------
$SHI = Ch 0x5341   # "ten"
$YI  = Ch 0x4E00   # "one"
$DUN = Ch 0x3001   # the ideographic comma in a heading
$DE  = Ch 0x7684   # the possessive particle
$DI  = Ch 0x7B2C   # "chapter/section"
$JIE = Ch 0x8282   # "section"

# The pointer in the root README that names the W1/W2 note's chapter 9.
# It is written out here from code points because a .ps1 without a BOM
# is read as ANSI, and a Chinese literal in here becomes a different
# character -- and a pattern that matches nothing is indistinguishable
# from a pass.
$PtrOld = '](./notes/notes_W1W2_Tests_2026-10.md) ' + $DE + $DI + ' 9 ' + $JIE
$PtrNew = '](./notes/notes_W1W2_Tests_2026-10.md) ' + $DE + $DI + ' 99 ' + $JIE
# The same pointer, but naming a file that is not in the tree.
$PtrTypo = '](./notes/notes_W1W2_Tests_TYPO.md) ' + $DE + $DI + ' 9 ' + $JIE

# ------------------------------------------------------------------
# The documentation the checkers read
# ------------------------------------------------------------------
#
# The same scope the checkers use (see Get-DocFiles in tools/lib/DocScan.ps1):
# every markdown file in the WORKING tree, with build/ and .git/ left
# out -- build products are not documentation, and .git holds objects,
# not notes.
function Get-MdRelSet([string]$root) {
    $base = (Get-Item -LiteralPath $root -ErrorAction Stop).FullName.TrimEnd([char]'\', [char]'/')
    $set = @{}
    foreach ($f in (Get-ChildItem -Path $root -Recurse -File -Filter *.md)) {
        $rel = $f.FullName.Substring($base.Length).TrimStart([char]'\', [char]'/')
        if ($rel -match '(^|[\\/])(build|\.git)([\\/]|$)') { continue }
        $set[$rel] = $true
    }
    return $set
}

# What a checker said, when it said something other than its summary.
# A checker that printed nothing has said nothing about the tree, and
# "it printed nothing" is not a diagnosis.
function Show-MutOutput([string]$text, [string]$who) {
    $lines = @($text -split "`r?`n" | Where-Object { $_.Trim() -ne '' })
    if ($lines.Count -eq 0) {
        Write-Host ("   {0} printed NOTHING at all." -f $who) -ForegroundColor Red
        return
    }
    Write-Host ("   {0} said:" -f $who) -ForegroundColor Red
    foreach ($l in ($lines | Select-Object -First 8)) { Write-Host ("      " + $l.Trim()) -ForegroundColor Red }
    if ($lines.Count -gt 8) {
        Write-Host ("      ... and {0} more line(s)" -f ($lines.Count - 8)) -ForegroundColor Red
    }
}

# ------------------------------------------------------------------
# The gates
# ------------------------------------------------------------------
#
# 'Copy' says what has to be copied for the gate to measure the same
# thing in the copy that it measures in the tree:
#
#   'git'  every TRACKED file, and the copy is a git repository with
#          the same index.  The number gate measures the tree with
#          `git ls-files`, so a copy that is not a git repository
#          measures nothing there.
#
# All three gates use the same copy.  It is tempting to give the link
# gate and the section gate a smaller copy -- they only read markdown
# -- and it is WRONG: a link is allowed to point at anything (a .cpp, a
# .txt, a picture), so a copy that holds only the markdown measures a
# tree where every link to a non-markdown file is broken.  Measured: the
# first run of the link gate on such a copy said nothing at all.
#
# The copy holds the TRACKED files.  The checkers read the WORKING
# tree, so an untracked markdown file is in the real tree and not in
# the copy -- and the harness says so, loudly, instead of measuring two
# different trees.
$Gates = @(
    [pscustomobject]@{
        Id       = '3c'
        Name     = 'the numbers the docs quote'
        Checker  = 'tools\check-doc-numbers.ps1'
        Summary  = '(?m)^DOC NUMBERS OK: .*$'
        Copy     = 'git'
        PerSuite = $true
        Expect   = @{
            'A' = 'root README: C++ size'
            'B' = 'the table says'
            'C' = 'README says'
        }
    }
    [pscustomobject]@{
        Id       = '3d'
        Name     = 'the links in the docs'
        Checker  = 'tools\check-links.ps1'
        Summary  = '(?m)^LINKS OK: .*$'
        Copy     = 'git'
        PerSuite = $false
        Expect   = @{
            'A' = 'does not resolve'
            'B' = 'has no heading'
            'C' = 'no such heading in that file'
        }
    }
    [pscustomobject]@{
        Id       = '3e'
        Name     = 'the section pointers in the docs'
        Checker  = 'tools\check-sections.ps1'
        Summary  = '(?m)^SECTIONS OK: .*$'
        Copy     = 'git'
        PerSuite = $false
        Expect   = @{
            'A' = 'does not resolve'
            'B' = 'cannot be checked'
            'C' = 'does not resolve'
        }
    }
)

# ------------------------------------------------------------------
# The mutations, one function per gate
# ------------------------------------------------------------------
#
# Every replacement asserts that it replaced exactly what it was
# supposed to replace.  A mutation that did not happen must be loud:
# a green light for a broken gate is the failure this project keeps
# paying for.
function Apply-3c([string]$dest, [string]$which) {
    $readme  = Join-Path $dest 'README.md'
    $w7      = Join-Path $dest 'W7_VisionMasterLite\README.md'
    $docscan = Join-Path $dest 'tools\lib\DocScan.ps1'
    switch ($which) {
        'A' {
            # The C++ size quoted in the root README drifts by one line.
            # The comparison that catches it is the LINE claim in
            # tools/lib/DocScan.ps1 -- the same blade mutation D breaks.
            Replace-MutOnce $readme '**45,272 ' '**45,271 ' '3c/A'
        }
        'B' {
            # A row of a per-suite table drifts.  The table is checked
            # against what the build measured, so this is the
            # distribution going wrong while the total stays right --
            # the exact shape the table was added to catch.
            Replace-MutOnce $w7 '| `pure_layer` | 33 |' '| `pure_layer` | 32 |' '3c/B'
        }
        'C' {
            # A row of the per-note size table in the root README
            # drifts.
            Replace-MutOnce $readme '- `notes_W4_Tests_2026-10.md` (16 KB)' `
                            '- `notes_W4_Tests_2026-10.md` (15 KB)' '3c/C'
        }
        'D' {
            # THE RULER IS BROKEN: the comparisons that decide whether a
            # quoted number is right now always say "right".
            #
            # There are TWO blades in tools/lib/DocScan.ps1 -- one for a
            # BLOCK claim (a run of lines, compared number by number) and
            # one for a LINE claim (a pattern, compared group by group).
            # Both are blinded, because a ruler with one blade still
            # standing is a ruler that can still catch something.
            #
            # What is left standing on purpose: the check that the
            # pattern still MATCHES at all ("the claim was NOT FOUND").
            # That is not a ruler, it is a tripwire: it says the docs
            # and the checker have drifted apart, and no blind ruler
            # should be able to hide THAT.
            Replace-MutOnce $docscan 'if ($nums[$g].Value -ne [int]$claim.Expect[$g]) {' `
                              'if ($false) {' '3c/D (the block blade)'
            Replace-MutOnce $docscan 'if ($got -ne $want) {' 'if ($false) {' '3c/D (the line blade)'
            # ... and the defect is STILL THERE.
            Replace-MutOnce $readme '**45,272 ' '**45,271 ' '3c/D+defect'
        }
        default { throw ("unknown mutation for 3c: " + $which) }
    }
}

function Apply-3d([string]$dest, [string]$which) {
    $readme = Join-Path $dest 'README.md'
    $links  = Join-Path $dest 'tools\check-links.ps1'
    switch ($which) {
        'A' {
            # A relative link points at a file that is not there.
            Replace-MutOnce $readme '(./W1_ImageIO/README.md)' `
                            '(./W1_ImageIO/README_MISSING.md)' '3d/A'
        }
        'B' {
            # A link names a heading in ANOTHER file that is not there.
            # (The repository had NO anchor links at all before this
            # harness -- so this branch of the checker had never been
            # seen to fire.)
            #
            # The '#' goes INSIDE the parentheses: the link pattern is
            # "](" ... ")", and an anchor written outside them is not
            # part of the link at all.  Measured: the first run of this
            # mutation came out GREEN, because the checker never saw an
            # anchor -- it saw a plain file link that resolves.
            Replace-MutOnce $readme '(./notes/notes_W7_Close_2026-10.md)' `
                            '(./notes/notes_W7_Close_2026-10.md#no-such-heading-zzz)' '3d/B'
        }
        'C' {
            # A link names a heading in ITS OWN file that is not there.
            Replace-MutOnce $readme '[README](./W7_VisionMasterLite/README.md)' `
                            '[README](./W7_VisionMasterLite/README.md) [broken](#no-such-heading-zzz)' '3d/C'
        }
        'D' {
            # THE RULER IS BROKEN -- and it has THREE blades, because
            # the checker can fail in three ways: the file is not
            # there, the target is a directory, the heading is not
            # there.  Breaking only one of them would leave the others
            # standing, and the harness would read "the ruler caught it"
            # when it did not.
            #
            # The two heading checks are the same sentence in the file
            # (once for a heading in this file, once for a heading in
            # another one), so that replacement is asserted twice.
            Replace-MutCount $links 'if (-not $set.Contains((ConvertTo-GitHubSlug $anchor))) {' `
                              'if ($false) {' '3d/D (the heading checks)' 2
            # The unresolvable-file branch: instead of reporting the
            # broken link, it now treats the link as resolved -- so the
            # summary it prints is EXACTLY the one it prints about a
            # perfect tree.
            Replace-MutBlock $links @(
                '        if ($null -eq $resolved) {',
                '            Fail ("{0}: link ''{1}'' does not resolve (looked for ''{2}'')" -f `',
                '                  $relFile, $target, (Join-Path $f.DirectoryName $pathPart))',
                '            continue',
                '        }'
            ) @(
                '        if ($null -eq $resolved) {',
                '            # MUTATED: the ruler is blind.',
                '            $resolved = $f.FullName',
                '        }'
            ) '3d/D (the unresolvable-file check)'
            Replace-MutCount $links 'if (-not (Test-Path $resolved -PathType Leaf)) {' `
                              'if ($false) {' '3d/D (the directory check)' 1
            # ... and the defect is STILL THERE.
            Replace-MutOnce $readme '(./W1_ImageIO/README.md)' `
                            '(./W1_ImageIO/README_MISSING.md)' '3d/D+defect'
        }
        default { throw ("unknown mutation for 3d: " + $which) }
    }
}

function Apply-3e([string]$dest, [string]$which) {
    $audit    = Join-Path $dest 'notes\notes_W7_Knob_Audit_2026-10.md'
    $readme   = Join-Path $dest 'README.md'
    $sections = Join-Path $dest 'tools\check-sections.ps1'
    switch ($which) {
        'A' {
            # A chapter of the debt register is RENUMBERED.  The
            # pointers do not change -- one character in one heading
            # changes, and every pointer aimed at that chapter starts
            # pointing at nothing while still reading perfectly.
            #
            # This is the mutation the section-pointer note measured:
            # one heading, dozens of pointers, all of them silently
            # wrong.
            Replace-MutOnce $audit ('## ' + $SHI + $DUN) ('## ' + $SHI + $YI + $DUN) '3e/A'
        }
        'B' {
            # The file the pointer NAMES is not in the tree.  This is
            # the checker's THIRD way to fail a pointer: not "that
            # chapter is not there" (mutation C), but "that FILE is not
            # there, so the claim cannot even be checked".
            #
            # The link gate would also go red on this copy -- the link
            # really is broken -- but that is a different gate, with its
            # own copy, and this one is about what the SECTION gate says
            # about a pointer bound to a file that does not exist.
            Replace-MutOnce $readme $PtrOld $PtrTypo '3e/B'
        }
        'C' {
            # A POINTER drifts, not a heading.  This one still reads
            # perfectly -- "chapter 99" is as readable as "chapter 9" --
            # and it now points at a chapter that does not exist.
            #
            # It is aimed at the W1/W2 note, and that file is not one of
            # the files whose pointer count the docs quote -- so the
            # only thing that changes is that one pointer.
            Replace-MutOnce $readme $PtrOld $PtrNew '3e/C'
        }
        'D' {
            # THE RULER IS BROKEN: the rule that decides whether a
            # pointer resolves now always says "it resolves".
            #
            # The bucket a pointer is counted in (cross-file or
            # same-file) comes from the BINDING, not from whether it
            # resolved -- so the counts the checker prints stay exactly
            # the ones it prints about a perfect tree, and the summary
            # is identical.  A blind ruler here does not even change
            # the numbers.
            Replace-MutOnce $sections '$ok = ($null -ne $set) -and $set.Contains($p.Token)' `
                              '$ok = ($null -ne $set) -and $true' '3e/D'
            # ... and the defect is STILL THERE.
            Replace-MutOnce $readme $PtrOld $PtrNew '3e/D+defect'
        }
        default { throw ("unknown mutation for 3e: " + $which) }
    }
}

# ------------------------------------------------------------------
# The harness
# ------------------------------------------------------------------
$HostExe = Get-MutHost
$verText = (& $HostExe -NoProfile -Command '$PSVersionTable.PSVersion.ToString()')
if ($LASTEXITCODE -ne 0 -or (($verText | Out-String) -notmatch '\d+\.\d+')) {
    throw ("the executable at " + $HostExe + " did not answer as PowerShell." +
           " It said: " + (($verText | Out-String).Trim()))
}
Write-Host ("   host: {0} (PowerShell {1})" -f $HostExe, (($verText | Out-String).Trim()))

$GitExe = (Get-Command git -ErrorAction SilentlyContinue)
if (-not $GitExe) { throw "git not found -- the number gate measures the tree with git ls-files" }
$GitExe = $GitExe.Source

$mutations = @($Mutation -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
if ($mutations.Count -eq 0) { throw "no mutation was asked for" }

# The per-suite counts come from the build that just ran.  They are
# handed to the copy by ABSOLUTE path: a relative path would be
# resolved against whatever directory the checker happens to be in.
$PerSuite = Join-Path $RepoRoot 'build\ci\assertions_by_suite.txt'

$results = New-Object System.Collections.ArrayList
$failed = 0

foreach ($g in $Gates) {
    if ($Gate -ne 'all' -and $g.Id -ne $Gate) { continue }

    Write-Host ""
    Write-Host ("=== GATE {0}: {1} ===" -f $g.Id, $g.Name)

    $extra = @()
    if ($g.PerSuite) {
        if (-not (Test-Path -LiteralPath $PerSuite)) {
            Write-Host ("FAILED: the number gate measures the tree with the counts the build wrote, and" +
                        " those counts are not here: " + $PerSuite) -ForegroundColor Red
            Write-Host "   They are produced by tools/ci.ps1 step 3b.  A harness that cannot measure the" -ForegroundColor Red
            Write-Host "   gate it claims to measure must not report a verdict about it." -ForegroundColor Red
            exit 1
        }
        $extra = @('-PerSuiteFile', $PerSuite)
    }

    # --- the real tree -------------------------------------------------
    $real = Invoke-MutChecker $HostExe (Join-Path $RepoRoot $g.Checker) $RepoRoot $extra $g.Summary
    if (-not $real.Ok) {
        Write-Host "THE REAL TREE IS NOT GREEN -- a harness cannot measure a gate on a tree that is already broken" -ForegroundColor Red
        Write-Host ("   the real tree said: " + $real.Summary) -ForegroundColor Red
        if ($real.Summary -eq '') { Show-MutOutput $real.Text 'the real tree' }
        $failed++
        continue
    }
    Write-Host ("   the real tree: " + $real.Summary)

    # --- the pristine copy ---------------------------------------------
    $pristineDir = Join-Path $Work ('pristine-' + $g.Id)
    $files = New-MutGitCopy $RepoRoot $pristineDir $GitExe
    Write-Host ("   the tree: {0} tracked files" -f $files.Count)

    # The markdown the checkers READ has to be the same markdown in the
    # copy as in the tree.  The copy holds the TRACKED files; the
    # checkers read the WORKING tree.  So an untracked note is in the
    # tree and not in the copy -- and the two are different trees.
    $srcMd = Get-MdRelSet $RepoRoot
    $dstMd = Get-MdRelSet $pristineDir
    # NOT named $missing / $extra: $extra is the checker's argument
    # list, and a file-set variable that overwrites it makes the copy
    # run without the per-suite counts -- which is a checker that
    # measures nothing, and a harness that cannot tell.
    $onlyInTree = @($srcMd.Keys | Where-Object { -not $dstMd.ContainsKey($_) } | Sort-Object)
    $onlyInCopy = @($dstMd.Keys | Where-Object { -not $srcMd.ContainsKey($_) } | Sort-Object)
    if ($onlyInTree.Count -gt 0 -or $onlyInCopy.Count -gt 0) {
        Write-Host "THE COPY IS NOT THE TREE: the copy does not hold the same documentation" -ForegroundColor Red
        Write-Host ("   in the tree but not in the copy ({0}): {1}" -f $onlyInTree.Count, (($onlyInTree | Select-Object -First 5) -join ', ')) -ForegroundColor Red
        Write-Host ("   in the copy but not in the tree ({0}): {1}" -f $onlyInCopy.Count, (($onlyInCopy | Select-Object -First 5) -join ', ')) -ForegroundColor Red
        Write-Host "   The copy holds the TRACKED files.  A file that is not tracked is not in the copy --" -ForegroundColor Red
        Write-Host "   git add it, or the harness would be measuring two different trees." -ForegroundColor Red
        $failed++
        continue
    }

    $pristine = Invoke-MutChecker $HostExe (Join-Path $pristineDir $g.Checker) $pristineDir $extra $g.Summary
    if (-not $pristine.Ok) {
        Write-Host "THE COPY IS NOT THE TREE: the checker does not see the same tree in the copy" -ForegroundColor Red
        Write-Host ("   the real tree said: " + $real.Summary) -ForegroundColor Red
        Write-Host ("   the copy said:     " + $pristine.Summary) -ForegroundColor Red
        # A checker that printed NOTHING has said nothing about the
        # tree.  Say what it did say, so the reason is on the record
        # instead of being guessed at.
        if ($pristine.Summary -eq '') { Show-MutOutput $pristine.Text 'the copy' }
        $failed++
        continue
    }
    if ($pristine.Summary -ne $real.Summary) {
        Write-Host "THE COPY IS NOT THE TREE: the copy measures something else" -ForegroundColor Red
        Write-Host ("   the real tree said: " + $real.Summary) -ForegroundColor Red
        Write-Host ("   the copy said:     " + $pristine.Summary) -ForegroundColor Red
        $failed++
        continue
    }
    Write-Host ("   the copy says the same: " + $pristine.Summary)

    # --- the mutations -------------------------------------------------
    foreach ($m in $mutations) {
        # A fresh copy per mutation: no reset, no leftovers, and a name
        # with no comma in it -- a path is not a list.
        $dest = Join-Path $Work ('mut-' + $g.Id + '-' + $m)
        $null = New-MutGitCopy $RepoRoot $dest $GitExe

        switch ($g.Id) {
            '3c' { Apply-3c $dest $m }
            '3d' { Apply-3d $dest $m }
            '3e' { Apply-3e $dest $m }
        }

        $r = Invoke-MutChecker $HostExe (Join-Path $dest $g.Checker) $dest $extra $g.Summary

        Write-Host ("=== MUTATION {0} of gate {1} ===" -f $m, $g.Id)
        Write-Host ("  reds: {0}   exit: {1}" -f $r.Reds, $r.Exit)
        Write-Host ("  verdict: {0}" -f $(if ($r.Ok) { 'GREEN' } else { 'RED' }))
        if ($r.Summary -ne '') { Write-Host ("  summary: " + $r.Summary) }
        foreach ($l in (($r.Text -split "`r?`n") | Where-Object { $_ -match 'FAIL: ' } | Select-Object -First 6)) {
            Write-Host ("  " + $l.Trim())
        }

        if ($m -eq 'D') {
            # The ruler is blind AND the defect is there.  The gate MUST
            # be green -- and it must say exactly what it says about
            # the PRISTINE tree, because a blind ruler that still prints
            # a different number is a ruler that is looking somewhere
            # else.  (A blind ruler pointed at the broken tree says
            # what it says about a perfect tree: it is not measuring.)
            $ok = $r.Ok -and ($r.Reds -eq 0) -and ($r.Exit -eq 0) -and ($r.Summary -eq $pristine.Summary)
            if (-not $ok) {
                Write-Host ("  UNEXPECTED: mutation D should be GREEN with the same summary as the pristine tree" +
                            " ({0}), but it reported {1} failures, exit {2}, and said: {3}" -f `
                            $pristine.Summary, $r.Reds, $r.Exit, $r.Summary) -ForegroundColor Red
            }
            else {
                Write-Host ("  OK: the blind ruler says exactly what it says about the PRISTINE tree" +
                            " -- the broken tree in front of it is not being measured") -ForegroundColor Green
            }
            [void]$results.Add($ok)
            if (-not $ok) { $failed++ }
            continue
        }

        # A defect MUST be caught, and it must be caught BY THE RULE the
        # mutation was aimed at -- not by some other failure that
        # happens to be red as well.
        $want = $g.Expect[$m]
        $said = ($r.Text -match [regex]::Escape($want))
        $ok = ($r.Reds -ge 1) -and (-not $r.Ok) -and ($r.Exit -ne 0) -and $said
        if (-not $ok) {
            if ($r.Reds -lt 1) {
                Write-Host ("  UNEXPECTED: mutation " + $m + " should have been caught (at least one failure), but the gate reported " +
                            $r.Reds + " failures and the verdict was " + $(if ($r.Ok) { 'GREEN' } else { 'RED' })) -ForegroundColor Red
            }
            elseif ($r.Ok) {
                Write-Host ("  UNEXPECTED: mutation " + $m + " reported failures but the gate still said OK") -ForegroundColor Red
            }
            else {
                Write-Host ("  UNEXPECTED: mutation " + $m + " went red, but NOT with the failure it was aimed at." +
                            " The gate should have said: " + $want) -ForegroundColor Red
            }
        }
        else {
            Write-Host ("  OK: caught, and caught by the rule it was aimed at") -ForegroundColor Green
        }
        [void]$results.Add($ok)
        if (-not $ok) { $failed++ }
    }
}

if (Test-Path -LiteralPath $Work) {
    if ($failed -eq 0) {
        Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
    }
    else {
        Write-Host ""
        Write-Host ("the working copy was KEPT, so the failing run can be looked at: " + $Work) -ForegroundColor Yellow
    }
}

Write-Host ""
if ($failed -gt 0 -or @($results | Where-Object { -not $_ }).Count -gt 0) {
    Write-Host "MUTATION HARNESS FAILED" -ForegroundColor Red
    exit 1
}
Write-Host ("MUTATION HARNESS OK: {0} mutation(s) across {1} gate(s) came out the way they had to" -f `
          $results.Count, @($Gates | Where-Object { $Gate -eq 'all' -or $_.Id -eq $Gate }).Count) -ForegroundColor Green
exit 0
