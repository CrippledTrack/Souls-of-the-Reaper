# Compiles launcher\Launcher.ps1 into a standalone "SoulsOfTheReaper.exe" using
# ps2exe, embedding the v6 HTML UI + the WebView2 SDK DLLs so the result is a
# single, self-contained launcher exe. Meant to sit at the repo/release root
# next to apply_config.ps1, game\, and diablo3.exe.

$ErrorActionPreference = 'Stop'
$LauncherDir = $PSScriptRoot
$DistDir = Join-Path $LauncherDir "dist"
if (-not (Test-Path $DistDir)) { New-Item -ItemType Directory -Path $DistDir -Force | Out-Null }

$shippedHtml = Join-Path $LauncherDir "v6_borderless_final.html"

$sources = @{
    'v6_borderless_final.html'              = $shippedHtml
    'ico_launcher.ico'                       = Join-Path $LauncherDir "ico_launcher.ico"
    'lib\Microsoft.Web.WebView2.Core.dll'    = Join-Path $LauncherDir "lib\Microsoft.Web.WebView2.Core.dll"
    'lib\Microsoft.Web.WebView2.WinForms.dll'= Join-Path $LauncherDir "lib\Microsoft.Web.WebView2.WinForms.dll"
    'lib\WebView2Loader.dll'                 = Join-Path $LauncherDir "lib\WebView2Loader.dll"
}
foreach ($kv in $sources.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $kv.Value)) { throw "Missing required source file: $($kv.Key) -> $($kv.Value)" }
}

if (-not (Get-Module -ListAvailable ps2exe)) {
    throw "ps2exe module not found. Install with: Install-Module ps2exe -Scope CurrentUser -Force"
}
Import-Module ps2exe -ErrorAction Stop

$embed = @{
    '%TEMP%\SotR_Launcher\v6_borderless_final.html'               = $sources['v6_borderless_final.html']
    '%TEMP%\SotR_Launcher\ico_launcher.ico'                        = $sources['ico_launcher.ico']
    '%TEMP%\SotR_Launcher\lib\Microsoft.Web.WebView2.Core.dll'     = $sources['lib\Microsoft.Web.WebView2.Core.dll']
    '%TEMP%\SotR_Launcher\lib\Microsoft.Web.WebView2.WinForms.dll' = $sources['lib\Microsoft.Web.WebView2.WinForms.dll']
    '%TEMP%\SotR_Launcher\lib\WebView2Loader.dll'                  = $sources['lib\WebView2Loader.dll']
}

Write-Host "==> Embedding $($embed.Count) files:" -ForegroundColor Cyan
foreach ($kv in $embed.GetEnumerator()) {
    $sizeKb = [math]::Round((Get-Item -LiteralPath $kv.Value).Length / 1KB, 0)
    Write-Host ("    {0,-55} <- {1} ({2} KB)" -f $kv.Key, $kv.Value, $sizeKb)
}

$inputScript = Join-Path $LauncherDir "Launcher.ps1"
$outExe = Join-Path $DistDir "SoulsOfTheReaper.exe"

Write-Host "`n==> Compiling -> $outExe" -ForegroundColor Cyan
Invoke-ps2exe -inputFile $inputScript -outputFile $outExe `
    -iconFile (Join-Path $LauncherDir "ico_launcher.ico") -embedFiles $embed `
    -title "Souls of the Reaper" -description "Launcher for the Souls of the Reaper Diablo III PC port" `
    -company "D3ReXPort" -product "Souls of the Reaper" -copyright "Fan-made port. Not affiliated with Blizzard Entertainment." `
    -version "1.0.0.0" -x64 -STA -noConsole -DPIAware -requireAdmin:$false

if (-not (Test-Path -LiteralPath $outExe)) { throw "ps2exe did not produce $outExe" }
Write-Host "==> Build OK: $outExe ($([math]::Round((Get-Item $outExe).Length/1MB,1)) MB)" -ForegroundColor Green
Write-Host "    Copy it to the repo/release root (next to apply_config.ps1, game\, diablo3.exe) to run it." -ForegroundColor Yellow
