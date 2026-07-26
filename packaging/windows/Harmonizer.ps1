$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Support = Join-Path $env:LOCALAPPDATA "Harmonizer"
$Port = if ($env:HARMONIZER_WEB_PORT) { $env:HARMONIZER_WEB_PORT } else { "8794" }
$Url = "http://127.0.0.1:$Port/"

try {
    Invoke-WebRequest "$($Url)health" -UseBasicParsing | Out-Null
    Start-Process $Url
    exit 0
} catch {
    # No existing server is listening.
}

New-Item -ItemType Directory -Force -Path (Join-Path $Support "web") | Out-Null
Copy-Item (Join-Path $Root "web\index.html") (Join-Path $Support "web\index.html") -Force
$RuntimePaths = @($Root)
if (Test-Path "C:\msys64\ucrt64\bin") {
    $RuntimePaths += "C:\msys64\ucrt64\bin"
}
$env:Path = "$($RuntimePaths -join ';');$env:Path"
Set-Location $Support

$Process = Start-Process -FilePath (Join-Path $Root "harmonizer_web.exe") -ArgumentList "--port", $Port -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $Support "harmonizer.log") -RedirectStandardError (Join-Path $Support "harmonizer-error.log")
try {
    for ($Attempt = 0; $Attempt -lt 60; $Attempt++) {
        try {
            Invoke-WebRequest "$($Url)health" -UseBasicParsing | Out-Null
            Start-Process $Url
            Wait-Process -Id $Process.Id
            exit $Process.ExitCode
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    throw "Harmonizer did not start. See $Support\harmonizer-error.log"
} finally {
    if (-not $Process.HasExited) { Stop-Process -Id $Process.Id }
}
