<#
.SYNOPSIS
    Build every project, run every test suite, and prove the source tree stays clean.

.DESCRIPTION
    This is the exact same set of steps as .github/workflows/ci.yml.
    A CI that only exists inside a YAML file is a CI nobody can run before
    pushing -- and therefore a CI whose first real execution is also its
    first debugging session.

    What it checks, in order:

      1. Preflight. Every tool and every third-party path must exist.
         A missing dependency FAILS HERE, loudly. It never "skips".
      2. All 7 projects configure and build (Release).
      3. W6 (3 suites) and W7 (15 suites) run and pass.
      4. W5's executable still runs. W5 has no test suite of its own,
         so this smoke check is the only thing CI says about W5.
         It asserts a serial number was actually decoded, using the
         *shape* of the value, not a copy of the value -- the exact
         value is asserted by the shared common/qr_ocr suite.
      5. The source tree did not change.
         Test products must land in build/, never in the source tree.
         This is the invariant introduced on 2026-10-20, and it is the
         one thing nobody remembers to check by hand.
         The check is "the tests added nothing", not "the working tree
         is pristine" -- a developer with uncommitted work must not get
         a red CI that has nothing to do with the tests.

    Exit code 0 = everything above held. Non-zero = the first thing
    that did not hold, with the command that failed printed verbatim.

.PARAMETER CmakeExe
    Path to cmake.exe. Auto-detected when omitted.

.PARAMETER CtestExe
    Path to ctest.exe. Auto-detected when omitted.

.PARAMETER OpenCvLib
    Directory that contains OpenCVConfig.cmake -- the "lib" subdirectory
    of the OpenCV install, NOT the install root.
    Default: D:/opencv-src/build/install/lib

.PARAMETER OpenCvBin
    Directory that contains the OpenCV DLLs. Derived from OpenCvLib when
    omitted (the install layout is <prefix>/lib + <prefix>/bin).

.PARAMETER QtRoot
    Qt6 install root. Only W7 needs it.
    Default: D:/Qt/6.8.3/msvc2022_64

.PARAMETER BuildRoot
    Where to put the CI build trees. Default: <repo>/build/ci
    Kept inside the repo on purpose -- "build/" is git-ignored, so the
    step 5 check ("source tree is clean") still means what it says.

.PARAMETER SkipBuild
    Do not compile. Configure still runs (it is cheap and it catches a
    broken CMakeLists immediately); only the "cmake --build" step is
    skipped. Use it when iterating on the test scripts themselves.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/ci.ps1

