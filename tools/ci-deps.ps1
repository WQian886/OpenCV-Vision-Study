<#
.SYNOPSIS
    Provision the three CI dependencies (Tesseract / Qt / OpenCV) and
    report where they landed.

.DESCRIPTION
    This file is the ONLY copy of the provisioning commands.
    .github/workflows/ci.yml calls it, and so does the local rehearsal
    (same file, -VerifyOnly).  That is the whole point: the thing we
    verify locally is literally the thing that runs on the runner.

    A CI whose provisioning lives only inside YAML is a CI whose first
    real execution is also its first debugging session.  Same argument
    as tools/ci.ps1 -- and the same reason this file exists.

    Three rules this file follows:

      1. FIND, DON'T GUESS.
         The official OpenCV Windows package is a *world* build:
         it ships opencv_world4130.lib / opencv_world4130.dll and no
         opencv_core4130.* at all.  A lookup that names a specific
         module therefore finds nothing, and the job dies with
         "no opencv_core*.dll found".  The discovery below never names
         a module: it takes whatever import lib is there and derives
         the DLL name from it.  That one rule covers both shapes --
         the official world package and a local modular build.

      2. ASSERT THE THING YOU WILL USE.
         "A config file exists somewhere" is not enough.  We assert
         the import lib and the DLL are really at the paths we report.
         A wrong path fails much later and much louder.

      3. FAIL LOUDLY AND SPECIFICALLY.
         A missing dependency must say which one, where we looked and
         what we expected.  Never "skip" and carry on: a CI that
         quietly skips the OCR path is worse than a CI that fails.

    Output contract:
      * writes KEY=VALUE lines to stdout
      * appends the same lines to $env:GITHUB_ENV when running on a
        runner (no BOM -- a BOM would corrupt the first line)

.NOTES
    Must stay pure ASCII: Windows PowerShell 5.1 reads a BOM-less .ps1
    as ANSI, and any non-ASCII byte turns into mojibake or a syntax
    error.  Chinese explanations live in notes/notes_CI_2026-10.md.
#>
[CmdletBinding()]
param(
    # Where to download / unpack / install.  Defaults to the runner temp.
    [string]$WorkDir = '',

    # Do not download or install anything.  Only run the discovery and
    # the assertions against whatever is already on this machine.
    [switch]$VerifyOnly,

    # A local OpenCV package (.exe) to verify against, instead of
    # downloading one.  This is the rehearsal path.
    [string]$OpenCVPackage = '',

    # An already-unpacked OpenCV tree to verify against (no download,
    # no extract).  Used to rehearse the discovery against a *different*
    # layout -- e.g. a local modular build instead of the official
    # world package.
    [string]$OpenCVRoot = '',

    [switch]$SkipTesseract,
    [switch]$SkipQt,
    [switch]$SkipOpenCV
)

$ErrorActionPreference = 'Stop'

# The Qt version this project is developed against.  A different minor
# version can change behaviour, and a CI that tests a different Qt
# than the one we ship is a lie.
$QtVersion = '6.8.3'

# The architecture string aqtinstall actually understands.
#
# It is NOT 'msvc2022_64'.  aqt has no such alias: the real name is
# 'win64_msvc2022_64' (verified with `aqt list-qt windows desktop
# --arch 6.8.3` and with `aqt install-qt --dry-run`).
# With the short form aqt fails with:
#     ERROR: The packages ['qt_base'] were not found
# while parsing XML of package information!
# and the job dies before it downloads anything.
$QtArch = 'win64_msvc2022_64'

$OpenCvUrl = 'https://github.com/opencv/opencv/releases/download/4.13.0/opencv-4.13.0-windows.exe'

