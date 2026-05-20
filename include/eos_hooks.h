// ============================================================
// EOS (Epic Online Services) hooks
//
// Games like Outbound use EOSSDK-Win64-Shipping.dll for
// multiplayer. They call EOS_Connect_Login with a Steam ticket;
// EOS then phones Steam Web API to validate it against the
// configured Steam AppId for the EOS app. Because UCOnline2's
// Steam-side runs under FakeAppId (480/Spacewar), the ticket
// fails validation -> "Failed(2): Ticket for other app".
//
// This module hooks EOS_Connect_Login (and a few related
// entry points) so the game thinks Connect auth succeeded
// without round-tripping to Epic / Steam Web API.
// ============================================================
#pragma once

#include <Windows.h>
#include <stdint.h>

// Minimal EOS SDK type declarations (from EOSSDK public header).
// We don't link against EOSSDK -- we resolve symbols dynamically.
typedef void*    EOS_HConnect;
typedef void*    EOS_HPlatform;
typedef void*    EOS_ProductUserId;
typedef void*    EOS_ContinuanceToken;

typedef int32_t  EOS_EResult;
#define EOS_EResult_Success                       0
#define EOS_EResult_InvalidUser                   2

// EOS_Connect_Login callback info -- output struct, no ApiVersion
typedef struct EOS_Connect_LoginCallbackInfo
{
    EOS_EResult          ResultCode;
    void*                ClientData;
    EOS_ProductUserId    LocalUserId;
    EOS_ContinuanceToken ContinuanceToken;
} EOS_Connect_LoginCallbackInfo;

typedef void (__cdecl *EOS_Connect_OnLoginCallback)(
    const EOS_Connect_LoginCallbackInfo* Data);

typedef struct EOS_Connect_Credentials
{
    int32_t     ApiVersion;
    const char* Token;
    int32_t     Type; // EOS_EExternalCredentialType
} EOS_Connect_Credentials;

typedef struct EOS_Connect_LoginOptions
{
    int32_t                              ApiVersion;
    const EOS_Connect_Credentials*       Credentials;
    const void*                          UserLoginInfo;
} EOS_Connect_LoginOptions;

typedef void (__cdecl *Fn_EOS_Connect_Login)(
    EOS_HConnect                       Handle,
    const EOS_Connect_LoginOptions*    Options,
    void*                              ClientData,
    EOS_Connect_OnLoginCallback        CompletionDelegate);

// Installer -- safe to call repeatedly. Returns true if EOSSDK
// was found loaded AND hooks were installed (or already in place).
bool InstallEosHooks();
