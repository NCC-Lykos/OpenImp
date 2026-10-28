// Windows session questions the badge service and the sign-in tile both ask.

#pragma once

#include <windows.h>
#include <string>

namespace Session
{
    // True when a user is signed in at the console and that session is unlocked,
    // which is when badge taps should lock or switch rather than sign in.
    bool GetUnlockedConsoleSession(DWORD& sessionId);

    // True only when the session's user and the account (DOMAIN\name) both
    // resolve and are different accounts. When unsure, it returns false.
    bool IsOtherUser(DWORD sessionId, const std::wstring& account);
}