if (-not $WorkDir) {
    if ($env:RUNNER_TEMP) {
        $WorkDir = $env:RUNNER_TEMP
    } else {
        $WorkDir = Join-Path $env:TEMP 'w7ci-deps'
    }
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

function Say([string]$msg) {
    Write-Host "[ci-deps] $msg"
}

# Report a value to stdout and to the runner's environment.
function Set-CiEnv([string]$name, [string]$value) {
    Say "$name=$value"
    if ($env:GITHUB_ENV) {
        # AppendAllText, not Out-File: Out-File -Encoding utf8 writes a
        # BOM, and a BOM in the middle of GITHUB_ENV corrupts a line.
        [System.IO.File]::AppendAllText($env:GITHUB_ENV, "$name=$value`n")
    }
}

# ================================================================
# Tesseract
# ================================================================
#
# The shared QR/OCR core calls tesseract as an external program.
# Without it the OCR half of common/qr_ocr's suite is SKIPPED (and
# says so) -- a CI where the OCR path is quietly never exercised is
# worse than a CI that fails, so the install is mandatory.

function Find-Tesseract {
    $candidates = @()
    if ($env:ProgramFiles) {
        $candidates += (Join-Path $env:ProgramFiles 'Tesseract-OCR\tesseract.exe')
    }
    # The chocolatey package installs to %ProgramFiles%\Tesseract-OCR
    # (verified by reading that package's chocolateyinstall.ps1).
    $candidates += 'C:\Program Files\Tesseract-OCR\tesseract.exe'
    # This machine has it on D:.  A path of one machine is a fact of
    # that machine -- it is checked, not assumed.
    $candidates += 'D:\Program Files\Tesseract-OCR\tesseract.exe'
    foreach ($d in ($env:PATH -split ';')) {
        if ($d) { $candidates += (Join-Path $d 'tesseract.exe') }
    }
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    return ''
}

function Install-Tesseract {
    $found = Find-Tesseract
    if ($found) {
        Say "tesseract already present: $found"
        Set-CiEnv 'TESSERACT_EXE' $found
        return $found
    }
    if ($VerifyOnly) {
        throw ("tesseract not found (looked in %ProgramFiles%\Tesseract-OCR, " +
               "C:\Program Files\Tesseract-OCR, D:\Program Files\Tesseract-OCR and PATH); " +
               "and -VerifyOnly forbids installing it")
    }

    Say 'installing tesseract via choco ...'
    & choco install tesseract -y --no-progress
    if ($LASTEXITCODE -ne 0) { throw "choco install tesseract failed with $LASTEXITCODE" }

    # choco puts the directory on the *machine* PATH.  This process does
    # not see that yet, so look at the standard location explicitly.
    $found = Find-Tesseract
    if (-not $found) {
        throw ("tesseract.exe still not found after `choco install tesseract` " +
               "(looked in %ProgramFiles%\Tesseract-OCR, " +
               "C:\Program Files\Tesseract-OCR, D:\Program Files\Tesseract-OCR and PATH)")
    }
    Say "tesseract installed: $found"
    Set-CiEnv 'TESSERACT_EXE' $found
    return $found
}

# ================================================================
# Qt
# ================================================================

function Install-Qt {
    $qtRoot = Join-Path $WorkDir 'Qt'
    # aqt lays it out as <version>/<arch>, where <arch> is the short
    # form (msvc2022_64) even though the argument was win64_msvc2022_64.
    $qtDir = Join-Path $qtRoot ("$QtVersion\msvc2022_64")

    if (-not (Test-Path (Join-Path $qtDir 'lib\cmake\Qt6'))) {
        Say "Qt is not installed under $qtDir -- asking aqt what it would do ..."
        # The dry run is cheap and turns a 2 GB download into an
        # instant, precise error when the package name is wrong.
        & python -m aqt install-qt windows desktop $QtVersion $QtArch `
                 --outputdir $qtRoot --dry-run
        if ($LASTEXITCODE -ne 0) {
            throw ("aqt cannot resolve `windows desktop $QtVersion $QtArch` " +
                   "(exit $LASTEXITCODE). Run `aqt list-qt windows desktop --arch $QtVersion` " +
                   "to see the real architecture names.")
        }
        if ($VerifyOnly) {
            Say "Qt package name verified (dry run only; nothing installed)"
            Set-CiEnv 'QT_ROOT' $qtDir
            return $qtDir
        }

        Say "installing Qt $QtVersion ($QtArch) -- this pulls a few GB ..."
        & python -m pip install --upgrade pip
        & python -m aqt install-qt windows desktop $QtVersion $QtArch --outputdir $qtRoot
        if ($LASTEXITCODE -ne 0) { throw "aqt install-qt failed with $LASTEXITCODE" }
    }

    if (-not (Test-Path (Join-Path $qtDir 'lib\cmake\Qt6'))) {
        throw "Qt6 cmake package not found under $qtDir (after a successful install)"
    }
    Say "Qt ready: $qtDir"
    Set-CiEnv 'QT_ROOT' $qtDir
    return $qtDir
}

# ================================================================
# OpenCV
# ================================================================

# The Windows package is a self-extracting 7z archive.
# 7-Zip unpacks it; so does bsdtar (libarchive scans for the archive
# signature).  Try both, so neither tool being absent is fatal.
function Expand-Package([string]$archive, [string]$dest) {
    $sevenZip = $null
    foreach ($c in @('C:\Program Files\7-Zip\7z.exe', 'C:\Program Files (x86)\7-Zip\7z.exe')) {
        if (Test-Path $c) { $sevenZip = $c; break }
    }
    if (-not $sevenZip) {
        $cmd = Get-Command 7z.exe -ErrorAction SilentlyContinue
        if ($cmd) { $sevenZip = $cmd.Source }
    }
    if ($sevenZip) {
        Say "extracting with 7-Zip: $sevenZip"
        & $sevenZip x $archive "-o$dest" -y | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip failed to extract $archive (exit $LASTEXITCODE)" }
        return
    }

    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if (-not $tar) {
        throw ("neither 7z.exe nor tar.exe is available to extract $archive -- " +
               "cannot unpack the OpenCV package")
    }
    Say "extracting with bsdtar: $($tar.Source)"
    & $tar.Source -xf $archive -C $dest
    if ($LASTEXITCODE -ne 0) { throw "tar failed to extract $archive (exit $LASTEXITCODE)" }
}

# The layout discovery.  Never names a specific OpenCV module.
function Find-OpenCVLayout {
    param([string]$Root)

    # 1. The import libs.  A world build and a modular build both match
    #    'opencv_*.lib'; the java bindings are not import libs for us.
    $libs = Get-ChildItem -Path $Root -Recurse -File -Filter 'opencv_*.lib' |
            Where-Object { $_.FullName -notmatch '\\java\\' } |
            Sort-Object -Property FullName
    if (-not $libs) {
        throw "no opencv_*.lib under $Root -- is this really an OpenCV package?"
    }
    $lib = $libs[0]
    $libDir = $lib.DirectoryName

    # 2. The config that belongs to those libs: the one sitting next to
    #    them, otherwise the closest one above.
    #    (The official package also has a top-level wrapper config that
    #     computes x64/vc16/lib itself; either one works, but the one
    #     next to the libs is unambiguous.)
    $cfgPath = Join-Path $libDir 'OpenCVConfig.cmake'
    if (-not (Test-Path $cfgPath)) {
        $cfg = Get-ChildItem -Path $libDir -Recurse -File -Filter 'OpenCVConfig.cmake' |
               Sort-Object -Property { $_.FullName.Length } |
               Select-Object -First 1
        if (-not $cfg) { throw "no OpenCVConfig.cmake near $libDir" }
        $cfgPath = $cfg.FullName
    }

    # 3. The DLL with the SAME base name as the import lib.
    #    Deriving it from the lib is what makes this work for a world
    #    build and a modular one with one rule.
    $dllName = [System.IO.Path]::GetFileNameWithoutExtension($lib.Name) + '.dll'
    $dll = Get-ChildItem -Path $Root -Recurse -File -Filter $dllName |
           Where-Object { $_.FullName -notmatch '\\java\\' } |
           Select-Object -First 1
    if (-not $dll) {
        throw ("no $dllName under $Root (the import lib found there is $($lib.Name)) " +
               "-- the package is incomplete")
    }

    return [pscustomobject]@{
        ConfigDir = (Split-Path -Parent $cfgPath)
        LibDir    = $libDir
        BinDir    = $dll.DirectoryName
        ImportLib = $lib.Name
        Dll       = $dll.Name
    }
}

function Assert-OpenCVLayout($layout) {
    # ASSERT what we are about to hand to cmake.  A path we report but
    # that does not hold the file is a lie the build will find out
    # about much later, with a much worse message.
    if (-not (Test-Path (Join-Path $layout.ConfigDir 'OpenCVConfig.cmake'))) {
        throw "OpenCVConfig.cmake is not in $($layout.ConfigDir) (as reported)"
    }
    if (-not (Test-Path (Join-Path $layout.LibDir $layout.ImportLib))) {
        throw "$($layout.ImportLib) is not in $($layout.LibDir) (as reported)"
    }
    if (-not (Test-Path (Join-Path $layout.BinDir $layout.Dll))) {
        throw "$($layout.Dll) is not in $($layout.BinDir) (as reported)"
    }
}

function Install-OpenCV {
    if ($OpenCVRoot) {
        # An already-unpacked tree: discovery only, nothing to fetch.
        Say "verifying an unpacked OpenCV tree: $OpenCVRoot"
        $layout = Find-OpenCVLayout -Root $OpenCVRoot
        Assert-OpenCVLayout $layout
        Say "opencv config: $($layout.ConfigDir)"
        Say "opencv libs  : $($layout.LibDir)  [$($layout.ImportLib)]"
        Say "opencv dlls  : $($layout.BinDir)  [$($layout.Dll)]"
        Set-CiEnv 'OPENCV_LIB' $layout.ConfigDir
        Set-CiEnv 'OPENCV_BIN' $layout.BinDir
        return $layout
    }

    $exe = $OpenCVPackage
    if (-not $exe) {
        $exe = Join-Path $WorkDir 'opencv-4.13.0-windows.exe'
    }
    if (-not (Test-Path $exe)) {
        if ($VerifyOnly) {
            throw ("no OpenCV package at $exe and -VerifyOnly forbids downloading it")
        }
        Say "downloading $OpenCvUrl ..."
        Invoke-WebRequest -Uri $OpenCvUrl -OutFile $exe
    }

    # The archive carries its own top-level "opencv/" directory, so we
    # unpack straight into the work dir and search that -- otherwise the
    # paths come out as opencv/opencv/build/... .
    $pkg = $WorkDir
    if (-not (Get-ChildItem -Path $pkg -Recurse -File -Filter 'OpenCVConfig.cmake' |
              Select-Object -First 1)) {
        Expand-Package $exe $pkg
    }

    $layout = Find-OpenCVLayout -Root $pkg
    Assert-OpenCVLayout $layout
    Say "opencv config: $($layout.ConfigDir)"
    Say "opencv libs  : $($layout.LibDir)  [$($layout.ImportLib)]"
    Say "opencv dlls  : $($layout.BinDir)  [$($layout.Dll)]"
    Set-CiEnv 'OPENCV_LIB' $layout.ConfigDir
    Set-CiEnv 'OPENCV_BIN' $layout.BinDir
    return $layout
}

# ================================================================
# main
# ================================================================

Say "work dir: $WorkDir"
if ($VerifyOnly) { Say 'mode: VERIFY ONLY (nothing will be downloaded or installed)' }

if (-not $SkipTesseract) { Install-Tesseract | Out-Null }
if (-not $SkipQt)         { Install-Qt         | Out-Null }
if (-not $SkipOpenCV)     { Install-OpenCV     | Out-Null }

Say 'all dependencies ready'
