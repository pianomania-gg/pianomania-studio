<#
.SYNOPSIS
    Build the headless Pianomania WebAssembly converter (pm-converter.js/.wasm) and
    copy the artifacts into WebApp/wwwroot/wasm for the beta-portal Converter tab.

.DESCRIPTION
    Prerequisites (install once, pin versions — emsdk/Qt-wasm version skew is the
    #1 cause of link failures):
      * emsdk (Emscripten) 3.1.56  — the version Qt 6.8 is built against
      * Qt 6.8.3 for WebAssembly (single-threaded)
    NOTE: The converter retains Qt 6.8.3 because its Emscripten toolchain is pinned.
    The desktop and release build use Qt 6.10.2. The headless converter excludes
    GUI/QML modules, where most newer-Qt breakage lives.

.PARAMETER QtWasmDir
    Path to the Qt wasm prefix (e.g. C:\Qt\6.8.3\wasm_singlethread).

.EXAMPLE
    . C:\emsdk\emsdk_env.ps1
    .\buildscripts\wasm\build.ps1 -QtWasmDir C:\Qt\6.8.3\wasm_singlethread
#>
param(
    [Parameter(Mandatory = $true)] [string] $QtWasmDir
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$museScoreRoot = (Resolve-Path (Join-Path $here '..\..')).Path
$repoRoot = (Resolve-Path (Join-Path $museScoreRoot '..')).Path
$buildDir = Join-Path $museScoreRoot 'build.wasm'
$webAppWasmDir = Join-Path $repoRoot 'WebApp\wwwroot\wasm'

if (-not (Get-Command emcmake -ErrorAction SilentlyContinue)) {
    throw "emcmake not found. Run emsdk_env.ps1 first."
}
if (-not $env:EMSDK) {
    throw "EMSDK env var not set. Run emsdk_env.ps1 first."
}

$emscriptenToolchain = Join-Path $env:EMSDK 'upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake'

# --- Stage fonts for embedding into MEMFS (--embed-file) ---
# Qt "big resource" qrc entries are unreadable in wasm (QFile size/read return
# garbage), so the engraving fonts are served from MEMFS instead (the wasm
# filesystem redirects ":/fonts/..." to "/fonts/..."). Stage a copy of the fonts
# tree with each music font's metadata normalized to "metadata.json" (sources use
# names like leland_metadata.json), matching what EngravingFont expects.
$fontsStage = Join-Path $buildDir 'fontstage'
if (Test-Path $fontsStage) { Remove-Item -Recurse -Force $fontsStage }
New-Item -ItemType Directory -Force -Path $fontsStage | Out-Null
Copy-Item -Recurse -Force (Join-Path $museScoreRoot 'fonts\*') $fontsStage
foreach ($dir in Get-ChildItem -Directory $fontsStage) {
    $meta = Join-Path $dir.FullName 'metadata.json'
    if (-not (Test-Path $meta)) {
        $alt = Get-ChildItem $dir.FullName -Filter '*_metadata.json' | Select-Object -First 1
        if ($alt) { Copy-Item -Force $alt.FullName $meta }
    }
}
$fontsDir = $fontsStage

# Use Qt's own wasm toolchain file (qt.toolchain.cmake). It chains to Emscripten
# (via $EMSDK) AND wires up the cross find-roots + host tools so find_package(Qt6…)
# resolves — which a raw `emcmake` + Emscripten.cmake does NOT (it confines
# find_package to the cross-root, hiding Qt under C:\Qt).
$qtToolchain = (Join-Path $QtWasmDir 'lib\cmake\Qt6\qt.toolchain.cmake').Replace('\', '/')

# Host Qt kit (same version) provides host moc/rcc/qmlcachegen. Auto-detect the
# sibling kit next to the wasm prefix (e.g. mingw_64 / msvc2019_64).
$qtVersionDir = Split-Path $QtWasmDir -Parent
$hostKit = Get-ChildItem $qtVersionDir -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'bin\moc.exe') } | Select-Object -First 1
if (-not $hostKit) {
    throw "No host Qt kit (with bin\moc.exe) found under $qtVersionDir. Install a desktop Qt 6.8.3 kit (e.g. mingw_64)."
}
$qtHostPath = $hostKit.FullName.Replace('\', '/')
Write-Host "Using Qt host kit: $qtHostPath"

# On Windows the Qt wasm kit ships qmake as BOTH an extensionless Unix shell
# script (qmake6) and a Windows wrapper (qmake6.bat). CMake's find_program picks
# the extensionless script, which Windows can't execute, so point MuseScore's
# qmake vars explicitly at the .bat. Forward slashes — CMake dislikes backslashes.
$qmakeBat = (Join-Path $QtWasmDir 'bin\qmake6.bat').Replace('\', '/')

Write-Host "==> Configuring (headless wasm profile)"
cmake -S $museScoreRoot -B $buildDir -G "MinGW Makefiles" `
    -C (Join-Path $museScoreRoot 'buildscripts\wasm\wasm-headless.cmake') `
    "-DCMAKE_TOOLCHAIN_FILE=$qtToolchain" `
    "-DQT_HOST_PATH=$qtHostPath" `
    "-DCMAKE_PREFIX_PATH=$QtWasmDir" `
    "-DQMAKE=$qmakeBat" `
    "-DQT_QMAKE_EXECUTABLE=$qmakeBat" `
    "-DEMCC_CMAKE_TOOLCHAIN=$emscriptenToolchain" `
    "-DEMCC_EMBED_FONTS_DIR=$fontsDir" `
    -DCMAKE_BUILD_TYPE=Release

Write-Host "==> Building"
# Build the converter's module set explicitly, then the app. We can't use a plain
# `--target MuseScoreStudio` because MinGW Makefiles doesn't build the modules it
# consumes via $<TARGET_OBJECTS:...> as dependencies; and we can't do a full build
# because peripheral targets (unit tests, stale module stubs, the print module's
# QPrinter) don't compile for wasm. So name exactly what the headless app embeds.
$targets = @(
    'muse_global', 'muse_draw', 'muse_network', 'muse_diagnostics',
    'muse_actions', 'muse_accessibility', 'muse_midi', 'muse_mpe',
    'engraving', 'context', 'commonscene',
    'beatroot', 'iex_mei', 'iex_midi', 'pmwasm',
    'MuseScoreStudio'
)
cmake --build $buildDir --parallel --target $targets

Write-Host "==> Collecting artifacts"
New-Item -ItemType Directory -Force -Path $webAppWasmDir | Out-Null
$js = Get-ChildItem -Path $buildDir -Recurse -Filter '*.js' |
    Where-Object { $_.FullName -match 'public_html' } | Select-Object -First 1
if (-not $js) {
    throw "Could not find emitted .js in $buildDir. Check the app target output."
}
$wasm = [System.IO.Path]::ChangeExtension($js.FullName, '.wasm')
Copy-Item -Force $js.FullName (Join-Path $webAppWasmDir 'pm-converter.js')
Copy-Item -Force $wasm (Join-Path $webAppWasmDir 'pm-converter.wasm')

Write-Host "==> Done. Artifacts in $webAppWasmDir"
Get-ChildItem $webAppWasmDir | Format-Table Name, Length
