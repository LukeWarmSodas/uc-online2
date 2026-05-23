@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM  Photon PUN (Mono) universal fix - interactive setup wizard
REM
REM  Walks the user through the full install:
REM    1. asks for the game folder
REM    2. asks for the Photon Realtime AppId GUID
REM    3. asks for the game's real Steam AppId
REM    4. drops photon_pun_mono.dll into <game>\plugins\
REM    5. writes a complete union-crax.ini at <game>\
REM
REM  The game's resources.assets is NOT modified. The plugin
REM  rewrites the AppId GUID on the wire at runtime, so the ini
REM  is the single source of truth. To change the GUID later
REM  just edit union-crax.ini -- no re-patching needed.
REM
REM  (Set-PhotonAppId.ps1 is kept in this folder as an optional
REM  fallback for cases where runtime override isn't enough.)
REM
REM  Aimed at Unity games using the Mono scripting backend
REM  (R.E.P.O. and similar). For IL2CPP games use the
REM  photon_pun setup instead.
REM ============================================================

title Photon PUN Mono Fix - Setup

set "SCRIPT_DIR=%~dp0"
set "DLL_PATH=%SCRIPT_DIR%relbuild\x64\photon_pun_mono.dll"

echo.
echo ============================================================
echo   Photon PUN Mono universal fix - setup
echo ============================================================
echo.
echo  This wizard configures UCOnline2's Mono variant of the
echo  PUN multiplayer fix. You will be prompted for three things:
echo.
echo    [ ] Path to the game folder (e.g. C:\Games\REPO)
echo    [ ] Your Photon Realtime-type app's AppId GUID
echo    [ ] The game's real Steam AppId number
echo.
echo  Before continuing you should already have:
echo    [ ] Created a Photon app of type "Realtime" on
echo        dashboard.photonengine.com. (PUN games use a
echo        Realtime-type app -- PUN is a C# library built
echo        on top of Photon Realtime; the dashboard product
echo        type for both is "Realtime".)
echo    [ ] If the game uses voice chat (e.g. R.E.P.O.), also
echo        created a Photon app of type "Voice"
echo        (separate AppId GUID)
echo    [ ] Set up a permissive Custom Authentication URL on
echo        both apps (Cloudflare Worker returning ResultCode=1)
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
set /p NEW_GUID="Your Photon Realtime-type app's AppId GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx): "

echo.
echo  Does this game use Photon Voice (voice chat)? If yes, paste
echo  your Photon Voice AppId GUID below. If no, just press Enter.
set /p VOICE_GUID="Your Photon Voice AppId GUID (or blank to skip): "

echo.
set /p OG_APPID="The game's real Steam AppId (e.g. 3241660 for REPO): "

echo.
echo ============================================================
echo   1/2  Copying photon_pun_mono.dll into the game folder...
echo ============================================================
echo.

if not exist "%DLL_PATH%" (
  echo  ERROR: photon_pun_mono.dll not found at:
  echo    %DLL_PATH%
  echo.
  echo  Build the plugin first via:
  echo    msbuild plugins\photon_pun_mono\photon_pun_mono_plugin.vcxproj ^
  echo            -p:Configuration=Release -p:Platform=x64 -m
  echo.
  pause
  exit /b 1
)

if not exist "%GAME_DIR%\plugins" (
  mkdir "%GAME_DIR%\plugins"
)
copy /Y "%DLL_PATH%" "%GAME_DIR%\plugins\photon_pun_mono.dll" >nul
if not %errorlevel%==0 (
  echo  ERROR: failed to copy DLL into %GAME_DIR%\plugins\
  pause
  exit /b 1
)
echo  Copied  photon_pun_mono.dll  -^>  %GAME_DIR%\plugins\

echo.
echo ============================================================
echo   2/2  Writing union-crax.ini at the game root...
echo ============================================================
echo.

set "INI_PATH=%GAME_DIR%\union-crax.ini"

if "%VOICE_GUID%"=="" (
  (
    echo [Settings]
    echo AppId=480
    echo ogAppId=%OG_APPID%
    echo PluginsFolder=plugins
    echo GetStubbedLol=false
    echo.
    echo [Realtime]
    echo PhotonAppIdRealtime=%NEW_GUID%
    echo ForcedAuthType=0
  ) > "%INI_PATH%"
) else (
  (
    echo [Settings]
    echo AppId=480
    echo ogAppId=%OG_APPID%
    echo PluginsFolder=plugins
    echo GetStubbedLol=false
    echo.
    echo [Realtime]
    echo PhotonAppIdRealtime=%NEW_GUID%
    echo PhotonAppIdVoice=%VOICE_GUID%
    echo ForcedAuthType=0
  ) > "%INI_PATH%"
)

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
echo    [+] photon_pun_mono.dll dropped into %GAME_DIR%\plugins\
echo    [+] union-crax.ini written at %GAME_DIR%\
echo.
echo  The game's resources.assets was NOT touched. The plugin
echo  rewrites the Photon AppId on the wire at runtime, driven
echo  by [Realtime] PhotonAppIdRealtime in the ini. To change the
echo  GUID later just edit union-crax.ini -- no re-patching.
echo.
echo  Remaining steps you still need to do yourself:
echo    [ ] Drop UCOnline2's steam_api64.dll into:
echo          %GAME_DIR%\^<Game^>_Data\Plugins\x86_64\
echo        (back up the original first)
echo    [ ] Verify on Photon dashboard:
echo          - App type = Realtime / PUN
echo          - Custom Auth provider type = Custom
echo          - Custom Auth URL = your Cloudflare Worker URL
echo          - "Reject Clients on Authentication Failure" UNCHECKED
echo.
echo  Then launch the game and try multiplayer. Watch the log:
echo    %%TEMP%%\uc_online2.log
echo.
pause
