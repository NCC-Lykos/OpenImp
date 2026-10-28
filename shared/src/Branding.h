// Settings for the branded sign-in screen (see Overlay.h), stored under
// HKLM\SOFTWARE\OpenImp\Branding. Only administrators can change them.
// OpenImp Manager copies the images into %ProgramFiles%\OpenImp\Branding so
// they can't be swapped by other users.

#pragma once

#include <windows.h>
#include <string>

struct BrandingSettings
{
    bool enabled = false;
    std::wstring headline = L"Welcome";
    std::wstring message;
    std::wstring backgroundImage;   // full path, or empty for a solid color
    std::wstring logoImage;         // full path, or empty for no logo
    COLORREF accentColor = RGB(0, 120, 212);
    COLORREF textColor = RGB(255, 255, 255);
    COLORREF backgroundColor = RGB(24, 32, 52);

    static BrandingSettings Load();
    HRESULT Save() const;

    // True when the branded screen should cover the sign-in screen: enabled,
    // and not in Safe Mode (the way out if it ever gets in the way).
    bool ShouldShow() const;
};
