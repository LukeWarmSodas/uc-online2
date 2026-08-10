@echo off
setlocal enableextensions enabledelayedexpansion
title UCOnline2 - patch.bat (auto-patcher)
color 0b

rem ============================================================
rem  UCOnline2 patch.bat
rem
rem  Drop a game folder onto this file (or run: patch.bat "C:\path\to\game").
rem
rem  It works out what the game is and does the whole deploy:
rem    * finds the engine (Unity or Unreal) and the real executable
rem    * finds where steam_api64.dll actually lives and installs ours THERE,
rem      backing up the original first
rem    * detects Photon / EOS / PlayFab and copies the matching plugin
rem    * writes a union-crax.ini with the right sections
rem
rem  It does NOT modify game assets. Any third-party app IDs (Photon GUIDs,
rem  your own Epic app, a PlayFab TitleId) are YOURS, so it prompts for them
rem  and will happily write a stub section you can fill in later.
rem
rem  DETECTION NOTE: backends are detected by FILE PRESENCE, and for Unreal
rem  also by ANSI module names inside the shipping exe. Deliberately not by
rem  scanning for endpoint strings -- Unreal stores most literals as UTF-16,
rem  so a byte-level search for "playfabapi.com" finds nothing even in a game
rem  that clearly uses it. Module names like OnlineSubsystemEOS are ANSI and
rem  do match. Enum values are avoided as signals for the same reason a plain
rem  "PlayFab" search is useless: a platform-abstraction plugin lists every
rem  provider it could ever target, so the string proves nothing.
rem ============================================================

set "SCRIPTDIR=%~dp0"

rem ---- locate OUR steam_api64.dll (the emulator itself) ----
rem Three layouts, in the order they turn up:
rem   1. repo checkout -> relbuild\x64\
rem   2. release zip   -> x64\   (release.yml packages the DLLs into x86\ and
rem      x64\, NOT at the package root. Miss this and the released copy of this
rem      script can never find the emulator it is supposed to install.)
rem   3. dropped beside this script
set "EMU_DLL="
if exist "%SCRIPTDIR%relbuild\x64\steam_api64.dll" set "EMU_DLL=%SCRIPTDIR%relbuild\x64\steam_api64.dll"
if not defined EMU_DLL if exist "%SCRIPTDIR%x64\steam_api64.dll" set "EMU_DLL=%SCRIPTDIR%x64\steam_api64.dll"
if not defined EMU_DLL if exist "%SCRIPTDIR%steam_api64.dll" set "EMU_DLL=%SCRIPTDIR%steam_api64.dll"
if not defined EMU_DLL (
  echo [WARN] UCOnline2's steam_api64.dll not found next to this script.
  echo        Build it:  msbuild uc_online2.vcxproj -p:Configuration=Release -p:Platform=x64
  echo        The ini and plugins will still be written; copy the DLL yourself.
  echo.
)

rem ---- key-only mode ----
rem
rem   patch.bat "C:\path\to\game" /keyonly
rem
rem Changes the coherence project a game points at and nothing else -- no DLL,
rem no plugins, no ini rewrite. Use it to switch between your own project and
rem the shared one without redoing, and possibly disturbing, a working install.
rem
rem For a FRESH install do a normal full run instead and answer SHARED at the
rem runtime-key prompt; key-only does not deploy the emulator or the plugin.
set "KEYONLY="
if /i "%~2"=="/keyonly" set "KEYONLY=1"
if /i "%~1"=="/keyonly" set "KEYONLY=1"
if /i "%~2"=="-keyonly" set "KEYONLY=1"

rem ---- resolve game folder (arg or prompt) ----
set "GAME=%~1"
if /i "%GAME%"=="/keyonly" set "GAME=%~2"
if not defined GAME set /p "GAME=Drag the game folder here (or paste its path): "
set "GAME=%GAME:"=%"
if not defined GAME echo [ERROR] No folder given.& goto :end
if not exist "%GAME%\" echo [ERROR] Not a folder: %GAME%& goto :end
rem Strip a trailing backslash so joined paths never end up with a double one.
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"
echo.
echo Game folder: %GAME%

