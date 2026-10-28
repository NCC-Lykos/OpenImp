// Maps badge IDs to Windows accounts.
//
// Each badge is a key under HKLM\SOFTWARE\OpenImp\Badges named by its badge ID,
// holding the account name ("User", in the qualified form from Account.h) and
// its password ("Password", encrypted with machine-scope DPAPI). Only SYSTEM and
// Administrators can read the key, which is what lets the sign-in screen
// (running as SYSTEM) use it while keeping it from everyone else.

#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct BadgeEntry
{
    std::wstring badgeId;
    std::wstring user;
};

// What the OpenImp service did with a badge tap before the sign-in screen came up.
enum class HandoffAction : DWORD
{
    Lock = 0,    // locked the badge owner's session: the tile ignores that badge until it's tapped again
    Switch = 1,  // left another user's session: the tile signs the badge owner in right away
};

namespace BadgeStore
{
    // Links a badge to an account, replacing any earlier link. Needs administrator rights.
    HRESULT Save(const std::wstring& badgeId, const std::wstring& user, const std::wstring& password);

    // Finds a badge's account and decrypts its password. Fails with
    // ERROR_FILE_NOT_FOUND when the badge isn't enrolled.
    HRESULT Lookup(const std::wstring& badgeId, std::wstring& user, std::wstring& password);

    // Forgets a badge. Needs administrator rights.
    HRESULT Remove(const std::wstring& badgeId);

    // Every enrolled badge and its account (no passwords).
    HRESULT List(std::vector<BadgeEntry>& entries);

    // Passes a badge tap to the next sign-in screen, under HKLM\SOFTWARE\OpenImp\Handoff
    // (SYSTEM and Administrators only). TakeHandoff returns it once, and only
    // within a few seconds of SaveHandoff.
    HRESULT SaveHandoff(const std::wstring& badgeId, HandoffAction action);
    bool TakeHandoff(std::wstring& badgeId, HandoffAction& action);
}
