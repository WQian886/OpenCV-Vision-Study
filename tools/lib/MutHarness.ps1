#requires -Version 5.1
<#
    Shared machinery for the mutation harnesses.

    A mutation harness answers ONE question: "has this gate ever been
    seen to go RED?"  A gate that has only ever been green is a gate
    nobody knows works -- the checker could be broken in a way that
    makes it say OK about anything at all, and the CI log would look
    exactly the same.

    So the harness breaks the tree on purpose, on a COPY, and demands
    that the gate notices.  Then it breaks the RULER -- the thing that
    decides red -- and demands that the same tree comes out GREEN.  That
    second half is the one that carries the proof: it shows the redness
    came from the gate and not from a coincidence.

    This file holds the parts that every harness needs.  It is dot-
    sourced, so its functions land in the caller's scope.

    Pure ASCII, no BOM: PowerShell 5.1 reads a .ps1 without a BOM as
    ANSI, and a non-ASCII byte in here turns into mojibake (or worse, a
    parse error).  The Chinese a harness needs is built from code
    points -- see Ch() below.

    The four rules this machinery learned the hard way (the first two
    runs on a real runner failed, and BOTH times the reason was in the
    harness, not in the gate):

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
         can be started by a launcher, and then the path is the
         LAUNCHER -- which cannot run a .ps1 at all.  Measured: the
         harness ran "dotnet.exe -NoProfile -File checker.ps1", every
         mutation came out RED with zero reds, and the harness said
         nothing about it.

      3. A MUTATION THAT DID NOT HAPPEN MUST BE LOUD.
         Every replacement asserts that it replaced EXACTLY the number
         of things it was supposed to replace.  A mutation that silently
         did nothing is a green light for a broken gate.

      4. A PATH IS NOT A STRING.
         Relative paths come from the provider's own answer (Get-Item),
         never from slicing a length off a handed-in string.  On a
         Windows runner $env:TEMP is the 8.3 short form
         ("C:\Users\RUNNER~1\...") and the provider expands it
         ("C:\Users\runneradmin\...") -- two strings, one directory --
         and a relative path sliced out of the wrong one starts three
         characters into the middle of a name.
#>

# The Chinese a harness needs, built from code points.  A [char] built
# from a code point is exact; a Chinese literal in a no-BOM .ps1 is
# read as ANSI and becomes a different character -- and a pattern that
# matches nothing is indistinguishable from a pass.
function Ch([int]$cp) { return [string][char]$cp }

# UTF-8 WITHOUT a BOM: the files in this repository are UTF-8 without a
# BOM, and a BOM in the middle of a copy would change what the copy
# measures.
$MutUtf8 = New-Object System.Text.UTF8Encoding($false)

function Read-MutText([string]$p) {
    return [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8)
}

function Write-MutText([string]$p, [string]$t) {
    [System.IO.File]::WriteAllText($p, $t, $MutUtf8)
}

# ------------------------------------------------------------------
# 1. The host that runs the checker
# ------------------------------------------------------------------
#
# PSHOME is the directory the engine lives in, and it holds the host
# executable.  This is the one place the harness can be sure of: the
# process path can be a launcher (rule 2 above).
function Get-MutHost {
    foreach ($n in @('pwsh.exe', 'powershell.exe')) {
        $p = Join-Path $PSHOME $n
        if (Test-Path -LiteralPath $p) { return $p }
    }
    throw ("no PowerShell host found under PSHOME (" + $PSHOME + ")." +
           " The harness runs the checker in a child process, and without a host it measures nothing.")
}