rem ============================================================
rem  Identify the engine
rem
rem  Unity  -> a "<Game>_Data" folder beside the exe
rem  Unreal -> "<Game>-Win64-Shipping.exe" under <Game>\Binaries\Win64\
rem
rem  The launcher in the root (Solarpunk.exe) is usually a thin shim; the
rem  Shipping exe is the real binary and the one worth inspecting.
rem ============================================================
set "ENGINE="
set "DATA="
set "GAME_EXE="

for /d %%D in ("%GAME%\*_Data") do set "DATA=%%~fD"

rem Some repacks nest the game one level down and put a loader at the top --
rem gbe's ColdClientLoader does exactly this ("Vampire Survivors.exe" beside a
rem "Vampire Survivors\" folder holding the real game). Only checking the top
rem level makes patch.bat report "could not identify the engine" for a perfectly
rem ordinary Unity game, so fall back to a recursive search.
if not defined DATA (
  for /d /r "%GAME%" %%D in (*_Data) do if not defined DATA set "DATA=%%~fD"
)
if defined DATA set "ENGINE=Unity"

if not defined ENGINE (
  for /r "%GAME%" %%F in (*-Win64-Shipping.exe) do (
    set "CAND=%%~fF"
    set "SKIP="
    rem CrashReportClient ships with every UE title and is never the game.
    echo !CAND! | findstr /i /c:"CrashReport" >nul && set "SKIP=1"
    if not defined SKIP if not defined GAME_EXE set "GAME_EXE=!CAND!"
  )
  if defined GAME_EXE set "ENGINE=Unreal"
)

if not defined ENGINE (
  echo [ERROR] Could not identify the engine.
  echo   Looked for a "<Game>_Data" folder ^(Unity^) and a
  echo   "*-Win64-Shipping.exe" ^(Unreal^) and found neither.
  goto :end
)

rem A competing emulator's loader will fight ours: gbe's ColdClientLoader
rem injects its own steamclient64.dll, so the game talks to that instead of the
rem real Steam client UCOnline2 needs.
if exist "%GAME%\steamclient64.ini" (
  echo.
  echo [WARN] This folder has a gbe/ColdClientLoader setup ^(steamclient64.ini^).
  echo        UCOnline2 is a passthrough and needs the REAL Steam client, but
  echo        that loader injects its own steamclient64.dll instead. Launch the
  echo        game's own exe directly rather than the loader in this folder.
  echo.
)

echo Engine:      %ENGINE%
if defined DATA     echo Data folder: %DATA%
if defined GAME_EXE echo Executable:  %GAME_EXE%

rem ============================================================
rem  Find where steam_api64.dll belongs
rem
rem  The authoritative answer is wherever the game ALREADY keeps one -- that
rem  path is engine-, version- and packager-specific and not worth guessing.
rem  Unreal buries it under Engine\Binaries\ThirdParty\Steamworks\Steamv1XX\
rem  Win64\, Unity uses <Game>_Data\Plugins\x86_64\, and repacks move it.
rem  Only fall back to a convention when the game ships no copy at all.
rem ============================================================
rem The "if exist" is load-bearing, not defensive. `for /r` with a plain
rem filename (no wildcard) does NOT test for the file -- it yields that name
rem joined to EVERY directory it walks, so the first result is always the
rem search root. Without the guard this picked the game folder itself and
rem installed the emulator where nothing loads it.
set "STEAM_DIR="
for /r "%GAME%" %%F in (steam_api64.dll) do (
  if exist "%%~fF" if not defined STEAM_DIR set "STEAM_DIR=%%~dpF"
)

rem A 32-bit game ships steam_api.dll and no steam_api64.dll. Installing our
rem x64 build there would be silently wrong -- the game would fail to load it
rem and the cause would not be obvious. Detect that case and refuse, rather
rem than "helpfully" writing the wrong architecture.
set "IS_32BIT="
if not defined STEAM_DIR (
  for /r "%GAME%" %%F in (steam_api.dll) do if exist "%%~fF" set "IS_32BIT=1"
)
if defined IS_32BIT (
  echo [WARN] This game is 32-bit ^(ships steam_api.dll, not steam_api64.dll^).
  echo        UCOnline2 is built x64 here, so the emulator will NOT be installed.
  echo        Build the Win32 configuration and copy steam_api.dll yourself.
  set "EMU_DLL="
)

