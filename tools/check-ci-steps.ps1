#requires -Version 5.1
<#
    The three CI steps that are NOT document gates, as ONE callable
    checker: step 4 (W5 smoke), step 5 (W7 headless pipeline), step 6
    (the source tree stayed clean).

    WHY THIS FILE EXISTS (2026-10-29):

      tools/ci.ps1 used to carry these three steps inline.  A step that
      lives inline can only be run by running the WHOLE CI -- and the
      whole CI takes ten minutes and needs every project to build.  So
      the question "does this check go RED when the thing it checks is
      broken?" could not be asked at all.

      That is the same hole the 3f/3g harnesses closed for the
      DOCUMENTATION gates, one level up: a check that has only ever
      been green is a check nobody knows works.

      So the three checks moved here, with their INPUTS as parameters.
      ci.ps1 calls this file; the mutation harness calls this file
      with the inputs broken on purpose.

    THE INPUTS ARE PARAMETERS ON PURPOSE:

      A check that can only look at one fixed thing cannot be asked
      "what if that thing were different?".  Every input a check
      depends on is named here:

        -W5Exe / -W5WorkDir     which program, and where it runs
                               (the W5 paths are RELATIVE -- that is
                               how the images are found)
        -W7Exe / -W7WorkDir     the same for W7 (recipes/demo.json is
                               relative too)
        -W7Pipeline             which pipeline file
        -W7GoodImage            the board that must judge PASS
        -W7BadImage             the board that must judge FAIL
        -W7NoImage              a path that does not exist
        -BaselineFile           what `git status --porcelain` said
                               BEFORE the tests ran

    ONE PLACE DECIDES RED:

      Every verdict goes through Assert-That().  A checker with nine
      separate `if (...) { exit 1 }` statements has nine places where
      the verdict is made, and a mutation harness has to know about
      all of them.  Here there is one.

    PURE ASCII, NO BOM: PowerShell 5.1 reads a .ps1 without a BOM as
    ANSI, and a non-ASCII byte in here turns into mojibake (or worse, a
    parse error).  The Chinese belongs in notes/.

    .PARAMETER Step
        Which steps to run: 4, 5, 6, or all.

    .EXAMPLE
        powershell -ExecutionPolicy Bypass -File tools/check-ci-steps.ps1 `
            -RepoRoot E:\qianwie\00study\OpenCV -BuildRoot E:\qianwie\00study\OpenCV\build\ci -Step all
#>
param(
    [string]$RepoRoot = '',
    [string]$BuildRoot = '',
    [string]$Step = 'all',
    [string]$W5Exe = '',
    [string]$W5WorkDir = '',
    [string]$W7Exe = '',
    [string]$W7WorkDir = '',
    [string]$W7Pipeline = '',
    [string]$W7GoodImage = '',
    [string]$W7BadImage = '',
    [string]$W7NoImage = '',
    [string]$BaselineFile = '',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Continue'
$global:LASTEXITCODE = 0

# UTF-8 WITHOUT a BOM: the baseline file is written by ci.ps1 and read
# back here, and a BOM in the middle of it would become part of the
# FIRST line -- which is a line of the snapshot, so it would compare
# unequal to the same line seen again.
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# ------------------------------------------------------------------
# Defaults: everything that ci.ps1 used to hardcode.
# ------------------------------------------------------------------
if ([string]::IsNullOrEmpty($RepoRoot)) { $RepoRoot = (Get-Location).Path }
if ([string]::IsNullOrEmpty($BuildRoot)) { $BuildRoot = Join-Path $RepoRoot 'build\ci' }
if ([string]::IsNullOrEmpty($OutDir)) { $OutDir = $BuildRoot }

$w5bin = Join-Path $BuildRoot 'W5_OCR_QRCode/Release'
if (-not (Test-Path -LiteralPath $w5bin)) { $w5bin = Join-Path $BuildRoot 'W5_OCR_QRCode' }
$w7bin = Join-Path $BuildRoot 'W7_VisionMasterLite/Release'
if (-not (Test-Path -LiteralPath $w7bin)) { $w7bin = Join-Path $BuildRoot 'W7_VisionMasterLite' }

if ([string]::IsNullOrEmpty($W5Exe)) { $W5Exe = Join-Path $w5bin 'w5_ocr_qrcode.exe' }
if ([string]::IsNullOrEmpty($W5WorkDir)) { $W5WorkDir = $w5bin }
if ([string]::IsNullOrEmpty($W7Exe)) { $W7Exe = Join-Path $w7bin 'w7_vm_lite.exe' }
if ([string]::IsNullOrEmpty($W7WorkDir)) { $W7WorkDir = $w7bin }
if ([string]::IsNullOrEmpty($W7Pipeline)) { $W7Pipeline = Join-Path $RepoRoot 'W7_VisionMasterLite/pipelines/smoke.json' }
if ([string]::IsNullOrEmpty($W7GoodImage)) { $W7GoodImage = Join-Path $RepoRoot 'W6_PCB_DefectProject/images/baseline.png' }
if ([string]::IsNullOrEmpty($W7BadImage)) { $W7BadImage = Join-Path $RepoRoot 'W6_PCB_DefectProject/images/missing.png' }
if ([string]::IsNullOrEmpty($W7NoImage)) { $W7NoImage = Join-Path $RepoRoot 'W6_PCB_DefectProject/images/no_such_image_for_ci.png' }

if (-not (Test-Path -LiteralPath $OutDir)) {
    [void][System.IO.Directory]::CreateDirectory($OutDir)
}

# ------------------------------------------------------------------
# The one place a verdict is made.
# ------------------------------------------------------------------
$script:Failures = 0

function Assert-That([bool]$ok, [string]$what) {
    if (-not $ok) {
        $script:Failures++
        Write-Host ("   FAIL: " + $what) -ForegroundColor Red
        return $false
    }
    return $true
}

# git, for step 6.
function Find-Git {
    $onPath = Get-Command git -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($f in @('C:/Program Files/Git/cmd/git.exe', 'C:/Tools/git/cmd/git.exe')) {
        if (Test-Path -LiteralPath $f) { return $f }
    }
    return ''
}

# ------------------------------------------------------------------
# Step 4: the W5 smoke check.
#
# W5 has no test suite of its own, so this is the ONLY thing CI says
# about W5.  It asserts a serial number was actually decoded, by the
# SHAPE of the value (PCB-2026-001-SN:A12345X) and not by a copy of
# it -- the exact value is asserted by the shared common/qr_ocr suite.
#
# Two blades, and BOTH of them are needed:
#   the exit code, AND the output.
# W5 returns 0 even when it finds no QR at all (measured: the exe
# returns 0 when its image is not there), so an exit code alone would
# call a broken W5 a working one.
# ------------------------------------------------------------------
function Invoke-Step4 {
    $script:Failures = 0
    if (-not (Test-Path -LiteralPath $W5Exe)) {
        [void](Assert-That $false ("W5 executable not found: " + $W5Exe))
        return
    }
    if (-not (Test-Path -LiteralPath $W5WorkDir)) {
        [void](Assert-That $false ("W5 working directory not found: " + $W5WorkDir))
        return
    }
    $w5out = Join-Path $OutDir 'w5_smoke_out.txt'
    Push-Location $W5WorkDir
    try {
        & $W5Exe 'qr' '--no-gui' *> $w5out
        $w5code = $LASTEXITCODE
    }
    finally { Pop-Location }

    $w5text = ''
    if (Test-Path -LiteralPath $w5out) { $w5text = Get-Content -LiteralPath $w5out -Raw }

    $okExit = Assert-That ($w5code -eq 0) ("W5 qr smoke check exited with " + $w5code)
    $okShape = Assert-That ($w5text -match 'PCB-\d{4}-\d{3}-SN:[A-Z0-9]+') `
        "W5 qr smoke check: no serial number in the output (the shape PCB-2026-001-SN:A12345X is expected)"

    if (-not ($okExit -and $okShape)) {
        Write-Host "   --- what W5 printed ---"
        if ($w5text) { Write-Host $w5text }
        Write-Host "   ------------------------"
    }
    if ($okExit -and $okShape) {
        Write-Host "   W5 decoded a serial number: OK"
    }
    # The summary line is printed ONLY when this step held all the
    # way through.  A summary that can also turn up in a FAILED run is
    # a summary nobody can read: the first version of this checker
    # printed one such line per blade, and a run that failed on the
    # FIRST blade still printed the other three -- which reads like a
    # pass to anything that only looks for a line ending in ": OK".
    if ($script:Failures -eq 0) {
        Write-Host "   STEP 4 OK: W5 decoded a serial number"
    }
}

