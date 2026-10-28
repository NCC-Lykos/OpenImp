// Badge enrollments and the service-to-tile handoff; see BadgeStore.h.

#include "BadgeStore.h"
#include <sddl.h>
#include <dpapi.h>
#include "Registry.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

using Registry::RegKey;

static const wchar_t c_szBadgesKey[] = L"SOFTWARE\\OpenImp\\Badges";
static const wchar_t c_szHandoffKey[] = L"SOFTWARE\\OpenImp\\Handoff";
static const ULONGLONG c_handoffLifetimeMs = 15000;

// Full control for SYSTEM and Administrators, nobody else, inherited by each badge key.
static const wchar_t c_szProtectedSddl[] = L"D:P(A;OICI;KA;;;SY)(A;OICI;KA;;;BA)";

// Opens one of OpenImp's SYSTEM/Administrators-only keys. For writing, the key
// is created if needed and its ACL is (re)applied, in case an older copy has a
// looser one.
static HRESULT OpenProtectedKey(PCWSTR path, bool forWrite, RegKey& key)
{
    if (!forWrite)
    {
        return HRESULT_FROM_WIN32(key.Open(HKEY_LOCAL_MACHINE, path, KEY_READ));
    }

    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(c_szProtectedSddl, SDDL_REVISION_1, &psd, nullptr))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), psd, FALSE };
    LSTATUS status = key.Create(HKEY_LOCAL_MACHINE, path, KEY_READ | KEY_WRITE | WRITE_DAC, &sa);
    if (status == ERROR_SUCCESS)
    {
        RegSetKeySecurity(key.Get(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, psd);
    }
    LocalFree(psd);
    return HRESULT_FROM_WIN32(status);
}

HRESULT BadgeStore::Save(const std::wstring& badgeId, const std::wstring& user, const std::wstring& password)
{
    DATA_BLOB plain = { static_cast<DWORD>(password.size() * sizeof(wchar_t)),
                        reinterpret_cast<BYTE*>(const_cast<wchar_t*>(password.data())) };
    DATA_BLOB cipher = {};
    if (!CryptProtectData(&plain, L"OpenImp", nullptr, nullptr, nullptr,
        CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &cipher))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    RegKey badges, badge;
    HRESULT hr = OpenProtectedKey(c_szBadgesKey, true, badges);
    if (SUCCEEDED(hr))
    {
        hr = HRESULT_FROM_WIN32(badge.Create(badges.Get(), badgeId.c_str(), KEY_WRITE));
    }
    if (SUCCEEDED(hr))
    {
        hr = Registry::WriteString(badge.Get(), L"User", user);
    }
    if (SUCCEEDED(hr))
    {
        hr = Registry::WriteBinary(badge.Get(), L"Password", cipher.pbData, cipher.cbData);
    }
    LocalFree(cipher.pbData);
    return hr;
}

HRESULT BadgeStore::Lookup(const std::wstring& badgeId, std::wstring& user, std::wstring& password)
{
    user.clear();
    password.clear();

    RegKey badges, badge;
    HRESULT hr = OpenProtectedKey(c_szBadgesKey, false, badges);
    if (SUCCEEDED(hr))
    {
        hr = HRESULT_FROM_WIN32(badge.Open(badges.Get(), badgeId.c_str(), KEY_READ));
    }
    if (FAILED(hr))
    {
        return hr;
    }

    std::vector<BYTE> cipherData;
    if (!Registry::ReadString(badge.Get(), L"User", user) || !Registry::ReadBinary(badge.Get(), L"Password", cipherData))
    {
        user.clear();
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    DATA_BLOB cipher = { static_cast<DWORD>(cipherData.size()), cipherData.data() };
    DATA_BLOB plain = {};
    if (!CryptUnprotectData(&cipher, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain))
    {
        user.clear();
        return HRESULT_FROM_WIN32(GetLastError());
    }
    password.assign(reinterpret_cast<const wchar_t*>(plain.pbData), plain.cbData / sizeof(wchar_t));
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return S_OK;
}

HRESULT BadgeStore::Remove(const std::wstring& badgeId)
{
    RegKey badges;
    HRESULT hr = OpenProtectedKey(c_szBadgesKey, true, badges);
    if (SUCCEEDED(hr))
    {
        hr = HRESULT_FROM_WIN32(RegDeleteKeyW(badges.Get(), badgeId.c_str()));
    }
    return hr;
}

HRESULT BadgeStore::List(std::vector<BadgeEntry>& entries)
{
    entries.clear();

    RegKey badges;
    HRESULT hr = OpenProtectedKey(c_szBadgesKey, false, badges);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
    {
        return S_OK; // nothing enrolled yet
    }
    if (FAILED(hr))
    {
        return hr;
    }

    wchar_t name[256];
    DWORD cch = ARRAYSIZE(name);
    for (DWORD i = 0; RegEnumKeyExW(badges.Get(), i, name, &cch, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        i++, cch = ARRAYSIZE(name))
    {
        BadgeEntry entry;
        entry.badgeId = name;
        RegKey badge;
        if (badge.Open(badges.Get(), name, KEY_READ) == ERROR_SUCCESS)
        {
            Registry::ReadString(badge.Get(), L"User", entry.user);
        }
        entries.push_back(entry);
    }
    return S_OK;
}

HRESULT BadgeStore::SaveHandoff(const std::wstring& badgeId, HandoffAction action)
{
    RegKey key;
    HRESULT hr = OpenProtectedKey(c_szHandoffKey, true, key);
    if (SUCCEEDED(hr))
    {
        hr = Registry::WriteString(key.Get(), L"Badge", badgeId);
    }
    if (SUCCEEDED(hr))
    {
        hr = Registry::WriteDword(key.Get(), L"Action", static_cast<DWORD>(action));
    }
    if (SUCCEEDED(hr))
    {
        // GetTickCount64 is system-wide, so the tile (another process) can tell how old this is.
        hr = Registry::WriteQword(key.Get(), L"Tick", GetTickCount64());
    }
    return hr;
}

bool BadgeStore::TakeHandoff(std::wstring& badgeId, HandoffAction& action)
{
    badgeId.clear();
    action = HandoffAction::Lock;

    std::wstring badge;
    DWORD actionValue = 0;
    ULONGLONG tick = 0;
    bool ok;
    {
        RegKey key;
        if (FAILED(OpenProtectedKey(c_szHandoffKey, false, key)))
        {
            return false;
        }
        ok = Registry::ReadString(key.Get(), L"Badge", badge)
            && Registry::ReadDword(key.Get(), L"Action", actionValue)
            && Registry::ReadQword(key.Get(), L"Tick", tick);
    }

    // Each handoff is used once, even if it turns out to be stale.
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, c_szHandoffKey);

    if (!ok || badge.empty() || GetTickCount64() - tick > c_handoffLifetimeMs)
    {
        return false;
    }
    badgeId = badge;
    action = static_cast<HandoffAction>(actionValue);
    return true;
}
