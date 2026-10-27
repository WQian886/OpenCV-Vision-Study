<#
.SYNOPSIS
    Check that every relative link in the documentation resolves.

.DESCRIPTION
    A link is a CLAIM: "that file is over there, and it is still there".
    The claim is invisible in the text -- a link that points at a file
    that was deleted reads exactly like a link that points at a file that
    exists. The only way to tell them apart is to FOLLOW the link.

    This script follows every relative link in every markdown file in the
    repository and fails when one does not resolve.

    Why this exists (2026-10-26):
      A card was deleted from notes/atomic/ because it duplicated another
      card. The delete was correct -- the content was folded into the old
      card as a "recurrence" section. But three OTHER cards linked to the
      deleted file, and nothing noticed: the link text still read
      perfectly, the markdown still rendered, and CI was green.

      The same shape had already been recorded for the per-suite tables
      (notes/atomic/at-the-table-nobody-checks-drifts.md): a thing that
      nobody checks is a thing that drifts. A link is a recorded fact
      about a file, and it was drifting.

      While writing this checker it immediately found a SECOND broken
      link, one that had been sitting in W7_VisionMasterLite/README.md
      for a while: it pointed at "W7_VisionMasterLite/operator_library_groups.h"
      from inside W7_VisionMasterLite/ -- i.e. at
      W7_VisionMasterLite/W7_VisionMasterLite/operator_library_groups.h,
      which does not exist. The file it meant is one directory up.

    Rules:
      1. Only RELATIVE links are checked. An http(s) link points outside
         the repository, and this script cannot know whether it is alive
         -- pretending to check it would be the same disease as a check
         that reports success without looking (see
         notes/atomic/at-an-audit-that-cannot-see-starts-lying.md).
         The count of skipped links is PRINTED, so "I did not check
         these" is visible rather than silent.
      2. A link is resolved RELATIVE TO THE FILE THAT CONTAINS IT, which
         is what a markdown renderer does. There is no "also try the
         repository root" fallback: that fallback would mask exactly the
         class of mistake this script exists to find.
      3. Fenced code blocks are skipped. A C++ line such as
         "operator[](qsizetype i)" is not a link -- it is a function call
         that happens to contain the characters "](". Inline code spans
         are skipped for the same reason, and so is any "](" whose
         destination is not valid markdown.
      4. If the checker finds NO links at all, that is a FAILURE, not a
         pass. A checker whose pattern silently stopped matching is worse
         than no checker (notes/atomic/at-a-lint-that-finds-nothing-looks-green.md).
      5. The number of files and links actually inspected is PRINTED. A
         count is the cheapest proof that the checker can see something.
         So is the number of things that were SKIPPED and why -- "I did
         not check these" has to be visible, or a check that cannot see
         starts lying (notes/atomic/at-an-audit-that-cannot-see-starts-lying.md).

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's folder.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/check-links.ps1
#>
param(
    [string]$RepoRoot = ""
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

$Failures = New-Object System.Collections.ArrayList

function Fail([string]$msg) {
    [void]$Failures.Add($msg)
    Write-Host "   FAIL: $msg" -ForegroundColor Red
}

# ------------------------------------------------------------------
# 1. Which files are in scope
# ------------------------------------------------------------------
# The WORKING TREE, not the git index: a link that is broken right now
# is broken right now, whether or not the file is staged. The index
# would hide an untracked note -- and an untracked note is exactly the
# one a new link is most likely to point at.
#
# build/ and .git/ are excluded: build products are not documentation,
# and .git holds objects, not notes.
$mdFiles = @()
foreach ($f in (Get-ChildItem -Path $RepoRoot -Recurse -File -Filter *.md)) {
    $rel = $f.FullName.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/')
    if ($rel -match '(^|[\\/])(build|\.git)([\\/]|$)') { continue }
    $mdFiles += $f
}
$mdFiles = @($mdFiles | Sort-Object FullName)

if ($mdFiles.Count -eq 0) {
    Write-Host "FAILED: no markdown files found under $RepoRoot" -ForegroundColor Red
    Write-Host "   A checker that sees no files sees nothing."
    exit 1
}
Write-Host ("   {0} markdown files in scope" -f $mdFiles.Count)

# ------------------------------------------------------------------
# 2. Read a file, drop the fenced code blocks
# ------------------------------------------------------------------
# The fence marker is remembered, so a ``` block is closed by ``` and a
# ~~~ block by ~~~. A ``` inside a ~~~ block does not close it.
function Get-LinesOutsideFences([string[]]$lines) {
    $out = New-Object System.Collections.ArrayList
    $inFence = $false
    $fenceChar = ''
    foreach ($l in $lines) {
        $m = [regex]::Match($l, '^\s*(`{3,}|~{3,})')
        if ($m.Success) {
            $c = $m.Groups[1].Value.Substring(0, 1)
            if (-not $inFence) {
                $inFence = $true
                $fenceChar = $c
            }
            elseif ($c -eq $fenceChar) {
                $inFence = $false
            }
            continue
        }
        if (-not $inFence) { [void]$out.Add($l) }
    }
    return ,$out.ToArray()
}

# The GitHub heading slug, simplified:
#   lowercase, drop punctuation, spaces become hyphens.
# CJK is kept (it is a letter, and GitHub keeps it too).
function ConvertTo-GitHubSlug([string]$heading) {
    $s = $heading.Trim().ToLowerInvariant()
    $s = $s -replace '[^\p{L}\p{N}\s\-_]', ''
    $s = $s -replace '\s+', '-'
    return $s
}

# The set of anchor slugs a markdown file offers.
#
# NOTE the comma: without it, PowerShell UNROLLS the HashSet into the
# pipeline and the caller gets an array of strings instead of a set --
# and .Contains() on an array is a LIST search that silently answers
# "false" for a heading that is right there.
function Get-AnchorSet([string]$path) {
    $set = New-Object 'System.Collections.Generic.HashSet[string]'
    if (-not (Test-Path $path)) { return ,$set }
    $lines = [System.IO.File]::ReadAllLines($path, [System.Text.Encoding]::UTF8)
    foreach ($l in (Get-LinesOutsideFences $lines)) {
        $m = [regex]::Match($l, '^\s{0,3}#{1,6}\s+(.*?)\s*#*\s*$')
        if ($m.Success) {
            [void]$set.Add((ConvertTo-GitHubSlug $m.Groups[1].Value))
        }
    }
    return ,$set
}

# The ranges of INLINE CODE spans (`` `like this` ``) in a text.
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
function Get-InlineCodeRanges([string]$text) {
    $ranges = New-Object System.Collections.ArrayList
    $ticks = [regex]::Matches($text, '`+')
    $i = 0
    while ($i -lt $ticks.Count) {
        $open = $ticks[$i]
        $len = $open.Length
        $j = $i + 1
        $close = -1
        while ($j -lt $ticks.Count) {
            if ($ticks[$j].Length -eq $len) { $close = $j; break }
            $j++
        }
        if ($close -lt 0) { break }
        [void]$ranges.Add(@($open.Index, $ticks[$close].Index + $len))
        $i = $close + 1
    }
    return ,$ranges.ToArray()
}

# ------------------------------------------------------------------
# 3. Walk the links
# ------------------------------------------------------------------
$TotalLinks = 0        # every markdown link found
$Checked = 0           # relative links that were resolved
$SkippedExternal = 0   # http/https/mailto/data
$SkippedAnchor = 0     # a bare "#something"
$SkippedEmpty = 0      # "](  )" and friends
$SkippedNotALink = 0   # "](" that is not a link at all: C++ code, or a
                        # destination that is not valid markdown

$LinkPattern = '\[[^\]]*\]\(([^)]*)\)'

# Is the rest after the destination a valid link TITLE?
#
# In markdown, "[x](path "the title")" is a link with a title, and
# "[x](path stray)" is NOT a link -- a title has to be quoted. That
# distinction is what separates a real link from a C++ call such as
# "operator[](qsizetype i)", where "i" is just a parameter name.
function Test-ValidTitle([string]$rest) {
    $r = $rest.Trim()
    if ($r -eq '') { return $true }
    if ($r.Length -ge 2) {
        $first = $r.Substring(0, 1)
        $last = $r.Substring($r.Length - 1, 1)
        if (($first -eq '"' -and $last -eq '"') -or
            ($first -eq "'" -and $last -eq "'") -or
            ($first -eq '(' -and $last -eq ')')) {
            return $true
        }
    }
    return $false
}

foreach ($f in $mdFiles) {
    $lines = [System.IO.File]::ReadAllLines($f.FullName, [System.Text.Encoding]::UTF8)
    $body = (Get-LinesOutsideFences $lines) -join "`n"
    $relFile = $f.FullName.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/')
    $codeRanges = Get-InlineCodeRanges $body

    foreach ($m in [regex]::Matches($body, $LinkPattern)) {
        $TotalLinks++

        # Is this "](" inside inline code? Then it is CODE -- a C++ call
        # like "operator[](qsizetype i)" -- and a markdown renderer shows
        # it literally. It is not a link and must not be followed.
        $inCode = $false
        foreach ($r in $codeRanges) {
            if ($m.Index -ge $r[0] -and $m.Index -lt $r[1]) { $inCode = $true; break }
        }
        if ($inCode) { $SkippedNotALink++; continue }

        $raw = $m.Groups[1].Value.Trim()

        # A link may carry a title: [x](path "the title")
        $target = $raw
        if ($target.StartsWith('<')) {
            $end = $target.IndexOf('>')
            if ($end -lt 0) { $SkippedEmpty++; continue }
            $target = $target.Substring(1, $end - 1)
            $rest = $target.Substring($end + 1)
            if (-not (Test-ValidTitle $rest)) { $SkippedNotALink++; continue }
        }
        else {
            $sp = $target.IndexOf(' ')
            if ($sp -ge 0) {
                $rest = $target.Substring($sp + 1)
                $target = $target.Substring(0, $sp)
                # A bare destination cannot contain a space, and what
                # follows one has to be a quoted title. If it is not,
                # this "](" was never a link.
                if (-not (Test-ValidTitle $rest)) { $SkippedNotALink++; continue }
            }
        }

        if ($target -eq '') { $SkippedEmpty++; continue }

        # External links: out of scope, and COUNTED so the skip is visible.
        if ($target -match '^(?i)(https?:|mailto:|data:|ftp:|//)') {
            $SkippedExternal++
            continue
        }

        # Split off the anchor.
        $pathPart = $target
        $anchor = ''
        $hash = $target.IndexOf('#')
        if ($hash -ge 0) {
            $pathPart = $target.Substring(0, $hash)
            $anchor = $target.Substring($hash + 1)
        }
        $pathPart = $pathPart -replace '%20', ' '

        if ($pathPart -eq '') {
            # A link to a heading in THIS file.
            $SkippedAnchor++
            if ($anchor -eq '') { continue }
            $set = Get-AnchorSet $f.FullName
            if (-not $set.Contains((ConvertTo-GitHubSlug $anchor))) {
                Fail ("{0}: link to heading '#{1}' -- no such heading in that file" -f $relFile, $anchor)
            }
            else {
                $Checked++
            }
            continue
        }

        # Resolve RELATIVE TO THE FILE THAT CONTAINS THE LINK -- which is
        # what a markdown renderer does.
        $resolved = $null
        if ($pathPart.StartsWith('/')) {
            # A repository-root-absolute path.
            $candidate = Join-Path $RepoRoot $pathPart.TrimStart('/')
            if (Test-Path $candidate) { $resolved = $candidate }
        }
        else {
            $candidate = Join-Path $f.DirectoryName $pathPart
            if (Test-Path $candidate) { $resolved = $candidate }
        }

        if ($null -eq $resolved) {
            Fail ("{0}: link '{1}' does not resolve (looked for '{2}')" -f `
                  $relFile, $target, (Join-Path $f.DirectoryName $pathPart))
            continue
        }

        # The file is there. If the link also names a heading, that
        # heading has to be there too.
        if ($anchor -ne '') {
            if (-not (Test-Path $resolved -PathType Leaf)) {
                Fail ("{0}: link '{1}' points at a directory, but names a heading" -f $relFile, $target)
                continue
            }
            $set = Get-AnchorSet $resolved
            if (-not $set.Contains((ConvertTo-GitHubSlug $anchor))) {
                Fail ("{0}: link '{1}' -- the file exists but it has no heading '#{2}'" -f `
                      $relFile, $target, $anchor)
                continue
            }
        }

        $Checked++
    }
}

