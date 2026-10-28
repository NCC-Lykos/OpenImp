// Maps badge IDs to Windows accounts.
//
// Each badge is a key under HKLM\SOFTWARE\OpenImp\Badges named by its badge ID,
// holding the account name ("User", DOMAIN\name) and its password ("Password",
// encrypted with machine-scope DPAPI). Only SYSTEM and Administrators can read
// the key, which is what lets the sign-in screen (running as SYSTEM) use it.

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
    HRESULT Save(const std::wstring& badgeId, const std::wstring& user, const std::wstring& password);
    HRESULT Lookup(const std::wstring& badgeId, std::wstring& user, std::wstring& password);
    HRESULT Remove(const std::wstring& badgeId);
    HRESULT List(std::vector<BadgeEntry>& entries);

    // Passes a badge tap to the next sign-in screen, under HKLM\SOFTWARE\OpenImp\Handoff
    // (SYSTEM and Administrators only). TakeHandoff returns it once, and only
    // within a few seconds of SaveHandoff.
    HRESULT SaveHandoff(const std::wstring& badgeId, HandoffAction action);
    bool TakeHandoff(std::wstring& badgeId, HandoffAction& action);
}

// Overwrites a string's characters before it is freed.
void SecureClear(std::wstring& s);
