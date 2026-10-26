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

namespace BadgeStore
{
    HRESULT Save(const std::wstring& badgeId, const std::wstring& user, const std::wstring& password);
    HRESULT Lookup(const std::wstring& badgeId, std::wstring& user, std::wstring& password);
    HRESULT Remove(const std::wstring& badgeId);
    HRESULT List(std::vector<BadgeEntry>& entries);
}

// Overwrites a string's characters before it is freed.
void SecureClear(std::wstring& s);
