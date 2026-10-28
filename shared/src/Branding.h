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

    // The saved settings, or the defaults above when nothing is saved.
    static BrandingSettings Load();

    // Saves the settings. Also turns Windows' lock screen (the clock picture)
    // off while the branded screen is enabled, so locking shows the branded
    // screen at once, and back on when it's disabled.
    HRESULT Save() const;

    // Turns the "Do not display the lock screen" policy (NoLockScreen) on, or
    // back off if OpenImp was the one that turned it on.
    static HRESULT SkipLockScreen(bool skip);

    // True when the branded screen should cover the sign-in screen: enabled,
    // and not in Safe Mode (the way out if it ever gets in the way).
    bool ShouldShow() const;
};
