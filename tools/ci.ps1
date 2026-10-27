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
      3. W1, W2, W6 and W7 suites run and pass.
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
      4. W5's executable still runs. W5 has no test suite of its own,
         so this smoke check is the only thing CI says about W5.
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

# ------------------------------------------------------------------
# Step 2: configure + build all 7 projects
# ------------------------------------------------------------------
# W1..W7. W3-W5 have no tests -- they are built only, and that fact is
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

# The projects that HAVE test suites.
#
# This is a list of its own, not "the projects except the ones without
# tests": the list of what is actually tested is a fact that changes
# (W1 and W2 got their first suites on 2026-10-26), and a fact that
# changes needs to be written down in ONE place -- otherwise a project
# gets a suite and nobody runs it, and the suite silently rots.
$TestProjects = @(
    'W1_ImageIO',
    'W2_ColorFilter',
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
Say "Step 4/6  W5 smoke check"

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
# NOTE: this file must stay PURE ASCII (no BOM). PowerShell 5.1 reads a
#   no-BOM .ps1 as ANSI/GBK, and a Chinese comment can swallow the next
#   line of CODE. Explanations in Chinese belong in notes/.
Say "Step 5/6  W7 headless pipeline smoke check"

$w7bin = Join-Path $BuildRoot 'W7_VisionMasterLite/Release'
if (-not (Test-Path $w7bin)) { $w7bin = Join-Path $BuildRoot 'W7_VisionMasterLite' }
$w7exe = Join-Path $w7bin 'w7_vm_lite.exe'
if (-not (Test-Path $w7exe)) {
    Fail "W7 executable not found: $w7exe"
}
$w7pipe = Join-Path $RepoRoot 'W7_VisionMasterLite/pipelines/smoke.json'
if (-not (Test-Path $w7pipe)) {
    Fail "W7 smoke pipeline not found: $w7pipe"
}
# The good board. It must judge PASS -- a defect would be a false alarm
# on the CI, and a broken recipe would look like a broken pipeline.
$w7img = Join-Path $RepoRoot 'W6_PCB_DefectProject/images/baseline.png'
if (-not (Test-Path $w7img)) {
    Fail "W7 smoke image not found: $w7img"
}
$w7out = Join-Path $BuildRoot 'w7_headless_smoke_out.txt'
$w7result = Join-Path $BuildRoot 'w7_headless_smoke_result.json'
$global:LASTEXITCODE = 0
$ErrorActionPreference = 'Continue'
Push-Location $w7bin
try {
    # WARNING: Start-Process -Wait, NOT "& $w7exe ...".
    #   w7_vm_lite.exe is a WIN32 (GUI subsystem) program, and PowerShell
    #   does NOT wait for GUI programs: "&" returns in a few ms while the
    #   program is still running. Measured: the result file did not exist
    #   yet when the check right after it ran -- it showed up 3 seconds
    #   later. That is the classic "checked before it was written" bug.
    #   cmd.exe waits for GUI programs; PowerShell does not.
    $p = Start-Process -FilePath $w7exe `
            -ArgumentList @('--run', $w7pipe, '--image', $w7img, '--out', $w7result) `
            -WorkingDirectory $w7bin -Wait -PassThru -NoNewWindow `
            -RedirectStandardOutput $w7out -RedirectStandardError "$w7out.err"
    $w7code = $p.ExitCode
}
finally { Pop-Location }
if ($w7code -ne 0) {
    Get-Content $w7out -Encoding UTF8 | Write-Host
    Fail "W7 headless smoke exited with $w7code (0 = ran and judged PASS)"
}
if (-not (Test-Path $w7result)) {
    Get-Content $w7out -Encoding UTF8 | Write-Host
    Fail "W7 headless smoke wrote no result file"
}
# WARNING: -Encoding UTF8, not the default.
#   PowerShell 5.1 reads a file without -Encoding as ANSI (GBK here).
#   The result JSON is UTF-8 with Chinese text in it, so the GBK read
#   turns it into mojibake -- and mojibake can break the JSON itself
#   (a mangled byte pair may produce a stray quote), which makes
#   ConvertFrom-Json fail with "invalid object".
#   The symptom looks like "the pipeline wrote a broken file", but the
#   file is fine; the reader is wrong.
$w7json = Get-Content $w7result -Raw -Encoding UTF8 | ConvertFrom-Json
if ($w7json.final_result -ne 'PASS') {
    Get-Content $w7out -Encoding UTF8 | Write-Host
    Fail ("W7 headless smoke: final_result is '$($w7json.final_result)', expected PASS." +
          "`n   An exit code of 0 is NOT enough -- the pipeline can 'succeed'" +
          "`n   while judging nothing at all.")
}
if ($w7json.errors -ne 0) {
    Get-Content $w7out -Encoding UTF8 | Write-Host
    Fail "W7 headless smoke: $($w7json.errors) step(s) reported an error"
}
# The judge node's uuid in W7_VisionMasterLite/pipelines/smoke.json.
#
# It MUST be a real UUID, not a made-up name. The parser tells "new
# format" from "old format" by QUuid::fromString(): a key like
# "smoke-judge" is not a UUID, so the edge is treated as old format
# (both ends hold an operator name), the lookup fails, the line is
# dropped -- and the pipeline then judges ERROR with no obvious
# reason. The smoke step below caught exactly that once.
$w7judgeUuid = '22222222-2222-2222-2222-222222222222'
$w7judgeProp = $w7json.outputs.PSObject.Properties |
    Where-Object { $_.Name -eq $w7judgeUuid }