.EXAMPLE
    # On a machine where the dependencies live somewhere else:
    powershell -ExecutionPolicy Bypass -File tools/ci.ps1 `
        -OpenCvLib 'C:/opencv/build/lib' -QtRoot 'C:/Qt/6.8.3/msvc2022_64'
#>
param(
    [string]$CmakeExe  = "",
    [string]$CtestExe  = "",
    [string]$OpenCvLib = "",
    [string]$OpenCvBin = "",
    [string]$QtRoot    = "",
    [string]$BuildRoot = "",
    [switch]$SkipBuild
)

# NOTE: this file is deliberately pure ASCII.
# Windows PowerShell 5.1 parses a .ps1 without a BOM as ANSI, so any
# non-ASCII byte in here turns into mojibake (or worse, a parse error).
# All commentary lives in the notes/ folder, which is UTF-8.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($BuildRoot)) {
    $BuildRoot = Join-Path $RepoRoot 'build\ci'
}
if ([string]::IsNullOrEmpty($OpenCvLib)) {
    $OpenCvLib = 'D:/opencv-src/build/install/lib'
}
if ([string]::IsNullOrEmpty($QtRoot)) {
    $QtRoot = 'D:/Qt/6.8.3/msvc2022_64'
}
if ([string]::IsNullOrEmpty($OpenCvBin)) {
    $OpenCvBin = (Split-Path -Parent $OpenCvLib) + '/bin'
}

# ------------------------------------------------------------------
# Output helpers
# ------------------------------------------------------------------
function Say([string]$msg) {
    Write-Host ""
    Write-Host "== $msg" -ForegroundColor Cyan
}

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "FAILED: $msg" -ForegroundColor Red
    exit 1
}

# Run a native command, echo it first, and die on a non-zero exit code.
#
# NOTE on $ErrorActionPreference: inside this function it is forced to
# 'Continue' for the duration of the call. With 'Stop', Windows PowerShell
# 5.1 turns a native command writing to STDERR into a *terminating*
# error -- and MSBuild/ctest write to STDERR all the time (warnings, and
# "Errors while running CTest" when a suite fails). The real signal is
# the exit code, which is what we check.
function Run([string]$exe, [string[]]$argv, [string]$cwd = "") {
    $shown = "$exe " + ($argv -join ' ')
    Write-Host "   > $shown"
    $global:LASTEXITCODE = 0
    $ErrorActionPreference = 'Continue'
    if ([string]::IsNullOrEmpty($cwd)) {
        & $exe @argv
    }
    else {
        Push-Location $cwd
        try { & $exe @argv } finally { Pop-Location }
    }
    if ($LASTEXITCODE -ne 0) {
        Fail "command failed (exit $LASTEXITCODE): $shown"
    }
}

# Same, but the output is captured as well as printed.
# The captured text is returned, so the caller can count things
# instead of hardcoding a number that silently drifts.
function RunCapture([string]$exe, [string[]]$argv, [string]$cwd = "") {
    $shown = "$exe " + ($argv -join ' ')
    Write-Host "   > $shown"
    $global:LASTEXITCODE = 0
    $ErrorActionPreference = 'Continue'
    $out = ""
    if ([string]::IsNullOrEmpty($cwd)) {
        $out = (& $exe @argv 2>&1 | Out-String)
    }
    else {
        Push-Location $cwd
        try { $out = (& $exe @argv 2>&1 | Out-String) } finally { Pop-Location }
    }
    Write-Host $out
    if ($LASTEXITCODE -ne 0) {
        Fail "command failed (exit $LASTEXITCODE): $shown"
    }
    return $out
}

# ------------------------------------------------------------------
# Step 1: preflight. Every dependency must be here. Nothing is skipped.
# ------------------------------------------------------------------
Say "Step 1/5  preflight"

function FindTool([string]$given, [string]$name, [string[]]$fallback) {
    if (-not [string]::IsNullOrEmpty($given)) {
        if (-not (Test-Path $given)) { Fail "explicitly requested $name not found: $given" }
        return (Resolve-Path $given).Path
    }
    $onPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($f in $fallback) {
        if (Test-Path $f) { return $f }
    }
    Fail ("$name not found. Install it, or pass the path explicitly " +
          "(-CmakeExe / -CtestExe). Looked on PATH and in: " + ($fallback -join ', '))
}

$Cmake = FindTool $CmakeExe 'cmake' @(
    'D:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe',
    'D:/Qt/Tools/CMake_64/bin/cmake.exe',
    'C:/Program Files/CMake/bin/cmake.exe'
)
$Ctest = FindTool $CtestExe 'ctest' @(
    'D:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe',
    'D:/Qt/Tools/CMake_64/bin/ctest.exe',
    'C:/Program Files/CMake/bin/ctest.exe'
)
Write-Host "   cmake : $Cmake"
Write-Host "   ctest : $Ctest"

if (-not (Test-Path (Join-Path $OpenCvLib 'OpenCVConfig.cmake'))) {
    Fail ("OpenCVConfig.cmake not found under: $OpenCvLib" +
          "`n   OpenCvLib must point at the 'lib' subdirectory of the OpenCV" +
          "`n   install, not at the install root -- the root one is a vcpkg-style" +
          "`n   wrapper whose regex does not recognise MSVC 19.51 (VS 2026).")
}
if (-not (Test-Path $OpenCvBin)) {
    Fail "OpenCV DLL directory not found: $OpenCvBin"
}
if (-not (Test-Path (Join-Path $QtRoot 'lib/cmake/Qt6'))) {
    Fail ("Qt6 not found under: $QtRoot" +
          "`n   Only W7 needs Qt. If you are only checking W1-W6, that is fine --" +
          "`n   but the script builds all 7, so it needs a real Qt6 install.")
}
Write-Host "   opencv: $OpenCvLib"
Write-Host "   opencv bin: $OpenCvBin"
Write-Host "   qt   : $QtRoot"

