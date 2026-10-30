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
      3. W1, W2, W3, W4, W6 and W7 suites run and pass.
      3b. The assertion count is counted FROM THIS BUILD TREE and
          printed (plus written to build/ci/assertions.txt).
          The number quoted in the README and in the notes must come
          from here -- never from a hand count against a tree that
          might be stale. (That mistake cost two rounds a wrong
          number; see the notes.)
      3c. Every CURRENT number quoted in the documentation is compared
          against what was just measured. The expected values are READ
          FROM the docs, never hardcoded here -- a hardcoded expectation
          is a second copy, and a second copy is what drifts.
          This is debt 11 (see the knob audit, section 10).
      3d. Every relative link in the documentation resolves. A link is a
          recorded fact too -- "that file is over there, and it is still
          there" -- and a link nobody follows drifts exactly like a
          table nobody checks.
      3e. Every section pointer in the documentation resolves. A
          pointer -- "chapter 10 of that note" -- is a recorded fact
          about a PLACE rather than about a file, and it drifts the
          same way. 55 of the 115 pointers aim at one file, so this
          is the check that notices when that file is renumbered.
      3f. The census gate's OWN failure path: the tree is broken on
          purpose, on a copy, and the gate must notice.
      3g. The same for the three DOCUMENTATION gates (3c numbers, 3d
          links, 3e section pointers): each one is run against a
          deliberately broken copy and must go RED, and against a
          copy whose RULER is broken -- with the defect still there --
          must go GREEN with the same summary it prints about the
          PRISTINE tree.  The broken tree in front of a blind ruler is
          not being measured at all, and that is the point.
      4. W5's executable still runs. W5 got its OWN test suite on
         2026-10-30 (present_pipeline), so this smoke check is no
         longer the only thing CI says about W5 -- but it is still
         worth having, because it runs the REAL executable and looks
         at what it prints.
         It asserts a serial number was actually decoded, using the
         *shape* of the value, not a copy of the value -- the exact
         value is asserted by the shared common/qr_ocr suite.
      5. W7's executable runs a WHOLE PIPELINE headless and judges it.
         Until 2026-10-24 every CI run was unit tests only -- nobody had
         ever run a complete pipeline from end to end in CI. This step
         is the first time that question gets an answer.
         It asserts the exit code AND the content of the result JSON:
         an exit code of 0 is not enough on its own (the same trap the
         W5 check had).
      6. The source tree did not change.
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
Say "Step 1/6  preflight"

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

# ------------------------------------------------------------------
# Qt's DLLs have to be findable at RUN time.
#
# Why this is here and not in the workflow file:
#
#   W7 links Qt6::Core/Gui/Widgets, and NONE of the seven projects copy
#   the Qt DLLs next to the exe (only the OpenCV ones are copied). So
#   the exes find Qt6Core.dll the Windows way: through PATH.
#
#   On the developer's machine that works because Qt was installed with
#   its bin directory on the machine PATH. On a GitHub runner it does
#   NOT: the runner installs Qt into D:\a\_temp\Qt\6.8.3\msvc2022_64,
#   and that bin directory is not on PATH.
#
#   That is the same disease as the aqtinstall bug: the local rehearsal
#   passes because of a fact about THIS MACHINE, and the runner fails.
#   So the path is put on PATH HERE, from the $QtRoot the caller gave
#   us -- the same value the build used to find the headers and libs.
#   One source, not two.
# ------------------------------------------------------------------
$qtBin = Join-Path $QtRoot 'bin'
if (-not (Test-Path $qtBin)) {
    Fail ("Qt bin directory not found: $qtBin" +
          "`n   The exes need Qt6Core.dll at RUN time, and they find it" +
          "`n   through PATH -- this script puts it there.")
}
$pathEntries = @($env:PATH -split ';' | Where-Object { $_ -ne '' })
if ($pathEntries -notcontains $qtBin) {
    $env:PATH = "$qtBin;" + ($pathEntries -join ';')
    Write-Host "   qt bin added to PATH: $qtBin"
}
else {
    Write-Host "   qt bin already on PATH: $qtBin"
}

