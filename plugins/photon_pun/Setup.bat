@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM  Photon PUN universal fix - interactive setup wizard
REM
REM  Walks the user through the full install:
REM    1. asks for the game folder
REM    2. asks for the Photon Realtime AppId GUID
REM    3. asks for the game's real Steam AppId
REM    4. patches the embedded PhotonServerSettings GUID in
REM       the game's resources.assets
REM    5. drops photon_pun.dll into <game>\plugins\
REM    6. writes a complete union-crax.ini at <game>\
REM
REM  For IL2CPP Unity games using Photon PUN.
REM  For Mono games, use plugins/photon_pun_mono/Setup.bat.
REM  For Photon Fusion 2 games, use plugins/photon_fusion/Setup.bat.
REM ============================================================

title Photon PUN Fix - Setup

set "SCRIPT_DIR=%~dp0"
set "PS1_PATH=%SCRIPT_DIR%Set-PhotonAppId.ps1"
set "DLL_PATH=%SCRIPT_DIR%relbuild\x64\photon_pun.dll"

echo.
echo ============================================================
echo   Photon PUN universal fix - setup
echo ============================================================
echo.
echo  This wizard configures UCOnline2's PUN multiplayer fix.
echo  You will be prompted for three things:
echo.
echo    [ ] Path to the game folder
echo    [ ] Your Photon Realtime-type app's AppId GUID
echo    [ ] The game's real Steam AppId number
echo.
echo  Before continuing you should already have:
echo    [ ] Created a Photon app of type "Realtime" on
echo        dashboard.photonengine.com. (PUN games use a
echo        Realtime-type app -- PUN is a C# library built
echo        on top of Photon Realtime; the dashboard product
echo        type for both is "Realtime".)
echo    [ ] If the game uses voice chat (e.g. Phasmophobia,
echo        R.E.P.O.), also created a Photon app of type
echo        "Voice" -- separate AppId GUID
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
set /p OG_APPID="The game's real Steam AppId (e.g. 739630 for Phasmophobia): "

echo.
echo ============================================================
echo   1/3  Patching game assets...
echo ============================================================
echo.

if "%VOICE_GUID%"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_PATH%" -GameDir "%GAME_DIR%" -NewAppId "%NEW_GUID%"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_PATH%" -GameDir "%GAME_DIR%" -NewAppId "%NEW_GUID%" -NewVoiceAppId "%VOICE_GUID%"
)
set RC=%errorlevel%

if not %RC%==0 (
  echo.
  echo ============================================================
  echo   ERROR: GUID NOT FOUND - the plugin will NOT work
  echo ============================================================
  echo.
  echo  The script could not find a Photon AppId GUID in this
  echo  game's assets. Possible causes:
  echo    - Wrong folder picked (must contain the .exe and the
  echo      ^<Game^>_Data subfolder)
  echo    - Game does not use Photon PUN
  echo    - Game is Mono not IL2CPP -- try photon_pun_mono\Setup.bat
  echo    - Game uses Fusion 2 -- try photon_fusion\Setup.bat
  echo    - Already patched on a previous run (rerun with -Revert
  echo      first if you want to change to a different GUID)
  echo.
  pause
  exit /b 1
)

echo.
echo ============================================================
echo   2/3  Copying photon_pun.dll into the game folder...
echo ============================================================
echo.

if not exist "%DLL_PATH%" (
  echo  ERROR: photon_pun.dll not found at:
  echo    %DLL_PATH%
  echo.
  echo  Build the plugin first via:
  echo    msbuild plugins\photon_pun\photon_pun_plugin.vcxproj ^
  echo            -p:Configuration=Release -p:Platform=x64 -m
  echo.
  pause
  exit /b 1
)

if not exist "%GAME_DIR%\plugins" (
  mkdir "%GAME_DIR%\plugins"
)
copy /Y "%DLL_PATH%" "%GAME_DIR%\plugins\photon_pun.dll" >nul
if not %errorlevel%==0 (
  echo  ERROR: failed to copy DLL into %GAME_DIR%\plugins\
  pause
  exit /b 1
)
echo  Copied  photon_pun.dll  -^>  %GAME_DIR%\plugins\

echo.
echo ============================================================
echo   3/3  Writing union-crax.ini at the game root...
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
  echo [PUN]
  echo PhotonAppIdRealtime=%NEW_GUID%
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
echo    [+] resources.assets patched to use your Photon AppId
echo    [+] photon_pun.dll dropped into %GAME_DIR%\plugins\
echo    [+] union-crax.ini written at %GAME_DIR%\
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
