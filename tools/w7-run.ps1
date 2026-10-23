# ============================================================
# w7-run.ps1 -- run a W7 pipeline headless, the PowerShell-friendly way
# ============================================================
#
# WHY THIS WRAPPER EXISTS
# ============================================================
#
#   w7_vm_lite.exe is a WIN32 (GUI subsystem) program. PowerShell does
#   NOT wait for GUI programs:
#
#       & w7_vm_lite.exe --run p.json      # returns in ~4 ms, program
#                                         # is still running
#
#   Measured (2026-10-24): the result file did not exist yet when the
#   line right after the call checked for it -- it appeared 3 seconds
#   later. That is the classic "checked before it was written" bug,
#   and it is why the CI smoke check failed on its first run.
#
#   cmd.exe DOES wait for GUI programs. PowerShell does not.
#
#   So: use this wrapper from PowerShell, or use Start-Process -Wait.
#
# WHAT IT ALSO FIXES
# ============================================================
#
#   It runs the program with the working directory set to the exe's
#   own directory. That matters because pcb_inspect reads its board
#   recipe by the RELATIVE path "recipes/demo.json" (same as the GUI
#   does), and the recipes are copied next to the exe at build time.
#   From anywhere else the relative path does not resolve and the
#   pipeline fails on the recipe -- not on the pipeline.
#
# USAGE
# ============================================================
#
#   powershell -ExecutionPolicy Bypass -File tools/w7-run.ps1 `
#              --run W7_VisionMasterLite/pipelines/smoke.json `
#              --image W6_PCB_DefectProject/images/baseline.png `
#              --out build/result.json
#
#   The exit code is the program's exit code:
#     0 = ran and judged PASS
#     1 = ran and judged FAIL
#     2 = bad usage / file unreadable / pipeline could not run
#     3 = ran, but a step reported an error
#
# PURE ASCII ON PURPOSE (see tools/ci.ps1 for the same note).
# ============================================================

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot

# Find the executable: the CI build tree first, then the local one.
$candidates = @(
    (Join-Path $RepoRoot 'build\ci\W7_VisionMasterLite\Release\w7_vm_lite.exe'),
    (Join-Path $RepoRoot 'W7_VisionMasterLite\build\Release\w7_vm_lite.exe')
)
$exe = $null
foreach ($c in $candidates) { if (Test-Path $c) { $exe = $c; break } }
if (-not $exe) {
    Write-Host "w7_vm_lite.exe not found. Looked in:" -ForegroundColor Red
    $candidates | ForEach-Object { Write-Host "   $_" -ForegroundColor Red }
    exit 2
}

$exeDir = Split-Path -Parent $exe
$outFile = Join-Path $exeDir 'w7_run_out.txt'

# ⚠ Relative paths on the command line are resolved against the
#   CALLER's working directory, not the exe's.
#   The program itself runs in the exe directory (the board recipes are
#   read by a relative path), so a relative --out would land next to
#   the exe instead of where the user meant it.
$relKeys = @('--run', '--out', '--image', '-r', '-o', '-i')
$newArgs = @()
for ($i = 0; $i -lt $args.Count; $i++) {
    $a = $args[$i]
    $newArgs += $a
    if ($relKeys -contains $a -and ($i + 1) -lt $args.Count) {
        $v = $args[$i + 1]
        if ($v -and -not [System.IO.Path]::IsPathRooted($v)) {
            $v = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $v))
        }
        $newArgs += $v
        $i++
    }
}

$p = Start-Process -FilePath $exe `
        -ArgumentList $newArgs `
        -WorkingDirectory $exeDir -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput $outFile -RedirectStandardError "$outFile.err"

# Echo what the program said, then hand back its exit code.
# ⚠ -Encoding UTF8: the program writes UTF-8 (Qt's default), and
#   PowerShell 5.1's Get-Content defaults to the ANSI code page --
#   so without it the Chinese output comes out as mojibake.
if (Test-Path $outFile) { Get-Content $outFile -Encoding UTF8 | Write-Host }
if (Test-Path "$outFile.err") {
    $e = Get-Content "$outFile.err" -Raw -Encoding UTF8
    if ($e -and $e.Trim().Length -gt 0) { Write-Host $e }
}

exit $p.ExitCode
