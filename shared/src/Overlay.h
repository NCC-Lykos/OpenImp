// The branded sign-in screen: a full-screen window the sign-in tile opens on
// top of the Windows sign-in and lock screens. It shows the background, logo,
// headline and message from BrandingSettings, the badge status, and username /
// password boxes. Cancel (or Esc) closes it and reveals the normal Windows
// screen underneath. OpenImp Manager opens the same window as a preview.
//
// The window runs on its own thread. Overlay methods may be called from any
// thread except the overlay's own (OverlayEvents callbacks run on it, so don't
// destroy the Overlay from inside one).

#pragma once

#include <memory>
#include <string>
#include "Branding.h"

enum class OverlayStatus
{
    Info,
    Error,
    NeedsWindows,   // Windows has to show its own screen (e.g. password must change): close the overlay
};

class OverlayEvents
{
public:
    // The user clicked Sign in. account is as typed.
    virtual void OnOverlaySignIn(const std::wstring& account, const std::wstring& password) = 0;
    // The user cancelled; the window is closing.
    virtual void OnOverlayCancel() = 0;

protected:
    ~OverlayEvents() = default;
};

class Overlay
{
public:
    // Opens the window on the calling thread's desktop. Returns null if it
    // couldn't be shown; the caller then just carries on without it.
    // events may be null (preview).
    static std::unique_ptr<Overlay> Show(const BrandingSettings& settings, OverlayEvents* events, bool preview);

    // Closes the window if it's still open and waits for its thread.
    virtual ~Overlay() = default;

    virtual void SetStatus(const std::wstring& text, bool isError) = 0;
    virtual bool IsOpen() const = 0;
};
