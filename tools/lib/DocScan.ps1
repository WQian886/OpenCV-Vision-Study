<#
.SYNOPSIS
    Shared markdown-scanning helpers for the checkers in tools/.

.DESCRIPTION
    The checkers need the same three things over and over:

      1. which markdown files are in scope;
      2. which lines of a file are prose and which are fenced code;
      3. how to read a file as UTF-8 (not as ANSI).

    Each of them used to carry its own copy. A copy is a SECOND thing
    that can drift -- the same disease the per-suite table had
    (notes/atomic/at-the-table-nobody-checks-drifts.md), one object
    over. So the copies live here, once, and the checkers dot-source
    this file.

    Two traps this file exists to make impossible to re-introduce:

      * Reading a file with Get-Content (no -Encoding) reads ANSI/GBK
        under PowerShell 5.1, which mangles the Chinese text and can
        swallow line endings (see
        notes/atomic/at-the-decoder-eats-the-newline.md). Everything
        here goes through [System.IO.File] with an explicit UTF-8.

      * A fence marker closes the fence it opened: ``` opens and closes
        ```, ~~~ opens and closes ~~~, and a ``` inside a ~~~ block does
        NOT close it. Getting that wrong makes a checker read code as
        prose, which is how a checker starts reporting nonsense.

    NOTE: this file is deliberately pure ASCII, no BOM.
    Windows PowerShell 5.1 parses a .ps1 without a BOM as ANSI, so any
    non-ASCII byte in here turns into mojibake (or worse, a parse error).

.NOTES
    This file has NO param block on purpose. A dot-sourced script runs
    in the CALLER's scope, so a `param([string]$RepoRoot = "")` here
    would reset the caller's $RepoRoot to the empty string the moment
    the caller dot-sourced it. Everything takes its root as an argument
    instead.
#>

# ------------------------------------------------------------------
# Which markdown files are in scope
# ------------------------------------------------------------------
#
# The WORKING TREE, not the git index: a claim that is broken right now
# is broken right now, whether or not the file is staged. The index
# would hide an untracked note -- and an untracked note is exactly the
# one a new link is most likely to point at.
#
# build/ and .git/ are excluded: build products are not documentation,
# and .git holds objects, not notes.
function Get-DocFiles([string]$RepoRoot) {
    $mdFiles = @()
    foreach ($f in (Get-ChildItem -Path $RepoRoot -Recurse -File -Filter *.md)) {
        $rel = $f.FullName.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/')
        if ($rel -match '(^|[\\/])(build|\.git)([\\/]|$)') { continue }
        $mdFiles += $f
    }
    return ,@($mdFiles | Sort-Object FullName)
}

# ------------------------------------------------------------------
# Read a file as UTF-8
# ------------------------------------------------------------------
function Get-DocLinesUtf8([string]$Path) {
    if (-not (Test-Path $Path)) { return ,@() }
    return ,([System.IO.File]::ReadAllLines($Path, [System.Text.Encoding]::UTF8))
}

# ------------------------------------------------------------------
# Which lines are FENCED CODE
# ------------------------------------------------------------------
#
# The result is one flag per line: $true means "this line is inside a
# fenced code block" (the fence marker line itself counts as inside).
#
# The fence marker is remembered, so a ``` block is closed by ``` and a
# ~~~ block by ~~~. A ``` inside a ~~~ block does not close it. Getting
# that wrong makes a checker read code as prose, which is how a checker
# starts reporting nonsense.
#
# It lives here so that the prose scan and the quote scan cannot
# disagree about where the code is.
function Get-FenceMap([string[]]$Lines) {
    $map = New-Object 'bool[]' $Lines.Count
    $inFence = $false
    $fenceChar = ''
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $m = [regex]::Match($Lines[$i], '^\s*(`{3,}|~{3,})')
        if ($m.Success) {
            $c = $m.Groups[1].Value.Substring(0, 1)
            if (-not $inFence) {
                $inFence = $true
                $fenceChar = $c
            }
            elseif ($c -eq $fenceChar) {
                $inFence = $false
            }
            $map[$i] = $true
            continue
        }
        $map[$i] = $inFence
    }
    return ,$map
}

# ------------------------------------------------------------------
# The prose lines of a file, with their ORIGINAL line numbers
# ------------------------------------------------------------------
#
# Fenced code blocks are skipped: a C++ line such as
# "operator[](qsizetype i)" is code, and a markdown renderer shows it
# literally. A checker that reads code as prose starts reporting
# nonsense.
#
# The fence marker is remembered, so a ``` block is closed by ``` and a
# ~~~ block by ~~~. A ``` inside a ~~~ block does not close it.
#
# The original line number is kept because "notes/x.md:412" is what a
# human needs in order to go and look at the line; a number into the
# PROSE-only array is not.
function Get-ScannedLines([string[]]$Lines) {
    $out = New-Object System.Collections.ArrayList
    $map = Get-FenceMap $Lines
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($map[$i]) { continue }
        [void]$out.Add([pscustomobject]@{ Text = $Lines[$i]; Index = $i })
    }
    return ,$out.ToArray()
}

