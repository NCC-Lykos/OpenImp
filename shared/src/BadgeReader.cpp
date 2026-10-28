#include <windows.h>
#include "BadgeReader.h"
#include "..\headers\RFIDeas_pcProxAPI.h"

static const wchar_t c_szPauseMutex[] = L"Global\\OpenImpReaderPause";

ReaderPause::ReaderPause() :
    _hMutex(CreateMutexW(nullptr, FALSE, c_szPauseMutex))
{
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
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, c_szPauseMutex);
    if (h)
    {
        CloseHandle(h);
        return true;
    }
    return false;
}

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

    short bits = getActiveID(32);
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
