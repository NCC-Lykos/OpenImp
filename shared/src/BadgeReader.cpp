// Badge reader access through the RFIDeas pcProx SDK; see BadgeReader.h.

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include "BadgeReader.h"
#include "..\headers\RFIDeas_pcProxAPI.h"

#pragma comment(lib, "advapi32.lib")

static const wchar_t c_szPauseMutex[] = L"Global\\OpenImpReaderPause";

// ---- TapFilter -------------------------------------------------------------

bool TapFilter::IsNewTap(const std::wstring& badge)
{
    if (badge.empty())
    {
        return false;
    }
    auto now = std::chrono::steady_clock::now();
    bool isNew = badge != _last || now - _lastSeen > c_retapGap;
    _last = badge;
    _lastSeen = now;
    return isNew;
}

void TapFilter::Ignore(const std::wstring& badge)
{
    _last = badge;
    _lastSeen = std::chrono::steady_clock::now();
}

// ---- BadgeThrottle ---------------------------------------------------------

bool BadgeThrottle::IsBlocked()
{
    std::lock_guard<std::mutex> guard(_lock);
    return std::chrono::steady_clock::now() < _blockedUntil;
}

void BadgeThrottle::OnUnknownBadge()
{
    std::lock_guard<std::mutex> guard(_lock);
    auto now = std::chrono::steady_clock::now();
    if (_misses > 0 && now - _lastMiss > c_forgetAfter)
    {
        _misses = 0;
    }
    _lastMiss = now;
    if (++_misses < c_freeMisses)
    {
        return;
    }
    // 30 s, 60 s, 120 s, ... up to 15 minutes.
    int doublings = (std::min)(_misses - c_freeMisses, 5);
    std::chrono::steady_clock::duration block = c_firstBlock * (1 << doublings);
    _blockedUntil = now + (std::min)(block, std::chrono::steady_clock::duration(c_maxBlock));
}

void BadgeThrottle::OnEnrolledBadge()
{
    std::lock_guard<std::mutex> guard(_lock);
    _misses = 0;
}

// ---- ReaderPause -----------------------------------------------------------

// True when a kernel object is owned by Administrators or SYSTEM, so no
// standard user could have created it.
static bool HasTrustedOwner(HANDLE h)
{
    bool trusted = false;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &psd) == ERROR_SUCCESS
        && owner)
    {
        trusted = IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid);
    }
    LocalFree(psd);
    return trusted;
}

// Owned by Administrators; full access for SYSTEM and Administrators only.
// Setting the owner to Administrators fails unless the caller is elevated.
static const wchar_t c_szPauseSddl[] = L"O:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)";

ReaderPause::ReaderPause() :
    _hMutex(nullptr)
{
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(c_szPauseSddl, SDDL_REVISION_1, &psd, nullptr))
    {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), psd, FALSE };
        _hMutex = CreateMutexW(&sa, FALSE, c_szPauseMutex);
        // If the name already existed, our security wasn't applied; it only
        // counts if an administrator's process made it.
        _held = _hMutex && (GetLastError() != ERROR_ALREADY_EXISTS || HasTrustedOwner(_hMutex));
        LocalFree(psd);
    }
}

ReaderPause::~ReaderPause()
{
    if (_hMutex)
    {
        CloseHandle(_hMutex);
    }
}

bool ReaderPause::IsActive()
{
    // The mutex only exists while some process holds a ReaderPause. Any user
    // can create a mutex with this name, so it only counts if its owner is
    // Administrators or SYSTEM.
    HANDLE h = OpenMutexW(SYNCHRONIZE | READ_CONTROL, FALSE, c_szPauseMutex);
    if (!h)
    {
        return false;
    }
    bool trusted = HasTrustedOwner(h);
    CloseHandle(h);
    return trusted;
}

// ---- BadgeReader -----------------------------------------------------------

BadgeReader::~BadgeReader()
{
    Disconnect();
}

bool BadgeReader::Connect()
{
    if (!_connected)
    {
        _connected = (usbConnect() == 1);
    }
    return _connected;
}

void BadgeReader::Disconnect()
{
    if (_connected)
    {
        USBDisconnect();
        _connected = false;
    }
}

std::wstring BadgeReader::ReadBadge()
{
    if (!_connected)
    {
        return L"";
    }

    const short c_maxBits = 32 * 8;     // the ID buffer size passed to getActiveID
    short bits = getActiveID(32);
    if (bits > c_maxBits)
    {
        // Not a real badge (a faulty or spoofed reader); don't read past the SDK's buffer.
        return L"";
    }
    if (bits <= 0)
    {
        if (bits < 0)
        {
            // The reader went away; reconnect on the next Connect().
            Disconnect();
        }
        return L"";
    }

    // getActiveID_byte(0) is the least significant byte. Print most significant
    // first, and include the bit count so different card formats can't collide.
    static const wchar_t hexDigits[] = L"0123456789ABCDEF";
    std::wstring id = std::to_wstring(bits) + L"-";
    for (short i = static_cast<short>((bits + 7) / 8 - 1); i >= 0; --i)
    {
        unsigned char b = getActiveID_byte(i);
        id += hexDigits[b >> 4];
        id += hexDigits[b & 0x0F];
    }
    return id;
}