if not defined STEAM_DIR if not defined IS_32BIT (
  if "%ENGINE%"=="Unity"  set "STEAM_DIR=%DATA%\Plugins\x86_64\"
  if "%ENGINE%"=="Unreal" for %%F in ("%GAME_EXE%") do set "STEAM_DIR=%%~dpF"
  echo [WARN] Game ships no steam_api64.dll; falling back to the usual place.
)
if defined STEAM_DIR echo Steam DLL:   %STEAM_DIR%

rem ============================================================
rem  Where union-crax.ini and plugins\ must live
rem
rem  NOT the game folder -- UCOnline2's ReadConfig builds the ini path from
rem  GetModuleFileName(NULL), i.e. the directory of the RUNNING EXE. For Unity
rem  that is the game root, so the two coincide. For Unreal the running process
rem  is the Shipping exe several folders down, and an ini in the root is simply
rem  never read: the emulator falls back to AppId=480 with no ogAppId and logs
rem  "No usable ogAppId". PluginsFolder is relative to the same place, so
rem  plugins\ belongs here too.
rem ============================================================
rem For Unity the exe sits beside <Game>_Data, which is NOT necessarily the
rem folder that was dropped on this script -- see the nested-layout note above.
set "INI_DIR=%GAME%"
if "%ENGINE%"=="Unity"  for %%F in ("%DATA%") do set "INI_DIR=%%~dpF"
if "%ENGINE%"=="Unreal" for %%F in ("%GAME_EXE%") do set "INI_DIR=%%~dpF"
if "%INI_DIR:~-1%"=="\" set "INI_DIR=%INI_DIR:~0,-1%"
echo Config dir:  %INI_DIR%

rem ============================================================
rem  Detect backends
rem ============================================================
set "FLAVOR="
set "HAS_VOICE="
set "HAS_EOS="
set "HAS_PLAYFAB="
set "BACKEND="

if "%ENGINE%"=="Unity" (
  if exist "%DATA%\Managed" (
    set "BACKEND=Mono"
    if exist "%DATA%\Managed\PhotonUnityNetworking.dll" set "FLAVOR=Realtime"
    if not defined FLAVOR if exist "%DATA%\Managed\PhotonRealtime.dll" set "FLAVOR=Realtime"
    if exist "%DATA%\Managed\Fusion.Realtime.dll" set "FLAVOR=Fusion"
    if exist "%DATA%\Managed\PhotonVoice.dll" set "HAS_VOICE=1"
    if exist "%DATA%\Managed\PhotonVoice.PUN.dll" set "HAS_VOICE=1"
    if exist "%DATA%\Managed\PlayFabAllSDK.dll" set "HAS_PLAYFAB=1"
    for %%F in ("%DATA%\Managed\PlayFab*.dll") do set "HAS_PLAYFAB=1"
  ) else if exist "%DATA%\il2cpp_data\Metadata\global-metadata.dat" (
    set "BACKEND=IL2CPP"
    set "META=%DATA%\il2cpp_data\Metadata\global-metadata.dat"
    findstr /m /c:"NetworkRunner" "!META!" >nul 2>&1 && set "FLAVOR=Fusion"
    if not defined FLAVOR findstr /m /c:"LoadBalancingClient" "!META!" >nul 2>&1 && set "FLAVOR=Realtime"
    if not defined FLAVOR findstr /m /c:"PhotonNetwork" "!META!" >nul 2>&1 && set "FLAVOR=Realtime"
    findstr /m /c:"PhotonVoice" "!META!" >nul 2>&1 && set "HAS_VOICE=1"
    findstr /m /c:"PlayFabSettings" "!META!" >nul 2>&1 && set "HAS_PLAYFAB=1"
  ) else (
    echo [ERROR] Found neither Managed\ nor il2cpp_data\ under the data folder.
    goto :end
  )
) else (
  set "BACKEND=Unreal"
  rem ANSI module names -- see the DETECTION NOTE at the top.
  findstr /m /c:"OnlineSubsystemEOS" "%GAME_EXE%" >nul 2>&1 && set "HAS_EOS=1"
  findstr /m /c:"OnlineSubsystemPlayFab" "%GAME_EXE%" >nul 2>&1 && set "HAS_PLAYFAB=1"
  findstr /m /c:"PhotonUnityNetworking" "%GAME_EXE%" >nul 2>&1 && set "FLAVOR=Realtime"
)

