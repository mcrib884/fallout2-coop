@echo off
rem Builds only the co-op launcher and copies it into fallout2coopdist.
setlocal

cmake --preset windows-x64
if errorlevel 1 exit /b 1

cmake --build --preset windows-x64-release --target fallout2coop_launcher
if errorlevel 1 exit /b 1

copy /y "out\build\windows-x64\tools\launcher\RelWithDebInfo\fallout2coop_launcher.exe" "fallout2coopdist\"
if errorlevel 1 exit /b 1

echo.
echo Launcher built: fallout2coopdist\fallout2coop_launcher.exe
endlocal
