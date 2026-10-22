<#
.SYNOPSIS
    Run the GitHub Actions workflow's own steps on this machine.

.DESCRIPTION
    Executes the `run:` blocks of .github/workflows/ci.yml, in order, the
    same way the runner does:

      * each block is written to a temp script and run by pwsh
        ($ErrorActionPreference='stop' + exit $LASTEXITCODE, like the
        runner's own shell wrapper)
      * GITHUB_ENV / GITHUB_PATH are honoured: after a step, the values
        that step wrote become environment variables for the next step
      * RUNNER_TEMP, GITHUB_WORKSPACE and CI are set the way the runner
        sets them
      * `uses:` steps (actions/checkout) are skipped -- we are already
        in the repository

    Why this file exists:
    the workflow used to be the one piece of CI that could not be run
    before pushing -- it is a YAML file, and YAML is not a program you
    can execute on your own machine.  The provisioning steps have since
    moved into tools/ci-deps.ps1, but the *workflow* itself (which
    command runs when, and which variables reach which step) was still
    unverified.

    This script closes that gap without copying anything: it reads the
    YAML and runs it.  If the workflow and this script ever disagree,
    it is because the workflow changed -- not because a copy drifted.

    What it does NOT verify:
    GitHub's own infrastructure -- the job scheduler, the runner image,
    the network from the runner.  Those are the environment, not the
    code.  See notes/notes_CI_2026-10.md, chapter 4.

.NOTES
    Must stay pure ASCII: Windows PowerShell 5.1 reads a BOM-less .ps1
    as ANSI, and any non-ASCII byte turns into mojibake or a syntax
    error.  Chinese explanations live in notes/notes_CI_2026-10.md.
#>
[CmdletBinding()]
param(
    # The workflow to run.  Default: the one in this repository.
    [string]$Workflow = '',

    # Where the runner would put its scratch space.
    [string]$RunnerTemp = '',

    # The pwsh to execute the steps with.  Default: pwsh on PATH.
    # Pass a portable PowerShell 7 to match the runner's shell exactly.
    [string]$PwshExe = '',

    # Only run steps whose title matches one of these (regex, case-insensitive).
    [string[]]$Step = @(),

    # List the parsed steps and exit.
    [switch]$ListOnly
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($Workflow)) {
    $Workflow = Join-Path $RepoRoot '.github\workflows\ci.yml'
}
if (-not (Test-Path $Workflow)) { throw "workflow not found: $Workflow" }
if ([string]::IsNullOrEmpty($RunnerTemp)) {
    $RunnerTemp = Join-Path $env:TEMP 'w7-ci-local'
}
New-Item -ItemType Directory -Force -Path $RunnerTemp | Out-Null

# ---------------------------------------------------------------
# Find a pwsh.  The runner uses PowerShell 7; Windows PowerShell 5.1
# is a *different shell* (different encoding defaults, different
# error semantics), so running the steps under 5.1 would be a
# different experiment than the one we want to run.
# ---------------------------------------------------------------
if ([string]::IsNullOrEmpty($PwshExe)) {
    $cmd = Get-Command pwsh -ErrorAction SilentlyContinue
    if ($cmd) {
        $PwshExe = $cmd.Source
    } else {
        throw ("pwsh (PowerShell 7) not found. The runner uses PowerShell 7, " +
               "so running the steps under Windows PowerShell 5.1 would not be " +
               "the same experiment. Pass -PwshExe <path to pwsh.exe>.")
    }
}
Write-Host "pwsh    : $PwshExe"

# ---------------------------------------------------------------
# Parse the workflow: pull out every step and its `run:` block.
#
# This is a deliberately small parser -- it only understands the
# shape this repository's workflow uses:
#     - name: <title>          (or - uses: <action>)
#     shell: pwsh
#     run: |
#         <indented block>
# ---------------------------------------------------------------
function Get-Indent([string]$line) {
    return ($line -replace '^(\s*).*$', '$1').Length
}

function Parse-Workflow([string]$path) {
    $lines = Get-Content $path
    $steps = @()
    $cur = $null
    $i = 0
    while ($i -lt $lines.Count) {
        $l = $lines[$i]

        # a new step starts with "- name:" or "- uses:"
        if ($l -match '^\s*-\s+(name|uses):\s*(.*?)\s*$') {
            if ($null -ne $cur) { $steps += $cur }
            $cur = [pscustomobject]@{ Kind = $Matches[1]; Title = $Matches[2]; Run = '' }
            $i++
            continue
        }

        # a `run: |` literal block
        if ($null -ne $cur -and $l -match '^\s*run:\s*\|\s*$') {
            $indent = Get-Indent $l
            $block = @()
            $j = $i + 1
            while ($j -lt $lines.Count) {
                $t = $lines[$j]
                if ($t.Trim() -eq '') {
                    $block += ''
                    $j++
                    continue
                }
                if ((Get-Indent $t) -le $indent) { break }
                $block += $t
                $j++
            }
            $nonBlank = @($block | Where-Object { $_.Trim() -ne '' })
            if ($nonBlank.Count -gt 0) {
                $min = ($nonBlank | ForEach-Object { Get-Indent $_ } | Measure-Object -Minimum).Minimum
                $cur.Run = (($block | ForEach-Object {
                    if ($_.Trim() -eq '') { '' } else { $_.Substring([Math]::Min($min, $_.Length)) }
                }) -join "`n")
            }
            $i = $j
            continue
        }
        $i++
    }
    if ($null -ne $cur) { $steps += $cur }
    return $steps
}

$steps = Parse-Workflow $Workflow
$runSteps = @($steps | Where-Object { $_.Run -ne '' })

# WARNING: a parser that silently finds nothing is a lint that always passes.
#   If the workflow's shape changes and this stops finding the steps,
#   it must say so loudly instead of reporting a green run of zero steps.
if ($runSteps.Count -eq 0) {
    throw ("no `run:` block found in $Workflow -- the parser no longer " +
           "understands this workflow (it only handles `- name:` / `run: |`). " +
           "A green run of ZERO steps would be a lie.")
}
Write-Host "workflow: $Workflow"
Write-Host "steps   : $($steps.Count) total, $($runSteps.Count) with a run block"
foreach ($s in $steps) {
    $mark = if ($s.Run -ne '') { 'run ' } else { 'skip' }
    Write-Host ("  [{0}] {1} {2}" -f $mark, $s.Kind, $s.Title)
}

if ($ListOnly) { return }

# ---------------------------------------------------------------
# Emulate the runner's environment.
# ---------------------------------------------------------------
$env:RUNNER_TEMP      = $RunnerTemp
$env:GITHUB_WORKSPACE = $RepoRoot
$env:GITHUB_ENV       = Join-Path $RunnerTemp 'github_env'
$env:GITHUB_PATH      = Join-Path $RunnerTemp 'github_path'
$env:CI               = 'true'
foreach ($f in @($env:GITHUB_ENV, $env:GITHUB_PATH)) {
    if (Test-Path $f) { Remove-Item $f -Force }
    New-Item -ItemType File -Force -Path $f | Out-Null
}

function Import-RunnerEnv {
    # The runner applies GITHUB_ENV / GITHUB_PATH to the *following*
    # steps.  Same here, so a step that forgot to write a variable
    # shows up as an empty one -- exactly like it would on the runner.
    if (Test-Path $env:GITHUB_ENV) {
        foreach ($line in (Get-Content $env:GITHUB_ENV)) {
            if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
                Set-Item -Path ("Env:" + $Matches[1]) -Value $Matches[2]
            }
        }
    }
    if (Test-Path $env:GITHUB_PATH) {
        foreach ($line in (Get-Content $env:GITHUB_PATH)) {
            if ($line.Trim() -eq '') { continue }
            $env:PATH = $line + ';' + $env:PATH
        }
    }
}

