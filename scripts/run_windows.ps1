<#
.SYNOPSIS
  Launch a Windows source build. -ExtraFeatures switches on the optional PC features
  (compiled in by default); -Plain runs the build compiled without them.
.EXAMPLE
  pwsh -File scripts/run_windows.ps1 -ExtraFeatures -Config RelWithDebInfo
.EXAMPLE
  pwsh -File scripts/run_windows.ps1 -ExtraFeatures -ResScale 2
#>
param(
    [switch]$ExtraFeatures,
    [switch]$Plain,
    [ValidateSet("tu2")]
    [string]$TitleUpdate,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'RelWithDebInfo',
    [string]$GameDir = (Join-Path $PSScriptRoot '..\game'),
    [string]$UserDataRoot = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'diablo3'),
    [ValidateSet(1, 2, 3)]
    [int]$ResScale = 1,
    [ValidateSet("Windowed", "Borderless")]
    [string]$WindowMode,
    # ROV is the faithful path; auto/RTV render Diablo III's aliased lighting black on some GPUs.
    [ValidateSet("rov", "rtv")]
    [string]$RenderPath = "rov"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$suffix = $Config.ToLowerInvariant()
if ($Plain -and $ExtraFeatures) { throw '-ExtraFeatures needs the default build; drop -Plain.' }
$buildName = if ($Plain) { "win-amd64-plain-$suffix" } else { "win-amd64-$suffix" }
if ($TitleUpdate) {
    $variant = if ($Plain) { 'tu2-plain' } else { 'tu2' }
    $buildName = "win-amd64-$variant-$suffix"
    if (-not $PSBoundParameters.ContainsKey('UserDataRoot')) { $UserDataRoot += '-tu2' }
}
$buildDir = Join-Path $root "port\out\build\$buildName"
$exe = Join-Path $buildDir 'diablo3.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    $updateFlag = if ($TitleUpdate) { " -TitleUpdate $TitleUpdate -GameDir `"$GameDir`"" } else { "" }
    $featureFlag = if ($Plain) { ' -Plain' } else { '' }
    throw "Executable missing. Build with: pwsh -File port/build.ps1 -Config $Config$featureFlag$updateFlag"
}
$game = (Resolve-Path -LiteralPath $GameDir).Path
# TU2 is either an overlay (<game>\tu2 holds the patched executable and update CPKs) or a full staged copy.
$updateRoot = $game
$xex = Join-Path $game 'Default.xex'
if ($TitleUpdate -and (Test-Path -LiteralPath (Join-Path $game 'tu2\Default.xex'))) {
    $updateRoot = Join-Path $game 'tu2'
    $xex = Join-Path $updateRoot 'Default.xex'
}
if (-not (Test-Path -LiteralPath $xex)) {
    throw 'GameDir must contain Default.xex (for TU2: build or stage TU2 first).'
}
if ($TitleUpdate) {
    $expected = '447652ffa8abe4c7b8bed590a3887efc23e1181fd836b7a3192b8a2a37ddf80f'
    if ((Get-FileHash -LiteralPath $xex -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
        if ($updateRoot -eq $game) {
            throw "TU2 is not installed in $game (there is no tu2\Default.xex). Add it with: python scripts/build_client.py --title-update <Title Update 2 package> --game-dir `"$game`" --variants tu2"
        }
        throw 'The executable in GameDir\tu2 is not the verified USA TU2 executable.'
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
    '--user_data_root', (ConvertTo-QuotedArgument $state),
    "--render_target_path_d3d12=$RenderPath")
if ($ExtraFeatures) { $arguments += '--extra_features=true' }
if ($TitleUpdate) { $arguments += @('--update_data_root', (ConvertTo-QuotedArgument $updateRoot)) }
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
