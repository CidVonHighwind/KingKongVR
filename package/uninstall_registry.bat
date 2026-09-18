@echo off
rem Removes the registry keys written by install_registry.bat. Right-click it
rem and choose "Run as administrator".
rem
rem Only use it if you ran install_registry.bat: on a game installed with its
rem own installer these are the installer's keys.
setlocal
set "GUID={111E336D-30BF-4CD4-8D69-4541732AFB27}"

net session >nul 2>&1
if errorlevel 1 goto noadmin

reg delete "HKLM\SOFTWARE\Ubisoft\KingKong\%GUID%" /f /reg:32 >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\%GUID%" /f /reg:32 >nul 2>&1
echo The registry keys are removed.
goto end

:noadmin
echo This needs administrator rights: right-click uninstall_registry.bat and
echo choose "Run as administrator".

:end
pause
