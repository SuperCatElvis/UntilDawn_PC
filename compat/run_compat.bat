@echo off
cd /d "%~dp0"
echo Until Dawn native port - hardware compatibility test
echo ===================================================
echo Checks this PC against what the port needs and runs the port's own code on your CPU and GPU.
echo Nothing is installed or changed; a small window opens and closes. Takes 1-2 minutes, or a few
echo minutes more when your game folder is found (every shader of the game is then compiled here).
echo.
set GAME=
if exist "..\Config\settings.ini" for /f "tokens=1,* delims==" %%a in ('findstr /b /i "game_dir" "..\Config\settings.ini"') do set GAME=%%b
if defined GAME for /f "tokens=* delims= " %%c in ("%GAME%") do set GAME=%%c
if not defined GAME if exist "..\Game\eboot.bin" set GAME=..\Game
if defined GAME if not exist "%GAME%\eboot.bin" set GAME=
if defined GAME (echo Game folder: %GAME%) else (echo Game folder not found: the shader part is skipped. To include it, put the folder with eboot.bin after --game below.)
echo.
if defined GAME (ud_compat.exe --fpctl --game "%GAME%" > ud_compat_console.txt 2>&1) else (ud_compat.exe --fpctl > ud_compat_console.txt 2>&1)
echo exit code %errorlevel% (0 = all checks passed)
echo.
findstr /b /c:"FAIL" /c:"RESULT" ud_compat_report.txt
echo.
set OUT=compat_report.zip
if exist "%OUT%" del "%OUT%"
if exist "..\Logs\UntilDawn.log" (tar -a -cf "%OUT%" ud_compat_report.txt ud_compat_console.txt -C ..\Logs UntilDawn.log 2>nul) else (tar -a -cf "%OUT%" ud_compat_report.txt ud_compat_console.txt 2>nul)
if exist "%OUT%" (echo Please send the file compat_report.zip from this folder.) else (echo Please send ud_compat_report.txt and ud_compat_console.txt from this folder.)
pause