# ------------------------------------------------------------------
# Step 5: the W7 headless pipeline check.
#
# It runs a WHOLE pipeline and judges it.  The exit code alone is not
# enough (the same trap the W5 check had): the pipeline can "succeed"
# while judging nothing at all, so the RESULT is read too -- the
# verdict, the error count, the judge's own output, and the number of
# components it saw.
#
# Then the exit code CONTRACT: 0 = judged PASS, 1 = judged FAIL,
# 3 = a step errored, 2 = never started.  CI leans on those codes to
# tell "the tool is broken" from "the pipeline is broken", so they are
# checked too.
# ------------------------------------------------------------------
function Invoke-Step5 {
    $script:Failures = 0
    if (-not (Test-Path -LiteralPath $W7Exe)) {
        [void](Assert-That $false ("W7 executable not found: " + $W7Exe))
        return
    }
    if (-not (Test-Path -LiteralPath $W7Pipeline)) {
        [void](Assert-That $false ("W7 smoke pipeline not found: " + $W7Pipeline))
        return
    }
    if (-not (Test-Path -LiteralPath $W7GoodImage)) {
        [void](Assert-That $false ("W7 smoke image not found: " + $W7GoodImage))
        return
    }

    $w7out = Join-Path $OutDir 'w7_headless_smoke_out.txt'
    $w7result = Join-Path $OutDir 'w7_headless_smoke_result.json'

    # WARNING: Start-Process -Wait, NOT "& $W7Exe ...".
    #   w7_vm_lite.exe is a WIN32 (GUI subsystem) program, and
    #   PowerShell does NOT wait for GUI programs: "&" returns in a few
    #   ms while the program is still running.  Measured: the result
    #   file did not exist yet when the check right after it ran.
    $p = Start-Process -FilePath $W7Exe `
            -ArgumentList @('--run', $W7Pipeline, '--image', $W7GoodImage, '--out', $w7result) `
            -WorkingDirectory $W7WorkDir -Wait -PassThru -NoNewWindow `
            -RedirectStandardOutput $w7out -RedirectStandardError "$w7out.err"
    $w7code = $p.ExitCode

    $okExit = Assert-That ($w7code -eq 0) ("W7 headless smoke exited with " + $w7code + " (0 = ran and judged PASS)")
    $okFile = Assert-That (Test-Path -LiteralPath $w7result) "W7 headless smoke wrote no result file"

    if ($okExit -and $okFile) {
        # WARNING: -Encoding UTF8, not the default.
        #   PowerShell 5.1 reads a file without -Encoding as ANSI, and
        #   the result JSON is UTF-8 with Chinese text in it -- the
        #   GBK read turns it into mojibake, and mojibake can break
        #   the JSON itself.
        $w7json = Get-Content -LiteralPath $w7result -Raw -Encoding UTF8 | ConvertFrom-Json
        $okVerdict = Assert-That ($w7json.final_result -eq 'PASS') `
            ("W7 headless smoke: final_result is '" + $w7json.final_result + "', expected PASS")
        $okErrors = Assert-That ($w7json.errors -eq 0) `
            ("W7 headless smoke: " + $w7json.errors + " step(s) reported an error")

        # The judge node's uuid in W7_VisionMasterLite/pipelines/smoke.json.
        # It MUST be a real UUID: the parser tells "new format" from
        # "old format" by QUuid::fromString(), and a key that is not a
        # UUID makes the edge look like the old format -- the lookup
        # fails, the line is dropped, and the pipeline judges ERROR
        # with no obvious reason.
        $w7judgeUuid = '22222222-2222-2222-2222-222222222222'
        $w7judgeProp = $w7json.outputs.PSObject.Properties |
            Where-Object { $_.Name -eq $w7judgeUuid }
        $okJudge = Assert-That ($null -ne $w7judgeProp) `
            ("W7 headless smoke: no output for the judge node (" + $w7judgeUuid + ")" +
             "  outputs keys: " + (($w7json.outputs.PSObject.Properties.Name) -join ', '))
        $w7comps = 0
        if ($okJudge) { $w7comps = $w7judgeProp.Value.component_count }
        $okComps = Assert-That ($w7comps -eq 6) `
            ("W7 headless smoke: the judge saw " + $w7comps + " components, expected 6")

        if ($okVerdict -and $okErrors -and $okJudge -and $okComps) {
            Write-Host "   W7 ran a whole pipeline headless and judged it PASS: OK"
        }
    }

    if (-not ($okExit -and $okFile)) {
        Write-Host "   --- what W7 printed ---"
        if (Test-Path -LiteralPath $w7out) { Get-Content -LiteralPath $w7out -Encoding UTF8 | Write-Host }
        Write-Host "   -------------------------"
    }

    # ---- the exit code contract ----
    function Run-W7([string[]]$argl, [string]$tag) {
        $o = Join-Path $OutDir ('w7_headless_code_out_' + $tag + '.txt')
        $q = Start-Process -FilePath $W7Exe -ArgumentList $argl `
                -WorkingDirectory $W7WorkDir -Wait -PassThru -NoNewWindow `
                -RedirectStandardOutput $o -RedirectStandardError "$o.err"
        return $q.ExitCode
    }

    # A board WITH a defect -> the judge says FAIL -> exit 1 (not 0).
    $codeFail = Run-W7 @('--run', $W7Pipeline, '--image', $W7BadImage,
                          '--out', (Join-Path $OutDir 'w7_fail_result.json')) 'fail'
    $okFail = Assert-That ($codeFail -eq 1) `
        ("W7 headless exit code contract: a FAIL pipeline exited with " + $codeFail + ", expected 1")
    if ($okFail) { Write-Host "   exit 1 (judged FAIL)             : OK" }

    # An image that does not exist -> the operator reports an error -> exit 3.
    $codeErr = Run-W7 @('--run', $W7Pipeline, '--image', $W7NoImage,
                         '--out', (Join-Path $OutDir 'w7_err_result.json')) 'err'
    $okErr = Assert-That ($codeErr -eq 3) `
        ("W7 headless exit code contract: a step error exited with " + $codeErr + ", expected 3")
    if ($okErr) { Write-Host "   exit 3 (a step reported an error) : OK" }

    # A pipeline file that does not exist -> we never got started -> exit 2.
    $codeMissing = Run-W7 @('--run', (Join-Path $OutDir 'w7_no_such_pipeline.json')) 'missing'
    $okMissing = Assert-That ($codeMissing -eq 2) `
        ("W7 headless exit code contract: a missing pipeline file exited with " + $codeMissing + ", expected 2")
    if ($okMissing) { Write-Host "   exit 2 (could not even start)     : OK" }

    if ($script:Failures -eq 0) {
        Write-Host "   STEP 5 OK: the pipeline judged PASS, and the exit code contract held"
    }
}

