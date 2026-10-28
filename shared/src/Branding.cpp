// Branded sign-in screen settings; see Branding.h.

#include "Branding.h"
#include "Registry.h"

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
    s.backgroundImage = Registry::ReadStringOr(h, L"BackgroundImage", L"");
    s.logoImage = Registry::ReadStringOr(h, L"LogoImage", L"");
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
