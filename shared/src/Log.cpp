// The diagnostic log; see Log.h.

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <cstdarg>
#include <cwchar>
#include <atomic>
#include <string>
#include "Log.h"

#pragma comment(lib, "advapi32.lib")

static const ULONGLONG c_maxLogBytes = 1024 * 1024;  // then openimp.log becomes openimp.old.log

// %SystemRoot%\System32\LogFiles\OpenImp
static std::wstring LogDir()
{
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, ARRAYSIZE(windir));
    return std::wstring(windir) + L"\\System32\\LogFiles\\OpenImp";
}

// Creates the log folder readable by SYSTEM and Administrators only, or locks
// down one an older version left readable by all users (the change is inherited
// by the files already in it). The log names accounts and badge IDs; a Prox
// badge ID is all it takes to clone the badge, and anything typed into the
// branded screen's username box ends up here too.
static bool ProtectLogDir(const std::wstring& dir)
{
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
        SDDL_REVISION_1, &psd, nullptr))
    {
        return false;
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), psd, FALSE };
    bool ok = CreateDirectoryW(dir.c_str(), &sa) != FALSE;
    if (!ok && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        BOOL present = FALSE, defaulted = FALSE;
        PACL dacl = nullptr;
        ok = GetSecurityDescriptorDacl(psd, &present, &dacl, &defaulted) && present
            && SetNamedSecurityInfoW(const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    }
    LocalFree(psd);
    return ok;
}

// Each call opens, appends to and closes the file, so the sign-in tile and the
// service (separate processes) can both write without coordinating.
void Log(const wchar_t* format, ...)
{
    wchar_t message[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);

    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const wchar_t* process = wcsrchr(module, L'\\');
    process = process ? process + 1 : module;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1280];
    _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s[%lu:%lu] %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        process, GetCurrentProcessId(), GetCurrentThreadId(), message);

    char utf8[4096];
    int cb = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (cb <= 1)
    {
        return;
    }

    std::wstring dir = LogDir();
    // Until the folder is locked down, write nothing: tried again on each call.
    static std::atomic<bool> s_protected{ false };
    if (!s_protected && !(s_protected = ProtectLogDir(dir)))
    {
        return;
    }
    std::wstring path = dir + L"\\openimp.log";

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)
        && ((static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) > c_maxLogBytes)
    {
        MoveFileExW(path.c_str(), (dir + L"\\openimp.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    HANDLE hFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(hFile, utf8, static_cast<DWORD>(cb - 1), &written, nullptr);
        CloseHandle(hFile);
    }
}