# ------------------------------------------------------------------
# Tesseract's directory has to be on PATH too.
#
# The shared QR/OCR core calls tesseract as an EXTERNAL program, so the
# tests need it on PATH -- not just present. The workflow file used to
# do this in YAML, which meant there were two places that knew about it
# and the local rehearsal (tools/ci.ps1 on its own) did not do it at
# all. One place now: here, driven by TESSERACT_EXE -- the value
# tools/ci-deps.ps1 exported after it installed tesseract.
# ------------------------------------------------------------------
if ($env:TESSERACT_EXE) {
    if (-not (Test-Path $env:TESSERACT_EXE)) {
        Fail ("TESSERACT_EXE is set but the file is not there: $env:TESSERACT_EXE" +
              "`n   The OCR half of the QR/OCR suite would be skipped, and a" +
              "`n   skipped path is worse than a failing one.")
    }
    $tessDir = Split-Path -Parent $env:TESSERACT_EXE
    if ($pathEntries -notcontains $tessDir) {
        $env:PATH = "$tessDir;" + ($env:PATH)
        Write-Host "   tesseract dir added to PATH: $tessDir"
    }
    else {
        Write-Host "   tesseract dir already on PATH: $tessDir"
    }
}
else {
    Write-Host "   tesseract: TESSERACT_EXE is not set -- the OCR suite will report it"
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

# The snapshot is also written to a FILE, because step 6 was pulled
# out into tools/check-ci-steps.ps1 -- its inputs are parameters, so
# that a mutation harness can break them on purpose -- and that
# script takes the baseline as a file.
#
# UTF-8 WITHOUT a BOM, one line per entry.  The reader is
# [System.IO.File]::ReadAllLines, and a BOM would become part of the
# FIRST line -- which is a line of the snapshot, so it would compare
# unequal to the very same line seen again.
$TreeBaselineFile = Join-Path $BuildRoot 'ci_tree_baseline.txt'
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
#
# The build root may NOT EXIST YET.
#
# This snapshot is taken BEFORE the build, and the build is what
# creates build\ci.  On a clean clone there is no build\ci at all --
# and WriteAllLines into a missing directory THROWS.  The throw was
# swallowed, because $ErrorActionPreference is 'Continue' in this
# section, so the script carried on and the missing file only turned
# up 16 minutes later, at step 6, as "baseline file not found".
#
# The local tree was green because it had been built before: the
# directory was there, left over from the last build.  "The local run
# was green" and "the repository can pass" are two different
# questions (notes/atomic/at-an-empty-dir-is-not-in-git.md).
#
# => create the directory first, and CHECK that the file is there.
# A failure that happens NOW is worth a hundred that happen later.
try {
    [void][System.IO.Directory]::CreateDirectory($BuildRoot)
    [System.IO.File]::WriteAllLines($TreeBaselineFile, [string[]]$TreeBefore, $Utf8NoBom)
}
catch {
    Fail ("the tree snapshot could not be written (" + $_.Exception.GetType().Name + "): " + $TreeBaselineFile)
}
if (-not (Test-Path -LiteralPath $TreeBaselineFile)) {
    Fail ("the tree snapshot is not there: " + $TreeBaselineFile)
}
$BaselineLines = [System.IO.File]::ReadAllLines($TreeBaselineFile, $Utf8NoBom)
if ($BaselineLines.Count -ne $TreeBefore.Count) {
    Fail ("the tree snapshot holds " + $BaselineLines.Count + " line(s), the tree holds " + $TreeBefore.Count)
}

# ------------------------------------------------------------------
# Step 2: configure + build all 7 projects
# ------------------------------------------------------------------
# W1..W7. Every one of them has a test suite now:
# W1 and W2 on 2026-10-26, W3 on 2026-10-28, W4 on 2026-10-29,
# W5 on 2026-10-30.
#
# (Each one got its first suite the same way: the logic was pulled out
# of main() into a pipeline.{h,cpp} so that a headless test could
# reach it at all.)
$Projects = @(
    'W1_ImageIO',
    'W2_ColorFilter',
    'W3_EdgeContour',
    'W4_TemplateFeature',
    'W5_OCR_QRCode',
    'W6_PCB_DefectProject',
    'W7_VisionMasterLite'
)

# The projects that HAVE test suites.
#
# This is a list of its own, not "the projects except the ones without
# tests": the list of what is actually tested is a fact that changes
# (W1 and W2 got their first suites on 2026-10-26, W3 on 2026-10-28,
# W4 on 2026-10-29, W5 on 2026-10-30),
# and a fact that changes needs to be written down in ONE place --
# otherwise a project gets a suite and nobody runs it, and the suite
# silently rots.
$TestProjects = @(
    'W1_ImageIO',
    'W2_ColorFilter',
    'W3_EdgeContour',
    'W4_TemplateFeature',
    'W5_OCR_QRCode',
    'W6_PCB_DefectProject',
    'W7_VisionMasterLite'
)

Say "Step 2/6  configure + build (Release)"

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
Say "Step 3/6  test suites"

$SuiteCount = 0
foreach ($p in $TestProjects) {
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
# Step 3b/6: count the assertions, from THIS build tree only
# ------------------------------------------------------------------
#
# Why this step exists (2026-10-25):
#
#   The assertion count in the README and in the notes used to be
#   measured BY HAND, against a build tree that was not always
#   current. The local tree had a stale w7_engine_test.exe (99
#   assertions instead of 111) because an earlier session only
#   rebuilt SOME of the targets. Every number taken from that tree
#   was 12 too low -- for two rounds, and nobody noticed.
#
#   The number was wrong because:
#     * it was measured by hand, and
#     * it was measured against a tree that could be stale.
#
#   So now CI prints it, from a tree it just built. If the tree is
#   stale, CI cannot be -- it rebuilds everything in step 2.
#
#   NOTE: this step does NOT hardcode an expected number. A hardcoded
#   expectation is a number that drifts silently, which is the very
#   disease this step exists to cure. It PRINTS the number and writes
#   it to build/ci/assertions.txt, so the docs can be checked against
#   a value that came from a fresh build.
#
#   (Turning "printed" into "enforced" is debt 11; see
#    notes/notes_W7_Knob_Audit_2026-10.md, section 10.)
# ------------------------------------------------------------------
Say "Step 3b/6  assertion count"

$AssertionTotal = 0
$AssertionReport = @()
$PerSuiteLines = @()
foreach ($p in $TestProjects) {
    $bld = Join-Path $BuildRoot $p
    # A second ctest run, this time verbose, so that each suite's
    # "pass=N" line is in the captured text. --output-on-failure only
    # prints the output of FAILING tests, which is exactly the output
    # we need when the count is wrong.
    $verbose = RunCapture $Ctest @('--test-dir', $bld, '-C', 'Release', '-V')
    $lines = $verbose -split "`r?`n"
    $perSuite = @{}
    # The suite NAMES come from the same output:
    #   " 1/26 Test  #1: qr_ocr ............ Passed  0.41 sec"
    # They are needed because the docs quote a per-suite table, and a
    # table of counts without names cannot be checked -- only the total
    # would be, and a total can be right while the distribution is wrong.
    $suiteNames = @{}
    foreach ($l in $lines) {
        $m = [regex]::Match($l, '^\s*\d+/\d+\s+Test\s+#(\d+):\s+(\S+)')
        if ($m.Success) { $suiteNames[[int]$m.Groups[1].Value] = $m.Groups[2].Value }
    }
    foreach ($l in $lines) {
        # Two shapes exist:
        #   "12: SUMMARY: pass= 90  fail= 0   RESULT=PASS"
        #   "19: pass=166  fail=0"
        # Both are anchored to the start of the line (after the ctest
        # test-number prefix), so a "[PASS] ... (actual pass=1)" line
        # can never be mistaken for a count.
        $m = [regex]::Match($l, '^\s*(\d+):\s*SUMMARY:\s*pass=\s*(\d+)')
        if (-not $m.Success) {
            $m = [regex]::Match($l, '^\s*(\d+):\s*pass=\s*(\d+)\s+fail=')
        }
        if ($m.Success) {
            $idx = [int]$m.Groups[1].Value
            $n = [int]$m.Groups[2].Value
            if (-not $perSuite.ContainsKey($idx)) { $perSuite[$idx] = 0 }
            if ($n -gt $perSuite[$idx]) { $perSuite[$idx] = $n }
        }
    }
    if ($perSuite.Count -eq 0) {
        Fail ("could not find a single 'pass=' count in the ctest output for $p." +
              "`n   If the suites changed their summary line, fix the counter here." +
              "`n   A count of zero would be worse than no count at all.")
    }

    # ------------------------------------------------------------------
    # Step 3b-bis: how many suites did ctest actually RUN, and did the
    # counter see ALL of them?
    #
    # Why this cross-check exists (2026-10-26):
    #   The counter counted whatever it could SEE. Eight suites (added
    #   2026-10-24) were missing from the QT_FORCE_STDERR_LOGGING list in
    #   W7's CMakeLists, so their qInfo summary never reached ctest. The
    #   counter counted 18 of 26 suites and reported 1804 assertions --
    #   and said nothing about the 8 it could not see.
    #
    #   A total that is quietly SHORT is the same disease as "the
    #   pipeline passed because nothing was judged": the number looks
    #   fine, it is just missing part of the population. Step 3c then
    #   compares that short total against the docs and fails for the
    #   WRONG reason -- the docs were right, the MEASUREMENT was not.
    #
    #   So the measuring step has to say "I could not see N suites" out
    #   loud, instead of reporting a smaller number as if it were the
    #   whole truth.
    # ------------------------------------------------------------------
    $mRan = [regex]::Match($verbose, '(?m)^\d+% tests passed.*?out of (\d+)')
    if (-not $mRan.Success) {
        $mRan = [regex]::Match($verbose, 'out of (\d+)')
    }
    if (-not $mRan.Success) {
        Fail ("could not read the suite count out of the ctest -V output for $p." +
              "`n   Without it there is nothing to compare the counter against.")
    }
    $ranSuites = [int]$mRan.Groups[1].Value
    if ($suiteNames.Count -ne $ranSuites) {
        Fail ("the ctest output lists $($suiteNames.Count) suite lines but ctest says it ran $ranSuites suites." +
              "`n   The name parsing is out of step with the output -- fix the counter" +
              "`n   before trusting anything it says.")
    }
    if ($perSuite.Count -ne $ranSuites) {
        $missed = @()
        foreach ($idx in ($suiteNames.Keys | Sort-Object)) {
            if (-not $perSuite.ContainsKey($idx)) {
                $missed += $suiteNames[$idx]
            }
        }
        Fail ("for $p, ctest ran $ranSuites suites but the counter only saw $($perSuite.Count)." +
              "`n   Suites whose output never reached ctest: " + ($missed -join ', ') +
              "`n   Their summary line went somewhere else: Qt sends qInfo to the" +
              "`n   Windows debug channel unless QT_FORCE_STDERR_LOGGING=1 is set." +
              "`n   Check set_tests_properties(... ENVIRONMENT 'QT_FORCE_STDERR_LOGGING=1')" +
              "`n   in the project's CMakeLists -- and make sure the list it names" +
              "`n   EVERY suite, not a hand-kept copy of that list.")
    }
    foreach ($idx in ($perSuite.Keys | Sort-Object)) {
        if (-not $suiteNames.ContainsKey($idx)) {
            Fail ("could not find the NAME of suite #$idx in the ctest output for $p." +
                  "`n   A count without a name cannot be checked against a table.")
        }
        $PerSuiteLines += ("{0}|{1}|{2}" -f $p, $suiteNames[$idx], $perSuite[$idx])
    }
    $sub = ($perSuite.Values | Measure-Object -Sum).Sum
    $AssertionTotal += $sub
    $AssertionReport += ("{0,-24} {1,3} suites / {2,6} assertions" -f $p, $perSuite.Count, $sub)
    Write-Host ("   {0,-24} {1,3} suites / {2,6} assertions" -f $p, $perSuite.Count, $sub)
}
$AssertionReport += ("{0,-24} {1,3} suites / {2,6} assertions" -f 'TOTAL', $SuiteCount, $AssertionTotal)
Write-Host ("   {0,-24} {1,3} suites / {2,6} assertions" -f 'TOTAL', $SuiteCount, $AssertionTotal)

# Machine-readable: the docs quote these numbers, so they must be
# quotable from a file that came out of a fresh build.
$assertFile = Join-Path $BuildRoot 'assertions.txt'
$AssertionReport | Set-Content -Encoding ASCII -LiteralPath $assertFile
Write-Host "   written to $assertFile"

# The same numbers, but per suite and WITH NAMES. Step 3c compares them
# with the per-suite table in W7_VisionMasterLite/README.md.
$perSuiteFile = Join-Path $BuildRoot 'assertions_by_suite.txt'
$PerSuiteLines | Set-Content -Encoding ASCII -LiteralPath $perSuiteFile
Write-Host "   written to $perSuiteFile"

# ------------------------------------------------------------------
# Step 3c/6: the numbers quoted in the docs must match reality
# ------------------------------------------------------------------
#
# Why this step exists (debt 11):
#
#   Step 3b prints the assertion count. Printing is not enforcing.
#   The README and the notes quote that count in a dozen places, and
#   those quotes had already drifted twice (and a third copy was found
#   stale in the very same round this step was written).
#
#   The obvious way to enforce it would be to hardcode 2371 in this
#   script. That is exactly the disease: a hardcoded expectation is a
#   SECOND copy of the number, and the second copy is what drifts.
#
#   So the expected value is READ FROM the documentation and compared
#   with what was just measured. The docs stay the single source of
#   truth; this step only checks that the truth has not moved under
#   them.
#
#   The checker also verifies the per-suite table, not just the totals:
#   a total can be right while the distribution is wrong.
Say "Step 3c/6  documentation numbers"

$checker = Join-Path $RepoRoot 'tools\check-doc-numbers.ps1'
if (-not (Test-Path $checker)) {
    Fail "doc number checker not found: $checker"
}
# Run it in a separate powershell process: its `exit 1` must fail THIS
# script, and a child script called with `&` would exit the child only.
$psExe = (Get-Process -Id $PID).Path
Run $psExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $checker,
             '-RepoRoot', $RepoRoot, '-PerSuiteFile', $perSuiteFile)
Write-Host "   documentation numbers agree with the measured build: OK"

# ------------------------------------------------------------------
# Step 3d/6: the links between the documents
# ------------------------------------------------------------------
#
# Why this step exists (2026-10-26):
#
#   Step 3c checks that the NUMBERS in the docs match reality. It does
#   not check the LINKS -- and a link is a recorded fact too: "that file
#   is over there, and it is still there".
#
#   The same round that added Step 3c also deleted a card from
#   notes/atomic/ (it duplicated another card, so its content was folded
#   into the old one as a "recurrence" section). Three other cards
#   linked to the deleted file. Nothing noticed: the link text still
#   read perfectly, the markdown still rendered, and CI was green.
#
#   While writing the checker it found a SECOND broken link, one that
#   had been sitting in W7_VisionMasterLite/README.md for a while: it
#   pointed at "W7_VisionMasterLite/operator_library_groups.h" from
#   inside W7_VisionMasterLite/ -- i.e. at
#   W7_VisionMasterLite/W7_VisionMasterLite/operator_library_groups.h,
#   which does not exist. The file it meant is one directory up.
#
#   => A link nobody follows is a link that drifts. Same disease as the
#      per-suite table, one object over.
Say "Step 3d/6  documentation links"

$linkChecker = Join-Path $RepoRoot 'tools\check-links.ps1'
if (-not (Test-Path $linkChecker)) {
    Fail "link checker not found: $linkChecker"
}
Run $psExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $linkChecker,
             '-RepoRoot', $RepoRoot)
