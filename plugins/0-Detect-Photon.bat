@echo off
setlocal
title UCOnline2 - Photon plugin detector
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Detect-Photon.ps1"
echo.
pause
