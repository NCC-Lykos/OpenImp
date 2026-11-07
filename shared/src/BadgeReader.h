// Reads badge IDs from an RFIDeas (Imprivata) reader through the pcProx SDK.

#pragma once

#include <chrono>
#include <mutex>
#include <string>

// One connection to the reader. The SDK keeps global state, so use one
// BadgeReader from one thread only.
class BadgeReader
{
public:
    ~BadgeReader();

    // Connects if not already connected. Cheap to call on every poll.
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
    bool IsNewTap(const std::wstring& badge);

    // Treats this badge as already seen, so it only counts once it's taken off
    // the reader and tapped again.
    void Ignore(const std::wstring& badge);

private:
    static constexpr std::chrono::seconds c_retapGap{ 3 };
    std::wstring _last;
    std::chrono::steady_clock::time_point _lastSeen;
};

// Slows down guessing badge IDs (for example with a card emulator held to the
// reader): after a few unenrolled badges in a row, every tap is ignored for a
// while, doubling each time. Password sign-in is unaffected. Thread-safe.
class BadgeThrottle
{
public:
    // True while taps should be ignored.
    bool IsBlocked();
    void OnUnknownBadge();
    void OnEnrolledBadge();

private:
    static constexpr int c_freeMisses = 5;
    static constexpr std::chrono::seconds c_firstBlock{ 30 };
    static constexpr std::chrono::minutes c_maxBlock{ 15 };
    static constexpr std::chrono::minutes c_forgetAfter{ 15 };   // misses this old no longer count

    std::mutex _lock;
    int _misses = 0;
    std::chrono::steady_clock::time_point _lastMiss;
    std::chrono::steady_clock::time_point _blockedUntil;
};

// While a ReaderPause exists (OpenImp Manager, openimp enroll), the OpenImp
// service leaves the reader alone, so tapping a badge to enroll it doesn't lock
// the PC or switch users. Creating one needs administrator rights; without them
// it does nothing. IsActive only counts a pause object owned by Administrators
// or SYSTEM, so a standard user can't create one to switch the service off.
class ReaderPause
{
public:
    ReaderPause();
    ~ReaderPause();
    ReaderPause(const ReaderPause&) = delete;
    ReaderPause& operator=(const ReaderPause&) = delete;

    // True when this pause is in effect. False without administrator rights,
    // or when another user's process already took the name (the service then
    // keeps acting on taps, so an enrollment tap may lock the PC).
    bool IsHeld() const { return _held; }

    // True while any process holds a ReaderPause.
    static bool IsActive();

private:
    void* _hMutex;
    bool _held = false;
};
