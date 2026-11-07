// Windows session questions the badge service and the sign-in tile both ask.

#pragma once

#include <windows.h>
#include <string>

namespace Session
{
    // True when a user is signed in at the console and that session is unlocked,
    // which is when badge taps should lock or switch rather than sign in.
    bool GetUnlockedConsoleSession(DWORD& sessionId);

    // The session the calling process runs in.
    bool GetCurrentSession(DWORD& sessionId);

    // True when the calling process's session is attached to the physical
    // console rather than a Remote Desktop client. The badge reader is on the
    // physical PC, so a tap must never sign in or unlock a remote session.
    // When unsure, it returns false.
    bool IsCurrentSessionLocalConsole();

    // IsCurrentSessionLocalConsole, and the calling process's session is the
    // one attached to the console right now (not a session switched away from
    // with Fast User Switching). Checked on every tap and before every sign-in.
    bool IsCurrentSessionAtConsole();

    // True only when the session's user and the account (in the qualified form
    // from Account.h) both resolve and are different accounts. When unsure, it
    // returns false.
    bool IsOtherUser(DWORD sessionId, const std::wstring& account);
}