# ------------------------------------------------------------------
# 2. Paths
# ------------------------------------------------------------------
#
# The relative path of a file under the tree.  NOT by slicing a length
# off an absolute path: the string we were handed and the enumerated
# paths can be two different strings for the same directory (rule 4).
function Get-MutRel([string]$base, [string]$full) {
    if (-not $full.StartsWith($base + [char]'\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ("'" + $full + "' is not under the tree '" + $base + "'")
    }
    return $full.Substring($base.Length + 1)
}

# ------------------------------------------------------------------
# 3. The files that make the tree
# ------------------------------------------------------------------
#
# Two different sets, because two different gates measure two different
# things:
#
#   Get-MutDocFiles     every markdown file (build/ and .git/ excluded
#                       -- build products are not documentation) plus
#                       every script under tools/ (the checkers
#                       dot-source their own library).  This is what the
#                       LINK gate and the SECTION gate read.
#
#   Get-MutTrackedFiles every file git knows about.  This is what the
#                       NUMBER gate measures, because it measures the
#                       tree with `git ls-files` -- so the copy has to
#                       be a git repository with the same index, or the
#                       copy measures a different tree.
function Get-MutDocFiles([string]$root) {
    $base = (Get-Item -LiteralPath $root -ErrorAction Stop).FullName.TrimEnd([char]'\', [char]'/')
    $out = New-Object System.Collections.ArrayList
    foreach ($f in (Get-ChildItem -Path $root -Recurse -File -Filter *.md)) {
        $rel = Get-MutRel $base $f.FullName
        if ($rel -match '(^|[\\/])(build|\.git)([\\/]|$)') { continue }
        [void]$out.Add([pscustomobject]@{ Rel = $rel; Full = $f.FullName })
    }
    $tools = Join-Path $root 'tools'
    if (Test-Path -LiteralPath $tools) {
        foreach ($f in (Get-ChildItem -Path $tools -Recurse -File -Filter *.ps1)) {
            $rel = Get-MutRel $base $f.FullName
            if ($rel -match '(^|[\\/])(build|\.git)([\\/]|$)') { continue }
            [void]$out.Add([pscustomobject]@{ Rel = $rel; Full = $f.FullName })
        }
    }
    return ,@($out)
}

function Get-MutTrackedFiles([string]$root, [string]$gitExe) {
    $out = New-Object System.Collections.ArrayList
    Push-Location $root
    try {
        $names = @(& $gitExe ls-files)
    }
    finally { Pop-Location }
    foreach ($n in $names) {
        if ([string]::IsNullOrWhiteSpace($n)) { continue }
        $rel = $n.Trim().Replace('/', '\')
        $full = Join-Path $root $rel
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            throw ("tracked file '" + $n + "' is not on disk -- the tree is not what git thinks it is")
        }
        [void]$out.Add([pscustomobject]@{ Rel = $rel; Full = $full })
    }
    return ,@($out)
}

# ------------------------------------------------------------------
# 4. The copy
# ------------------------------------------------------------------
#
# The copy is made with the .NET file API, not with Copy-Item: the
# provider parses paths, and a path is not a wildcard, a list, or a
# pattern.  The copy is a copy.
function Copy-MutTree([string]$root, [string]$dest, $files) {
    if (Test-Path -LiteralPath $dest) { Remove-Item -Recurse -Force $dest }
    [void][System.IO.Directory]::CreateDirectory($dest)
    foreach ($e in $files) {
        $target = Join-Path $dest $e.Rel
        $parent = [System.IO.Path]::GetDirectoryName($target)
        [void][System.IO.Directory]::CreateDirectory($parent)
        [System.IO.File]::Copy($e.Full, $target, $true)
    }
}

# A copy that is ALSO a git repository, with the same index.
#
# The number gate measures the C++ in the tree with `git ls-files`, so a
# copy that is not a git repository measures NOTHING there and reports
# a clean sheet about zero files.  The index is built with `git add -f`
# so that it holds exactly the files that were copied -- the copy is
# the tracked set, and .gitignore must not quietly drop one of them.
function New-MutGitCopy([string]$root, [string]$dest, [string]$gitExe) {
    $files = Get-MutTrackedFiles $root $gitExe
    Copy-MutTree $root $dest $files

    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        Push-Location $dest
        try {
            # `git add` has NO --quiet option (git 2.50.1 says
            # "unknown option `quiet'"), so its output is thrown away
            # instead.  A failed `git add` leaves an index that holds
            # almost nothing -- and the check below is what turns that
            # into a loud failure instead of a harness that measures
            # a tree of one file and reports a clean sheet.
            $null = & $gitExe init --quiet 2>$null
            $null = & $gitExe add -A -f 2>$null
        }
        finally { Pop-Location }
    }
    finally { $ErrorActionPreference = $prev }

    # NOTE: no @() around the call.  Get-MutTrackedFiles returns the
    # array wrapped in one more layer (so that an EMPTY result stays an
    # array), and @() around that would count the WRAPPER -- one
    # element, not the files.  Measured: the check below reported
    # "the copy's git index holds 1 files" for a copy of 353.
    $after = Get-MutTrackedFiles $dest $gitExe
    if ($after.Count -ne $files.Count) {
        throw ("the copy's git index holds " + $after.Count + " files, the tree holds " + $files.Count +
               " -- the copy is not the tree")
    }
    return ,@($files)
}

# ------------------------------------------------------------------
# 5. The mutations
# ------------------------------------------------------------------
#
# Every replacement asserts that it replaced EXACTLY the number of
# things it was supposed to replace.  A mutation that did not happen
# must be loud (rule 3 above).
function Replace-MutOnce([string]$path, [string]$old, [string]$new, [string]$what) {
    $t = Read-MutText $path
    $n = ([regex]::Matches($t, [regex]::Escape($old))).Count
    if ($n -ne 1) {
        throw ("mutation " + $what + ": expected exactly one '" + $old + "' in " + $path + ", found " + $n)
    }
    Write-MutText $path ($t.Replace($old, $new))
}

function Replace-MutCount([string]$path, [string]$old, [string]$new, [string]$what, [int]$want) {
    $t = Read-MutText $path
    $n = ([regex]::Matches($t, [regex]::Escape($old))).Count
    if ($n -ne $want) {
        throw ("mutation " + $what + ": expected exactly " + $want + " '" + $old + "' in " + $path + ", found " + $n)
    }
    Write-MutText $path ($t.Replace($old, $new))
}

# A replacement of several lines.  The lines are joined with the
# newline the file already uses, so the block that is replaced is
# byte-for-byte the block that is on disk -- and the rest of the file
# keeps its own line endings.
function Replace-MutBlock([string]$path, [string[]]$oldLines, [string[]]$newLines, [string]$what) {
    $t = Read-MutText $path
    $nl = "`n"
    if ($t.Contains("`r`n")) { $nl = "`r`n" }
    $old = $oldLines -join $nl
    $new = $newLines -join $nl
    $n = ([regex]::Matches($t, [regex]::Escape($old))).Count
    if ($n -ne 1) {
        throw ("mutation " + $what + ": expected exactly one block in " + $path + ", found " + $n)
    }
    Write-MutText $path ($t.Replace($old, $new))
}

# ------------------------------------------------------------------
# 6. Running the checker
# ------------------------------------------------------------------
#
# The checker runs in a CHILD process, so a checker that dies takes the
# harness with it otherwise.  The harness reads the child's verdict
# from three things: the exit code, the number of "FAIL:" lines, and
# the summary line -- and the summary line is what the fidelity gate
# compares, because it is the checker's own sentence about the tree.
function Invoke-MutChecker([string]$exe, [string]$checker, [string]$root, [string[]]$extra, [string]$summaryPattern) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $checker, '-RepoRoot', $root)
    if ($null -ne $extra -and $extra.Count -gt 0) { $a += $extra }
    try {
        $out = & $exe @a 2>&1
    }
    finally { $ErrorActionPreference = $prev }
    $text = ($out | Out-String)
    $m = [regex]::Match($text, $summaryPattern)
    return [pscustomobject]@{
        Text    = $text
        Exit    = $LASTEXITCODE
        Reds    = ([regex]::Matches($text, '(?m)^\s+FAIL: ')).Count
        Ok      = $m.Success
        Summary = $(if ($m.Success) { $m.Value.Trim() } else { '' })
    }
}