Write-Host "   every relative link in the docs resolves: OK"

# ------------------------------------------------------------------
# Step 3e/6: the section pointers in the documents
# ------------------------------------------------------------------
#
# Why this step exists (2026-10-27):
#
#   Step 3d follows LINKS. A link is a recorded fact about a FILE.
#   A section pointer -- "chapter 10 of that note", "section 6.3 of
#   the other one" -- is a recorded fact about a PLACE, and it drifts
#   exactly the same way: a pointer at a chapter that was renumbered
#   reads perfectly well, and nothing in the text says so.
#
#   The measurement that motivated this: 193 section pointers, of which
#   60 aim at ONE file -- notes/notes_W7_Knob_Audit_2026-10.md, the
#   debt register this project keeps editing. When its chapter 10 is
#   renumbered, 37 of those pointers break. A mutation test confirmed
#   that number: the checker was run against a copy of the tree with
#   that one heading renumbered, and 37 pointers went red.
#
#   => A pointer nobody follows is a pointer that drifts. Same disease
#      as the link and the table, one object over.
#
#   The checker is tools/check-sections.ps1. It binds each pointer to
#   the file name that comes BEFORE it on the same line, so that a
#   pointer is checked against the file it actually means. Its rule
#   was measured, not guessed: a first version that accepted "any
#   file named on the line" turned 18 of those 37 breaks red and
#   silently let 19 of them point at a chapter of the SAME NUMBER in
#   the reader's own file -- a pointer that quietly points somewhere
#   else is worse than one that is obviously broken.
Say "Step 3e/6  documentation section pointers"