# ------------------------------------------------------------
# Snapshot the working tree BEFORE anything runs.
#
# The check at the end is "the tests did not ADD anything to the
# source tree", not "the working tree is pristine". The difference
# matters: a developer with uncommitted work would otherwise get a
# red CI that has nothing to do with the tests.
#
# (The first version of this script compared against an empty tree
# and duly failed on the author's own uncommitted changes.)
# ------------------------------------------------------------
$git = FindTool '' 'git' @(
    'C:/Program Files/Git/cmd/git.exe',
    'C:/Tools/git/cmd/git.exe'
)
$ErrorActionPreference = 'Continue'
$global:LASTEXITCODE = 0
$TreeBefore = @(& $git -C $RepoRoot status --porcelain | Where-Object { $_ -ne '' })
if ($LASTEXITCODE -ne 0) { Fail "git status failed (exit $LASTEXITCODE)" }
Write-Host "   working tree before: $($TreeBefore.Count) uncommitted entr(y/ies)"

# ------------------------------------------------------------------
# Step 2: configure + build all 7 projects
# ------------------------------------------------------------------
# W1..W7. W1-W4 have no tests -- they are built only, and that fact is
# reported in the summary at the end so nobody mistakes "built" for
# "tested".
$Projects = @(
    'W1_ImageIO',
    'W2_ColorFilter',
    'W3_EdgeContour',
    'W4_TemplateFeature',
    'W5_OCR_QRCode',
    'W6_PCB_DefectProject',
    'W7_VisionMasterLite'
)

Say "Step 2/5  configure + build (Release)"

foreach ($p in $Projects) {
    $src = Join-Path $RepoRoot $p
    $bld = Join-Path $BuildRoot $p
    Write-Host ""
    Write-Host "-- $p"

    $cfg = @('-S', $src, '-B', $bld,
             '-DCMAKE_BUILD_TYPE=Release',
             "-DOpenCV_DIR=$OpenCvLib",
             "-DOpenCV_BIN_DIR=$OpenCvBin",
             "-DCMAKE_PREFIX_PATH=$QtRoot")
    Run $Cmake $cfg

    if (-not $SkipBuild) {
        Run $Cmake @('--build', $bld, '--config', 'Release')
    }
    else {
        Write-Host "   (skipped build: -SkipBuild)"
    }
}

# ------------------------------------------------------------------
# Step 3: run the test suites
# ------------------------------------------------------------------
Say "Step 3/5  test suites"

$SuiteCount = 0
foreach ($p in @('W6_PCB_DefectProject', 'W7_VisionMasterLite')) {
    $bld = Join-Path $BuildRoot $p
    Write-Host ""
    Write-Host "-- $p"
    $ct = RunCapture $Ctest @('--test-dir', $bld, '-C', 'Release', '--output-on-failure')
    # "100% tests passed, 0 tests failed out of 14"
    $m = [regex]::Match($ct, 'out of (\d+)')
    if (-not $m.Success) {
        Fail "could not read the suite count out of the ctest output for $p"
    }
    $SuiteCount += [int]$m.Groups[1].Value
}

