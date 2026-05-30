@echo off
setlocal enableextensions enabledelayedexpansion
title UCOnline2 - patch.bat (Photon auto-patcher)
color 0b

rem ============================================================
rem  UCOnline2 patch.bat
rem
rem  Drop a game folder onto this file (or run: patch.bat "C:\path\to\game").
rem  It detects whether the game uses Photon, and if so:
rem    * copies photon_universal.dll into <game>\plugins\
rem    * writes a union-crax.ini with the right [Realtime] or [Fusion] section
rem
rem  It does NOT modify game assets. The GUIDs are YOUR Photon app IDs
rem  (create them at https://dashboard.photonengine.com/), so the script
rem  prompts for them. For non-Photon games it tells you and exits.
rem ============================================================

set "SCRIPTDIR=%~dp0"

rem ---- locate the plugin DLL (built output, else release-style fallback) ----
set "PLUGIN_DLL="
if exist "%SCRIPTDIR%plugins\photon_universal\relbuild\x64\photon_universal.dll" set "PLUGIN_DLL=%SCRIPTDIR%plugins\photon_universal\relbuild\x64\photon_universal.dll"
if not defined PLUGIN_DLL if exist "%SCRIPTDIR%photon_universal.dll" set "PLUGIN_DLL=%SCRIPTDIR%photon_universal.dll"
if not defined PLUGIN_DLL (
  echo [ERROR] photon_universal.dll not found.
  echo   Build it:  msbuild plugins\photon_universal\photon_universal_plugin.vcxproj -p:Configuration=Release -p:Platform=x64
  echo   or drop photon_universal.dll next to this patch.bat.
  goto :end
)

rem ---- resolve game folder (arg or prompt) ----
set "GAME=%~1"
if "%GAME%"=="" set /p "GAME=Drag the game folder here (or paste its path): "
set "GAME=%GAME:"=%"
if "%GAME%"=="" ( echo [ERROR] No folder given. & goto :end )
if not exist "%GAME%\" ( echo [ERROR] Not a folder: %GAME% & goto :end )
echo.
echo Game folder: %GAME%

rem ---- find the *_Data folder ----
set "DATA="
for /d %%D in ("%GAME%\*_Data") do set "DATA=%%~fD"
if not defined DATA (
  echo [ERROR] No "<Game>_Data" folder found. Is this a Unity game folder?
  goto :end
)
echo Data folder: !DATA!

rem ============================================================
rem  Detect backend + Photon flavor
rem ============================================================
set "BACKEND="
set "FLAVOR="
set "HAS_VOICE="

if exist "!DATA!\Managed" (
  set "BACKEND=Mono"
  if exist "!DATA!\Managed\PhotonUnityNetworking.dll" set "FLAVOR=Realtime"
  if not defined FLAVOR if exist "!DATA!\Managed\PhotonRealtime.dll" set "FLAVOR=Realtime"
  if exist "!DATA!\Managed\Fusion.Realtime.dll" set "FLAVOR=Fusion"
  for %%F in ("!DATA!\Managed\PhotonVoice*.dll") do if exist "%%~fF" set "HAS_VOICE=1"
) else if exist "!DATA!\il2cpp_data\Metadata\global-metadata.dat" (
  set "BACKEND=IL2CPP"
  set "META=!DATA!\il2cpp_data\Metadata\global-metadata.dat"
  findstr /m /c:"NetworkRunner" "!META!" >nul 2>&1 && set "FLAVOR=Fusion"
  if not defined FLAVOR findstr /m /c:"LoadBalancingClient" "!META!" >nul 2>&1 && set "FLAVOR=Realtime"
  if not defined FLAVOR findstr /m /c:"PhotonNetwork" "!META!" >nul 2>&1 && set "FLAVOR=Realtime"
  findstr /m /c:"PhotonVoice" "!META!" >nul 2>&1 && set "HAS_VOICE=1"
) else (
  echo [ERROR] Could not find Managed\ or il2cpp_data\ under the data folder.
  goto :end
)

if not defined FLAVOR (
  echo.
  echo [RESULT] No Photon detected ^(backend: !BACKEND!^).
  echo   This game is not a photon_universal target. Nothing was changed.
  echo   If its multiplayer is pure Steam P2P, try it bare with no plugin first.
  goto :end
)

echo.
echo [DETECTED] Backend=!BACKEND!  Photon flavor=!FLAVOR!
if defined HAS_VOICE echo            Photon Voice present ^(a separate Voice app is required^).
echo.

rem ============================================================
rem  Gather config
rem ============================================================
set "OGAPPID="
set /p "OGAPPID=Real Steam AppId of the game (the ogAppId): "
if "%OGAPPID%"=="" ( echo [ERROR] AppId is required. & goto :end )

if /i "!FLAVOR!"=="Fusion" goto :cfg_fusion

rem ---- Realtime / PUN ----
set "RTGUID="
set /p "RTGUID=Your Photon REALTIME app GUID: "
if "%RTGUID%"=="" ( echo [ERROR] Realtime GUID is required. & goto :end )
set "VOICEGUID="
if defined HAS_VOICE (
  set /p "VOICEGUID=Your Photon VOICE app GUID (required for this game): "
) else (
  set /p "VOICEGUID=Photon VOICE app GUID (optional, press Enter to skip): "
)
goto :write_ini

:cfg_fusion
set "FUSIONGUID="
set /p "FUSIONGUID=Your Photon FUSION app GUID: "
if "%FUSIONGUID%"=="" ( echo [ERROR] Fusion GUID is required. & goto :end )

rem ============================================================
rem  Write union-crax.ini
rem ============================================================
:write_ini
set "INI=%GAME%\union-crax.ini"
(
  echo [Settings]
  echo AppId=480
  echo ogAppId=%OGAPPID%
  echo PluginsFolder=plugins
  echo GetStubbedLol=false
  echo.
  if /i "!FLAVOR!"=="Fusion" (
    echo [Fusion]
    echo PhotonAppIdFusion=%FUSIONGUID%
    echo ForcedAuthType=0
  ) else (
    echo [Realtime]
    echo PhotonAppIdRealtime=%RTGUID%
    if not "%VOICEGUID%"=="" ( echo PhotonAppIdVoice=%VOICEGUID% )
    echo ForcedAuthType=0
  )
) > "!INI!"
echo [OK] Wrote !INI!

rem ============================================================
rem  Deploy the plugin DLL
rem ============================================================
if not exist "%GAME%\plugins\" mkdir "%GAME%\plugins"
copy /y "%PLUGIN_DLL%" "%GAME%\plugins\photon_universal.dll" >nul
if errorlevel 1 ( echo [ERROR] Failed to copy plugin DLL. & goto :end )
echo [OK] Copied photon_universal.dll to %GAME%\plugins\

echo.
echo ============================================================
echo  DONE.
echo  Still to do yourself:
echo   1. Put UCOnline2's steam_api64.dll in !DATA!\Plugins\x86_64\ (back up the original).
echo   2. On each Photon app: Manage -^> Authentication -^> Add Provider -^> Custom,
echo      paste your permissive Cloudflare Worker URL, UNCHECK "Reject Clients
echo      on Authentication Failure", Save.
if defined HAS_VOICE echo   3. This game uses Photon Voice - you MUST create a Voice-type app too.
echo  Then launch. Tail %%TEMP%%\uc_online2.log for [Realtime]/[Fusion] lines.
echo ============================================================

:end
echo.
pause
endlocal