$sectionChecker = Join-Path $RepoRoot 'tools\check-sections.ps1'
if (-not (Test-Path $sectionChecker)) {
    Fail "section checker not found: $sectionChecker"
}
Run $psExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $sectionChecker,
             '-RepoRoot', $RepoRoot)
Write-Host "   every section pointer in the docs resolves: OK"

# ------------------------------------------------------------------
# Step 3f/6: the census gate's OWN failure path
# ------------------------------------------------------------------
#
# Why this step exists (2026-10-27):
#
#   Step 3e checks the numbers the documents quote.  But a gate that
#   has never been seen to go RED is a gate nobody knows works:
#   "CI is green" only proves the SUCCESS path.  The failure path
#   runs only when the tree is broken -- and the tree is never broken.
#
#   So the four mutations run here, on every CI run, against COPIES
#   of the tree (the real tree is never touched):
#
#     A  a quoted number drifts          -> must be RED
#     B  another quoted number drifts     -> must be RED
#     C  the CHECKER'S OWN header drifts  -> must be RED
#     D  the ruler is broken              -> must be GREEN
#
#   D is the one that matters: it proves the red in A/B/C came from
#   the CHECKER and not from a coincidence.  A broken ruler must
#   produce ZERO reds -- so if D ever comes out RED, the ruler is
#   measuring nothing at all, and A/B/C going red would mean nothing.
#
#   The harness is tools/mutate-census.ps1.  It used to live under
#   build/probe/, which .gitignore blocks -- i.e. the script that
#   measured the census was NOT in the repository, so the census table
#   in the notes was hearsay (notes/atomic/at-a-number-nobody-can-
#   recompute-is-hearsay.md).
Say "Step 3f/6  the census gate's own failure path"

