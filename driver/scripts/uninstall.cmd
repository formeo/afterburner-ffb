@echo off
rem Remove the Afterburner FFB effect driver. Run as Administrator.
net session >nul 2>&1 || (echo Run this script as Administrator. & pause & exit /b 1)
set "DEST=%ProgramFiles%\AfbFFB"
"%SystemRoot%\System32\regsvr32.exe" /u "%DEST%\afbffb64.dll"
"%SystemRoot%\SysWOW64\regsvr32.exe" /u "%DEST%\afbffb32.dll"
rmdir /s /q "%DEST%"
echo Removed. Unplug and replug the joystick.
pause
