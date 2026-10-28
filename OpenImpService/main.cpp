// OpenImpService: badge taps while someone is signed in.
//
// The sign-in tile (OpenImpAuth) only runs while the sign-in or lock screen is up.
// This service covers the rest of the time. While a user is signed in at the
// console and the session is unlocked, it watches the reader:
//   - their own badge locks the PC;
//   - another enrolled user's badge disconnects the session (like "Switch user",
//     so their apps keep running) and the tile signs the new user straight in;
//   - badges that aren't enrolled are ignored.
// Whenever the session is locked, nobody is signed in, or OpenImp Manager is
// enrolling a badge, the service lets go of the reader for them.
//
//   OpenImpService.exe        run as the "OpenImp" service (started by Windows)
//   OpenImpService.exe lock   lock the current session; the service runs this in the user's session

#include <windows.h>
#include <wtsapi32.h>
#include <string>
#include "..\shared\src\BadgeReader.h"
#include "..\shared\src\BadgeStore.h"
#include "..\shared\src\Log.h"
#include "..\shared\src\Session.h"

#pragma comment(lib, "wtsapi32.lib")

static wchar_t c_szServiceName[] = L"OpenImp";

static const DWORD c_pollMs = 250;
static const DWORD c_reconnectMs = 2000;
static const DWORD c_afterTapMs = 2000;  // gives the sign-in screen time to come up and take the reader
static const ULONGLONG c_settleMs = 3000;  // badges seen this soon after watching starts aren't taps

static SERVICE_STATUS_HANDLE g_hStatus;
static SERVICE_STATUS g_status = { SERVICE_WIN32_OWN_PROCESS };
static HANDLE g_hStop;

static void SetState(DWORD state, DWORD exitCode = NO_ERROR)
{
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_status.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 5000;
    SetServiceStatus(g_hStatus, &g_status);
}

static DWORD WINAPI ServiceHandler(DWORD control, DWORD, LPVOID, LPVOID)
{
    switch (control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        SetState(SERVICE_STOP_PENDING);
        SetEvent(g_hStop);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// Only a process on the user's own desktop can lock it, so this runs
// "OpenImpService.exe lock" as the signed-in user, in their session.
static bool LockSession(DWORD sessionId)
{
    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &hToken))
    {
        return false;
    }

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring cmdLine = L"\"" + std::wstring(exe) + L"\" lock";

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessAsUserW(hToken, exe, &cmdLine[0], nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hToken);
    if (!ok)
    {
        return false;
    }

    DWORD exitCode = 1;
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

static bool DisconnectSession(DWORD sessionId)
{
    return WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, sessionId, FALSE) != FALSE;
}

static void OnTap(BadgeReader& reader, DWORD sessionId, const std::wstring& badge)
{
    std::wstring user, password;
    HRESULT hr = BadgeStore::Lookup(badge, user, password);
    SecureClear(password);
    if (FAILED(hr))
    {
        Log(L"tap %s in session %lu: not enrolled (0x%08X), ignored", badge.c_str(), sessionId, static_cast<unsigned>(hr));
        return;
    }

    // The sign-in screen is about to need the reader.
    reader.Disconnect();

    if (Session::IsOtherUser(sessionId, user))
    {
        Log(L"tap %s in session %lu: %s is another user, switching", badge.c_str(), sessionId, user.c_str());
        BadgeStore::SaveHandoff(badge, HandoffAction::Switch);
        if (!DisconnectSession(sessionId))
        {
            // Fast user switching is probably turned off. At least lock the PC.
            Log(L"disconnect failed (%lu), locking instead", GetLastError());
            BadgeStore::SaveHandoff(badge, HandoffAction::Lock);
            LockSession(sessionId);
        }
    }
    else
    {
        Log(L"tap %s in session %lu: %s is this session's user, locking", badge.c_str(), sessionId, user.c_str());
        BadgeStore::SaveHandoff(badge, HandoffAction::Lock);
        if (!LockSession(sessionId))
        {
            Log(L"lock failed (%lu), disconnecting instead", GetLastError());
            DisconnectSession(sessionId);
        }
    }
}

static void WatchReader()
{
    BadgeReader reader;
    TapFilter taps;
    bool watching = false;
    ULONGLONG watchStart = 0;
    bool paused = false;

    for (;;)
    {
        DWORD wait = c_pollMs;
        DWORD sessionId = 0;
        bool pauseActive = ReaderPause::IsActive();
        if (pauseActive != paused)
        {
            Log(pauseActive ? L"OpenImp Manager/enroll is using the reader; pausing" : L"reader pause ended");
            paused = pauseActive;
        }
        if (pauseActive || !Session::GetUnlockedConsoleSession(sessionId))
        {
            if (watching)
            {
                Log(L"stopped watching (session locked, signed out, or paused)");
            }
            reader.Disconnect();
            watching = false;
        }
        else if (!reader.Connect())
        {
            wait = c_reconnectMs;
        }
        else
        {
            std::wstring badge = reader.ReadBadge();
            if (!watching)
            {
                Log(L"watching session %lu", sessionId);
                watching = true;
                watchStart = GetTickCount64();
            }
            if (GetTickCount64() - watchStart < c_settleMs)
            {
                // The badge that just signed in or unlocked the PC may still be
                // resting on the reader, and the reader can miss it for a poll or
                // two, so nothing counts as a tap until things settle.
                if (!badge.empty())
                {
                    taps.Ignore(badge);
                }
            }
            else if (taps.IsNewTap(badge))
            {
                OnTap(reader, sessionId, badge);
                watching = false;
                wait = c_afterTapMs;
            }
        }

        if (WaitForSingleObject(g_hStop, wait) == WAIT_OBJECT_0)
        {
            return;
        }
    }
}

static void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_hStatus = RegisterServiceCtrlHandlerExW(c_szServiceName, ServiceHandler, nullptr);
    if (!g_hStatus)
    {
        return;
    }
    g_hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hStop)
    {
        SetState(SERVICE_STOPPED, GetLastError());
        return;
    }
    SetState(SERVICE_RUNNING);
    Log(L"service started");
    WatchReader();
    Log(L"service stopping");
    CloseHandle(g_hStop);
    SetState(SERVICE_STOPPED);
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc > 1 && lstrcmpiW(argv[1], L"lock") == 0)
    {
        return LockWorkStation() ? 0 : 1;
    }

    SERVICE_TABLE_ENTRYW table[] =
    {
        { c_szServiceName, ServiceMain },
        { nullptr, nullptr },
    };
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
