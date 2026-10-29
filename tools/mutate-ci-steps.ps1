#requires -Version 5.1
<#
    Mutation harness for the three CI STEPS that are not document
    gates:

      4    tools/check-ci-steps.ps1 -Step 4   the W5 smoke check
      5    tools/check-ci-steps.ps1 -Step 5   the W7 headless pipeline
      6    tools/check-ci-steps.ps1 -Step 6   the source tree stayed clean

    Why this file is in the repository:

      tools/mutate-gates.ps1 (step 3g) proved that the DOCUMENTATION
      gates can go red.  These three had never been asked: they live
      inside tools/ci.ps1, and a step inside ci.ps1 can only be run by
      running the WHOLE CI -- ten minutes, seven projects, and the
      answer to "would this notice?" is buried in a log nobody reads.

      So the three steps were pulled out into tools/check-ci-steps.ps1
      with their INPUTS as parameters, and this harness breaks those
      inputs on purpose.

    What each mutation does:

      A, B, C   an input is broken on purpose.  The check MUST go RED,
                and it must be caught BY THE RULE aimed at it -- and by
                no other rule (the "not expected" list says which
                rules must stay quiet).

      D         THE RULER IS BROKEN, and the defect is STILL THERE.
                The check MUST go GREEN, and it must print EXACTLY
                what it prints about the PRISTINE tree -- byte for
                byte.  The broken input in front of it is not being
                measured at all.

    D is the one that carries the proof.  "A, B and C came out red"
    only shows that the check and the mutation behaved consistently at
    some moment; it does not show that the CHECKER is what made it
    red.  D shows that: take the same broken input, break the thing
    that decides red, and the redness goes away.

    WHY THE MUTATIONS LOOK DIFFERENT FROM THE DOCUMENTATION ONES:

      The documentation gates read the tree, so their mutations change
      the tree.  These three read INPUTS -- an executable, a working
      directory, a pipeline file, an image -- so their mutations change
      the INPUTS.  Same question, same answer shape, different lever.

      ONE of the two gate-4 mutations cannot be produced by the real
      program at all, and the harness says so instead of pretending:

        gate 4/B  W5 has TWO exits: 0, and 1 on an OpenCV exception --
                  and the 1 prints an exception, never a serial.
                  Measured: a missing image and a corrupt image both
                  exit 0.  So "a perfect serial AND a non-zero exit"
                  is unreachable, and a stub stands in for it.

      The other one (4/C) USED to have a stub too, and that was WRONG:

        gate 4/C  W5 finds its image by a HARD-CODED RELATIVE path
                  ("images/qrcode_test.png"), so swapping the image
                  makes the REAL program decode a serial of the wrong
                  shape.  Measured: exit 0, and the serial comes out
                  as "PCB-2026-01-SN:A12345X" -- two digits where the
                  format has three.  So C runs the REAL program with
                  the wrong image (tools/fixtures/qr_wrongshape.png)
                  and there is no stub at all.

      The first version of this harness asserted, without measuring,
      that "the real program cannot do this" for BOTH of them.  That
      was a conclusion written down as a reason, and it was FALSE for
      C -- see notes/atomic/at-cannot-be-tested-is-a-conclusion-not-a-reason.md.

    Pure ASCII, no BOM: PowerShell 5.1 reads a .ps1 without a BOM as
    ANSI, and a non-ASCII byte in here turns into mojibake (or worse,
    a parse error).  The Chinese belongs in notes/.

    Usage:
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\mutate-ci-steps.ps1
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\mutate-ci-steps.ps1 -Gate 5
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\mutate-ci-steps.ps1 -Mutation A,B,C,D
#>

[CmdletBinding()]
param(
    [string]$Mutation = 'A,B,C,D',
    [string]$RepoRoot,
    [string]$Work,
    [string]$Gate = 'all',
    [string]$BuildRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $RepoRoot) { $RepoRoot = (Split-Path -Parent $PSScriptRoot) }