rem coherence: the SDK bakes a schema next to the game. That file is present
rem in every coherence build regardless of engine version, which makes it a
rem better marker than any symbol name.
set "HAS_COHERENCE="
for /r "%GAME%" %%F in (combined.schema) do (
  if exist "%%~fF" set "HAS_COHERENCE=1"
)

rem File presence beats any string scan -- if the SDK ships, it is in use.
rem Same `for /r` trap as above: without "if exist" these fire in every
rem directory walked and every game on earth "uses EOS".
for /r "%GAME%" %%F in (EOSSDK-Win64-Shipping.dll) do if exist "%%~fF" set "HAS_EOS=1"
for /r "%GAME%" %%F in (EOSSDK.dll) do if exist "%%~fF" set "HAS_EOS=1"

echo.
echo [DETECTED] Engine=%ENGINE%  Backend=%BACKEND%
if defined FLAVOR (
  echo            Photon: %FLAVOR%
  if defined HAS_VOICE echo            Photon Voice present ^(a separate Voice app is required^).
) else (
  echo            Photon: none
)
if defined HAS_EOS     echo            EOS: yes ^(EOS_custom plugin^)
if defined HAS_PLAYFAB echo            PlayFab: yes ^(playfab_universal plugin^)
if defined HAS_COHERENCE echo            coherence: yes ^(coherence_universal plugin^)
if not defined FLAVOR if not defined HAS_EOS if not defined HAS_PLAYFAB if not defined HAS_COHERENCE (
  echo            No secondary backend found -- if multiplayer is pure Steam
  echo            P2P this needs no plugin at all. TEST IT BARE FIRST.
)
echo.

if defined KEYONLY (
  if not defined HAS_COHERENCE (
    echo [ERROR] /keyonly only applies to coherence games, and this is not one.
    goto :end
  )
  echo Key-only mode: the runtime key will be replaced and NOTHING else.
  echo.
  goto :ask_coherence
)

rem ============================================================
rem  Gather config
rem
rem  set /p misbehaves inside a ( ) block, so every prompt is at top level
rem  and reached by goto. Do not "tidy" these into an if-block.
rem ============================================================
set "OGAPPID="
set /p "OGAPPID=Real Steam AppId of the game (the ogAppId): "
if not defined OGAPPID echo [ERROR] AppId is required.& goto :end

set "RTGUID="
set "VOICEGUID="
set "FUSIONGUID="
set "EOS_PRODUCT="
set "EOS_SANDBOX="
set "EOS_DEPLOY="
set "EOS_CLIENT="
set "EOS_SECRET="
set "PF_TITLE="

if not defined FLAVOR goto :ask_eos
if /i "%FLAVOR%"=="Fusion" goto :ask_fusion

:ask_realtime
set /p "RTGUID=Your Photon REALTIME app GUID: "
if not defined RTGUID echo [ERROR] Realtime GUID is required.& goto :end
if defined HAS_VOICE goto :ask_voice_required
set /p "VOICEGUID=Photon VOICE app GUID (optional, press Enter to skip): "
goto :ask_eos

:ask_voice_required
set /p "VOICEGUID=Your Photon VOICE app GUID (required for this game): "
goto :ask_eos

:ask_fusion
set /p "FUSIONGUID=Your Photon FUSION app GUID: "
if not defined FUSIONGUID echo [ERROR] Fusion GUID is required.& goto :end

