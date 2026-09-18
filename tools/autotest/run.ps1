# Runs the game with a test script (see src/debug/autotest.h) and prints the log.
# The game starts through game\PlayKingKong.bat, exactly like the user starts it.
#
#   powershell -File tools\autotest\run.ps1 -Script tools\autotest\play.txt
#       [-Timeout 120] [-Ini "vr.simulate_headset=1","display.fps_limit=120"] [-Pattern "timing:|xr pose"]
#
# The game's kkvr.ini is restored afterwards. -Ini entries are
# "section.key=value" (e.g. "vr.simulate_headset=1"): the key is replaced in
# that section or added to it. A plain "key=value" means [test].
# Screenshots land in game\screenshots.
param(
  [Parameter(Mandatory = $true)][string]$Script,
  [int]$Timeout = 180,
  [string[]]$Ini = @(),
  [string]$Pattern = "autotest|screenshot|!!!|timing: |xr pose|xr \(simulated\)"
)

. (Join-Path $PSScriptRoot "ini.ps1")

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$game = Join-Path $root "game"
if (Get-Process kingkong9d -ErrorAction SilentlyContinue) { throw "the game is already running" }

Copy-Item (Join-Path $root "build\Release\d3d9.dll") (Join-Path $game "d3d9.dll") -Force
Copy-Item (Resolve-Path $Script) (Join-Path $game "autotest_script.txt") -Force

$iniPath = Join-Path $game "kkvr.ini"
$backup = Join-Path $game "kkvr.ini.autotest-backup"
# A run that was interrupted before restoring leaves its backup behind:
# restore the user's settings from it first.
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
  # "powershell -File" passes -Ini "a","b" as one string "a,b": split it.
  $overrides = @("script=autotest_script.txt") + ($Ini | ForEach-Object { $_ -split "," })
  foreach ($entry in $overrides) {
    $name, $value = $entry -split "=", 2
    $section = "test"
    $key = $name
    if ($name.Contains(".")) { $section, $key = $name -split "\.", 2 }
    $text = Set-IniValue $text $section $key $value
  }
  [IO.File]::WriteAllText($iniPath, $text, [Text.Encoding]::ASCII)

  $screens = Join-Path $game "screenshots"
  if (Test-Path $screens) { Get-ChildItem $screens -Filter *.png | Remove-Item -Force }

  # The .bat returns at once ("start"): wait for the game process it starts.
  Start-Process -FilePath (Join-Path $game "PlayKingKong.bat") -WorkingDirectory $game
  $p = $null
  for ($i = 0; $i -lt 50 -and -not $p; $i++) {
    Start-Sleep -Milliseconds 200
    $p = Get-Process kingkong9d -ErrorAction SilentlyContinue | Select-Object -First 1
  }
  if (-not $p) { throw "PlayKingKong.bat did not start the game" }
  if (-not $p.WaitForExit($Timeout * 1000)) {
    Write-Output "timeout after $Timeout s, killing the game"
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit(10000) | Out-Null
  }
} finally {
  [IO.File]::WriteAllText($iniPath, $original, [Text.Encoding]::ASCII)
  if (Test-Path $saveBackup) {
    Copy-Item $saveBackup $savePath -Force
    Remove-Item $saveBackup
    Write-Output "KingKong.sav restored"
  }
  Remove-Item $backup -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $game "autotest_script.txt") -ErrorAction SilentlyContinue
}
Select-String -Path (Join-Path $game "kkvr.log") -Pattern $Pattern | ForEach-Object { $_.Line }
