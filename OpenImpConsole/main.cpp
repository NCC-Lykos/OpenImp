// openimp.exe: test the badge reader and enroll badges for badge sign-in.
//
//   openimp read               show each badge tapped on the reader
//   openimp enroll [account]   link a badge to a Windows account (run as administrator)
//   openimp list               show enrolled badges
//   openimp remove <badge-id>  forget a badge (run as administrator)

#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include "..\shared\src\BadgeReader.h"
#include "..\shared\src\BadgeStore.h"

#pragma comment(lib, "secur32.lib")

static void PrintError(const wchar_t* what, HRESULT hr)
{
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, hr, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    fwprintf(stderr, L"%s failed (0x%08X): %s\n", what, static_cast<unsigned>(hr), msg ? msg : L"");
    LocalFree(msg);
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
    {
        fwprintf(stderr, L"Run this from an administrator command prompt.\n");
    }
}

static bool ConnectReader(BadgeReader& reader)
{
    // The badge service may still have the reader for a moment after a ReaderPause.
    for (int i = 0; i < 8 && !reader.Connect(); i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!reader.IsConnected())
    {
        fwprintf(stderr, L"Could not connect to the badge reader. Is it plugged in, and is pcProxAPI.dll next to openimp.exe?\n");
        return false;
    }
    return true;
}

// Waits for a badge to be tapped and returns its ID, or "" after the timeout.
static std::wstring WaitForBadge(BadgeReader& reader, std::chrono::seconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::wstring badge = reader.ReadBadge();
        if (!badge.empty())
        {
            return badge;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return L"";
}

static std::wstring ReadHidden(const wchar_t* prompt)
{
    wprintf(L"%s", prompt);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hIn, &mode);
    SetConsoleMode(hIn, mode & ~ENABLE_ECHO_INPUT);
    std::wstring value;
    std::getline(std::wcin, value);
    SetConsoleMode(hIn, mode);
    wprintf(L"\n");
    return value;
}

// Turns "name" into "COMPUTER\name"; leaves "DOMAIN\name" alone.
static std::wstring QualifyAccount(const std::wstring& account)
{
    if (account.find(L'\\') != std::wstring::npos)
    {
        return account;
    }
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD cch = ARRAYSIZE(computer);
    GetComputerNameW(computer, &cch);
    return std::wstring(computer) + L"\\" + account;
}

static std::wstring CurrentAccount()
{
    wchar_t name[512];
    ULONG cch = ARRAYSIZE(name);
    return GetUserNameExW(NameSamCompatible, name, &cch) ? name : L"";
}

static int CmdRead()
{
    ReaderPause pause;  // keeps the badge service from acting on taps (administrator only)
    BadgeReader reader;
    if (!ConnectReader(reader))
    {
        return 1;
    }
    wprintf(L"Tap a badge on the reader. Press Ctrl+C to stop.\n");
    // Same once-per-tap rule as the sign-in tile.
    std::wstring last;
    auto lastSeen = std::chrono::steady_clock::time_point();
    for (;;)
    {
        std::wstring badge = reader.ReadBadge();
        if (!badge.empty())
        {
            auto now = std::chrono::steady_clock::now();
            if (badge != last || now - lastSeen > std::chrono::seconds(3))
            {
                wprintf(L"Badge ID: %s\n", badge.c_str());
            }
            last = badge;
            lastSeen = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

static int CmdEnroll(const std::wstring& accountArg)
{
    std::wstring account = QualifyAccount(accountArg.empty() ? CurrentAccount() : accountArg);
    size_t slash = account.find(L'\\');
    std::wstring domain = account.substr(0, slash);
    std::wstring name = account.substr(slash + 1);

    ReaderPause pause;  // keeps the badge service from acting on the enrollment tap
    BadgeReader reader;
    if (!ConnectReader(reader))
    {
        return 1;
    }

    wprintf(L"Enrolling a badge for %s.\nTap the badge on the reader (30 seconds)...\n", account.c_str());
    std::wstring badge = WaitForBadge(reader, std::chrono::seconds(30));
    if (badge.empty())
    {
        fwprintf(stderr, L"No badge was read.\n");
        return 1;
    }
    wprintf(L"Badge ID: %s\n", badge.c_str());

    std::wstring password = ReadHidden((L"Windows password for " + account + L": ").c_str());

    // Check the password now, so a typo doesn't turn into a failed sign-in later.
    HANDLE hToken = nullptr;
    if (!LogonUserW(name.c_str(), domain.c_str(), password.c_str(), LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hToken))
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        SecureClear(password);
        PrintError(L"Checking the password", hr);
        return 1;
    }
    CloseHandle(hToken);

    HRESULT hr = BadgeStore::Save(badge, account, password);
    SecureClear(password);
    if (FAILED(hr))
    {
        PrintError(L"Saving the badge", hr);
        return 1;
    }
    wprintf(L"Done. Lock the PC (Windows+L) and tap the badge to test.\n");
    return 0;
}

static int CmdList()
{
    std::vector<BadgeEntry> entries;
    HRESULT hr = BadgeStore::List(entries);
    if (FAILED(hr))
    {
        PrintError(L"Reading enrolled badges", hr);
        return 1;
    }
    if (entries.empty())
    {
        wprintf(L"No badges enrolled.\n");
    }
    for (const BadgeEntry& entry : entries)
    {
        wprintf(L"%-24s %s\n", entry.badgeId.c_str(), entry.user.c_str());
    }
    return 0;
}

static int CmdRemove(const std::wstring& badge)
{
    HRESULT hr = BadgeStore::Remove(badge);
    if (FAILED(hr))
    {
        PrintError(L"Removing the badge", hr);
        return 1;
    }
    wprintf(L"Removed %s.\n", badge.c_str());
    return 0;
}

static int Usage()
{
    wprintf(L"Usage:\n"
            L"  openimp read               show each badge tapped on the reader\n"
            L"  openimp enroll [account]   link a badge to a Windows account (administrator)\n"
            L"  openimp list               show enrolled badges (administrator)\n"
            L"  openimp remove <badge-id>  forget a badge (administrator)\n");
    return 2;
}

int wmain(int argc, wchar_t* argv[])
{
    // Print immediately, even when output is piped to another program.
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::wstring cmd = argc > 1 ? argv[1] : L"";
    std::wstring arg = argc > 2 ? argv[2] : L"";

    if (cmd == L"read")   return CmdRead();
    if (cmd == L"enroll") return CmdEnroll(arg);
    if (cmd == L"list")   return CmdList();
    if (cmd == L"remove" && !arg.empty()) return CmdRemove(arg);
    return Usage();
}
