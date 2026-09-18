# Play on the desktop with the headset simulated at the Quest 3's resolution,
# and the game window showing a magnified part of one eye image. Lets rendering
# artefacts be seen and captured (F2) without putting the headset on.
#
#   powershell -File tools\desktop_test.ps1 [-Zoom 3] [-X 0.5] [-Y 0.5] [-Scale 1.0]
#
# -X/-Y are the centre of the magnified part in the eye image (0..1, 0.5 =
# middle). The game's kkvr.ini is restored when the game closes.
param(
  [double]$Zoom = 3.0,
  [double]$X = 0.5,
  [double]$Y = 0.5,
  [double]$Scale = 1.0,
  # The eye image the simulated headset asks for (Quest 3 through Virtual Desktop).
  [int]$HeadsetWidth = 3072,
  [int]$HeadsetHeight = 3264,
  [double]$HeadsetFov = 94.0,
  # Degrees per numpad press for the simulated head (Shift = 4x).
  [double]$Step = 0.5
)

. (Join-Path $PSScriptRoot "autotest\ini.ps1")

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$game = Join-Path $root "game"
if (Get-Process kingkong9d -ErrorAction SilentlyContinue) { throw "the game is already running" }

Copy-Item (Join-Path $root "build\Release\d3d9.dll") (Join-Path $game "d3d9.dll") -Force

$iniPath = Join-Path $game "kkvr.ini"
$backup = Join-Path $game "kkvr.ini.desktop-backup"
if (Test-Path $backup) {
  Copy-Item $backup $iniPath -Force
  Remove-Item $backup
  Write-Output "restored kkvr.ini from a previous interrupted run"
}
# The game writes its profile and progress into KingKong.sav, and a scripted
# run walks through menus: keep a copy and put it back afterwards (a run
# overwrote the user's save, 2026-09-17).
$savePath = Join-Path $game "KingKong.sav"
$saveBackup = Join-Path $game "KingKong.sav.run-backup"
if (Test-Path $savePath) { Copy-Item $savePath $saveBackup -Force }
$original = [IO.File]::ReadAllText($iniPath)
Copy-Item $iniPath $backup -Force

try {
  $text = $original
  $text = Set-IniValue $text "vr" "simulate_headset" "1"
  $text = Set-IniValue $text "test" "sim_headset_width" "$HeadsetWidth"
  $text = Set-IniValue $text "test" "sim_headset_height" "$HeadsetHeight"
  $text = Set-IniValue $text "test" "sim_headset_fov" "$HeadsetFov"
  $text = Set-IniValue $text "test" "script" ""
  # The head stands still: only the numpad keys move it.
  $text = Set-IniValue $text "test" "sim_head_amplitude" "0"
  $text = Set-IniValue $text "test" "sim_head_rotation" "0"
  $text = Set-IniValue $text "test" "sim_head_step" "$Step"
  # The frame-bisect keys ('-', '=', F11, F12): off for players.
  $text = Set-IniValue $text "test" "debug_keys" "1"
  $text = Set-IniValue $text "render" "render_scale" "$Scale"
  $text = Set-IniValue $text "display" "preview_zoom" "$Zoom"
  $text = Set-IniValue $text "display" "preview_zoom_x" "$X"
  $text = Set-IniValue $text "display" "preview_zoom_y" "$Y"
  [IO.File]::WriteAllText($iniPath, $text)

  Write-Output "eye images at the Quest 3's pixel density (render_scale $Scale), desktop view zoomed ${Zoom}x at ($X, $Y)"
  Write-Output "head stands still; F6 = yaw, F7 = pitch, F9 = roll by $Step deg (Shift = the other way), F10 = straight ahead"
  Write-Output "(numpad 4/6/8/2/7/9/5 does the same); pause the game with Start to freeze the scene"
  Write-Output "F2 = capture, F3/F4 = window size/distance, close the game to restore kkvr.ini"
  $p = Start-Process -FilePath (Join-Path $game "PlayKingKong.bat") -WorkingDirectory $game -PassThru
  $p.WaitForExit()
  Start-Sleep -Milliseconds 500
  Get-Process kingkong9d -ErrorAction SilentlyContinue | Wait-Process -Timeout 600 -ErrorAction SilentlyContinue
} finally {
  [IO.File]::WriteAllText($iniPath, $original)
  if (Test-Path $saveBackup) {
    Copy-Item $saveBackup $savePath -Force
    Remove-Item $saveBackup
    Write-Output "KingKong.sav restored"
  }
  if (Test-Path $backup) { Remove-Item $backup }
  Write-Output "kkvr.ini restored"
}