# The PROVIDER's path, not the string we were handed (MutHarness rule 4).
$RepoRoot = (Get-Item -LiteralPath $RepoRoot -ErrorAction Stop).FullName.TrimEnd([char]'\', [char]'/')
if (-not $BuildRoot) { $BuildRoot = Join-Path $RepoRoot 'build\ci' }
if (-not $Work) {
    $Work = Join-Path $env:TEMP ('opencv-mutate-ci-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
}
[void][System.IO.Directory]::CreateDirectory($Work)

. (Join-Path $PSScriptRoot 'lib\MutHarness.ps1')

# ------------------------------------------------------------------
# The checker under test, and the paths its inputs come from.
# ------------------------------------------------------------------
$Checker = Join-Path $RepoRoot 'tools\check-ci-steps.ps1'
if (-not (Test-Path -LiteralPath $Checker)) {
    throw ("the checker is not there: " + $Checker +
           "  The CI steps 4/5/6 have to be pulled out of tools/ci.ps1 into a" +
           " script of their own before they can be measured at all.")
}

$W5Bin = Join-Path $BuildRoot 'W5_OCR_QRCode/Release'
if (-not (Test-Path -LiteralPath $W5Bin)) { $W5Bin = Join-Path $BuildRoot 'W5_OCR_QRCode' }
$W7Bin = Join-Path $BuildRoot 'W7_VisionMasterLite/Release'
if (-not (Test-Path -LiteralPath $W7Bin)) { $W7Bin = Join-Path $BuildRoot 'W7_VisionMasterLite' }
$W5Exe = Join-Path $W5Bin 'w5_ocr_qrcode.exe'
$W7Exe = Join-Path $W7Bin 'w7_vm_lite.exe'
$GoodImage = Join-Path $RepoRoot 'W6_PCB_DefectProject\images\baseline.png'
$BadImage  = Join-Path $RepoRoot 'W6_PCB_DefectProject\images\missing.png'

# The fixture for gate 4/C: a QR code whose serial has the WRONG
# SHAPE.  It is a TRACKED file, and the generator that made it sits
# next to it (tools/fixtures/gen_qr_wrongshape.py) -- a binary with
# no second source is a binary nobody can review.
$WrongShapeImage = Join-Path $RepoRoot 'tools\fixtures\qr_wrongshape.png'
if (-not (Test-Path -LiteralPath $WrongShapeImage)) {
    throw ("the wrong-shape fixture is not there: " + $WrongShapeImage +
           "  It is a tracked file, and the generator that made it" +
           " sits next to it (tools/fixtures/gen_qr_wrongshape.py)." +
           " Without it the mutation IS the pristine tree, and a" +
           " mutation that did not happen must be LOUD.")
}

# The judge node's uuid in W7_VisionMasterLite/pipelines/smoke.json.
$JudgeUuid = '22222222-2222-2222-2222-222222222222'

# The summary is the ONE line the checker prints when the step held
# from end to end.  It has to be that, and not "a line ending in : OK":
# the first version of this harness matched the ": OK" lines, and a
# run that failed on its first blade still printed the other three --
# so the harness read a run with two FAIL lines as GREEN.
$SummaryPattern = '(?m)^   STEP \d OK: .*$'

# ------------------------------------------------------------------
# Files this harness put into the SOURCE TREE, so that it can put them
# back out again.  A harness that leaves a file behind in the tree is
# a harness that makes the NEXT CI step (the tree-clean check) go red
# for a reason that has nothing to do with the tests.
# ------------------------------------------------------------------
$script:Created = New-Object System.Collections.ArrayList

function Remove-Created {
    foreach ($f in @($script:Created)) {
        if (Test-Path -LiteralPath $f) {
            Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue
        }
    }
    $script:Created.Clear()
}

# The tree as git sees it, written the way the checker reads it back:
# UTF-8 without a BOM, one line per entry.
function Write-TreeSnapshot([string]$path) {
    $lines = @(& $GitExe -C $RepoRoot status --porcelain | Where-Object { $_ -ne '' })
    [System.IO.File]::WriteAllLines($path, [string[]]$lines, $MutUtf8)
    return ,@($lines)
}

# ------------------------------------------------------------------
# What each mutation breaks, and what the check MUST say about it.
#
#   Expect     every one of these strings must appear in the output
#   NotExpect  none of these strings may appear -- the rule aimed at
#              the mutation is the ONLY one allowed to fire
#   MustGreen  the check must stay GREEN (and say what it says about
#              the pristine tree) -- a mutation that must NOT be caught
# ------------------------------------------------------------------
$Gates = @(
    [pscustomobject]@{
        Id    = '4'
        Name  = 'the W5 smoke check'
        Step  = '4'
        Expect = @{
            'A' = @('no serial number in the output')
            'B' = @('W5 qr smoke check exited with 3')
            'C' = @('no serial number in the output')
        }
        NotExpect = @{
            'A' = @('exited with')
            'B' = @('no serial number in the output')
            'C' = @('exited with')
        }
        MustGreen = @{}
    }
    [pscustomobject]@{
        Id    = '5'
        Name  = 'the W7 headless pipeline check'
        Step  = '5'
        Expect = @{
            'A' = @('W7 headless smoke exited with 1')
            'B' = @('a FAIL pipeline exited with 0, expected 1')
            'C' = @('no output for the judge node')
        }
        NotExpect = @{
            # A: the exit code is the only blade that gets to speak -- a
            #    run that exits 1 is not read any further.  (That is the
            #    original behaviour: Fail exited the step at once.)
            'A' = @('no output for the judge node', 'the judge saw')
            # B: the MAIN check must stay quiet -- the good board is
            #    still good.  Only the exit code contract may notice.
            'B' = @('W7 headless smoke exited with', 'no output for the judge node', 'the judge saw')
            'C' = @('final_result is', 'exited with')
        }
        MustGreen = @{}
    }
    [pscustomobject]@{
        Id    = '6'
        Name  = 'the source tree stayed clean'
        Step  = '6'
        Expect = @{
            'A' = @('test products leaked into the source tree', '__mut_probe_leak.txt')
            'C' = @('__mut_probe_leak.txt')
        }
        NotExpect = @{
            'C' = @('__mut_probe_dirty.txt')
        }
        MustGreen = @{ 'B' = $true }
    }
)

# ------------------------------------------------------------------
# The breaks.
#
# Each returns the PARAMETERS that break the input, as a hashtable --
# not as a list of command line arguments.  A list would be APPENDED
# to the defaults, and a parameter given twice is a binding error:
# PowerShell refuses to start the script, the checker never runs, and
# the harness sees "exit 1, zero failures" -- which is what the first
# run of gate 6 looked like.
#
# Whatever a break had to create, it leaves for Remove-Created.
# ------------------------------------------------------------------
function Break-Gate4([string]$which, [string]$ctx) {
    switch ($which) {
        'A' {
            # The working directory IS the input: W5 finds its image by
            # a RELATIVE path, so running it where there is no image
            # makes it decode nothing.
            #
            # And it still exits 0 -- measured -- which is exactly why
            # the check cannot look at the exit code alone.
            $empty = Join-Path $ctx 'no_images'
            [void][System.IO.Directory]::CreateDirectory($empty)
            return @{ 'W5WorkDir' = $empty }
        }
        'B' {
            # A stub that exits 3 and prints a PERFECT serial.
            # The exit code and the output disagree, and the exit code
            # is the one that must be believed.
            $stub = Join-Path $ctx 'w5_exit3.cmd'
            $lines = @('@echo off', 'echo   decoded: PCB-2026-001-SN:A12345X', 'exit /b 3')
            [System.IO.File]::WriteAllText($stub, ($lines -join "`r`n"), $MutUtf8)
            return @{ 'W5Exe' = $stub; 'W5WorkDir' = $ctx }
        }
        'C' {
            # THE REAL PROGRAM, WITH THE WRONG IMAGE.
            #
            # W5 finds its image by a HARD-CODED RELATIVE path
            # ("images/qrcode_test.png"), so the working directory IS
            # the input that decides what it decodes.  Put a serial of
            # the WRONG SHAPE there (two digits where the format has
            # three) and the real program prints it and still exits 0.
            #
            # This is the case that separates "the check looks at the
            # output" from "the check looks at the OUTPUT'S SHAPE": a
            # check that only asked "is there any output" would call
            # this a pass.
            #
            # MEASURED (2026-10-29, the real w5_ocr_qrcode.exe):
            #   decoded: PCB-2026-01-SN:A12345X
            #   exit 0
            # and the shape regex does not match it.
            #
            # The first version of this mutation used a STUB that
            # printed the same string, and the note said the real
            # program could not do this.  That was never measured: it
            # was assumed, and it was false.  The stub is gone.
            $w = Join-Path $ctx 'wrong_shape'
            $imgDir = Join-Path $w 'images'
            [void][System.IO.Directory]::CreateDirectory($imgDir)
            $dst = Join-Path $imgDir 'qrcode_test.png'
            [System.IO.File]::Copy($WrongShapeImage, $dst, $true)
            # THE COPY MUST BE VERIFIED BEFORE IT IS BELIEVED
            # (MutHarness rule 1).  A copy that silently did nothing
            # would leave the RIGHT image in place, the real program
            # would decode the RIGHT serial, and the mutation would
            # come out GREEN -- a green light for a broken mutation.
            $want = [System.IO.File]::ReadAllBytes($WrongShapeImage)
            $got = [System.IO.File]::ReadAllBytes($dst)
            if ($got.Length -ne $want.Length) {
                throw ("gate 4/C: the copy is " + $got.Length + " bytes, the fixture is " + $want.Length)
            }
            for ($i = 0; $i -lt $want.Length; $i++) {
                if ($got[$i] -ne $want[$i]) {
                    throw ("gate 4/C: the copy differs from the fixture at byte " + $i)
                }
            }
            return @{ 'W5WorkDir' = $w }
        }
        default { throw ("unknown mutation for gate 4: " + $which) }
    }
}

function Break-Gate5([string]$which, [string]$ctx) {
    switch ($which) {
        'A' {
            # The board WITH a defect, handed in where the good board
            # goes.  The pipeline runs, and judges FAIL.
            return @{ 'W7GoodImage' = $BadImage }
        }
        'B' {
            # The board that is supposed to have a DEFECT is handed in
            # where the defect board goes -- and it is a GOOD board.
            #
            # The pipeline then judges PASS and exits 0, where the
            # exit code contract says a FAIL pipeline has to exit 1.
            # This is the silent failure the contract exists for: "it
            # looks like it passed".
            #
            # (It cannot be done by pointing the good slot at a
            # missing image: the check has a guard that refuses to run
            # at all when the image is not there, so the program never
            # runs and the exit code contract is never exercised.)
            return @{ 'W7BadImage' = $GoodImage }
        }
        'C' {
            # A pipeline whose judge node has a DIFFERENT uuid.
            #
            # The pipeline still runs and still judges PASS -- the
            # defect is not a crash, it is a verdict about something
            # else.  The check looks the judge up BY UUID, so the
            # output it is looking for is not there.
            $src = Join-Path $RepoRoot 'W7_VisionMasterLite\pipelines\smoke.json'
            $txt = Read-MutText $src
            $new = '33333333-3333-3333-3333-333333333333'
            $n = ([regex]::Matches($txt, [regex]::Escape($JudgeUuid))).Count
            if ($n -ne 2) {
                throw ("gate 5/C: expected the judge uuid twice in " + $src + ", found " + $n +
                       " -- the pipeline changed under the harness")
            }
            $dst = Join-Path $ctx 'smoke_judge_renamed.json'
            Write-MutText $dst ($txt.Replace($JudgeUuid, $new))
            return @{ 'W7Pipeline' = $dst }
        }
        default { throw ("unknown mutation for gate 5: " + $which) }
    }
}

function Break-Gate6([string]$which, [string]$ctx) {
    switch ($which) {
        'A' {
            # A file that appeared in the source tree.
            $f = Join-Path $RepoRoot '__mut_probe_leak.txt'
            [System.IO.File]::WriteAllText($f, "probe`n", $MutUtf8)
            [void]$script:Created.Add($f)
            return @{}
        }
        'B' {
            # The tree was ALREADY dirty before the run.
            #
            # The check asks "did the tests ADD anything", not "is the
            # tree pristine" -- a developer with uncommitted work must
            # not get a red CI that has nothing to do with the tests.
            # This is the case that proves the check knows the
            # difference, and it has never been verified before.
            $f = Join-Path $RepoRoot '__mut_probe_dirty.txt'
            [System.IO.File]::WriteAllText($f, "probe`n", $MutUtf8)
            [void]$script:Created.Add($f)
            # The baseline is taken AFTER the file is there, so the file
            # is part of "before" and not part of "after".
            $null = Write-TreeSnapshot (Join-Path $ctx 'baseline_dirty.txt')
            return @{ 'BaselineFile' = (Join-Path $ctx 'baseline_dirty.txt') }
        }
        'C' {
            # A dirty tree AND a new file.  Only the NEW one may be
            # named -- naming the dirty one would be the check saying
            # "your uncommitted work is a test product".
            $dirty = Join-Path $RepoRoot '__mut_probe_dirty.txt'
            $leak  = Join-Path $RepoRoot '__mut_probe_leak.txt'
            [System.IO.File]::WriteAllText($dirty, "probe`n", $MutUtf8)
            [void]$script:Created.Add($dirty)
            $null = Write-TreeSnapshot (Join-Path $ctx 'baseline_dirty.txt')
            [System.IO.File]::WriteAllText($leak, "probe`n", $MutUtf8)
            [void]$script:Created.Add($leak)
            return @{ 'BaselineFile' = (Join-Path $ctx 'baseline_dirty.txt') }
        }
        default { throw ("unknown mutation for gate 6: " + $which) }
    }
}

# ------------------------------------------------------------------
# The blinds: the RULER is broken, the defect stays.
#
# Each blind replaces the COMPARISON -- the thing that decides red --
# and nothing else.  The comparison it breaks is the one the defect
# was aimed at, so the "red" that A, B or C produced is exactly the
# red that has to disappear.
# ------------------------------------------------------------------
function Blind-Gate4([string]$path) {
    # The shape blade: "does the output hold a serial of the right
    # shape".
    Replace-MutOnce $path "(`$w5text -match 'PCB-\d{4}-\d{3}-SN:[A-Z0-9]+')" `
                          '($true)' '4/D (the shape blade)'
}

function Blind-Gate5([string]$path) {
    # Two blades fire for the broken board: the exit code (1, not 0)
    # and the verdict (FAIL, not PASS).  Both are blinded -- a ruler
    # with one blade still standing is a ruler that can still catch
    # something.
    Replace-MutOnce $path '($w7code -eq 0)' '($true)' '5/D (the exit code blade)'
    Replace-MutOnce $path "(`$w7json.final_result -eq 'PASS')" '($true)' '5/D (the verdict blade)'
}

function Blind-Gate6([string]$path) {
    # The comparison between the tree before the run and the tree
    # after it.  Blinded, the tree in front of it is not being measured
    # at all -- and it says exactly what it says about a perfect tree.
    Replace-MutOnce $path 'if ($added.Count -gt 0) {' 'if ($false) {' '6/D (the comparison)'
}

# The command line for one run.
#
# The defaults, with the break's overrides REPLACED -- never appended.
# A parameter given twice is a binding error, and a binding error means
# the checker never starts: the harness would see "exit 1, zero
# failures" and could not tell that from a check that ran and found
# nothing wrong.
function Get-RunArgs([string]$step, [hashtable]$over, [string]$outDir) {
    $p = @{
        'BuildRoot'    = $BuildRoot
        'Step'         = $step
        'BaselineFile' = $BaseBaseline
        'OutDir'       = $outDir
    }
    foreach ($k in $over.Keys) { $p[$k] = $over[$k] }
    $a = New-Object System.Collections.ArrayList
    foreach ($k in @('BuildRoot', 'Step', 'BaselineFile', 'OutDir',
                     'W5Exe', 'W5WorkDir', 'W7Exe', 'W7WorkDir', 'W7Pipeline',
                     'W7GoodImage', 'W7BadImage', 'W7NoImage')) {
        if ($p.ContainsKey($k)) {
            # TWO tokens, appended one at a time on purpose.  See the
            # trap below before you "simplify" this into one line.
            [void]$a.Add('-' + $k)
            [void]$a.Add([string]$p[$k])
        }
    }

    # LOUD, OR IT DOES NOT COUNT AS A CHECK.
    #
    # The command line must be FLAT: one token for the name, one for
    # the value, and a name that is a name and nothing else.  The
    # first version of this function wrote:
    #
    #     $a += @('-' + $k, [string]$p[$k])
    #
    # and that is a trap, because in PowerShell the COMMA binds
    # tighter than the PLUS.  It does not parse as "two elements"; it
    # parses as
    #
    #     '-' + ($k, [string]$p[$k])
    #
    # -- a string plus an ARRAY, which PowerShell joins with a space.
    # So each parameter became ONE token with the name and the value
    # welded together: four tokens where eight were meant.  Each of
    # those reached the checker as a single unknown argument, the
    # parameter binding failed, the checker never started, and the
    # harness was handed "exit 1, zero failures" -- which it could
    # not tell apart from a check that ran and found nothing wrong.
    #
    # So the shape of the command line is ASSERTED, not assumed.
    if (($a.Count % 2) -ne 0) {
        throw ("the command line has an odd number of tokens (" + $a.Count + "): " + ($a -join ' | '))
    }
    for ($i = 0; $i -lt $a.Count; $i += 2) {
        if ($a[$i] -notmatch '^-[A-Za-z][A-Za-z0-9]*$') {
            throw ("token " + $i + " is not a parameter name: '" + $a[$i] + "'" +
                   " -- the name and the value must be two separate tokens")
        }
        if ([string]$a[$i + 1] -eq '') {
            throw ("the value of " + $a[$i] + " is empty")
        }
    }
    return ,$a.ToArray()
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

$GitCmd = Get-Command git -ErrorAction SilentlyContinue
if (-not $GitCmd) { throw "git not found -- the tree check measures the tree with git status" }
$GitExe = $GitCmd.Source

$mutations = @($Mutation -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
if ($mutations.Count -eq 0) { throw "no mutation was asked for" }

# The trees are not COPIED here: the executables were built against the
# real tree, and the images and the pipeline live in it.  What is copied
# is the CHECKER, for the D runs -- a copy of the checker with the
# comparison blinded.
#
# The baseline of the tree check is taken ONCE, before any mutation, so
# that every mutation is measured against the same "before".
$BaseBaseline = Join-Path $Work 'baseline_pristine.txt'
$null = Write-TreeSnapshot $BaseBaseline

$results = New-Object System.Collections.ArrayList
$failed = 0

try {
    foreach ($g in $Gates) {
        if ($Gate -ne 'all' -and $g.Id -ne $Gate) { continue }

        Write-Host ""
        Write-Host ("=== GATE {0}: {1} ===" -f $g.Id, $g.Name)

        # --- the real thing, unbroken ---------------------------------
        $pristineOut = Join-Path $Work ('pristine-' + $g.Id)
        [void][System.IO.Directory]::CreateDirectory($pristineOut)
        $pristineArgs = @('-BuildRoot', $BuildRoot, '-Step', $g.Step,
                          '-BaselineFile', $BaseBaseline, '-OutDir', $pristineOut)
        $pristine = Invoke-MutChecker $HostExe $Checker $RepoRoot $pristineArgs $SummaryPattern
        if (-not $pristine.Ok) {
            Write-Host "THE REAL TREE IS NOT GREEN -- a harness cannot measure a check on something that is already broken" -ForegroundColor Red
            Write-Host ("   the check said: " + $pristine.Summary) -ForegroundColor Red
            if ($pristine.Summary -eq '') {
                Write-Host ("   it said: " + ($pristine.Text -replace "`r?`n", ' | ')) -ForegroundColor Red
            }
            $failed++
            continue
        }
        Write-Host ("   the real thing: " + ($pristine.Summary -replace "`r?`n", ' / '))

        foreach ($m in $mutations) {
            Write-Host ("=== MUTATION {0} of gate {1} ===" -f $m, $g.Id)
            $ctx = Join-Path $Work ('mut-' + $g.Id + '-' + $m)
            [void][System.IO.Directory]::CreateDirectory($ctx)
            $outDir = Join-Path $ctx 'out'
            [void][System.IO.Directory]::CreateDirectory($outDir)

            $checkerToRun = $Checker
            $over = @{}

            if ($m -eq 'D') {
                # THE RULER IS BROKEN, and the defect is STILL THERE.
                # The defect is A's break: the one this gate's D is
                # about.  A blind ruler on a perfect tree is green for a
                # trivial reason, and that proves nothing.
                $blindDir = Join-Path $ctx 'blind'
                [void][System.IO.Directory]::CreateDirectory($blindDir)
                $checkerToRun = Join-Path $blindDir 'check-ci-steps.ps1'
                [System.IO.File]::Copy($Checker, $checkerToRun, $true)
                switch ($g.Id) {
                    '4' { Blind-Gate4 $checkerToRun }
                    '5' { Blind-Gate5 $checkerToRun }
                    '6' { Blind-Gate6 $checkerToRun }
                }
                switch ($g.Id) {
                    '4' { $over = Break-Gate4 'A' $ctx }
                    '5' { $over = Break-Gate5 'A' $ctx }
                    '6' { $over = Break-Gate6 'A' $ctx }
                }
            }
            else {
                switch ($g.Id) {
                    '4' { $over = Break-Gate4 $m $ctx }
                    '5' { $over = Break-Gate5 $m $ctx }
                    '6' { $over = Break-Gate6 $m $ctx }
                }
            }

            # NOT named $args: that is PowerShell's automatic variable
            # for the unbound arguments of the running script, and a
            # name like that is one refactor away from biting.
            $runArgs = Get-RunArgs $g.Step $over $outDir
            $r = Invoke-MutChecker $HostExe $checkerToRun $RepoRoot $runArgs $SummaryPattern

            Write-Host ("  reds: {0}   exit: {1}" -f $r.Reds, $r.Exit)
            Write-Host ("  verdict: {0}" -f $(if ($r.Ok) { 'GREEN' } else { 'RED' }))
            if ($r.Summary -ne '') { Write-Host ("  summary: " + ($r.Summary -replace "`r?`n", ' / ')) }
            foreach ($l in (($r.Text -split "`r?`n") | Where-Object { $_ -match 'FAIL: ' } | Select-Object -First 6)) {
                Write-Host ("  " + $l.Trim())
            }

            $ok = $true
            $why = ''

            # --- the two GREEN cases ----------------------------------
            if ($m -eq 'D' -or $g.MustGreen.ContainsKey($m)) {
                if ($m -eq 'D') {
                    $ok = ($r.Reds -eq 0) -and ($r.Exit -eq 0) -and ($r.Text -eq $pristine.Text)
                    if (-not $ok) {
                        $why = ("the blind ruler must go GREEN and print exactly what it prints about the pristine tree" +
                                " (exit {0}, {1} failure(s), same output: {2})" -f `
                                $r.Exit, $r.Reds, ($r.Text -eq $pristine.Text))
                    }
                }
                else {
                    # A mutation that must NOT be caught: the tree was
                    # already dirty, and the check must not say so.
                    $ok = ($r.Reds -eq 0) -and ($r.Exit -eq 0) -and ($r.Text -eq $pristine.Text)
                    if (-not $ok) {
                        $why = ("this case must stay GREEN and say exactly what it says about the pristine tree" +
                                " (exit {0}, {1} failure(s), same output: {2})" -f `
                                $r.Exit, $r.Reds, ($r.Text -eq $pristine.Text))
                    }
                }
            }
            # --- the RED cases -----------------------------------------
            else {
                $want = @($g.Expect[$m])
                $notWant = @()
                if ($g.NotExpect.ContainsKey($m)) { $notWant = @($g.NotExpect[$m]) }
                $said = $true
                foreach ($w in $want) {
                    if ($r.Text -notmatch [regex]::Escape($w)) { $said = $false; $why = ("the check did not say: " + $w) }
                }
                $alsoSaid = $true
                foreach ($w in $notWant) {
                    if ($r.Text -match [regex]::Escape($w)) {
                        $alsoSaid = $false
                        $why = ("the check said '" + $w + "', but that is NOT the rule this mutation was aimed at")
                    }
                }
                $ok = ($r.Reds -ge 1) -and (-not $r.Ok) -and ($r.Exit -ne 0) -and $said -and $alsoSaid
                if (-not $ok -and $why -eq '') {
                    if ($r.Reds -lt 1) { $why = ("the check reported " + $r.Reds + " failures -- it did not notice") }
                    elseif ($r.Ok) { $why = "the check reported failures but still said OK" }
                    elseif ($r.Exit -eq 0) { $why = "the check reported failures but exited 0" }
                }
            }

            if ($ok) {
                if ($m -eq 'D') {
                    Write-Host ("  OK: the blind ruler says EXACTLY what it says about the pristine tree" +
                                " -- the broken input in front of it is not being measured") -ForegroundColor Green
                }
                elseif ($g.MustGreen.ContainsKey($m)) {
                    Write-Host ("  OK: caught nothing, and said nothing -- an uncommitted file is not a test product") -ForegroundColor Green
                }
                else {
                    Write-Host ("  OK: caught, and caught by the rule it was aimed at") -ForegroundColor Green
                }
            }
            else {
                Write-Host ("  UNEXPECTED: " + $why) -ForegroundColor Red
            }
            [void]$results.Add($ok)
            if (-not $ok) { $failed++ }

            # Every file this mutation put into the source tree goes
            # back out, before the next mutation measures anything.
            Remove-Created
        }
    }
}
finally {
    Remove-Created
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
    Write-Host "CI STEP MUTATION HARNESS FAILED" -ForegroundColor Red
    exit 1
}
Write-Host ("CI STEP MUTATION HARNESS OK: {0} mutation(s) across {1} step(s) came out the way they had to" -f `
          $results.Count, @($Gates | Where-Object { $Gate -eq 'all' -or $_.Id -eq $Gate }).Count) -ForegroundColor Green
exit 0