:ask_eos
if not defined HAS_EOS goto :ask_playfab
echo.
echo This game uses EOS. EOS_custom points it at YOUR OWN free Epic app
echo (dev.epicgames.com) and logs in anonymously with a Device ID, because
echo Epic always rejects an emulated Steam ticket. Press Enter on any of
echo these to write a stub section you can fill in later.
set /p "EOS_PRODUCT=  EOS ProductId: "
set /p "EOS_SANDBOX=  EOS SandboxId: "
set /p "EOS_DEPLOY=  EOS DeploymentId: "
set /p "EOS_CLIENT=  EOS ClientId: "
set /p "EOS_SECRET=  EOS ClientSecret: "

:ask_playfab
if not defined HAS_PLAYFAB goto :ask_coherence
echo.
if defined HAS_COHERENCE (
  echo   NOTE: this game also uses coherence, which is far more likely to be
  echo   what multiplayer actually runs on. Plenty of games bundle the PlayFab
  echo   SDK for storefront or account bits without using it for co-op.
  echo   Leave this EMPTY unless you know otherwise -- with no TitleId the
  echo   PlayFab plugin stays idle and installs no hooks at all.
  echo.
)
set /p "PF_TITLE=  PlayFab TitleId (press Enter to skip): "

:ask_coherence
if not defined HAS_COHERENCE goto :write_ini
echo.
echo This game uses coherence. Multiplayer authenticates against the publisher's
echo coherence project, which will not accept an emulated Steam ticket -- so the
echo game has to be pointed at a project you control.
echo.
echo Create one at coherence.io ^(the free tier is enough^), upload the game's
echo schema, enable ONE region, and paste the project's RUNTIME KEY below.
echo Full instructions: plugins\coherence_universal\README.md
echo.
echo   Type SHARED to use the community project instead -- no coherence
echo   account, no Unity, no schema upload. Availability is not guaranteed.
echo.
set /p "COH_KEY=  coherence runtime key, or SHARED (press Enter to skip): "

rem The shared project's RUNTIME key. Safe to publish: a runtime key is a
rem client-side identifier that ships inside every coherence game by design,
rem like a Photon AppId. Portal/service tokens are a different thing entirely
rem and must never appear here.
if /i "%COH_KEY%"=="shared" (
  set "COH_KEY=fce1ea692a854b50b9f945ef6aa17758"
  echo.
  echo   Using the shared project. It is a free tier, unmonitored, and shared
  echo   with everyone else using it -- if co-op stops working, suspect this
  echo   first and set up your own project.
)
echo.
echo   NOTE: your project also needs the game's schema uploaded to it, which
echo   requires the Unity editor. tools\coherence_schema automates that, or
echo   the plugin README lists a shared project usable with no Unity at all.
echo   Without a matching schema the game fails with SchemaNotFound.

rem ============================================================
rem  Write union-crax.ini  (sequential appends -- robust)
rem ============================================================
:write_ini
if defined KEYONLY goto :deploy_plugins
set "INI=%INI_DIR%\union-crax.ini"
> "%INI%" echo [Settings]
>> "%INI%" echo AppId=480
>> "%INI%" echo ogAppId=%OGAPPID%
>> "%INI%" echo PluginsFolder=plugins
>> "%INI%" echo GetStubbedLol=false

