<#
.SYNOPSIS
  Merge launcher-managed cvars into diablo3.toml WITHOUT clobbering user keybind
  remaps (which the in-game F4 settings overlay persists to the same file).
  Re-writes only the managed keys; preserves every other line (e.g. keybind_*).
#>
param(
  [Parameter(Mandatory)][string]$Toml,
  [int]$Fps = 60,
  [string]$Vsync = "true",
  [string]$Rtp = "rov",
  [string]$MnkMode = "true",
  [string]$MnkMouse = "false",
  [string]$CursorVisible = "true",
  [string]$Fullscreen = "false",
  [string]$DepthFix = "false",   # obsoleto (s19): sin efecto, se acepta por compat
  # s24: post-proceso de escalado/nitidez en el PRESENT final (no toca la
  # resolucion interna del render, que sigue siendo 1280x720). Solo cambia como
  # se escala esa imagen a la ventana/pantalla. Valores: bilinear | cas | fsr.
  #   fsr = FidelityFX Super Resolution 1.0 (EASU + RCAS) -> mejor para 720p->1080p/1440p
  #   cas = Contrast Adaptive Sharpening (nitidez, hasta 2x)
  #   bilinear = stretch plano (filtro desactivado)
  # Funciona igual con ROV y RTV (es host-side, sobre el framebuffer final).
  # NOTA: la nitidez (present_cas_additional_sharpness / present_fsr_sharpness_reduction)
  # NO se maneja aca a proposito -> es "user-owned": se ajusta desde el overlay F4
  # in-game (categoria UI/Presenter) + "Save to config", y este script la PRESERVA
  # (igual que los keybinds). Asi tus ajustes de nitidez sobreviven al launcher.
  [ValidateSet('bilinear','cas','fsr')]
  [string]$PresentEffect = "fsr",
  # Resolucion interna de render (1 = 1280x720 nativo del backbuffer 360; 2/3 =
  # el motor dibuja cada draw a 2x/3x por eje = 4x/9x pixeles). Mas nitidez REAL
  # que el present_effect (no es un upscaler heuristico). s27: 2x CONFIRMADO por
  # el usuario sin bugs visuales en RTV 60fps + FSR (los fixes de iluminacion
  # s18-s21 siguen intactos con el EDRAM escalado). Cuesta ~4x/9x GPU segun el
  # eje -> a mayor fps target, mas probable quedarse GPU-bound (ver s26: 2x a
  # 120fps RTV llevo la GPU a 99%). Default 1x = el mas liviano.
  [ValidateSet(1, 2, 3)]
  [int]$ResScale = 1,
  # Idioma del JUEGO. D3 lo lee via ExGetXConfigSetting(XCONFIG_USER_LANGUAGE)
  # -> cvar user_language (1=en 2=ja 3=de 4=fr 5=es 6=it 7=ko 8=zh 9=pt 11=pl
  # 12=ru 14=tr; ver xam_locale.cpp xeXamGetLanguageString). user_country
  # desambigua la variante regional (esES vs esMX, ptBR vs ptPT, zhCN vs zhTW,
  # enUS vs enGB). Al arrancar, D3 sondea TODOS los <locale>_Common.cpk y usa el
  # que exista para el idioma pedido; si el CPK de ese idioma no esta instalado,
  # cae de forma limpia a ingles (como el viejo enGB->enUS). Por eso el launcher
  # solo ofrece los idiomas cuyos CPKs realmente estan en game/CPKs/. Defaults =
  # ingles US (1/103), identico al comportamiento previo cuando no se pasan.
  [int]$UserLanguage = 1,
  [int]$UserCountry = 103
)