Write-Host ("   {0} links found, {1} relative links resolved" -f $TotalLinks, $Checked)
Write-Host ("   skipped: {0} external, {1} in-page anchors, {2} empty, {3} not-a-link" -f `
          $SkippedExternal, $SkippedAnchor, $SkippedEmpty, $SkippedNotALink)

# ------------------------------------------------------------------
# 4. Did the checker see anything at all?
# ------------------------------------------------------------------
#
# A link checker that finds no links has either been pointed at the wrong
# tree or its pattern has stopped matching. Both look exactly like a
# pass, and both are the failure this project keeps paying for.
if ($Checked -eq 0) {
    Fail ("no relative link was checked at all." +
          "`n   Either the tree is wrong, or the link pattern stopped matching." +
          "`n   A checker that cannot see anything must not report success.")
}

# ------------------------------------------------------------------
# 5. Summary
# ------------------------------------------------------------------
Write-Host ""
if ($Failures.Count -gt 0) {
    Write-Host ("{0} broken link(s):" -f $Failures.Count) -ForegroundColor Red
    foreach ($f in $Failures) { Write-Host "   - $f" -ForegroundColor Red }
    Write-Host ""
    Write-Host "A link is a claim that a file is still there. Follow it." -ForegroundColor Red
    Write-Host "LINKS FAILED" -ForegroundColor Red
    exit 1
}

Write-Host ("LINKS OK: {0} relative links across {1} files all resolve" -f `
          $Checked, $mdFiles.Count) -ForegroundColor Green
exit 0
