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
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'RelWithDebInfo',
    [string]$GameDir = (Join-Path $PSScriptRoot '..\game'),
    [string]$UserDataRoot = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'diablo3'),
    [ValidateSet(1, 2, 3)]
    [int]$ResScale = 1
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$suffix = $Config.ToLowerInvariant()
$buildName = if ($ExtraFeatures) { "win-amd64-extras-$suffix" } else { "win-amd64-$suffix" }
$buildDir = Join-Path $root "port\out\build\$buildName"
$exe = Join-Path $buildDir 'diablo3.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    $featureFlag = if ($ExtraFeatures) { ' -ExtraFeatures' } else { '' }
    throw "Executable missing. Build with: pwsh -File port/build.ps1 -Config $Config$featureFlag"
}
$game = (Resolve-Path -LiteralPath $GameDir).Path
if (-not (Test-Path -LiteralPath (Join-Path $game 'Default.xex'))) {
    throw 'GameDir must contain Default.xex.'
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
if ($PSBoundParameters.ContainsKey('ResScale')) {
    $arguments += @("--draw_resolution_scale_x=$ResScale", "--draw_resolution_scale_y=$ResScale")
    if ($ExtraFeatures) { $arguments += '--pc_use_saved_render_scale=false' }
}
$process = Start-Process -FilePath $exe -WorkingDirectory $buildDir -ArgumentList $arguments -PassThru -Wait
exit $process.ExitCode