$mutationHarness = Join-Path $RepoRoot 'tools\mutate-census.ps1'
if (-not (Test-Path $mutationHarness)) {
    Fail "mutation harness not found: $mutationHarness"
}
Run $psExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $mutationHarness,
             '-RepoRoot', $RepoRoot, '-Mutation', 'A,B,C,D')
Write-Host "   the census gate still catches a broken ruler: OK"

# ------------------------------------------------------------------
# Step 3g/6: the three documentation gates' OWN failure paths
# ------------------------------------------------------------------
#
# Why this step exists (2026-10-29):
#
#   Step 3f proves the CENSUS gate can go red.  The three gates that
#   came before it -- 3c (the numbers), 3d (the links), 3e (the
#   section pointers) -- had never been seen to go red at all.  They
#   were green on every run since the day they were written, and a
#   gate that has only ever been green is a gate nobody knows works:
#   the checker could be broken in a way that makes it say OK about
#   anything, and the log would look exactly the same.
#
#   So the same treatment, one harness for the three of them:
#
#     A, B, C   a defect is put into a COPY of the tree.  The gate
#               must go RED, and it must say so in the way the
#               mutation was aimed at -- the number comparison, the
#               link resolution, the pointer resolution -- not by some
#               other failure that happens to be red as well.
#
#     D         THE RULER IS BROKEN, and the defect is STILL THERE.
#               The gate must go GREEN, and it must print EXACTLY the
#               summary it prints about the PRISTINE tree -- the broken
#               tree in front of it is not being measured at all.
#
#   D is what carries the proof.  "A, B and C came out red" only
#   shows that the checker and the mutation behaved consistently;
#   D shows that the RED came from the checker and not from a
#   coincidence.  A gate that stays red with a blind ruler was never
#   measuring the tree at all.
#
#   The real tree is never touched.  Every run happens on a COPY
#   under the temp directory, and the copy is verified against the
#   real tree first (same summary line) -- otherwise the harness
#   would be measuring air and reporting a clean sheet.
#
#   The harness is tools/mutate-gates.ps1, and the machinery it
#   shares with tools/mutate-census.ps1 lives in tools/lib/.
Say "Step 3g/6  the documentation gates' own failure paths"

