@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch_two_instances.ps1"
exit /b %errorlevel%
