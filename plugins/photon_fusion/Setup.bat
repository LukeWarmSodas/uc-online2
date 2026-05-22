@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM  Photon Fusion universal fix - setup wizard
REM
REM  This script must run BEFORE you copy photon_fusion.dll
REM  into the game. It rewrites the game's bundled Photon
REM  AppId GUID so the runtime plugin can take over from there.
REM ============================================================

title Photon Fusion Fix - Setup

echo.
echo ============================================================
echo   Photon Fusion universal fix - setup
echo ============================================================
echo.
echo  IMPORTANT: Run this BEFORE copying photon_fusion.dll
echo  into the game's plugins folder. The DLL will not work
echo  unless this script has patched the game's assets first.
echo.
echo  Before continuing you should already have:
echo    [ ] A Photon Fusion 2 app on dashboard.photonengine.com
echo    [ ] The AppId GUID from your Photon app
echo.
pause

echo.
set /p GAME_DIR="Full path to the game folder (e.g. C:\Steam\steamapps\common\Outbound): "

if not exist "%GAME_DIR%" (
  echo.
  echo  ERROR: That folder does not exist.
  echo.
  pause
  exit /b 1
)

echo.
set /p NEW_GUID="Your Photon Fusion AppId GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx): "

echo.
echo Patching assets...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Set-PhotonAppId.ps1" -GameDir "%GAME_DIR%" -NewAppId "%NEW_GUID%"
set RC=%errorlevel%

if not %RC%==0 (
  echo.
  echo ============================================================
  echo   ERROR: GUID NOT FOUND - plugin will NOT work
  echo ============================================================
  echo.
  echo  The script could not find a Photon Fusion AppId GUID in
  echo  the game's assets, so the plugin has nothing to redirect.
  echo.
  echo  Possible causes:
  echo    - Wrong folder picked (must be the folder containing
  echo      the game's .exe and the ^<Game^>_Data subfolder)
  echo    - Game doesn't use Photon Fusion 2
  echo    - GUID format invalid (must be 8-4-4-4-12 hex chars
  echo      separated by dashes)
  echo    - Already patched on a previous run (rerun with -Revert
  echo      first if you want to change to a different GUID)
  echo.
  pause
  exit /b 1
)

echo.
echo ============================================================
echo   SUCCESS - assets patched
echo ============================================================
echo.
echo  Now finish the setup:
echo.
echo  1. Copy photon_fusion.dll into:
echo       %GAME_DIR%\plugins\
echo     (create the 'plugins' folder if it doesn't exist)
echo.
echo  2. Edit %GAME_DIR%\union-crax.ini and add:
echo.
echo       [Fusion]
echo       PhotonAppIdFusion=%NEW_GUID%
echo       ForcedAuthType=0
echo.
echo  3. Make sure your Photon app's dashboard has:
echo       - Custom Authentication URL pointing at a permissive
echo         backend (e.g. a Cloudflare Worker that returns
echo         ResultCode=1 for every request)
echo       - "Reject Clients on Authentication Failure" UNCHECKED
echo.
echo  4. Launch the game and trigger multiplayer.
echo.
echo  See plugins\photon_fusion\README.md for the full guide.
echo.
pause
