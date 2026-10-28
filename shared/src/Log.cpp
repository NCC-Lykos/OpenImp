// The diagnostic log; see Log.h.

#include <windows.h>
#include <cstdarg>
#include <cwchar>
#include <string>
#include "Log.h"

static const ULONGLONG c_maxLogBytes = 1024 * 1024;  // then openimp.log becomes openimp.old.log

// %SystemRoot%\System32\LogFiles\OpenImp
static std::wstring LogDir()
{
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, ARRAYSIZE(windir));
    return std::wstring(windir) + L"\\System32\\LogFiles\\OpenImp";
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
    CreateDirectoryW(dir.c_str(), nullptr);
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
