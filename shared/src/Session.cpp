// Session helpers; see Session.h.

#include "Session.h"
#include <wtsapi32.h>
#include <vector>
#include "Account.h"

#pragma comment(lib, "wtsapi32.lib")

bool Session::GetUnlockedConsoleSession(DWORD& sessionId)
{
    sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF)
    {
        return false;   // the console is between sessions
    }

    WTSINFOEXW* pInfo = nullptr;
    DWORD cb = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSSessionInfoEx,
        reinterpret_cast<LPWSTR*>(&pInfo), &cb) || !pInfo)
    {
        return false;
    }
    bool unlocked = pInfo->Level == 1
        && pInfo->Data.WTSInfoExLevel1.SessionState == WTSActive
        && pInfo->Data.WTSInfoExLevel1.UserName[0] != L'\0'
        && pInfo->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
    WTSFreeMemory(pInfo);
    return unlocked;
}

bool Session::GetCurrentSession(DWORD& sessionId)
{
    return ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) != FALSE;
}

// Resolves an account name to its SID.
static bool AccountSid(const std::wstring& account, std::vector<BYTE>& sid)
{
    DWORD cbSid = 0, cchDomain = 0;
    SID_NAME_USE use;
    LookupAccountNameW(nullptr, account.c_str(), nullptr, &cbSid, nullptr, &cchDomain, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        return false;
    }
    sid.resize(cbSid);
    std::vector<wchar_t> domain(cchDomain);
    return LookupAccountNameW(nullptr, account.c_str(), sid.data(), &cbSid, domain.data(), &cchDomain, &use) != FALSE;
}

// One string property of a session, such as its user or domain name.
static std::wstring SessionString(DWORD sessionId, WTS_INFO_CLASS infoClass)
{
    LPWSTR pValue = nullptr;
    DWORD cb = 0;
    std::wstring value;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, infoClass, &pValue, &cb) && pValue)
    {
        value = pValue;
    }
    WTSFreeMemory(pValue);
    return value;
}

bool Session::IsOtherUser(DWORD sessionId, const std::wstring& account)
{
    std::wstring user = SessionString(sessionId, WTSUserName);
    std::wstring domain = SessionString(sessionId, WTSDomainName);
    if (user.empty())
    {
        return false;
    }

    // Comparing SIDs avoids being fooled by different spellings of the same account.
    std::vector<BYTE> sessionSid, accountSid;
    if (!AccountSid(domain.empty() ? user : domain + L"\\" + user, sessionSid)
        || !AccountSid(Account::LookupName(account), accountSid))
    {
        return false;
    }
    return !EqualSid(sessionSid.data(), accountSid.data());
}