# Keys managed by the launcher (everything else in the toml is preserved as-is).
$managed = [ordered]@{
  'render_target_path_d3d12' = '"' + $Rtp + '"'
  # s19: ownership mode 8 = herencia cross-map clear-like (arregla la iluminacion
  # del suelo en RTV). Solo lo lee el path RTV; inofensivo en ROV.
  'rtv_color_depth_ownership_mode' = '8'
  # s21: VS de draws sin clipping ejecutado en CPU para estimar el area REAL
  # escrita (default original de Xenia). Sin esto, los quads screen-space claman
  # TODO el EDRAM y la escena hace round-trips lossy por otros buffers cada
  # frame -> paneles negros/desplazados en el humo (BUG B) y fugas del buffer
  # de siluetas (lineas rojas/azules, BUG A) en RTV. Inofensivo en ROV.
  'execute_unclipped_draw_vs_on_cpu' = 'true'
  # Resolucion interna de render, elegida en el launcher (default 1x, ver
  # $ResScale arriba; 2x confirmado sin bugs en RTV, s27). Forzado como key
  # MANAGED para que un cambio accidental en el overlay F4 no la deje pegada
  # en el toml sin que el launcher se entere (antes era "user cvar" y asi paso
  # una regresion de gpu al 99%/-15fps en RTV, s26).
  'draw_resolution_scale_x'  = "$ResScale"
  'draw_resolution_scale_y'  = "$ResScale"
  'd3_frame_limit'           = "$Fps"
  'vsync'                    = $Vsync
  'mnk_mode'                 = $MnkMode
  'mnk_mouse'                = $MnkMouse
  'mnk_cursor_visible'       = $CursorVisible
  # Arranca la ventana en pantalla completa sin bordes (cvar del SDK,
  # kRequiresRestart -> se lee al crear la ventana, por eso lo escribe el
  # launcher antes de lanzar). false = ventana.
  'fullscreen'               = $Fullscreen
  # s24: filtro de escalado/nitidez del present final (cvar del SDK rexui,
  # kRequiresRestart). present_effect="fsr" gana detalle real al subir de 720p;
  # ="cas" solo nitidez; ="bilinear" desactiva el filtro. La INTENSIDAD de la
  # nitidez NO se maneja aca (se ajusta desde el overlay F4 y se preserva).
  'present_effect'                 = '"' + $PresentEffect + '"'
  # Idioma del juego (ver params arriba). Managed para que el launcher lo controle
  # de forma explicita; el kernel ya trae estos cvars (defaults 1/103 = ingles US).
  'user_language'                  = "$UserLanguage"
  'user_country'                   = "$UserCountry"
}
# depth_float24_*: probados SIN efecto (s18/s19) - se limpian del toml si quedaron.
$depthKeys = @('depth_float24_convert_in_pixel_shader', 'depth_float24_round')

$ownedKeys = @($managed.Keys) + $depthKeys
$preserved = @()
if (Test-Path -LiteralPath $Toml) {
  foreach ($line in Get-Content -LiteralPath $Toml) {
    $t = $line.Trim()
    if ($t -eq '') { continue }
    if ($t.StartsWith('#')) { continue }
    $k = (($t -split '=', 2)[0]).Trim()
    if ($ownedKeys -contains $k) { continue }
    $preserved += $line
  }
}

$out = @('# Managed by SoulsOfTheReaper.exe. Your F4 overlay remaps are preserved below.')
foreach ($k in $managed.Keys) { $out += "$k = $($managed[$k])" }
if ($preserved.Count -gt 0) {
  $out += ''
  $out += '# --- User remaps / cvars (F4 overlay) ---'
  $out += $preserved
}

Set-Content -LiteralPath $Toml -Value $out -Encoding utf8
Write-Host ("Config: {0}fps vsync={1} render={2} mnk_mode={3} mouse={4} cursor_visible={5} fullscreen={6} present={7} res_scale={8}x lang={9} country={10}" -f `
  $Fps, $Vsync, $Rtp, $MnkMode, $MnkMouse, $CursorVisible, $Fullscreen, $PresentEffect, $ResScale, $UserLanguage, $UserCountry)
