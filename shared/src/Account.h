// Windows account names, as typed by people and as stored by OpenImp.
//
// OpenImp stores accounts in a "qualified" form that the sign-in tile can pass
// straight to Windows:
//   COMPUTER\name   a local account (what a bare "name" becomes)
//   DOMAIN\name     a domain account, kept as typed
//   \name@domain    a UPN; the empty domain tells Windows to resolve it

#pragma once

#include <windows.h>
#include <string>

namespace Account
{
    // This PC's name, for example DESKTOP-L9HN4TS.
    std::wstring ComputerName();

    // Turns a typed account into the qualified form described above.
    std::wstring Qualify(const std::wstring& account);

    // The name to give LookupAccountName: the qualified form without the empty
    // domain a UPN carries.
    std::wstring LookupName(const std::wstring& qualified);

    // Checks a qualified account's password by signing it in once, so a typo is
    // caught when a badge is enrolled rather than at the sign-in screen.
    HRESULT CheckPassword(const std::wstring& qualified, const std::wstring& password);
}