if (-not $w7judgeProp) {
    Get-Content $w7out -Encoding UTF8 | Write-Host
    Fail ("W7 headless smoke: no output for the judge node ($w7judgeUuid)." +
          "`n   outputs keys: " + (($w7json.outputs.PSObject.Properties.Name) -join ', '))
}
$w7comps = $w7judgeProp.Value.component_count
if ($w7comps -ne 6) {
    Get-Content $w7out -Encoding UTF8 | Write-Host
    Fail ("W7 headless smoke: the judge saw $w7comps components, expected 6." +
          "`n   The verdict may still be PASS, but it was decided on the wrong data.")
}
Write-Host "   W7 ran a whole pipeline headless and judged it PASS: OK"

# ---- the exit code contract: all four codes must come out right ----
#
# The smoke check above only exercises exit code 0. The other three
# are the ones CI actually leans on to tell "the tool is broken"
# from "the pipeline is broken":
#
#   1 = ran, judged FAIL       (the pipeline says no)
#   2 = could not even start   (usage / file / JSON / cycle)
#   3 = ran, but a step erred  (the tool works, the pipeline does not)
#
# A pipeline that judges FAIL but exits 0 is the classic silent
# failure: it looks like it passed. That is exactly what the exit
# code split exists to prevent, so it gets checked here.
function Run-W7([string[]]$argl) {
    $o = Join-Path $BuildRoot 'w7_headless_code_out.txt'
    $p = Start-Process -FilePath $w7exe -ArgumentList $argl `
            -WorkingDirectory $w7bin -Wait -PassThru -NoNewWindow `
            -RedirectStandardOutput $o -RedirectStandardError "$o.err"
    return $p.ExitCode
}

# A board WITH a defect -> the judge says FAIL -> exit 1 (not 0).
$w7bad = Join-Path $RepoRoot 'W6_PCB_DefectProject/images/missing.png'
if (-not (Test-Path $w7bad)) { Fail "W7 smoke defect image not found: $w7bad" }
$codeFail = Run-W7 @('--run', $w7pipe, '--image', $w7bad,
                      '--out', (Join-Path $BuildRoot 'w7_fail_result.json'))
if ($codeFail -ne 1) {
    Fail ("W7 headless exit code contract: a FAIL pipeline exited with $codeFail, expected 1." +
          "`n   A pipeline that judges FAIL but exits 0 is the worst kind of silent failure.")
}
Write-Host "   exit 1 (judged FAIL)             : OK"

# An image that does not exist -> the operator reports an error -> exit 3.
$w7noimg = Join-Path $RepoRoot 'W6_PCB_DefectProject/images/no_such_image_for_ci.png'
$codeErr = Run-W7 @('--run', $w7pipe, '--image', $w7noimg,
                     '--out', (Join-Path $BuildRoot 'w7_err_result.json'))
if ($codeErr -ne 3) {
    Fail ("W7 headless exit code contract: a step error exited with $codeErr, expected 3." +
          "`n   3 means 'the tool works, the pipeline is broken' -- CI must be able to tell.")
}
Write-Host "   exit 3 (a step reported an error) : OK"

# A pipeline file that does not exist -> we never got started -> exit 2.
$codeMissing = Run-W7 @('--run', (Join-Path $BuildRoot 'w7_no_such_pipeline.json'))
if ($codeMissing -ne 2) {
    Fail ("W7 headless exit code contract: a missing pipeline file exited with $codeMissing, expected 2." +
          "`n   2 means 'we never ran' -- that is a broken tool, not a broken pipeline.")
}
Write-Host "   exit 2 (could not even start)     : OK"

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
Write-Host "   tested  : $SuiteCount ctest suites (W1 + W2 + W6 + W7)"
Write-Host "   asserts : $AssertionTotal assertions (counted from this build)"
Write-Host "   docs    : every number quoted in the docs matches the build"
Write-Host "   links   : every relative link in the docs resolves"
Write-Host "   smoke   : W5 qr --no-gui + W7 headless pipeline"
Write-Host "   clean   : tests added nothing to the source tree"
Write-Host ""
Write-Host "ALL CHECKS PASSED" -ForegroundColor Green
exit 0
