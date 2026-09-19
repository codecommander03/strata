# Builds the engine to WebAssembly and drops strata.js / strata.wasm into web/.
#
# The output of this script plus the three static files already in web/ is the
# entire deployment. There is no server, no build step on the host, and nothing
# to pay for: every visitor runs their own copy of the database in their own
# tab, against Emscripten's in-memory filesystem.
#
#   .\scripts\build_wasm.ps1                  release build
#   .\scripts\build_wasm.ps1 -Debug           assertions on, no optimisation
#   .\scripts\build_wasm.ps1 -Serve           build, then serve web/ locally

param(
    [switch]$Debug,
    [switch]$Serve,
    [int]$Port = 8080,
    [string]$EmsdkRoot = "C:\emsdk"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$web = Join-Path $root "web"

# --- locate emcc -------------------------------------------------------------

# em++ rather than emcc: emcc does not link libc++ for C++ sources, and the
# link fails on `operator new` with a message that does not obviously say so.
$emcc = Get-Command em++ -ErrorAction SilentlyContinue
if (-not $emcc) {
    $activate = Join-Path $EmsdkRoot "emsdk_env.ps1"
    if (-not (Test-Path $activate)) {
        throw @"
Emscripten not found.

  git clone https://github.com/emscripten-core/emsdk.git $EmsdkRoot
  cd $EmsdkRoot
  .\emsdk.bat install latest
  .\emsdk.bat activate latest

Then re-run this script. It is free and needs no account.
"@
    }
    Write-Host "activating emsdk from $EmsdkRoot"
    # emsdk_env writes its banner to stderr, and Windows PowerShell turns a
    # native command's stderr into a terminating error when ErrorActionPreference
    # is Stop. Silence the banner and relax the preference across the call.
    $env:EMSDK_QUIET = "1"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $activate 2>&1 | Out-Null
    } finally {
        $ErrorActionPreference = $previous
    }
    $emcc = Get-Command em++ -ErrorAction SilentlyContinue
    if (-not $emcc) {
        throw "emsdk_env.ps1 ran but em++ is still not on PATH"
    }
}

Write-Host "em++: $($emcc.Source)"

# --- sources -----------------------------------------------------------------

$sources = @(
    "src/page.cpp"
    "src/pager.cpp"
    "src/wal.cpp"
    "src/btree.cpp"
    "src/db.cpp"
    "src/sql/lexer.cpp"
    "src/sql/ast.cpp"
    "src/sql/parser.cpp"
    "src/sql/catalog.cpp"
    "src/sql/index_catalog.cpp"
    "src/sql/executor.cpp"
    "src/sql/session.cpp"
    "src/wasm/bindings.cpp"
) | ForEach-Object { Join-Path $root $_ }

foreach ($s in $sources) {
    if (-not (Test-Path $s)) { throw "missing source: $s" }
}

# Exported by name because nothing calls them from C++ — the linker would
# otherwise drop every one of them.
$exports = '["_strata_open","_strata_exec","_strata_parse","_strata_schema","_malloc","_free"]'

$flags = @(
    "-std=c++20"
    "-sWASM=1"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createStrataModule"
    "-sEXPORTED_FUNCTIONS=$exports"
    '-sEXPORTED_RUNTIME_METHODS=["cwrap","ccall"]'
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=33554432"
    "-sENVIRONMENT=web"
    "-sFILESYSTEM=1"
    # No exceptions and no RTTI: decision 008 returns Status rather than
    # throwing, precisely so this build can drop both.
    "-fno-exceptions"
    "-fno-rtti"
    "-I$(Join-Path $root 'include')"
)

if ($Debug) {
    $flags += @("-O0", "-g", "-sASSERTIONS=2", "-sSAFE_HEAP=1")
} else {
    $flags += @("-O3", "-flto", "-sASSERTIONS=0", "--closure", "0")
}

$output = Join-Path $web "strata.js"

Write-Host "building $(if ($Debug) { 'debug' } else { 'release' }) -> $output"

# emcc reports progress on stderr even on success, which Windows PowerShell
# turns into a terminating error under ErrorActionPreference = Stop. Judge the
# build by its exit code, which is the only thing that actually says.
$previous = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    & $emcc.Source @flags @sources -o $output 2>&1 | ForEach-Object { Write-Host $_ }
} finally {
    $ErrorActionPreference = $previous
}
if ($LASTEXITCODE -ne 0) { throw "emcc failed with exit code $LASTEXITCODE" }

$wasm = Join-Path $web "strata.wasm"
$js = Get-Item $output
$wa = Get-Item $wasm
Write-Host ""
Write-Host ("  strata.js    {0,8:N0} bytes" -f $js.Length)
Write-Host ("  strata.wasm  {0,8:N0} bytes" -f $wa.Length)
Write-Host ("  total        {0,8:N0} bytes ({1:N0} KB)" -f ($js.Length + $wa.Length), (($js.Length + $wa.Length) / 1KB))
Write-Host ""
Write-Host "deploy: upload web/ to any static host. No build command, no backend."

if ($Serve) {
    Write-Host ""
    Write-Host "serving http://localhost:$Port  (ctrl-c to stop)"
    Push-Location $web
    try {
        python -m http.server $Port
    } finally {
        Pop-Location
    }
}
