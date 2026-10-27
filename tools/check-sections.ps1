<#
.SYNOPSIS
    Check that every section pointer in the documentation resolves.

.DESCRIPTION
    A markdown link is a claim: "that FILE is over there, and it is
    still there". Step 3d follows those.

    A section pointer is the same kind of claim about a PLACE:
    "chapter 10 of that note", "section 6.3 of the other one". The
    pointer is invisible in the text -- a pointer at a chapter that was
    renumbered reads exactly like a pointer at a chapter that is still
    there. The only way to tell them apart is to CHECK.

    This script finds every section pointer in every markdown file in
    the repository and fails when one does not resolve.

    Why this exists (2026-10-27):
      The link checker (tools/check-links.ps1) landed the previous
      round and found three real dangling links on its first run. The
      same round measured what the link checker CANNOT see: a link is
      a recorded fact about a FILE, and a section pointer is a
      recorded fact about a PLACE. 164 section pointers, of which 57
      point into ONE file -- notes/notes_W7_Knob_Audit_2026-10.md,
      the debt register that this project keeps editing.

      Measured, not estimated: renumbering that one chapter breaks
      35 of those pointers, and every one of them still reads
      perfectly. (A mutation run on a copy of the tree confirmed
      the 35.)

      Measured on the day it was written: 164 pointers, all of them
      resolving -- so this gate is not reporting a fire that is already
      burning, it is stopping the next one from starting. (SIXTEEN
      lines of prose had to be REWRITTEN first -- eighteen pointers --
      because they pointed at a chapter of a file they did not name;
      see notes/notes_SectionPointers_2026-10.md.)

    THE RULE (one rule, so that "it resolves" means one thing):
      A section pointer is bound to the file name that PRECEDES it on
      the same line. It must resolve in THAT file. If no file name
      precedes it, the pointer is about the file the line is IN, and it
      must resolve there.

      Why "precedes" and not "is somewhere on the line": a line can
      name a file for a reason that has nothing to do with the
      pointer. "see chapter 9.6, and also
      notes/atomic/at-ninja-mtime-stale-binary.md" names the card as a
      RELATED link, not as the target of "chapter 9.6" -- the pointer
      comes FIRST there. And "see notes/notes_W7_Knob_Audit_2026-10.md
      chapter 10" names the file BEFORE the pointer, and that one IS
      the target.

      Why this matters more than it looks: if the binding were "any
      file named on the line", then a pointer to a chapter that no
      longer exists would quietly fall back to a chapter of the SAME
      NUMBER in the reader's own file, and the gate would stay green
      while the reader lands in the wrong place. A pointer that
      silently points somewhere else is worse than one that is
      obviously broken.

      A sentence terminator between the file name and the pointer ends
      the binding -- "notes/x.md. See chapter 3" is a new sentence,
      and chapter 3 is about the current file.

    Rules:
      1. Fenced code blocks are skipped: a C++ line is not a pointer.
      2. Chapter numbers are matched in BOTH notations. "chapter 4"
         and "chapter 4" are the same chapter, and a pointer in
         either notation resolves against a heading in either
         notation. The docs use both, and the same chapter is
         referred to both ways.
      3. "item 3" (the 3rd rule in a list) is NOT a pointer. It is
         an enumeration, and there is no chapter behind it.
      4. If the checker finds NO pointers at all, that is a FAILURE,
         not a pass. A checker whose pattern silently stopped matching
         is worse than no checker
         (notes/atomic/at-a-lint-that-finds-nothing-looks-green.md).
      5. The number of files, headings and pointers actually inspected
         is PRINTED. A count is the cheapest proof that the checker
         can see something -- and so is the number of things it could
         not check, and why.
      6. The Chinese characters this checker has to recognise are built
         from their CODE POINTS, because a .ps1 without a BOM is read
         as ANSI by PowerShell 5.1. A wrong code point would make the
         patterns match nothing -- which rule 4 turns into a loud
         failure instead of a silent pass. The code points were
         measured from the documents themselves, not from memory.
      7. A pointer inside INLINE CODE is code, not a claim. A pointer
         inside a QUOTATION (a Chinese corner-quote pair) is quoted
         TEXT, not a claim about a place -- unless the quotation
         itself names a file, in which case the pointer IS a claim
         about that file. Both skips are counted and printed, because
         a skip that is not counted is a pointer nobody checked and
         nobody knows about.
         Why the asymmetry with rule 2: a markdown link is STRUCTURAL
         -- "[x](y)" is a link whether it is quoted or not, and the
         link checker follows it either way. A section pointer is
         PROSE: "chapter 10" inside a quotation is a phrase being
         quoted, and a table of quotes ("here is what I changed it
         to") is a record of the past, not a claim about the present.
         The same asymmetry is why this rule is about the SAME LINE
         only: a quote that spans lines, or a whole-file mask, would
         hide far more claims than it protects (measured: a whole-file
         mask would have called 87% of the pointers quoted).
      8. The binding picks the NEAREST file name BEFORE the pointer.
         The mentions are collected pattern by pattern -- backtick
         form, link-target form, bare form -- so their ARRAY order is
         the order of the patterns, not the order of the text.
         Taking "the last one in the array" therefore binds a pointer
         to whichever file the last pattern happened to match, which
         is not the file the reader would follow. Measured: that bug
         bound a pointer to a file 100 lines away from the one the
         prose names.

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's folder.

.PARAMETER ShowSkips
    Print every pointer the checker SKIPPED, with the reason and the
    place it was found. The summary always prints how MANY were
    skipped; this prints WHICH ones, so that a human can judge each
    one. A count nobody can check is a number nobody can trust
    (notes/atomic/at-the-table-nobody-checks-drifts.md).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/check-sections.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/check-sections.ps1 -ShowSkips
#>
param(
    [string]$RepoRoot = "",
    [switch]$ShowSkips
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

. (Join-Path $PSScriptRoot 'lib\DocScan.ps1')

$Failures = New-Object System.Collections.ArrayList

function Fail([string]$msg) {
    [void]$Failures.Add($msg)
    Write-Host "   FAIL: $msg" -ForegroundColor Red
}

# ------------------------------------------------------------------
# 1. The characters, from their code points
# ------------------------------------------------------------------
function Ch([int]$cp) { return [string][char]$cp }

# The ordinal prefix: "chapter 4", "section 4", "item 3".
$DI    = Ch 0x7B2C
# "section"
$JIE   = Ch 0x8282
# "chapter"
$ZHANG = Ch 0x7AE0
# "item" -- an ENUMERATION ("the 3rd rule"), never a pointer.
$TIAO  = Ch 0x6761
# The ideographic comma in a heading like "## 10, ...".
$DUN   = Ch 0x3001
# The section sign: "3.3".
$SEC   = Ch 0x00A7

# The Chinese numerals one..ten, as code points.
# (measured from the documents, not from memory)
$CN_CP = @(0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03, 0x516B, 0x4E5D, 0x5341)

$CN_DIGITS = @{}
$CN_CHARS = ''
for ($i = 0; $i -lt $CN_CP.Count; $i++) {
    $c = Ch $CN_CP[$i]
    $CN_CHARS += $c
    if ($i -lt 9) { $CN_DIGITS[$c] = $i + 1 }
}
$SHI = Ch 0x5341

# The patterns, built from those characters.
$CN_CLASS   = '[' + $CN_CHARS + ']'
$ORD_NUM    = "$DI\s*(\d+(?:\.\d+)*)\s*[$JIE$ZHANG]"
$ORD_CN     = "$DI($CN_CLASS+)[$JIE$ZHANG]"
$SEC_NUM    = "$SEC\s*(\d+(?:\.\d+)*)"
# The section sign followed by a CHINESE numeral: "10" with the
# section sign and no "chapter" in front of it. The documents use
# this form too, and a pointer in a notation the checker cannot read
# is a pointer the checker cannot check.
$SEC_CN     = "$SEC\s*($CN_CLASS+)"
$HEAD_CN    = "^($CN_CLASS+)$DUN"
$HEAD_NUM   = '^(\d+(?:\.\d+)*)\.?\s'
$HEAD_ORD_CN  = "^$DI($CN_CLASS+)[$JIE]"
$HEAD_ORD_NUM = "^$DI\s*(\d+(?:\.\d+)*)\s*[$JIE]"

# "the 3rd rule" is an enumeration, not a pointer.  Counting how many
# were skipped is cheaper than explaining it in prose every time.
$ORD_ITEM   = "$DI\s*(\d+)\s*$TIAO"

# Sentence terminators. One of these between the file name and the
# pointer ends the binding: "notes/x.md. See chapter 3" starts a new
# sentence, and chapter 3 is about the current file.
$TerminatorChars = @(
    (Ch 0x3002)   # ideographic full stop
    (Ch 0xFF01)   # fullwidth exclamation mark
    (Ch 0xFF1F)   # fullwidth question mark
    (Ch 0xFF1B)   # fullwidth semicolon
    '!'
    '?'
    ';'
)
$TerminatorClass = '[' + ($TerminatorChars -join '') + ']'

# ------------------------------------------------------------------
# 2. Chinese numeral -> integer
# ------------------------------------------------------------------
#
# NOTE every key is a STRING. A hashtable indexed by a [char] does not
# match a [string] key, and the lookup then returns $null -- which
# silently turns "eleven" into "10" (10 + $null). That is not a
# hypothetical: it is exactly what the first version of this probe did.
function Convert-CnToInt([string]$s) {
    if ([string]::IsNullOrEmpty($s)) { return -1 }
    if ($s -eq $SHI) { return 10 }
    if ($s.Length -eq 2) {
        $a = [string]$s[0]
        $b = [string]$s[1]
        if ($a -eq $SHI -and $CN_DIGITS.ContainsKey($b)) { return 10 + $CN_DIGITS[$b] }
        if ($b -eq $SHI -and $CN_DIGITS.ContainsKey($a)) { return $CN_DIGITS[$a] * 10 }
    }
    if ($s.Length -eq 3) {
        $a = [string]$s[0]
        $b = [string]$s[1]
        $c = [string]$s[2]
        if ($b -eq $SHI -and $CN_DIGITS.ContainsKey($a) -and $CN_DIGITS.ContainsKey($c)) {
            return $CN_DIGITS[$a] * 10 + $CN_DIGITS[$c]
        }
    }
    if ($CN_DIGITS.ContainsKey($s)) { return $CN_DIGITS[$s] }
    return -1
}

# ------------------------------------------------------------------
# 3. The files
# ------------------------------------------------------------------
$mdFiles = Get-DocFiles $RepoRoot
if ($mdFiles.Count -eq 0) {
    Write-Host "FAILED: no markdown files found under $RepoRoot" -ForegroundColor Red
    Write-Host "   A checker that sees no files sees nothing."
    exit 1
}
Write-Host ("   {0} markdown files in scope" -f $mdFiles.Count)

# ------------------------------------------------------------------
# 4. The chapters each file has
# ------------------------------------------------------------------
#
# A chapter number is stored as a plain dotted string: "4", "6.3",
# "10". The Chinese notation and the Arabic notation share that one
# namespace, so "chapter 4" and "chapter 4" are the same chapter
# whichever way the heading or the pointer is written.
#
# The four heading shapes in this repository:
#   "## 10, ..."          -> Chinese numeral + ideographic comma
#   "## 6.3 ..."          -> Arabic, dotted
#   "## 9. ..."           -> Arabic, one level, with a trailing dot
#   "### chapter 10 ..."  -> Chinese ordinal
#   "### section 6.3 ..." -> Arabic ordinal
$ChapterTokens = @{}
$HeadingCount = 0
foreach ($f in $mdFiles) {
    $set = New-Object System.Collections.Generic.HashSet[string]
    $lines = Get-DocLinesUtf8 $f.FullName
    foreach ($s in (Get-ScannedLines $lines)) {
        $l = $s.Text
        $m = [regex]::Match($l, '^\s{0,3}#{1,6}\s+(.*?)\s*#*\s*$')
        if (-not $m.Success) { continue }
        $h = $m.Groups[1].Value

        $a = [regex]::Match($h, $HEAD_CN)
        if ($a.Success) {
            $n = Convert-CnToInt $a.Groups[1].Value
            if ($n -gt 0) { [void]$set.Add([string]$n) }
        }
        $b = [regex]::Match($h, $HEAD_NUM)
        if ($b.Success) { [void]$set.Add($b.Groups[1].Value) }
        $c = [regex]::Match($h, $HEAD_ORD_CN)
        if ($c.Success) {
            $n = Convert-CnToInt $c.Groups[1].Value
            if ($n -gt 0) { [void]$set.Add([string]$n) }
        }
        $d = [regex]::Match($h, $HEAD_ORD_NUM)
        if ($d.Success) { [void]$set.Add($d.Groups[1].Value) }
    }
    $ChapterTokens[$f.FullName] = $set
    $HeadingCount += $set.Count
}

if ($HeadingCount -eq 0) {
    Fail ("no chapter heading was recognised in any file." +
          "`n   Either the tree is wrong, or the heading patterns stopped matching." +
          "`n   A checker that cannot see a chapter cannot check a pointer to one.")
}

# ------------------------------------------------------------------
# 5. The pointers
# ------------------------------------------------------------------
$TotalPointers = 0
$CrossFile = 0
$SameFile = 0
$SkippedItems = 0
$PointersUncheckable = 0
$PointersInCode = 0
$PointersQuoted = 0
$Fragments = 0
$MissingNames = @{}
$AmbiguousNames = @{}
$PointedAt = @{}
# Every skip, with its reason and its place. The summary prints how
# many; -List prints which ones, so a human can judge each.
$Skips = New-Object System.Collections.ArrayList

# A markdown file named on a line. The repository uses three forms,
# and all three are matched:
#   `notes/notes_x.md`         backtick-quoted (the common one)
#   [text](notes/notes_x.md)   a markdown link target
#   notes/notes_x.md           bare
#
# The name is captured WITH ITS POSITION, because the binding rule is
# about ORDER: a pointer is bound to the name that comes before it.
# NOTE the parentheses around each element. In PowerShell the COMMA
# operator binds TIGHTER than "+", so
#     @('a' + $x, 'b' + $y)
# is ONE element -- the three patterns silently become one string, and
# a regex that matches nothing is indistinguishable from a pass.
# The parentheses make the element boundaries explicit.
$NameStem = '[A-Za-z0-9_' + (Ch 0x4E00) + '-' + (Ch 0x9FFF) + ']'
$MentionPatterns = @(
    ('`(?<name>[^`]*?' + $NameStem + '[^`]*?\.md)`'),
    ('\]\((?<name>[^)\s]*?' + $NameStem + '[^)\s]*?\.md)\)'),
    ('(?<name>[A-Za-z0-9_\-./]*' + $NameStem + '[A-Za-z0-9_\-./]*\.md)')
)
$UrlPattern = '(?i)https?://\S+'

# The bare pattern is the only one that can be cut in half by a
# Chinese character: the class stops at the first non-ASCII byte, so
# "notes/notes_W7_<CJK>C_2026-09.md" comes out as "C_2026-09.md".
# A mention that is glued to Chinese prose on its left is therefore
# a FRAGMENT of a longer name, and the exact patterns (backtick, link
# target) already found the real one on the same line. Counting the
# fragment would report a file that does not exist under a name that
# is not the one in the text -- so it is counted separately and
# disclosed, not folded into the missing-file count.

# Every markdown file, indexed by its repository-relative path.
#
# Resolving a name has to be a HASH LOOKUP, not a Test-Path per
# candidate: at 127 files and roughly 3000 lines there are a lot of
# mentions, and a file system round trip for each one turns a
# one-second check into a one-minute one.  The index also has to be
# built ONCE -- a check that has to run on every push cannot afford
# to re-walk the tree per line.
$FileIndex = @{}
foreach ($f in $mdFiles) {
    $rel = $f.FullName.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/').Replace('\', '/')
    $FileIndex[$rel] = $f.FullName
}

# Base name -> the files that carry it. Used only for a name written
# WITHOUT a directory. The measurement behind this: 127 files, and
# the ONLY base name that appears more than once is README.md.
$ByBaseName = @{}
foreach ($f in $mdFiles) {
    $b = $f.Name
    if (-not $ByBaseName.ContainsKey($b)) {
        $ByBaseName[$b] = New-Object System.Collections.ArrayList
    }
    [void]$ByBaseName[$b].Add($f.FullName)
}

# Normalise a relative path: collapse "." and "..", so that
# "notes/../notes/x.md" and "./x.md" can both be looked up in one
# index instead of three.
function Resolve-DocPath([string]$rel) {
    $parts = $rel.Replace('\', '/').Split('/')
    $stack = New-Object System.Collections.ArrayList
    foreach ($p in $parts) {
        if ($p -eq '' -or $p -eq '.') { continue }
        if ($p -eq '..') {
            if ($stack.Count -gt 0) { $stack.RemoveAt($stack.Count - 1) }
            continue
        }
        [void]$stack.Add($p)
    }
    return ($stack -join '/')
}

# Resolve a mentioned name to a file in this repository.
# Returns a full path, the string 'AMBIGUOUS', or $null.
function Resolve-Mention([string]$name, [string]$relDir) {
    $norm = Resolve-DocPath $name
    $cands = @()
    if ($relDir -ne '') { $cands += (Resolve-DocPath ($relDir + '/' + $norm)) }
    $cands += $norm
    $cands += ('notes/' + $norm)
    foreach ($c in $cands) {
        if ($FileIndex.ContainsKey($c)) { return $FileIndex[$c] }
    }
    # A name with no directory part: look it up by base name -- but
    # only when it is UNIQUE. If two files share the name, guessing
    # which one the author meant is a silent wrong answer, and a
    # wrong binding means the pointer gets checked against the wrong
    # file.
    if (-not $norm.Contains('/')) {
        if ($ByBaseName.ContainsKey($norm)) {
            $hits = $ByBaseName[$norm]
            if ($hits.Count -eq 1) { return $hits[0] }
            return 'AMBIGUOUS'
        }
    }
    return $null
}

# ------------------------------------------------------------------
# 4b. Where is the pointer, textually?
# ------------------------------------------------------------------
#
# Two things decide whether a pointer is a CLAIM about a place or
# something else entirely:
#
#   * INLINE CODE ("`like this`") -- a markdown renderer shows it
#     literally, and a chapter number in it is a literal too. This is
#     the same rule the link checker already uses for "](", and it
#     lives in tools/lib/DocScan.ps1 so that there is ONE copy.
#
#   * A QUOTATION (a Chinese corner-quote pair on the same line) --
#     quoted text is text ABOUT text. "the doc says 'chapter 10 of
#     that note'" quotes a phrase; it does not point anywhere.
#
# Both are skips, and both are COUNTED. A pointer that nobody checked
# and nobody knows about is the failure mode this project keeps
# paying for.
#
# The quote span is looked up by LINE and COLUMN, and the column test
# is what makes the difference: a quote that spans lines would
# otherwise swallow lines that have no quote on them at all.
function Test-InCode([int]$col, $ranges) {
    foreach ($r in $ranges) {
        if ($col -ge $r[0] -and $col -lt $r[1]) { return $true }
    }
    return $false
}

# The quote span that contains ($lineNo, $col), or $null.
# A span is @(startLine, startCol, endLine, endCol).
function Get-QuoteSpanAt([int]$lineNo, [int]$col, $spans) {
    foreach ($sp in $spans) {
        if ($lineNo -lt $sp[0] -or $lineNo -gt $sp[2]) { continue }
        # The opener is not inside its own span, and neither is the
        # closer.
        if ($lineNo -eq $sp[0] -and $col -le $sp[1]) { continue }
        if ($lineNo -eq $sp[2] -and $col -ge $sp[3]) { continue }
        return $sp
    }
    return $null
}

# The columns of a span ON ONE line -- what part of this line the
# quotation covers.
function Get-SpanColumnRange([int]$lineNo, $span, [int]$lineLen) {
    $lo = 0
    $hi = $lineLen
    if ($lineNo -eq $span[0]) { $lo = $span[1] + 1 }
    if ($lineNo -eq $span[2]) { $hi = $span[3] - 1 }
    return ,@($lo, $hi)
}

foreach ($f in $mdFiles) {
    $relFile = $f.FullName.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/')
    $relDir = Resolve-DocPath (Split-Path -Parent $relFile)
    if ($relDir -eq '.') { $relDir = '' }
    $lines = Get-DocLinesUtf8 $f.FullName
    # The quoted spans of THIS file. A span is @(startLine, startCol,
    # endLine, endCol), with 0-based line and column numbers.
    $quoteSpans = Get-CornerQuoteSpans $lines

    foreach ($s in (Get-ScannedLines $lines)) {
        $line = $s.Text
        $lineNoUrls = [regex]::Replace($line, $UrlPattern, ' ')
        # The inline code spans of THIS line. A code span cannot cross
        # a line break, so the line is the whole of it.
        $codeRanges = Get-InlineCodeRanges $line

        # The file name mentions on this line, with their positions.
        $mentions = New-Object System.Collections.ArrayList
        $seenMention = @{}
        for ($pi = 0; $pi -lt $MentionPatterns.Count; $pi++) {
            $pat = $MentionPatterns[$pi]
            $isBare = ($pi -eq ($MentionPatterns.Count - 1))
            foreach ($m in [regex]::Matches($lineNoUrls, $pat)) {
                $name = $m.Groups['name'].Value
                $idx = $m.Groups['name'].Index
                $key = "$idx|$name"
                if ($seenMention.ContainsKey($key)) { continue }
                $seenMention[$key] = $true

                # A bare name glued to Chinese prose on its left is the
                # tail of a longer name whose head the class could not
                # match. The exact patterns already found the real
                # name on this line, so this one is a fragment.
                if ($isBare -and $idx -gt 0) {
                    $prev = $lineNoUrls[$idx - 1]
                    if ($prev -match '[^\x00-\x7F]') {
                        $Fragments++
                        continue
                    }
                }

                $resolved = Resolve-Mention $name $relDir
                $state = 'ok'
                if ($resolved -eq 'AMBIGUOUS') {
                    $state = 'ambiguous'
                    if (-not $AmbiguousNames.ContainsKey($name)) { $AmbiguousNames[$name] = 0 }
                    $AmbiguousNames[$name]++
                }
                elseif ($null -eq $resolved) {
                    $state = 'missing'
                    if (-not $MissingNames.ContainsKey($name)) { $MissingNames[$name] = 0 }
                    $MissingNames[$name]++
                }
                [void]$mentions.Add([pscustomobject]@{
                    Index = $idx
                    End   = $idx + $name.Length
                    Name  = $name
                    Path  = $resolved
                    State = $state
                })
            }
        }

        # The pointers on this line, in the three notations.  Each one
        # keeps its POSITION too.
        $pointers = New-Object System.Collections.ArrayList
        foreach ($m in [regex]::Matches($lineNoUrls, $ORD_NUM)) {
            [void]$pointers.Add([pscustomobject]@{
                Token = $m.Groups[1].Value; Text = $m.Groups[0].Value; Index = $m.Index })
        }
        foreach ($m in [regex]::Matches($lineNoUrls, $ORD_CN)) {
            $n = Convert-CnToInt $m.Groups[1].Value
            if ($n -gt 0) {
                [void]$pointers.Add([pscustomobject]@{
                    Token = [string]$n; Text = $m.Groups[0].Value; Index = $m.Index })
            }
        }
        foreach ($m in [regex]::Matches($lineNoUrls, $SEC_NUM)) {
            [void]$pointers.Add([pscustomobject]@{
                Token = $m.Groups[1].Value; Text = $m.Groups[0].Value; Index = $m.Index })
        }
        foreach ($m in [regex]::Matches($lineNoUrls, $SEC_CN)) {
            $n = Convert-CnToInt $m.Groups[1].Value
            if ($n -gt 0) {
                [void]$pointers.Add([pscustomobject]@{
                    Token = [string]$n; Text = $m.Groups[0].Value; Index = $m.Index })
            }
        }
        # The enumerations on this line are skipped, and COUNTED. They
        # are counted on EVERY line, not only on the lines that have
        # no pointer: a line can have both, and counting only half of
        # them would understate what this checker did not check.
        foreach ($m in [regex]::Matches($lineNoUrls, $ORD_ITEM)) {
            $SkippedItems++
            [void]$Skips.Add([pscustomobject]@{
                File = $relFile; Line = ($s.Index + 1); Text = $m.Value
                Reason = "enumeration ('the Nth rule'), not a pointer" })
        }

        if ($pointers.Count -eq 0) { continue }

        foreach ($p in $pointers) {
            $TotalPointers++

            # A pointer inside INLINE CODE is code, not a claim.
            if (Test-InCode $p.Index $codeRanges) {
                $PointersInCode++
                [void]$Skips.Add([pscustomobject]@{
                    File = $relFile; Line = ($s.Index + 1); Text = $p.Text
                    Reason = 'inside INLINE CODE (code, not a claim)' })
                continue
            }

            # WHICH file is this pointer about?
            #
            # The nearest file name that comes BEFORE the pointer on
            # this line, with no sentence terminator in between.  If
            # there is no such file name, the pointer is about the file
            # the line is IN.
            #
            # "NEAREST" means nearest in the TEXT. The mentions are
            # collected PATTERN BY PATTERN -- backtick form, link
            # target form, bare form -- so their array order is the
            # order of the patterns and NOT the order of the words on
            # the line. Taking the last one in the array binds the
            # pointer to whichever file the last pattern happened to
            # match, which is not the file the reader would follow.
            $bound = $null

            # A pointer inside a QUOTATION is quoted text, not a claim
            # -- unless the quotation itself names a file, in which
            # case the pointer IS a claim about that file.
            $span = Get-QuoteSpanAt $s.Index $p.Index $quoteSpans
            if ($null -ne $span) {
                $rng = Get-SpanColumnRange $s.Index $span $lineNoUrls.Length
                foreach ($mn in $mentions) {
                    if ($mn.Index -lt $rng[0] -or $mn.Index -gt $rng[1]) { continue }
                    if ($null -eq $bound -or $mn.Index -gt $bound.Index) { $bound = $mn }
                }
                if ($null -eq $bound) {
                    $PointersQuoted++
                    [void]$Skips.Add([pscustomobject]@{
                        File = $relFile; Line = ($s.Index + 1); Text = $p.Text
                        Reason = 'inside a QUOTATION (quoted text, not a claim)' })
                    continue
                }
            }
            else {
                foreach ($mn in $mentions) {
                    if ($mn.End -le $p.Index) {
                        $between = $lineNoUrls.Substring($mn.End, $p.Index - $mn.End)
                        if ($between -notmatch "[$TerminatorClass]") {
                            if ($null -eq $bound -or $mn.Index -gt $bound.Index) { $bound = $mn }
                        }
                    }
                }
            }

            # The pointer names a file that is not in this tree, so
            # there is no chapter table to check it against.  That is
            # a SKIP, and it is counted -- a pointer that was never
            # checked must never be counted as one that passed.
            if ($null -ne $bound -and $bound.State -ne 'ok') {
                $PointersUncheckable++
                [void]$Skips.Add([pscustomobject]@{
                    File = $relFile; Line = ($s.Index + 1); Text = $p.Text
                    Reason = ("bound to '" + $bound.Name + "', which is not in this tree") })
                continue
            }

            $target = $f.FullName
            if ($null -ne $bound) { $target = $bound.Path }

            $set = $ChapterTokens[$target]
            $ok = ($null -ne $set) -and $set.Contains($p.Token)

            if ($ok) {
                if ($target -eq $f.FullName) { $SameFile++ } else { $CrossFile++ }
                if (-not $PointedAt.ContainsKey($target)) { $PointedAt[$target] = 0 }
                $PointedAt[$target]++
                continue
            }

            # Why did it not resolve?  Say so precisely: a message that
            # does not say WHERE it looked is a message that cannot be
            # acted on.
            $relTarget = $target.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/')
            $has = ""
            if ($null -ne $set) {
                # Sorted by the LEADING NUMBER, not as text: a text
                # sort puts "11" before "2", which reads like a list of
                # nonsense. And capped: the register has ninety
                # chapters, and a failure message that lists all of
                # them buries the one line the reader needs.
                #
                # Two traps, both measured, both paid for:
                #   - A local variable must NEVER share its name with a
                #     typed parameter. A [switch] parameter is a
                #     type-constrained variable, so assigning an array
                #     to it raises "Cannot convert value Object[] to
                #     type SwitchParameter" -- and the gate dies while
                #     reporting a failure, which is the one moment it
                #     must not die. It only bites at SCRIPT scope: in
                #     a function the assignment makes a new child
                #     variable and the trap never fires.
                #   - The sort must not be able to kill the message
                #     either, so it is guarded and falls back to the
                #     plain order. The order is cosmetic; the red is
                #     the point.
                $chapList = @($set)
                try {
                    $chapList = @($set | Sort-Object -Property @(
                        @{ Expression = { [int]$_.Split('.')[0] } },
                        @{ Expression = { $_ } }))
                } catch {
                    $chapList = @($set)
                }
                if ($chapList.Count -gt 0) {
                    $shown = $chapList
                    if ($chapList.Count -gt 12) { $shown = $chapList[0..11] }
                    $has = "; it has: " + ($shown -join ', ')
                    if ($chapList.Count -gt 12) {
                        $has += " (and " + ($chapList.Count - 12) + " more)"
                    }
                }
            }
            if ($null -ne $bound) {
                $why = ("the pointer is bound to '" + $bound.Name +
                        "', the file named before it on this line, and that file has" +
                        " no chapter '" + $p.Token + "'" + $has)
            }
            else {
                $why = ("this line names no file before the pointer, so the pointer is" +
                        " read against the file it is in ('" + $relTarget +
                        "'), and that file has no chapter '" + $p.Token + "'" + $has)
            }
            Fail ("{0}:{1} -- pointer '{2}' does not resolve: {3}" -f `
                  $relFile, ($s.Index + 1), $p.Text, $why)
        }
    }
}

# ------------------------------------------------------------------
# 6. Did the checker see anything at all?
# ------------------------------------------------------------------
#
# A checker that finds no pointers has either been pointed at the wrong
# tree or its pattern has stopped matching -- and a wrong code point
# in section 1 would do exactly that. Both look exactly like a pass.
if ($TotalPointers -eq 0) {
    Fail ("no section pointer was checked at all." +
          "`n   Either the tree is wrong, or the pointer patterns stopped matching." +
          "`n   A checker that cannot see anything must not report success.")
}

# The self-check for the code points: the characters this file builds
# must actually occur in the documents. If a code point is wrong, this
# fires -- instead of the checker quietly matching nothing.
$sample = $DI + $SHI + $JIE
$sampleFound = $false
foreach ($f in $mdFiles) {
    $t = [System.IO.File]::ReadAllText($f.FullName, [System.Text.Encoding]::UTF8)
    if ($t.Contains($sample)) { $sampleFound = $true; break }
}
if (-not $sampleFound) {
    Fail ("the Chinese characters built from the code points in this script do not" +
          "`n   occur anywhere in the documentation (looked for '" + $sample + "')." +
          "`n   The code points are wrong, so the patterns match nothing." +
          "`n   Measure them from the documents -- do not guess them.")
}

$Resolved = $CrossFile + $SameFile
Write-Host ("   {0} chapters recognised across {1} files" -f $HeadingCount, $mdFiles.Count)
Write-Host ("   {0} section pointers seen: {1} resolve ({2} cross-file, {3} same-file), {4} do not" -f `
          $TotalPointers, $Resolved, $CrossFile, $SameFile, $Failures.Count)
Write-Host ("   skipped: {0} enumerations ('the Nth rule')" -f $SkippedItems)
Write-Host ("   skipped: {0} pointers inside INLINE CODE (code, not claims)" -f $PointersInCode)
Write-Host ("   skipped: {0} pointers inside a QUOTATION (quoted text, not claims)" -f $PointersQuoted)

# The skips that are about FILES rather than about pointers. These are
# printed with their names, because a count without the names cannot
# be judged by a human -- and this checker is allowed to skip things
# ONLY if it says what it skipped.
if ($MissingNames.Count -gt 0) {
    $mn = @($MissingNames.Keys | Sort-Object)
    # EVERY name is printed, not the first few. A cap hides one of
    # them, and a hidden name is a name nobody can check: the count
    # would say "7" while the list said 6, and the two would not
    # agree. A count nobody can check is a number nobody can trust
    # (notes/atomic/at-the-table-nobody-checks-drifts.md).
    Write-Host ("   named files not in this tree: {0} distinct -- {1}" -f `
          $mn.Count, ($mn -join ', '))
}
if ($AmbiguousNames.Count -gt 0) {
    $an = @($AmbiguousNames.Keys | Sort-Object)
    Write-Host ("   named files that are AMBIGUOUS (more than one file has that name): {0} -- {1}" -f `
          $an.Count, ($an -join ', '))
}
Write-Host ("   pointers that could NOT be checked (their target file is not in this tree): {0}" -f `
          $PointersUncheckable)
Write-Host ("   bare names cut in half by a Chinese character (counted by the exact patterns instead): {0}" -f `
          $Fragments)

# Where the pointers aim. This is the blast radius: the file with the
# most pointers into it is the one whose renumbering breaks the most
# prose -- so it is the one to be careful with.
if ($PointedAt.Count -gt 0) {
    $top = @($PointedAt.GetEnumerator() | Sort-Object -Property Value -Descending | Select-Object -First 3)
    $parts = @()
    foreach ($e in $top) {
        $parts += ("{0} ({1})" -f $e.Key.Substring($RepoRoot.Length).TrimStart([char]'\', [char]'/'), $e.Value)
    }
    Write-Host ("   most pointed at: {0}" -f ($parts -join ', '))
}

# ------------------------------------------------------------------
# 6b. The list of skips, when asked for
# ------------------------------------------------------------------
#
# The summary below prints HOW MANY things were skipped. A count that
# cannot be checked is a number that cannot be trusted, so -List prints
# WHICH ones and WHY -- one line each, with the place it was found.
#
# The reasons are the same words the summary uses, so a line here can
# be read against the summary without a key.
if ($ShowSkips) {
    Write-Host ""
    Write-Host ("   the {0} skip(s) this checker made, one per line:" -f $Skips.Count)
    foreach ($sk in $Skips) {
        Write-Host ("     {0}:{1}  [{2}]  {3}" -f $sk.File, $sk.Line, $sk.Text, $sk.Reason)
    }
}

# ------------------------------------------------------------------
# 7. Summary
# ------------------------------------------------------------------
Write-Host ""
if ($Failures.Count -gt 0) {
    Write-Host ("{0} section pointer(s) do not resolve:" -f $Failures.Count) -ForegroundColor Red
    foreach ($f in $Failures) { Write-Host "   - $f" -ForegroundColor Red }
    Write-Host ""
    Write-Host "A section pointer is a claim that a chapter is still there. Check it." -ForegroundColor Red
    Write-Host "SECTIONS FAILED" -ForegroundColor Red
    exit 1
}

Write-Host ("SECTIONS OK: {0} of {1} section pointers resolve across {2} files; {3} pointers skipped (counted above), {4} enumerations" -f `
          $Resolved, $TotalPointers, $mdFiles.Count, `
          ($PointersInCode + $PointersQuoted + $PointersUncheckable), $SkippedItems) -ForegroundColor Green
exit 0
