# Builds the release package: dist\KingKongVR-<version>.zip with what a player
# copies into the game folder, plus the README.
#
#   powershell -File tools\package.ps1 [-NoBuild]
#
# The version comes from project(... VERSION x.y.z) in CMakeLists.txt. The
# package holds no game files: players need their own copy of the game.
param([switch]$NoBuild)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")

$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch "project\(\s*kingkong_vr\s+VERSION\s+([0-9.]+)") { throw "no VERSION in CMakeLists.txt" }
$version = $Matches[1]

if (-not $NoBuild) {
  cmake -S $root -B (Join-Path $root "build") -A Win32 | Out-Null
  cmake --build (Join-Path $root "build") --config Release --target d3d9
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
$dll = Join-Path $root "build\Release\d3d9.dll"
if (-not (Test-Path $dll)) { throw "build\Release\d3d9.dll missing: build first" }

$name = "KingKongVR-$version"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $dll $stage
foreach ($bat in "PlayKingKong.bat", "install_registry.bat", "uninstall_registry.bat") {
  Copy-Item (Join-Path $root "package\$bat") $stage
}
Copy-Item (Join-Path $root "README.md") (Join-Path $stage "README.md")
# The DLL links the OpenXR loader and JsonCpp: their notices ship with it.
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") $stage
$license = Join-Path $root "LICENSE"
if (Test-Path $license) { Copy-Item $license $stage } else { Write-Warning "no LICENSE file: the package ships without one" }

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Output "$zip"
Get-ChildItem $stage | ForEach-Object { Write-Output ("  {0,-20} {1,10:N0} bytes" -f $_.Name, $_.Length) }
