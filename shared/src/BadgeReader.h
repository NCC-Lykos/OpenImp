// Reads badge IDs from an RFIDeas (Imprivata) reader through the pcProx SDK.
// The SDK keeps global state, so use one BadgeReader from one thread only.

#pragma once

#include <chrono>
#include <string>

class BadgeReader
{
public:
    ~BadgeReader();

    bool Connect();
    void Disconnect();
    bool IsConnected() const { return _connected; }

    // Returns the badge currently on the reader as "<bits>-<hex>", for example
    // "26-02B453FF", or an empty string when no badge is present.
    std::wstring ReadBadge();

private:
    bool _connected = false;
};

// Turns reader polls into taps: a badge counts once when it arrives, not on every
// poll while it rests on the reader. The reader can drop a resting badge for a
// moment, so the same badge only counts again after a short gap.
class TapFilter
{
public:
    // Pass every read, including empty ones. True when this read is a new tap.
    bool IsNewTap(const std::wstring& badge)
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

    // Treats this badge as already seen, so it only counts once it's taken off
    // the reader and tapped again.
    void Ignore(const std::wstring& badge)
    {
        _last = badge;
        _lastSeen = std::chrono::steady_clock::now();
    }

private:
    static constexpr std::chrono::seconds c_retapGap{ 3 };
    std::wstring _last;
    std::chrono::steady_clock::time_point _lastSeen;
};

// While a ReaderPause exists (OpenImp Manager, openimp enroll), the OpenImp
// service leaves the reader alone, so tapping a badge to enroll it doesn't lock
// the PC or switch users. Creating one needs administrator rights; without them
// it does nothing.
class ReaderPause
{
public:
    ReaderPause();
    ~ReaderPause();
    ReaderPause(const ReaderPause&) = delete;
    ReaderPause& operator=(const ReaderPause&) = delete;

    static bool IsActive();

private:
    void* _hMutex;
};