if not defined FLAVOR goto :ini_eos
>> "%INI%" echo(
if /i "%FLAVOR%"=="Fusion" (
  >> "%INI%" echo [Fusion]
  >> "%INI%" echo PhotonAppIdFusion=%FUSIONGUID%
  >> "%INI%" echo ForcedAuthType=0
) else (
  >> "%INI%" echo [Realtime]
  >> "%INI%" echo PhotonAppIdRealtime=%RTGUID%
  if defined VOICEGUID >> "%INI%" echo PhotonAppIdVoice=%VOICEGUID%
  >> "%INI%" echo ForcedAuthType=0
)

:ini_eos
if not defined HAS_EOS goto :ini_coherence
>> "%INI%" echo(
>> "%INI%" echo [EOS]
if defined EOS_PRODUCT (>> "%INI%" echo ProductId=%EOS_PRODUCT%) else (>> "%INI%" echo ProductId=)
if defined EOS_SANDBOX (>> "%INI%" echo SandboxId=%EOS_SANDBOX%) else (>> "%INI%" echo SandboxId=)
if defined EOS_DEPLOY  (>> "%INI%" echo DeploymentId=%EOS_DEPLOY%) else (>> "%INI%" echo DeploymentId=)
if defined EOS_CLIENT  (>> "%INI%" echo ClientId=%EOS_CLIENT%) else (>> "%INI%" echo ClientId=)
if defined EOS_SECRET  (>> "%INI%" echo ClientSecret=%EOS_SECRET%) else (>> "%INI%" echo ClientSecret=)
>> "%INI%" echo DisplayName=Player

:ini_coherence
if not defined HAS_COHERENCE goto :ini_playfab
>> "%INI%" echo(
>> "%INI%" echo [Coherence]
>> "%INI%" echo ForceGuestLogin=true
if defined COH_KEY (>> "%INI%" echo RuntimeKey=%COH_KEY%) else (>> "%INI%" echo RuntimeKey=)
>> "%INI%" echo LocalMode=false

:ini_playfab
if not defined HAS_PLAYFAB goto :wrote_ini
>> "%INI%" echo(
>> "%INI%" echo [PlayFab]
if defined PF_TITLE (>> "%INI%" echo TitleId=%PF_TITLE%) else (>> "%INI%" echo TitleId=)

:wrote_ini
echo.
echo [OK] Wrote %INI%

rem ============================================================
rem  Install the emulator, backing up whatever is there
rem ============================================================
if defined KEYONLY goto :deploy_plugins
if not defined EMU_DLL goto :deploy_plugins
if not exist "%STEAM_DIR%" mkdir "%STEAM_DIR%" 2>nul

rem Never overwrite an existing .bak -- on a second run that would replace the
rem pristine original with our own DLL and lose it for good.
if exist "%STEAM_DIR%steam_api64.dll" (
  if exist "%STEAM_DIR%steam_api64.dll.bak" (
    echo [SKIP] Backup already exists, leaving it alone.
  ) else (
    copy /y "%STEAM_DIR%steam_api64.dll" "%STEAM_DIR%steam_api64.dll.bak" >nul
    echo [OK] Backed up original to steam_api64.dll.bak
  )
)

copy /y "%EMU_DLL%" "%STEAM_DIR%steam_api64.dll" >nul
if errorlevel 1 (
  echo [ERROR] Failed to install steam_api64.dll -- is the game running?
) else (
  echo [OK] Installed UCOnline2 steam_api64.dll to %STEAM_DIR%
)

rem ============================================================
rem  Deploy plugins
rem ============================================================
:deploy_plugins
set "NEEDDIR="
if defined KEYONLY goto :skip_other_deploys
if defined FLAVOR set "NEEDDIR=1"
if defined HAS_COHERENCE set "NEEDDIR=1"
if defined HAS_EOS set "NEEDDIR=1"
if defined HAS_PLAYFAB set "NEEDDIR=1"
if defined NEEDDIR if not exist "%INI_DIR%\plugins\" mkdir "%INI_DIR%\plugins"

if defined FLAVOR (
  call :deploy photon_universal
)
rem Only deploy EOS_custom once it has credentials to use. Without your own
rem Epic app it cannot redirect anything, and dropping an inert plugin in
rem front of a game whose co-op might be plain Steam P2P just adds a variable
rem to the first test. Detected-but-not-configured is reported instead.
if defined HAS_EOS (
  if defined EOS_PRODUCT (
    call :deploy EOS_custom
  ) else (
    echo [SKIP] EOS detected but no Epic app given -- EOS_custom NOT deployed.
    echo        Test the game bare first; if co-op fails on EOS, create a free
    echo        app at dev.epicgames.com, fill in [EOS], and re-run this script.
  )
)
:skip_other_deploys
if defined KEYONLY goto :coherence_deploy
if defined HAS_PLAYFAB (
  call :deploy playfab_universal
)

:coherence_deploy
if defined HAS_COHERENCE (
  if not defined KEYONLY call :deploy coherence_universal
  if defined COH_KEY call :patch_runtime_key
)

rem ============================================================
rem  Summary
rem ============================================================
echo.
echo ============================================================
echo  DONE.
echo.
if defined KEYONLY (
  echo  Key-only run: see the runtime-key result above. Nothing else in this
  echo  install was touched.
  goto :summary_end
)
if not defined FLAVOR if not defined HAS_EOS if not defined HAS_PLAYFAB (
  echo  No plugin needed. Launch the game and try multiplayer BARE --
  echo  pure Steam P2P titles work through passthrough with no plugin.
)
if defined FLAVOR (
  echo  Photon ^(%FLAVOR%^): on each Photon app, Manage -^> Authentication
  echo    -^> Add Provider -^> Custom, paste your permissive Cloudflare Worker
  echo    URL, UNCHECK "Reject Clients on Authentication Failure", Save.
  if defined HAS_VOICE echo    This game uses Photon Voice -- you MUST create a Voice app too.
)
if defined HAS_EOS (
  if defined EOS_PRODUCT (
    echo  EOS: EOS_custom deployed. Read the GAME'S OWN log for session
    echo    diagnosis -- it is far more informative than uc_online2.log.
  ) else (
    echo  EOS: detected, plugin NOT deployed ^(no Epic app given^). Try the game
    echo    bare first -- plenty of dual-stack titles run co-op over Steam.
  )
)
if defined HAS_PLAYFAB (
  if defined HAS_COHERENCE (
    echo  PlayFab: the SDK is present, but coherence is almost certainly the
    echo    multiplayer backend here. playfab_universal was copied in and is
    echo    IDLE with no TitleId -- it installs no hooks and cannot affect the
    echo    game's own PlayFab traffic. Get coherence working first; only set a
    echo    TitleId if multiplayer clearly still needs PlayFab.
  ) else (
    echo  PlayFab: set [PlayFab] TitleId if you skipped it.
  )
)
if defined HAS_COHERENCE (
  if defined COH_KEY (
    echo  coherence: see the runtime-key result above. Upload the game's schema
    echo    ^(StreamingAssets\combined.schema^) and enable ONE region -- with
    echo    several enabled, host and joiner can land in different ones, which
    echo    reads as "lobby doesn't exist or is full".
  ) else (
    echo  coherence: no runtime key given, so the game still points at the
    echo    publisher's project and multiplayer will be refused. Re-run with a
    echo    key, or set [Coherence] LocalMode=true to skip the cloud entirely.
  )
)
echo.
:summary_end
echo  Log: %%TEMP%%\uc_online2.log
echo ============================================================
goto :end

rem ------------------------------------------------------------
rem  :patch_runtime_key
rem
rem  Points the game at YOUR coherence project by replacing the publisher's
rem  runtime key in the game data.
rem
rem  This CANNOT be done at runtime. coherence copies the key during its own
rem  init, about a second before UCOnline2 loads plugins (plugins load at
rem  SteamAPI_Init, after the engine has booted). Patching the settings field,
rem  and even patching the string's characters in place, both lose that race.
rem
rem  Runtime keys are 32 hex characters, so this is a same-length replace and
rem  no offset in the asset file moves. Batch cannot edit binary files, so the
rem  work goes to a small PowerShell helper written to %TEMP%.
rem ------------------------------------------------------------
:patch_runtime_key
set "COH_ASSETS="
if defined DATA if exist "%DATA%\globalgamemanagers.assets" set "COH_ASSETS=%DATA%\globalgamemanagers.assets"
if not defined COH_ASSETS (
  echo [SKIP] globalgamemanagers.assets not found -- patch the runtime key by hand.
  goto :eof
)

set "PS1=%TEMP%\uco2_cohkey.ps1"
> "%PS1%" echo param([string]$File,[string]$NewKey)
>>"%PS1%" echo $bytes = [IO.File]::ReadAllBytes($File)
>>"%PS1%" echo $text  = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
>>"%PS1%" echo # Locating the key is the whole risk here: a bare 32-hex search matches
>>"%PS1%" echo # thousands of false positives, because binary data reads as hex-ish
>>"%PS1%" echo # ASCII. Anchoring on the first 40-char hex is not enough either -- an
>>"%PS1%" echo # unrelated one appears earlier in the file, and following it patched 32
>>"%PS1%" echo # bytes of unrelated data during testing.
>>"%PS1%" echo #
>>"%PS1%" echo # The serialized coherence RuntimeSettings lays out schemaID (40 hex),
>>"%PS1%" echo # then runtimeKey (32 hex), then localHost. So require ALL THREE in one
>>"%PS1%" echo # window, and reject a candidate with almost no character variety --
>>"%PS1%" echo # real keys are not runs of 0s and 1s.
>>"%PS1%" echo $found = $false
>>"%PS1%" echo foreach ($a in [regex]::Matches($text, '\b[0-9a-f]{40}\b')) {
>>"%PS1%" echo   $win = $text.Substring($a.Index, [Math]::Min(400, $text.Length - $a.Index))
>>"%PS1%" echo   if ($win -notmatch 'localhost') { continue }
>>"%PS1%" echo   foreach ($c in [regex]::Matches($win, '\b[0-9a-f]{32}\b')) {
>>"%PS1%" echo     if ((($c.Value.ToCharArray() ^| Sort-Object -Unique).Count) -lt 8) { continue }
>>"%PS1%" echo     $old = $c.Value; $idx = $a.Index + $c.Index; $found = $true; break
>>"%PS1%" echo   }
>>"%PS1%" echo   if ($found) { break }
>>"%PS1%" echo }
>>"%PS1%" echo if (-not $found) { Write-Host '  [SKIP] could not locate the runtime key; patch by hand - see plugins\coherence_universal\README.md'; exit 1 }
>>"%PS1%" echo if ($old -eq $NewKey) { Write-Host '  [OK] already points at your project'; exit 0 }
>>"%PS1%" echo if ($old.Length -ne $NewKey.Length) { Write-Host '  [ERROR] key length differs, refusing'; exit 1 }
>>"%PS1%" echo $bak = $File + '.uco2.bak'
>>"%PS1%" echo if (-not (Test-Path $bak)) { Copy-Item $File $bak; Write-Host ('  [OK] backed up -^> ' + $bak) }
>>"%PS1%" echo $newB = [Text.Encoding]::ASCII.GetBytes($NewKey)
>>"%PS1%" echo [Array]::Copy($newB, 0, $bytes, $idx, $newB.Length)
>>"%PS1%" echo [IO.File]::WriteAllBytes($File, $bytes)
>>"%PS1%" echo Write-Host ('  [OK] runtime key ' + $old + ' -^> ' + $NewKey)
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" "%COH_ASSETS%" "%COH_KEY%"
del "%PS1%" >nul 2>&1
goto :eof

rem ------------------------------------------------------------
rem  :deploy <plugin base name>
rem  Finds the DLL in any layout this script ships in and copies it.
rem ------------------------------------------------------------
:deploy
set "P=%~1"
set "SRC="
if exist "%SCRIPTDIR%plugins\%P%\relbuild\x64\%P%.dll" set "SRC=%SCRIPTDIR%plugins\%P%\relbuild\x64\%P%.dll"
if not defined SRC if exist "%SCRIPTDIR%plugins\%P%.dll" set "SRC=%SCRIPTDIR%plugins\%P%.dll"
if not defined SRC if exist "%SCRIPTDIR%%P%.dll" set "SRC=%SCRIPTDIR%%P%.dll"
if not defined SRC (
  echo [WARN] %P%.dll not found -- build it or drop it beside patch.bat.
  goto :eof
)
copy /y "%SRC%" "%INI_DIR%\plugins\%P%.dll" >nul
if errorlevel 1 (
  echo [ERROR] Failed to copy %P%.dll
) else (
  echo [OK] Copied %P%.dll to %INI_DIR%\plugins\
)
goto :eof

:end
echo.
pause
endlocal
