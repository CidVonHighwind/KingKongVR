@echo off
rem Desktop test run: headset simulated at the Quest 3's resolution, the game
rem window shows one eye image magnified 3x (point sampled), so single rendered
rem pixels are visible. F2 captures. kkvr.ini is restored when the game closes.
rem
rem   PlayDesktopZoom.bat            3x in the middle of the eye image
rem   PlayDesktopZoom.bat 4 0.3 0.7  4x, centred at 30%% across, 70%% down
rem
rem The simulated head stands still. F6 = yaw, F7 = pitch, F9 = roll by 0.5
rem deg (Shift = the other way), F10 = straight ahead. F2 captures. Pause the
rem game (Start) to freeze the scene while comparing two captures.
setlocal
set ZOOM=%1
if "%ZOOM%"=="" set ZOOM=3
set X=%2
if "%X%"=="" set X=0.5
set Y=%3
if "%Y%"=="" set Y=0.5
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\desktop_test.ps1" -Zoom %ZOOM% -X %X% -Y %Y%