$scriptDir = Join-Path $RunnerTemp 'steps'
New-Item -ItemType Directory -Force -Path $scriptDir | Out-Null

$n = 0
foreach ($s in $steps) {
    if ($s.Run -eq '') {
        Write-Host ""
        Write-Host "-- SKIP $($s.Kind): $($s.Title)  (a `uses:` action; nothing to run locally)"
        continue
    }
    if ($Step.Count -gt 0) {
        $hit = $false
        foreach ($pat in $Step) {
            if ($s.Title -match $pat) { $hit = $true }
        }
        if (-not $hit) {
            Write-Host ""
            Write-Host "-- SKIP $($s.Title)  (not selected by -Step)"
            continue
        }
    }

    $n++
    Write-Host ""
    Write-Host "-- STEP $n : $($s.Title)"

    # Mirror the runner's shell wrapper: stop on error, exit with the
    # last native command's code.
    $body = @(
        '$ErrorActionPreference = ''stop'''
        $s.Run
        'if ((Test-Path -LiteralPath variable:\LASTEXITCODE)) { exit $LASTEXITCODE }'
    ) -join "`n"
    $file = Join-Path $scriptDir ("step_{0}.ps1" -f $n)
    [System.IO.File]::WriteAllText($file, $body, (New-Object System.Text.UTF8Encoding($false)))

    & $PwshExe -NoProfile -NonInteractive -File $file
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw ("step '$($s.Title)' failed with exit code $code " +
               "(script: $file)")
    }
    Import-RunnerEnv
}

Write-Host ""
Write-Host "ALL WORKFLOW STEPS PASSED (run locally, on this machine)"
Write-Host "Not covered: GitHub's own infrastructure (scheduler, runner image, network)."
