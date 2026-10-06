<#
.SYNOPSIS
  Launch a Windows source build, optionally enabling the modified-game build.
.EXAMPLE
  pwsh -File scripts/run_windows.ps1 -ExtraFeatures -Config RelWithDebInfo
.EXAMPLE
  pwsh -File scripts/run_windows.ps1 -ExtraFeatures -ResScale 2
#>
param(
    [switch]$ExtraFeatures,
    [ValidateSet("tu2")]
    [string]$TitleUpdate,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'RelWithDebInfo',
    [string]$GameDir = (Join-Path $PSScriptRoot '..\game'),
    [string]$UserDataRoot = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'diablo3'),
    [ValidateSet(1, 2, 3)]
    [int]$ResScale = 1,
    [ValidateSet("Windowed", "Borderless")]
    [string]$WindowMode
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$suffix = $Config.ToLowerInvariant()
$buildName = if ($ExtraFeatures) { "win-amd64-extras-$suffix" } else { "win-amd64-$suffix" }
if ($TitleUpdate) {
    $variant = if ($ExtraFeatures) { 'tu2-extras' } else { 'tu2' }
    $buildName = "win-amd64-$variant-$suffix"
    if (-not $PSBoundParameters.ContainsKey('UserDataRoot')) { $UserDataRoot += '-tu2' }
}
$buildDir = Join-Path $root "port\out\build\$buildName"
$exe = Join-Path $buildDir 'diablo3.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    $updateFlag = if ($TitleUpdate) { " -TitleUpdate $TitleUpdate -GameDir `"$GameDir`"" } else { "" }
    $featureFlag = if ($ExtraFeatures) { ' -ExtraFeatures' } else { '' }
    throw "Executable missing. Build with: pwsh -File port/build.ps1 -Config $Config$featureFlag$updateFlag"
}
$game = (Resolve-Path -LiteralPath $GameDir).Path
if (-not (Test-Path -LiteralPath (Join-Path $game 'Default.xex'))) {
    throw 'GameDir must contain Default.xex.'
}
if ($TitleUpdate) {
    $expected = '447652ffa8abe4c7b8bed590a3887efc23e1181fd836b7a3192b8a2a37ddf80f'
    if ((Get-FileHash -LiteralPath (Join-Path $game 'Default.xex') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
        throw 'GameDir does not contain the verified USA TU2 executable.'
    }
}
$state = [System.IO.Path]::GetFullPath($UserDataRoot)
New-Item -ItemType Directory -Path $state -Force | Out-Null

# Start-Process joins arguments into a Windows command line. Quote path values
# explicitly so directories with spaces stay a single guest-runtime argument.
function ConvertTo-QuotedArgument([string]$Value) {
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}
$arguments = @('--game_data_root', (ConvertTo-QuotedArgument $game),
    '--user_data_root', (ConvertTo-QuotedArgument $state))
if ($TitleUpdate) { $arguments += @('--update_data_root', (ConvertTo-QuotedArgument $game)) }
if ($PSBoundParameters.ContainsKey('ResScale')) {
    $arguments += @("--draw_resolution_scale_x=$ResScale", "--draw_resolution_scale_y=$ResScale")
    if ($ExtraFeatures) { $arguments += '--pc_use_saved_render_scale=false' }
}
if ($PSBoundParameters.ContainsKey('WindowMode')) {
    $fullscreen = if ($WindowMode -eq 'Borderless') { 'true' } else { 'false' }
    $arguments += "--fullscreen=$fullscreen"
    if ($ExtraFeatures) { $arguments += '--pc_use_saved_window_mode=false' }
}
$process = Start-Process -FilePath $exe -WorkingDirectory $buildDir -ArgumentList $arguments -PassThru -Wait
exit $process.ExitCode