$gateHarness = Join-Path $RepoRoot 'tools\mutate-gates.ps1'
if (-not (Test-Path $gateHarness)) {
    Fail "mutation harness not found: $gateHarness"
}
Run $psExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $gateHarness,
             '-RepoRoot', $RepoRoot, '-Mutation', 'A,B,C,D')
Write-Host "   the number, link and section gates still catch a broken ruler: OK"

# ------------------------------------------------------------------
# Step 3h/6: the CI's OWN three steps' failure paths
# ------------------------------------------------------------------
#
# Why this step exists (2026-10-29):
#
#   Steps 3f and 3g prove that the DOCUMENTATION gates can go red.
#   The three steps that are not documentation gates -- 4 (the W5
#   smoke check), 5 (the W7 headless pipeline) and 6 (the source
#   tree stayed clean) -- had never been seen to go red either.  They
#   were green on every run since the day they were written, and the
#   same hole shows up: the checker could be broken in a way that
#   makes it say OK about anything, and the log would look exactly
#   the same.
#
#   So the three checks moved OUT of this file into
#   tools/check-ci-steps.ps1, with every input as a parameter, and
#   the harness asks the question that could not be asked before.
#
#   THE MUTATIONS CHANGE THE INPUTS, NOT THE TREE:
#
#     A, B, C   a defect is put into the INPUT -- the working
#               directory, the executable, the pipeline file, the
#               image, the tree the tests are measured against.
#               The step must go RED, and it must say so in the way
#               the mutation was aimed at, not by some other failure
#               that happens to be red as well.
#
#     D         THE RULER IS BROKEN, and the defect is STILL THERE.
#               The step must go GREEN, and it must print EXACTLY
#               what it prints about the PRISTINE tree.
#
#   D is what carries the proof, the same way it does in 3f and 3g.
#
#   The real tree is never touched: the probe files the harness puts
#   into the tree are removed again before it returns, and its
#   working copy lives under the temp directory.
#
#   The harness is tools/mutate-ci-steps.ps1, and the machinery it
#   shares with the other harnesses lives in tools/lib/.
Say "Step 3h/6  the CI's own steps' failure paths"

