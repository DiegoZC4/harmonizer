$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildBackends = if ($args -contains "--all-backends") { "ON" } else { "OFF" }
$Msys = "C:\msys64"
$Version = "4.0.0"
$ExpectedHash = "24300f48a8014b7c863b573a9647e61b1b19b37875e2cdd92005e64c6424d266"
$Deps = Join-Path $Root ".deps"
$RubberBand = Join-Path $Deps "rubberband-$Version"

if (-not (Test-Path "$Msys\msys2_shell.cmd")) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "Install MSYS2 from https://www.msys2.org/ and rerun this script."
    }
    winget install --id MSYS2.MSYS2 --exact --accept-package-agreements --accept-source-agreements
}

if (-not (Test-Path "$RubberBand\rubberband\RubberBandLiveShifter.h")) {
    New-Item -ItemType Directory -Force -Path $Deps | Out-Null
    $Archive = Join-Path $Deps "rubberband-v$Version.tar.gz"
    Invoke-WebRequest "https://github.com/breakfastquay/rubberband/archive/refs/tags/v$Version.tar.gz" -OutFile $Archive
    if ((Get-FileHash $Archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedHash) {
        throw "Rubber Band checksum mismatch"
    }
    if (Test-Path $RubberBand) { Remove-Item -Recurse -Force $RubberBand }
    tar -xzf $Archive -C $Deps
}

$Cygpath = "$Msys\usr\bin\cygpath.exe"
$PosixRoot = (& $Cygpath -u $Root).Trim()
$PosixRubberBand = (& $Cygpath -u $RubberBand).Trim()
$Packages = @(
    "mingw-w64-ucrt-x86_64-gcc",
    "mingw-w64-ucrt-x86_64-cmake",
    "mingw-w64-ucrt-x86_64-ninja",
    "mingw-w64-ucrt-x86_64-pkgconf",
    "mingw-w64-ucrt-x86_64-portaudio",
    "mingw-w64-ucrt-x86_64-aubio"
) -join " "
$UpdateCommand = "pacman -Syu --noconfirm"
& "$Msys\msys2_shell.cmd" -defterm -no-start -ucrt64 -c $UpdateCommand
if ($LASTEXITCODE -ne 0) { throw "MSYS2 update failed" }

$BuildCommand = @"
pacman -Syu --noconfirm --needed $Packages &&
cmake -S '$PosixRoot' -B '$PosixRoot/build-windows' -G Ninja -DCMAKE_BUILD_TYPE=Release -DHARMONIZER_BUILD_BACKEND_LAB=$BuildBackends -DFETCHCONTENT_SOURCE_DIR_RUBBERBAND='$PosixRubberBand' &&
cmake --build '$PosixRoot/build-windows' &&
cmake --install '$PosixRoot/build-windows' --prefix '$PosixRoot/dist/Harmonizer-Windows-x64'
"@

& "$Msys\msys2_shell.cmd" -defterm -no-start -ucrt64 -c $BuildCommand
if ($LASTEXITCODE -ne 0) { throw "Windows build failed" }

$Dist = Join-Path $Root "dist\Harmonizer-Windows-x64"
Copy-Item "$Root\packaging\windows\Harmonizer.cmd" $Dist -Force
Copy-Item "$Root\packaging\windows\Harmonizer.ps1" $Dist -Force
Write-Host "Installed to $Dist"
if ($args -notcontains "--no-launch") { Start-Process "$Dist\Harmonizer.cmd" }
