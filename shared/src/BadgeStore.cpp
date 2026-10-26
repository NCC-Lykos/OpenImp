#include "BadgeStore.h"
#include <sddl.h>
#include <dpapi.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

static const wchar_t c_szBadgesKey[] = L"SOFTWARE\\OpenImp\\Badges";

// Full control for SYSTEM and Administrators, nobody else, inherited by each badge key.
static const wchar_t c_szBadgesSddl[] = L"D:P(A;OICI;KA;;;SY)(A;OICI;KA;;;BA)";

void SecureClear(std::wstring& s)
{
    if (!s.empty())
    {
        SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
    }
    s.clear();
}

static HRESULT OpenBadgesKey(bool forWrite, HKEY* phKey)
{
    *phKey = nullptr;
    LSTATUS status;
    if (!forWrite)
    {
        status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, c_szBadgesKey, 0, KEY_READ, phKey);
        return HRESULT_FROM_WIN32(status);
    }

    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(c_szBadgesSddl, SDDL_REVISION_1, &psd, nullptr))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), psd, FALSE };
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, c_szBadgesKey, 0, nullptr, 0,
        KEY_READ | KEY_WRITE | WRITE_DAC, &sa, phKey, nullptr);
    if (status == ERROR_SUCCESS)
    {
        // The key may predate us with a looser ACL; always tighten it.
        RegSetKeySecurity(*phKey, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, psd);
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

    HKEY hBadges;
    HRESULT hr = OpenBadgesKey(true, &hBadges);
    if (SUCCEEDED(hr))
    {
        HKEY hBadge;
        hr = HRESULT_FROM_WIN32(RegCreateKeyExW(hBadges, badgeId.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hBadge, nullptr));
        if (SUCCEEDED(hr))
        {
            hr = HRESULT_FROM_WIN32(RegSetValueExW(hBadge, L"User", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(user.c_str()), static_cast<DWORD>((user.size() + 1) * sizeof(wchar_t))));
            if (SUCCEEDED(hr))
            {
                hr = HRESULT_FROM_WIN32(RegSetValueExW(hBadge, L"Password", 0, REG_BINARY, cipher.pbData, cipher.cbData));
            }
            RegCloseKey(hBadge);
        }
        RegCloseKey(hBadges);
    }
    LocalFree(cipher.pbData);
    return hr;
}

static HRESULT ReadValue(HKEY hKey, PCWSTR name, DWORD expectedType, std::vector<BYTE>& data)
{
    DWORD type = 0, cb = 0;
    LSTATUS status = RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb);
    if (status == ERROR_SUCCESS && type != expectedType)
    {
        status = ERROR_INVALID_DATA;
    }
    if (status == ERROR_SUCCESS)
    {
        data.resize(cb);
        status = RegQueryValueExW(hKey, name, nullptr, nullptr, data.data(), &cb);
        data.resize(cb);
    }
    return HRESULT_FROM_WIN32(status);
}

HRESULT BadgeStore::Lookup(const std::wstring& badgeId, std::wstring& user, std::wstring& password)
{
    user.clear();
    password.clear();

    HKEY hBadges;
    HRESULT hr = OpenBadgesKey(false, &hBadges);
    if (FAILED(hr))
    {
        return hr;
    }

    HKEY hBadge;
    hr = HRESULT_FROM_WIN32(RegOpenKeyExW(hBadges, badgeId.c_str(), 0, KEY_READ, &hBadge));
    RegCloseKey(hBadges);
    if (FAILED(hr))
    {
        return hr;
    }

    std::vector<BYTE> userData, cipherData;
    hr = ReadValue(hBadge, L"User", REG_SZ, userData);
    if (SUCCEEDED(hr))
    {
        hr = ReadValue(hBadge, L"Password", REG_BINARY, cipherData);
    }
    RegCloseKey(hBadge);

    if (SUCCEEDED(hr))
    {
        user.assign(reinterpret_cast<const wchar_t*>(userData.data()), userData.size() / sizeof(wchar_t));
        user.resize(wcsnlen(user.c_str(), user.size()));

        DATA_BLOB cipher = { static_cast<DWORD>(cipherData.size()), cipherData.data() };
        DATA_BLOB plain = {};
        if (CryptUnprotectData(&cipher, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain))
        {
            password.assign(reinterpret_cast<const wchar_t*>(plain.pbData), plain.cbData / sizeof(wchar_t));
            SecureZeroMemory(plain.pbData, plain.cbData);
            LocalFree(plain.pbData);
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            user.clear();
        }
    }
    return hr;
}

HRESULT BadgeStore::Remove(const std::wstring& badgeId)
{
    HKEY hBadges;
    HRESULT hr = OpenBadgesKey(true, &hBadges);
    if (SUCCEEDED(hr))
    {
        hr = HRESULT_FROM_WIN32(RegDeleteKeyW(hBadges, badgeId.c_str()));
        RegCloseKey(hBadges);
    }
    return hr;
}

HRESULT BadgeStore::List(std::vector<BadgeEntry>& entries)
{
    entries.clear();

    HKEY hBadges;
    HRESULT hr = OpenBadgesKey(false, &hBadges);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
    {
        return S_OK; // nothing enrolled yet
    }
    if (FAILED(hr))
    {
        return hr;
    }

    wchar_t name[256];
    for (DWORD i = 0; ; i++)
    {
        DWORD cch = ARRAYSIZE(name);
        LSTATUS status = RegEnumKeyExW(hBadges, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS)
        {
            break;
        }

        BadgeEntry entry;
        entry.badgeId = name;
        HKEY hBadge;
        if (RegOpenKeyExW(hBadges, name, 0, KEY_READ, &hBadge) == ERROR_SUCCESS)
        {
            std::vector<BYTE> userData;
            if (SUCCEEDED(ReadValue(hBadge, L"User", REG_SZ, userData)))
            {
                userData.push_back(0);
                userData.push_back(0);
                entry.user = reinterpret_cast<const wchar_t*>(userData.data());
            }
            RegCloseKey(hBadge);
        }
        entries.push_back(entry);
    }
    RegCloseKey(hBadges);
    return S_OK;
}