$ciStepHarness = Join-Path $RepoRoot 'tools\mutate-ci-steps.ps1'
if (-not (Test-Path $ciStepHarness)) {
    Fail "mutation harness not found: $ciStepHarness"
}
Run $psExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ciStepHarness,
             '-RepoRoot', $RepoRoot, '-BuildRoot', $BuildRoot, '-Mutation', 'A,B,C,D')
Write-Host "   the CI steps still catch a broken ruler: OK"

# The three steps below live in one script of their own, so that the
# harness above can run them with broken inputs.
$CiStepChecker = Join-Path $RepoRoot 'tools\check-ci-steps.ps1'
if (-not (Test-Path $CiStepChecker)) {
    Fail "CI step checker not found: $CiStepChecker"
}

# ------------------------------------------------------------------
# Step 4: W5 smoke check
# ------------------------------------------------------------------
# W5 has a test suite of its own now (2026-10-30), but this step is
# still worth running: the suite tests the LOGIC, this step runs the
# REAL EXECUTABLE and looks at what it prints.  The shared
# common/qr_ocr suite covers the QR and OCR *core*; the W5 suite covers
# W5's own presentation layer.  Neither one runs main() itself --
# argument parsing, image loading, the --no-gui path and the exit code
# are only exercised HERE.
#
# The check asserts a serial number was actually decoded, matching the
# SHAPE of the value (PCB-2026-001-SN:A12345X) rather than a copy of it.
# The exact value is asserted in common/qr_ocr/tests/test_qr_ocr.cpp --
# one place, not two.
#
# The check itself lives in tools/check-ci-steps.ps1 (moved out on
# 2026-10-29).  It used to sit HERE, inline, and an inline check can
# only be run by running the WHOLE CI -- ten minutes and every project
# -- so the question "does this go RED when W5 is broken?" could not be
# asked at all.  Step 3h asks it, on every run, against a broken W5.
Say "Step 4/6  W5 smoke check"

