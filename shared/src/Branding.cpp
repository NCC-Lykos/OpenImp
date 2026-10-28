#include "Branding.h"
#include <vector>

#pragma comment(lib, "advapi32.lib")

static const wchar_t c_szBrandingKey[] = L"SOFTWARE\\OpenImp\\Branding";
static const wchar_t c_szPersonalizationKey[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization";
static const wchar_t c_szNoLockScreen[] = L"NoLockScreen";

static std::wstring ReadString(HKEY hKey, PCWSTR name, const std::wstring& fallback)
{
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS || type != REG_SZ)
    {
        return fallback;
    }
    std::vector<wchar_t> data(cb / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(hKey, name, nullptr, nullptr, reinterpret_cast<BYTE*>(data.data()), &cb) != ERROR_SUCCESS)
    {
        return fallback;
    }
    return data.data();
}

static DWORD ReadDword(HKEY hKey, PCWSTR name, DWORD fallback)
{
    DWORD type = 0, value = 0, cb = sizeof(value);
    if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &cb) != ERROR_SUCCESS || type != REG_DWORD)
    {
        return fallback;
    }
    return value;
}

static LSTATUS WriteString(HKEY hKey, PCWSTR name, const std::wstring& value)
{
    return RegSetValueExW(hKey, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

static LSTATUS WriteDword(HKEY hKey, PCWSTR name, DWORD value)
{
    return RegSetValueExW(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

BrandingSettings BrandingSettings::Load()
{
    BrandingSettings s;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, c_szBrandingKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
    {
        return s;
    }
    s.enabled = ReadDword(hKey, L"Enabled", 0) != 0;
    s.headline = ReadString(hKey, L"Headline", s.headline);
    s.message = ReadString(hKey, L"Message", s.message);
    s.backgroundImage = ReadString(hKey, L"BackgroundImage", L"");
    s.logoImage = ReadString(hKey, L"LogoImage", L"");
    s.accentColor = ReadDword(hKey, L"AccentColor", s.accentColor);
    s.textColor = ReadDword(hKey, L"TextColor", s.textColor);
    s.backgroundColor = ReadDword(hKey, L"BackgroundColor", s.backgroundColor);
    RegCloseKey(hKey);
    return s;
}

HRESULT BrandingSettings::Save() const
{
    HKEY hKey;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, c_szBrandingKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (status != ERROR_SUCCESS)
    {
        return HRESULT_FROM_WIN32(status);
    }
    LSTATUS results[] =
    {
        WriteDword(hKey, L"Enabled", enabled ? 1 : 0),
        WriteString(hKey, L"Headline", headline),
        WriteString(hKey, L"Message", message),
        WriteString(hKey, L"BackgroundImage", backgroundImage),
        WriteString(hKey, L"LogoImage", logoImage),
        WriteDword(hKey, L"AccentColor", accentColor),
        WriteDword(hKey, L"TextColor", textColor),
        WriteDword(hKey, L"BackgroundColor", backgroundColor),
    };
    RegCloseKey(hKey);
    for (LSTATUS result : results)
    {
        if (result != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(result);
        }
    }
    return SkipLockScreen(enabled);
}

HRESULT BrandingSettings::SkipLockScreen(bool skip)
{
    // ManagesLockScreen records that OpenImp turned the policy on, so turning
    // it back off never undoes a policy someone else set.
    HKEY hBranding;
    LSTATUS status = skip
        ? RegCreateKeyExW(HKEY_LOCAL_MACHINE, c_szBrandingKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &hBranding, nullptr)
        : RegOpenKeyExW(HKEY_LOCAL_MACHINE, c_szBrandingKey, 0, KEY_READ | KEY_WRITE, &hBranding);
    if (status != ERROR_SUCCESS)
    {
        return skip ? HRESULT_FROM_WIN32(status) : S_OK;
    }
    bool ours = ReadDword(hBranding, L"ManagesLockScreen", 0) != 0;

    HKEY hPolicy;
    if (skip)
    {
        status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, c_szPersonalizationKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &hPolicy, nullptr);
        if (status == ERROR_SUCCESS)
        {
            if (ReadDword(hPolicy, c_szNoLockScreen, 0) != 1)
            {
                status = WriteDword(hPolicy, c_szNoLockScreen, 1);
                if (status == ERROR_SUCCESS)
                {
                    status = WriteDword(hBranding, L"ManagesLockScreen", 1);
                }
            }
            RegCloseKey(hPolicy);
        }
    }
    else if (ours)
    {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, c_szPersonalizationKey, 0, KEY_READ | KEY_WRITE, &hPolicy) == ERROR_SUCCESS)
        {
            if (ReadDword(hPolicy, c_szNoLockScreen, 0) == 1)
            {
                RegDeleteValueW(hPolicy, c_szNoLockScreen);
            }
            RegCloseKey(hPolicy);
        }
        RegDeleteValueW(hBranding, L"ManagesLockScreen");
    }
    RegCloseKey(hBranding);
    return HRESULT_FROM_WIN32(status);
}

bool BrandingSettings::ShouldShow() const
{
    return enabled && GetSystemMetrics(SM_CLEANBOOT) == 0;
}
