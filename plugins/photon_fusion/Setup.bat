@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM  Photon Fusion 2 universal fix - interactive setup wizard
REM
REM  Walks the user through the full install:
REM    1. asks for the game folder
REM    2. asks for the Photon Fusion 2 AppId GUID
REM    3. asks for the game's real Steam AppId
REM    4. drops photon_fusion.dll into <game>\plugins\
REM    5. writes a complete union-crax.ini at <game>\
REM
REM  The game's resources.assets is NOT modified. The plugin
REM  hooks PhotonAppSettings.get_Global at runtime to rewrite
REM  AppIdFusion in the loaded singleton, so the ini is the
REM  single source of truth. Change the GUID later -> just
REM  edit union-crax.ini, no re-patching.
REM
REM  (Set-PhotonAppId.ps1 is kept in this folder as an
REM  optional fallback for cases where runtime override
REM  isn't enough.)
REM
REM  For IL2CPP Unity games using Photon Fusion 2.
REM  For PUN games use plugins\photon_realtime\Setup.bat (IL2CPP)
REM  or plugins\photon_realtime_mono\Setup.bat (Mono).
REM ============================================================

title Photon Fusion 2 Fix - Setup

set "SCRIPT_DIR=%~dp0"
set "DLL_PATH=%SCRIPT_DIR%relbuild\x64\photon_fusion.dll"

echo.
echo ============================================================
echo   Photon Fusion 2 universal fix - setup
echo ============================================================
echo.
echo  This wizard configures UCOnline2's Fusion 2 multiplayer fix.
echo  You will be prompted for three things:
echo.
echo    [ ] Path to the game folder (e.g. C:\Steam\steamapps\common\Outbound)
echo    [ ] Your Photon Fusion 2 AppId GUID
echo    [ ] The game's real Steam AppId number
echo.
echo  Before continuing you should already have:
echo    [ ] Created a Photon "Fusion" app on
echo        dashboard.photonengine.com
echo    [ ] Set up a permissive Custom Authentication URL on
echo        that app (Cloudflare Worker returning ResultCode=1)
echo    [ ] Unchecked "Reject Clients on Authentication Failure"
echo.
pause

echo.
set /p GAME_DIR="Full path to the game folder: "
if not exist "%GAME_DIR%" (
  echo.
  echo  ERROR: That folder does not exist:
  echo    %GAME_DIR%
  echo.
  pause
  exit /b 1
)

echo.
set /p NEW_GUID="Your Photon Fusion 2 AppId GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx): "
echo.
set /p OG_APPID="The game's real Steam AppId (e.g. 2681030 for Outbound): "

echo.
echo ============================================================
echo   1/2  Copying photon_fusion.dll into the game folder...
echo ============================================================
echo.

if not exist "%DLL_PATH%" (
  echo  ERROR: photon_fusion.dll not found at:
  echo    %DLL_PATH%
  echo.
  echo  Build the plugin first via:
  echo    msbuild plugins\photon_fusion\photon_fusion_plugin.vcxproj ^
  echo            -p:Configuration=Release -p:Platform=x64 -m
  echo.
  pause
  exit /b 1
)

if not exist "%GAME_DIR%\plugins" (
  mkdir "%GAME_DIR%\plugins"
)
copy /Y "%DLL_PATH%" "%GAME_DIR%\plugins\photon_fusion.dll" >nul
if not %errorlevel%==0 (
  echo  ERROR: failed to copy DLL into %GAME_DIR%\plugins\
  pause
  exit /b 1
)
echo  Copied  photon_fusion.dll  -^>  %GAME_DIR%\plugins\

echo.
echo ============================================================
echo   2/2  Writing union-crax.ini at the game root...
echo ============================================================
echo.

set "INI_PATH=%GAME_DIR%\union-crax.ini"

(
  echo [Settings]
  echo AppId=480
  echo ogAppId=%OG_APPID%
  echo PluginsFolder=plugins
  echo GetStubbedLol=false
  echo.
  echo [Fusion]
  echo PhotonAppIdFusion=%NEW_GUID%
  echo ForcedAuthType=0
) > "%INI_PATH%"

if not %errorlevel%==0 (
  echo  ERROR: failed to write %INI_PATH%
  pause
  exit /b 1
)
echo  Wrote  %INI_PATH%

echo.
echo ============================================================
echo   SUCCESS - setup complete
echo ============================================================
echo.
echo  What was done:
echo    [+] photon_fusion.dll dropped into %GAME_DIR%\plugins\
echo    [+] union-crax.ini written at %GAME_DIR%\
echo.
echo  The game's resources.assets was NOT touched. The plugin
echo  rewrites the Fusion AppId at runtime via the
echo  PhotonAppSettings.get_Global hook, driven by
echo  [Fusion] PhotonAppIdFusion in the ini. To change the GUID
echo  later just edit union-crax.ini -- no re-patching.
echo.
echo  Remaining steps you still need to do yourself:
echo    [ ] Drop UCOnline2's steam_api64.dll into:
echo          %GAME_DIR%\^<Game^>_Data\Plugins\x86_64\
echo        (back up the original first)
echo    [ ] Verify on Photon dashboard:
echo          - App type = Fusion
echo          - Custom Auth provider type = Custom
echo          - Custom Auth URL = your Cloudflare Worker URL
echo          - "Reject Clients on Authentication Failure" UNCHECKED
echo.
echo  Then launch the game and trigger multiplayer. Watch the log:
echo    %%TEMP%%\uc_online2.log
echo.
pause