# ------------------------------------------------------------------
# The prose of a file as ONE string
# ------------------------------------------------------------------
#
# Used by the link checker, which searches the whole body at once
# (a markdown link can span... well, it cannot, but the body is one
# string in its pattern and the line number is not what it reports).
function Get-ScannedBody([string[]]$Lines) {
    $parts = @()
    foreach ($s in (Get-ScannedLines $Lines)) { $parts += $s.Text }
    return ($parts -join "`n")
}

# ------------------------------------------------------------------
# The ranges of INLINE CODE spans (`` `like this` ``) in a text
# ------------------------------------------------------------------
#
# Why this is needed: a markdown link and a C++ call look the same to a
# regex. "operator[](qsizetype i)" is a function call, not a link --
# and inside backticks it is CODE, which a markdown renderer shows
# literally and never turns into a link.
#
# The ranges are returned so a link can be rejected if it STARTS inside
# one. Note that this does NOT break a real link whose TEXT is
# backticked -- "[`notes_x.md`](./notes_x.md)" -- because there the
# code span is inside the brackets, and the link starts outside it.
#
# It lives in this library because TWO checkers need it, and two copies
# of the same rule are two things that drift apart.
function Get-InlineCodeRanges([string]$text) {
    $ranges = New-Object System.Collections.ArrayList
    $ticks = [regex]::Matches($text, '`+')
    $i = 0
    while ($i -lt $ticks.Count) {
        $len = $ticks[$i].Length
        $j = $i + 1
        $close = -1
        while ($j -lt $ticks.Count) {
            if ($ticks[$j].Length -eq $len) { $close = $j; break }
            $j++
        }
        if ($close -lt 0) { break }
        [void]$ranges.Add(@($ticks[$i].Index, $ticks[$close].Index + $len))
        $i = $close + 1
    }
    return ,$ranges.ToArray()
}

# ------------------------------------------------------------------
# Is a column inside one of these ranges?
# ------------------------------------------------------------------
#
# The ranges come from Get-InlineCodeRanges. Two callers need the
# same test, and a rule that exists in two places is a rule that will
# drift: the section checker asks "is this pointer inside code?", and
# the quote scan asks "is this corner quote inside code?".
function Test-ColInRanges([int]$col, $ranges) {
    foreach ($r in $ranges) {
        if ($col -ge $r[0] -and $col -lt $r[1]) { return $true }
    }
    return $false
}

# ------------------------------------------------------------------
# The spans of QUOTED text (the Chinese corner quotes)
# ------------------------------------------------------------------
#
# The corner quotes are U+300C and U+300D. They do NOT nest in this
# corpus: a closer takes the OLDEST unclosed opener (first in, first
# out), which is what the documents actually do.
#
# Three traps this function exists to avoid:
#
#   * A span that is never closed produces NO span. An opener with no
#     closer must not swallow the rest of the file -- two files in
#     this repository have an unclosed corner quote, and a whole-file
#     mask built on them hides every real claim after that point.
#   * A span is not "the whole file". A pointer on a line that has no
#     corner quote at all is not quoted, however many quotes the file
#     contains elsewhere.
#   * A corner quote INSIDE INLINE CODE is a literal character being
#     displayed, not a quotation mark. The note that explains this
#     very rule writes the character in backticks -- and counting
#     that literal as an opener makes the pairing run off and never
#     recover, so every later quote in the file is paired with the
#     wrong opener. Measured: 3 files have corner quotes inside
#     inline code, and in the worst one the mispairing turned 8
#     correctly-quoted pointers into false alarms.
#
# The result is a list of spans: @(startLine, startCol, endLine, endCol),
# with 0-based line and column numbers.
function Get-CornerQuoteSpans([string[]]$Lines) {
    $lq = [char]0x300C
    $rq = [char]0x300D
    $spans = New-Object System.Collections.ArrayList
    $openers = New-Object System.Collections.ArrayList
    $map = Get-FenceMap $Lines
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($map[$i]) { continue }
        $line = $Lines[$i]
        $codeRanges = Get-InlineCodeRanges $line
        for ($k = 0; $k -lt $line.Length; $k++) {
            $c = $line[$k]
            if ($c -eq $lq) {
                if (Test-ColInRanges $k $codeRanges) { continue }
                [void]$openers.Add(@($i, $k))
            }
            elseif ($c -eq $rq) {
                if (Test-ColInRanges $k $codeRanges) { continue }
                if ($openers.Count -gt 0) {
                    $o = $openers[0]
                    [void]$openers.RemoveAt(0)
                    [void]$spans.Add(@($o[0], $o[1], $i, $k))
                }
            }
        }
    }
    return ,$spans.ToArray()
}