Run $psExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $CiStepChecker,
             '-RepoRoot', $RepoRoot, '-BuildRoot', $BuildRoot, '-Step', '4')

# ------------------------------------------------------------------
# Step 5/6: W7 headless smoke check -- run a WHOLE PIPELINE
# ------------------------------------------------------------------
# Why this step exists:
#
#   Until 2026-10-24 every CI run was unit tests only. Nobody had ever
#   run a complete W7 pipeline from end to end in CI -- so the question
#   "does a whole pipeline actually run?" had no answer at all.
#   The unit suites prove the pieces; nothing proved the whole.
#
#   This step runs the real exe with the real operators on a real image.
#
# Why it asserts the OUTPUT and not just the exit code:
#   Same trap the W5 check had: an exit code of 0 is not enough.
#   The pipeline could "succeed" while judging nothing at all
#   (final_result = UNKNOWN). The result JSON is what says
#   "it really judged PASS, and it really found 6 components".
#
# Why it runs from the exe directory:
#   pcb_inspect reads its board recipe by the RELATIVE path
#   "recipes/demo.json" -- same as the GUI does. The recipes are copied
#   next to the exe at build time, so that directory is the one place
#   where the relative path resolves. Running from anywhere else would
#   fail on the recipe, not on the pipeline.
#
# The check itself lives in tools/check-ci-steps.ps1 (moved out on
# 2026-10-29).  Same reason as step 4: inline, it could only be run by
# running the whole CI, so nobody could ask whether it goes red when
# the pipeline is broken.  Step 3h asks, on every run.
#
# NOTE: this file must stay PURE ASCII (no BOM). PowerShell 5.1 reads a
#   no-BOM .ps1 as ANSI/GBK, and a Chinese comment can swallow the next
#   line of CODE. Explanations in Chinese belong in notes/.
Say "Step 5/6  W7 headless pipeline smoke check"

Run $psExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $CiStepChecker,
             '-RepoRoot', $RepoRoot, '-BuildRoot', $BuildRoot, '-Step', '5')

# ------------------------------------------------------------------
# Step 6/6: the source tree must be clean
# ------------------------------------------------------------------
# This is the invariant from 2026-10-20: test products live in
# build/, and the source tree stays byte-identical.
#
# Before that change, 14 suites wrote into W7_VisionMasterLite/images/
# and the .gitignore rules hid the evidence. A CI job that checks
# "the source tree is clean" would have caught that on day one.
Say "Step 6/6  source tree must be clean"

# The "before" snapshot was written to a file near the top of this
# script (it is taken before the build, not before this step).  The
# check itself lives in tools/check-ci-steps.ps1, for the same reason
# as steps 4 and 5: a check that can only be run by running the whole
# CI is a check nobody can ask a question of.
Run $psExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $CiStepChecker,
             '-RepoRoot', $RepoRoot, '-BuildRoot', $BuildRoot, '-Step', '6',
             '-BaselineFile', $TreeBaselineFile)

# ------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------
Say "summary"
Write-Host "   built   : $($Projects.Count) projects (Release)"
Write-Host "   tested  : $SuiteCount ctest suites (W1 + W2 + W3 + W4 + W5 + W6 + W7)"
Write-Host "   asserts : $AssertionTotal assertions (counted from this build)"
Write-Host "   docs    : every number quoted in the docs matches the build"
Write-Host "   links   : every relative link in the docs resolves"
Write-Host "   smoke   : W5 qr --no-gui + W7 headless pipeline"
Write-Host "   clean   : tests added nothing to the source tree"
Write-Host ""
Write-Host "ALL CHECKS PASSED" -ForegroundColor Green
exit 0
