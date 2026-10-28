// Account name helpers; see Account.h.

#include "Account.h"

#pragma comment(lib, "advapi32.lib")

namespace Account
{
    std::wstring ComputerName()
    {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD cch = ARRAYSIZE(name);
        GetComputerNameW(name, &cch);
        return name;
    }

    std::wstring Qualify(const std::wstring& account)
    {
        if (account.find(L'\\') != std::wstring::npos)
        {
            return account;
        }
        if (account.find(L'@') != std::wstring::npos)
        {
            return L"\\" + account;
        }
        return ComputerName() + L"\\" + account;
    }

    std::wstring LookupName(const std::wstring& qualified)
    {
        return !qualified.empty() && qualified[0] == L'\\' ? qualified.substr(1) : qualified;
    }

    HRESULT CheckPassword(const std::wstring& qualified, const std::wstring& password)
    {
        size_t slash = qualified.find(L'\\');
        std::wstring domain = slash == std::wstring::npos ? L"" : qualified.substr(0, slash);
        std::wstring name = slash == std::wstring::npos ? qualified : qualified.substr(slash + 1);

        // A UPN has no domain part; LogonUser wants a null domain for it.
        HANDLE hToken = nullptr;
        if (!LogonUserW(name.c_str(), domain.empty() ? nullptr : domain.c_str(), password.c_str(),
            LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hToken))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        CloseHandle(hToken);
        return S_OK;
    }
}