# ------------------------------------------------------------------
# Step 6: the source tree stayed clean.
#
# The invariant: test products live in build/, never in the source
# tree.  Before that change, 14 suites wrote into
# W7_VisionMasterLite/images/ and the .gitignore rules hid the
# evidence.
#
# The check is "the tests ADDED nothing", not "the working tree is
# pristine": a developer with uncommitted work must not get a red CI
# that has nothing to do with the tests.  So the comparison is
# against a BASELINE taken before the run, not against an empty tree.
# ------------------------------------------------------------------
function Invoke-Step6 {
    $script:Failures = 0
    if ([string]::IsNullOrEmpty($BaselineFile)) {
        [void](Assert-That $false "no baseline file: the tree snapshot taken before the run is missing")
        return
    }
    if (-not (Test-Path -LiteralPath $BaselineFile)) {
        [void](Assert-That $false ("baseline file not found: " + $BaselineFile))
        return
    }
    $git = Find-Git
    if ([string]::IsNullOrEmpty($git)) {
        [void](Assert-That $false "git was not found: the tree snapshot cannot be taken")
        return
    }
    $baseline = @([System.IO.File]::ReadAllLines($BaselineFile, $Utf8NoBom) | Where-Object { $_ -ne '' })
    $after = @(& $git -C $RepoRoot status --porcelain | Where-Object { $_ -ne '' })

    # Compare-Object needs a non-empty reference set, hence the placeholder.
    $ref = $baseline
    if ($ref.Count -eq 0) { $ref = @('(clean tree)') }
    $added = @(Compare-Object -ReferenceObject $ref -DifferenceObject $after |
               Where-Object { $_.SideIndicator -eq '=>' })
    if ($added.Count -gt 0) {
        foreach ($a in $added) {
            [void](Assert-That $false ("test products leaked into the source tree: " + $a.InputObject +
                "  (every suite must run in build/test_work/<suite>/ -- see the" +
                " w7_add_test / w6_add_test functions in the CMakeLists files)"))
        }
    }
    if ($script:Failures -eq 0) {
        Write-Host "   source tree unchanged by the tests: OK"
        Write-Host "   STEP 6 OK: the tests added nothing to the source tree"
    }
}

# ------------------------------------------------------------------
# Run
# ------------------------------------------------------------------
switch ($Step) {
    '4' { Invoke-Step4 }
    '5' { Invoke-Step5 }
    '6' { Invoke-Step6 }
    'all' { Invoke-Step4; Invoke-Step5; Invoke-Step6 }
    default { Write-Host ("unknown step: " + $Step) -ForegroundColor Red; exit 1 }
}

if ($script:Failures -gt 0) {
    Write-Host ""
    Write-Host ("FAILED: " + $script:Failures + " check(s) did not hold") -ForegroundColor Red
    exit 1
}
exit 0