# ------------------------------------------------------------------
# Step 4: W5 smoke check
# ------------------------------------------------------------------
# W5 has no test suite. The shared common/qr_ocr suite covers the QR and
# OCR *core*; what it does not cover is W5's own main.cpp -- argument
# parsing, image loading, the --no-gui path, the exit code.
#
# The check asserts a serial number was actually decoded, matching the
# SHAPE of the value (PCB-2026-001-SN:A12345X) rather than a copy of it.
# The exact value is asserted in common/qr_ocr/tests/test_qr_ocr.cpp --
# one place, not two.
Say "Step 4/5  W5 smoke check"

$w5bin = Join-Path $BuildRoot 'W5_OCR_QRCode/Release'
if (-not (Test-Path $w5bin)) { $w5bin = Join-Path $BuildRoot 'W5_OCR_QRCode' }
$w5exe = Join-Path $w5bin 'w5_ocr_qrcode.exe'
if (-not (Test-Path $w5exe)) {
    Fail "W5 executable not found: $w5exe"
}
$w5out = Join-Path $BuildRoot 'w5_smoke_out.txt'
$global:LASTEXITCODE = 0
$ErrorActionPreference = 'Continue'
Push-Location $w5bin
try {
    & $w5exe 'qr' '--no-gui' *> $w5out
    $w5code = $LASTEXITCODE
}
finally { Pop-Location }
if ($w5code -ne 0) {
    Get-Content $w5out | Write-Host
    Fail "W5 qr smoke check exited with $w5code"
}
$w5text = Get-Content $w5out -Raw
if ($w5text -notmatch 'PCB-\d{4}-\d{3}-SN:[A-Z0-9]+') {
    Get-Content $w5out | Write-Host
    Fail ("W5 qr smoke check: no serial number in the output." +
          "`n   An exit code of 0 is NOT enough -- W5 returns 0 even when it" +
          "`n   finds no QR at all, so the output has to be checked too.")
}
Write-Host "   W5 decoded a serial number: OK"

# ------------------------------------------------------------------
# Step 5: the source tree must be clean
# ------------------------------------------------------------------
# This is the invariant from 2026-10-20: test products live in
# build/, and the source tree stays byte-identical.
#
# Before that change, 14 suites wrote into W7_VisionMasterLite/images/
# and the .gitignore rules hid the evidence. A CI job that checks
# "the source tree is clean" would have caught that on day one.
Say "Step 5/5  source tree must be clean"

$global:LASTEXITCODE = 0
$TreeAfter = @(& $git -C $RepoRoot status --porcelain | Where-Object { $_ -ne '' })
if ($LASTEXITCODE -ne 0) { Fail "git status failed (exit $LASTEXITCODE)" }

# Anything in the AFTER set that was not in the BEFORE set is new.
# Compare-Object needs a non-empty reference set, hence the placeholder.
$ref = $TreeBefore
if ($ref.Count -eq 0) { $ref = @('(clean tree)') }
$added = @(Compare-Object -ReferenceObject $ref -DifferenceObject $TreeAfter |
           Where-Object { $_.SideIndicator -eq '=>' })
if ($added.Count -gt 0) {
    Write-Host ""
    Write-Host "The tests ADDED files to the source tree:" -ForegroundColor Red
    $added | ForEach-Object { Write-Host "   $($_.InputObject)" -ForegroundColor Red }
    Fail ("test products leaked into the source tree." +
          "`n   Every suite must run in build/test_work/<suite>/ -- see the" +
          "`n   w7_add_test / w6_add_test functions in the CMakeLists files.")
}
Write-Host "   source tree unchanged by the tests: OK"

# ------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------
Say "summary"
Write-Host "   built   : $($Projects.Count) projects (Release)"
Write-Host "   tested  : $SuiteCount ctest suites (W6 + W7)"
Write-Host "   smoke   : W5 qr --no-gui"
Write-Host "   clean   : tests added nothing to the source tree"
Write-Host ""
Write-Host "ALL CHECKS PASSED" -ForegroundColor Green
exit 0
