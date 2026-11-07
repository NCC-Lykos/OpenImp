// Branded sign-in screen settings; see Branding.h.

#include "Branding.h"
#include <shlobj.h>
#include "Registry.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using Registry::RegKey;

static const wchar_t c_szBrandingKey[] = L"SOFTWARE\\OpenImp\\Branding";
static const wchar_t c_szManagesLockScreen[] = L"ManagesLockScreen";

// Group Policy's "Do not display the lock screen" setting.
static const wchar_t c_szPersonalizationKey[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization";
static const wchar_t c_szNoLockScreen[] = L"NoLockScreen";

BrandingSettings BrandingSettings::Load()
{
    BrandingSettings s;
    RegKey key;
    if (key.Open(HKEY_LOCAL_MACHINE, c_szBrandingKey, KEY_READ) != ERROR_SUCCESS)
    {
        return s;
    }
    HKEY h = key.Get();
    s.enabled = Registry::ReadDwordOr(h, L"Enabled", 0) != 0;
    s.headline = Registry::ReadStringOr(h, L"Headline", s.headline);
    s.message = Registry::ReadStringOr(h, L"Message", s.message);
    auto image = [&](PCWSTR name)
    {
        std::wstring path = Registry::ReadStringOr(h, name, L"");
        return IsInImageDir(path) ? path : L"";
    };
    s.backgroundImage = image(L"BackgroundImage");
    s.logoImage = image(L"LogoImage");
    s.accentColor = Registry::ReadDwordOr(h, L"AccentColor", s.accentColor);
    s.textColor = Registry::ReadDwordOr(h, L"TextColor", s.textColor);
    s.backgroundColor = Registry::ReadDwordOr(h, L"BackgroundColor", s.backgroundColor);
    return s;
}

HRESULT BrandingSettings::Save() const
{
    RegKey key;
    HRESULT hr = HRESULT_FROM_WIN32(key.Create(HKEY_LOCAL_MACHINE, c_szBrandingKey, KEY_WRITE));
    if (FAILED(hr))
    {
        return hr;
    }
    HKEY h = key.Get();
    const HRESULT results[] =
    {
        Registry::WriteDword(h, L"Enabled", enabled ? 1 : 0),
        Registry::WriteString(h, L"Headline", headline),
        Registry::WriteString(h, L"Message", message),
        Registry::WriteString(h, L"BackgroundImage", backgroundImage),
        Registry::WriteString(h, L"LogoImage", logoImage),
        Registry::WriteDword(h, L"AccentColor", accentColor),
        Registry::WriteDword(h, L"TextColor", textColor),
        Registry::WriteDword(h, L"BackgroundColor", backgroundColor),
    };
    for (HRESULT result : results)
    {
        if (FAILED(result))
        {
            return result;
        }
    }
    key.Close();
    return SkipLockScreen(enabled);
}

HRESULT BrandingSettings::SkipLockScreen(bool skip)
{
    // ManagesLockScreen records that OpenImp turned the policy on, so turning
    // it back off never undoes a policy someone else set.
    RegKey branding;
    LSTATUS status = skip
        ? branding.Create(HKEY_LOCAL_MACHINE, c_szBrandingKey, KEY_READ | KEY_WRITE)
        : branding.Open(HKEY_LOCAL_MACHINE, c_szBrandingKey, KEY_READ | KEY_WRITE);
    if (status != ERROR_SUCCESS)
    {
        // Nothing to undo if OpenImp never stored any branding.
        return skip ? HRESULT_FROM_WIN32(status) : S_OK;
    }

    RegKey policy;
    if (skip)
    {
        HRESULT hr = HRESULT_FROM_WIN32(policy.Create(HKEY_LOCAL_MACHINE, c_szPersonalizationKey, KEY_READ | KEY_WRITE));
        if (SUCCEEDED(hr) && Registry::ReadDwordOr(policy.Get(), c_szNoLockScreen, 0) != 1)
        {
            hr = Registry::WriteDword(policy.Get(), c_szNoLockScreen, 1);
            if (SUCCEEDED(hr))
            {
                hr = Registry::WriteDword(branding.Get(), c_szManagesLockScreen, 1);
            }
        }
        return hr;
    }

    if (Registry::ReadDwordOr(branding.Get(), c_szManagesLockScreen, 0) != 0)
    {
        if (policy.Open(HKEY_LOCAL_MACHINE, c_szPersonalizationKey, KEY_READ | KEY_WRITE) == ERROR_SUCCESS
            && Registry::ReadDwordOr(policy.Get(), c_szNoLockScreen, 0) == 1)
        {
            RegDeleteValueW(policy.Get(), c_szNoLockScreen);
        }
        RegDeleteValueW(branding.Get(), c_szManagesLockScreen);
    }
    return S_OK;
}

bool BrandingSettings::ShouldShow() const
{
    return enabled && GetSystemMetrics(SM_CLEANBOOT) == 0;
}

std::wstring BrandingSettings::ImageDir()
{
    PWSTR programFiles = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles)))
    {
        dir = std::wstring(programFiles) + L"\\OpenImp\\Branding";
    }
    CoTaskMemFree(programFiles);
    return dir;
}

bool BrandingSettings::IsInImageDir(const std::wstring& path)
{
    std::wstring dir = ImageDir();
    if (dir.empty() || path.empty())
    {
        return false;
    }
    wchar_t full[MAX_PATH];
    DWORD cch = GetFullPathNameW(path.c_str(), ARRAYSIZE(full), full, nullptr);
    if (cch == 0 || cch >= ARRAYSIZE(full))
    {
        return false;
    }
    std::wstring resolved(full, cch);
    return resolved.size() > dir.size() + 1
        && _wcsnicmp(resolved.c_str(), dir.c_str(), dir.size()) == 0
        && resolved[dir.size()] == L'\\'
        && resolved.find_first_of(L"\\:", dir.size() + 1) == std::wstring::npos;  // no subfolder, no stream
}
