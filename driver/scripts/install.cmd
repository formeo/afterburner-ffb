@echo off
rem Install the Afterburner FFB effect driver. Run as Administrator from the folder with the DLLs.
net session >nul 2>&1 || (echo Run this script as Administrator. & pause & exit /b 1)
set "SRC=%~dp0"
set "DEST=%ProgramFiles%\AfbFFB"
mkdir "%DEST%" 2>nul
copy /y "%SRC%afbffb64.dll" "%DEST%\" >nul || goto :fail
copy /y "%SRC%afbffb32.dll" "%DEST%\" >nul || goto :fail
"%SystemRoot%\System32\regsvr32.exe" "%DEST%\afbffb64.dll" || goto :fail
"%SystemRoot%\SysWOW64\regsvr32.exe" "%DEST%\afbffb32.dll" || goto :fail
echo.
echo Installed to %DEST%. Unplug and replug the joystick, then run fftest.exe
pause
exit /b 0
:fail
echo Installation failed.
pause
exit /b 1
