# Stage 1 gate: kill the writer mid-transaction, repeatedly, and prove that
# every committed key survives and the tree is structurally intact afterwards.
#
# The writer prints "committed N" only after the commit fsync returns, so the
# last line this script sees is a lower bound on what recovery must produce.
# Anything less than that is lost data; more is fine, because the writer may
# have committed again between our last read and the kill.
#
#   .\scripts\chaos.ps1 -Kills 40 -Keys 20000 -Batch 200

param(
    [int]$Kills = 20,
    [int]$Keys = 20000,
    [int]$Batch = 200,
    [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\$Config\strata_crash.exe"
if (-not (Test-Path $exe)) {
    throw "not built: $exe"
}

$work = Join-Path $env:TEMP ("strata_chaos_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$db = Join-Path $work "chaos.db"
$log = Join-Path $work "writer.out"

Write-Host "strata chaos harness"
Write-Host "  database : $db"
Write-Host "  kills    : $Kills"
Write-Host ""

$survived = 0
$highWater = 0

for ($i = 1; $i -le $Kills; $i++) {
    if (Test-Path $log) { Remove-Item $log -Force }

    $proc = Start-Process -FilePath $exe `
        -ArgumentList @("write", $db, $Keys, $Batch) `
        -RedirectStandardOutput $log `
        -PassThru -WindowStyle Hidden

    # Let it get a few transactions in, then kill it at an unpredictable point
    # so the cut lands in different phases of the commit path across runs.
    Start-Sleep -Milliseconds (Get-Random -Minimum 60 -Maximum 400)

    $killed = $false
    if (-not $proc.HasExited) {
        taskkill /F /PID $proc.Id 2>&1 | Out-Null
        $killed = $true
    }
    $proc.WaitForExit()

    # The last durable commit the writer announced before we cut it down.
    $committed = 0
    if (Test-Path $log) {
        $lines = Get-Content $log -ErrorAction SilentlyContinue
        foreach ($line in $lines) {
            if ($line -match '^committed (\d+)$') {
                $committed = [int]$Matches[1]
            }
        }
    }
    if ($committed -gt $highWater) { $highWater = $committed }

    $verify = & $exe verify $db $committed 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "FAILED on kill $i" -ForegroundColor Red
        Write-Host "  last announced commit : $committed keys"
        Write-Host "  verifier said         : $verify"
        exit 1
    }

    $survived++
    $state = if ($killed) { "killed" } else { "finished first" }
    Write-Host ("  kill {0,3}  {1,-15} committed>={2,-7} {3}" -f $i, $state, $committed, $verify)
}

Write-Host ""
Write-Host "PASSED" -ForegroundColor Green
Write-Host "  $survived of $Kills runs recovered with every committed key intact"
Write-Host "  high water mark: $highWater keys"
Remove-Item -Recurse -Force $work
