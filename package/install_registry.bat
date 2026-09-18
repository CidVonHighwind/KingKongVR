@echo off
rem Lets the game's SettingsApplication.exe start on a copied (not installed)
rem game: it refuses to run ("not properly installed") unless the game's
rem install location is in the registry.
rem
rem Put this file in the game folder (the one with kingkong9d.exe), then
rem right-click it and choose "Run as administrator". Or pass the folder:
rem   install_registry.bat "D:\Games\King Kong"
rem
rem The settings application is 32-bit, so the keys go into the 32-bit view of
rem HKLM (WOW6432Node). uninstall_registry.bat removes them again.
setlocal
set "GUID={111E336D-30BF-4CD4-8D69-4541732AFB27}"
set "GAME=%~1"
if not defined GAME set "GAME=%~dp0"
if not "%GAME:~-1%"=="\" set "GAME=%GAME%\"

if not exist "%GAME%kingkong9d.exe" goto nogame
net session >nul 2>&1
if errorlevel 1 goto noadmin

rem The value keeps its trailing backslash; the doubled one stops reg.exe from
rem reading \" as an escaped quote.
reg add "HKLM\SOFTWARE\Ubisoft\KingKong\%GUID%" /v InstallLocation /t REG_SZ /d "%GAME%\" /f /reg:32 >nul
if errorlevel 1 goto failed
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\%GUID%" /v InstallLocation /t REG_SZ /d "%GAME%\" /f /reg:32 >nul
if errorlevel 1 goto failed

echo The game folder is registered:
echo   "%GAME%"
echo SettingsApplication.exe should start now.
goto end

:nogame
echo kingkong9d.exe was not found in
echo   "%GAME%"
echo Put this file in the game folder, or pass the game folder as argument.
goto end

:noadmin
echo This needs administrator rights: right-click install_registry.bat and
echo choose "Run as administrator".
goto end

:failed
echo Writing the registry failed.

:end
pause
