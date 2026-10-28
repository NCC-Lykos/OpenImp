// Credential buffer helpers; see helpers.h. Adapted from Microsoft's
// credential provider sample.

#include "helpers.h"
#include <intsafe.h>

HRESULT FieldDescriptorCoAllocCopy(
    _In_ const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR& rcpfd,
    _Outptr_result_nullonfailure_ CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd)
{
    *ppcpfd = nullptr;
    auto* pcpfd = static_cast<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR*>(CoTaskMemAlloc(sizeof(**ppcpfd)));
    if (!pcpfd)
    {
        return E_OUTOFMEMORY;
    }

    pcpfd->dwFieldID = rcpfd.dwFieldID;
    pcpfd->cpft = rcpfd.cpft;
    pcpfd->guidFieldType = rcpfd.guidFieldType;
    pcpfd->pszLabel = nullptr;
    HRESULT hr = rcpfd.pszLabel ? SHStrDupW(rcpfd.pszLabel, &pcpfd->pszLabel) : S_OK;
    if (FAILED(hr))
    {
        CoTaskMemFree(pcpfd);
        return hr;
    }
    *ppcpfd = pcpfd;
    return S_OK;
}

// Points a UNICODE_STRING at pwz. It does NOT copy the string, so pwz must
// outlive the UNICODE_STRING; that's only safe within GetSerialization.
static HRESULT UnicodeStringInitWithString(_In_ PWSTR pwz, _Out_ UNICODE_STRING* pus)
{
    if (!pwz)
    {
        return E_INVALIDARG;
    }
    USHORT cch;
    HRESULT hr = SizeTToUShort(wcslen(pwz), &cch);
    if (SUCCEEDED(hr))
    {
        hr = UShortMult(cch, static_cast<USHORT>(sizeof(wchar_t)), &pus->Length);   // in bytes, without the terminator
        if (FAILED(hr))
        {
            return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        }
        pus->MaximumLength = pus->Length;
        pus->Buffer = pwz;
    }
    return hr;
}

// Copies a string's characters into a packed buffer. Only for
// KerbInteractiveUnlockLogonPack, which sizes the buffer exactly.
static void PackedUnicodeStringCopy(const UNICODE_STRING& rus, PWSTR pwzBuffer, UNICODE_STRING* pus)
{
    pus->Length = rus.Length;
    pus->MaximumLength = rus.Length;
    pus->Buffer = pwzBuffer;
    CopyMemory(pus->Buffer, rus.Buffer, pus->Length);
}

HRESULT KerbInteractiveUnlockLogonInit(
    _In_ PWSTR pwzDomain,
    _In_ PWSTR pwzUsername,
    _In_ PWSTR pwzPassword,
    _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
    _Out_ KERB_INTERACTIVE_UNLOCK_LOGON* pkiul)
{
    // CredPackAuthenticationBuffer could do this, but it always produces a
    // KerbInteractiveLogon message; unlocking needs KerbWorkstationUnlockLogon.
    KERB_INTERACTIVE_UNLOCK_LOGON kiul;
    ZeroMemory(&kiul, sizeof(kiul));
    KERB_INTERACTIVE_LOGON* pkil = &kiul.Logon;

    HRESULT hr = UnicodeStringInitWithString(pwzDomain, &pkil->LogonDomainName);
    if (SUCCEEDED(hr))
    {
        hr = UnicodeStringInitWithString(pwzUsername, &pkil->UserName);
    }
    if (SUCCEEDED(hr))
    {
        hr = UnicodeStringInitWithString(pwzPassword, &pkil->Password);
    }
    if (FAILED(hr))
    {
        return hr;
    }

    switch (cpus)
    {
    case CPUS_UNLOCK_WORKSTATION:
        pkil->MessageType = KerbWorkstationUnlockLogon;
        break;
    case CPUS_LOGON:
        pkil->MessageType = KerbInteractiveLogon;
        break;
    case CPUS_CREDUI:
        pkil->MessageType = static_cast<KERB_LOGON_SUBMIT_TYPE>(0); // doesn't apply to CredUI
        break;
    default:
        return E_FAIL;
    }

    // The structure is just nested structs, so a flat copy is enough.
    CopyMemory(pkiul, &kiul, sizeof(*pkiul));
    return S_OK;
}

// Winlogon and LSA take a "packed" KERB_INTERACTIVE_UNLOCK_LOGON: one buffer
// holding the structure followed by the three strings, where each
// UNICODE_STRING's Buffer is a byte offset from the start of the buffer rather
// than a pointer, and the strings aren't null-terminated. For example:
//   LogonDomainName.Buffer = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON)
//   UserName.Buffer        = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) + LogonDomainName.Length
//   Password.Buffer        = ... + UserName.Length
HRESULT KerbInteractiveUnlockLogonPack(
    _In_ const KERB_INTERACTIVE_UNLOCK_LOGON& rkiulIn,
    _Outptr_result_bytebuffer_(*pcb) BYTE** prgb,
    _Out_ DWORD* pcb)
{
    const KERB_INTERACTIVE_LOGON* pkilIn = &rkiulIn.Logon;
    DWORD cb = sizeof(rkiulIn) + pkilIn->LogonDomainName.Length + pkilIn->UserName.Length + pkilIn->Password.Length;

    auto* pkiulOut = static_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(CoTaskMemAlloc(cb));
    if (!pkiulOut)
    {
        return E_OUTOFMEMORY;
    }
    ZeroMemory(&pkiulOut->LogonId, sizeof(pkiulOut->LogonId));

    BYTE* pbBuffer = reinterpret_cast<BYTE*>(pkiulOut) + sizeof(*pkiulOut);
    KERB_INTERACTIVE_LOGON* pkilOut = &pkiulOut->Logon;
    pkilOut->MessageType = pkilIn->MessageType;

    // Copy each string after the structure, then turn its pointer into an offset.
    auto pack = [&](const UNICODE_STRING& in, UNICODE_STRING& out)
    {
        PackedUnicodeStringCopy(in, reinterpret_cast<PWSTR>(pbBuffer), &out);
        out.Buffer = reinterpret_cast<PWSTR>(pbBuffer - reinterpret_cast<BYTE*>(pkiulOut));
        pbBuffer += out.Length;
    };
    pack(pkilIn->LogonDomainName, pkilOut->LogonDomainName);
    pack(pkilIn->UserName, pkilOut->UserName);
    pack(pkilIn->Password, pkilOut->Password);

    *prgb = reinterpret_cast<BYTE*>(pkiulOut);
    *pcb = cb;
    return S_OK;
}

// Points an LSA_STRING at a constant ANSI string, for LsaLookupAuthenticationPackage.
static HRESULT LsaInitString(_Out_ PSTRING pszDestinationString, _In_ PCSTR pszSourceString)
{
    USHORT usLength;
    HRESULT hr = SizeTToUShort(strlen(pszSourceString), &usLength);
    if (SUCCEEDED(hr))
    {
        pszDestinationString->Buffer = const_cast<PCHAR>(pszSourceString);
        pszDestinationString->Length = usLength;
        pszDestinationString->MaximumLength = pszDestinationString->Length + 1;
    }
    return hr;
}

HRESULT RetrieveNegotiateAuthPackage(_Out_ ULONG* pulAuthPackage)
{
    HANDLE hLsa;
    NTSTATUS status = LsaConnectUntrusted(&hLsa);
    if (FAILED(HRESULT_FROM_NT(status)))
    {
        return HRESULT_FROM_NT(status);
    }

    ULONG ulAuthPackage;
    LSA_STRING lsaszPackageName;
    LsaInitString(&lsaszPackageName, NEGOSSP_NAME_A);
    status = LsaLookupAuthenticationPackage(hLsa, &lsaszPackageName, &ulAuthPackage);
    LsaDeregisterLogonProcess(hLsa);
    if (FAILED(HRESULT_FROM_NT(status)))
    {
        return HRESULT_FROM_NT(status);
    }
    *pulAuthPackage = ulAuthPackage;
    return S_OK;
}

// Returns a copy of pwzToProtect encrypted with CredProtect. pwzToProtect must
// not be null or empty.
static HRESULT ProtectAndCopyString(_In_ PCWSTR pwzToProtect, _Outptr_result_nullonfailure_ PWSTR* ppwzProtected)
{
    *ppwzProtected = nullptr;

    // CredProtect takes a non-const string.
    PWSTR pwzCopy;
    HRESULT hr = SHStrDupW(pwzToProtect, &pwzCopy);
    if (FAILED(hr))
    {
        return hr;
    }

    // The first call, with no output buffer, reports the encrypted length. The
    // length passed in must include the terminator.
    DWORD cchIn = static_cast<DWORD>(wcslen(pwzCopy) + 1);
    DWORD cchProtected = 0;
    if (CredProtectW(FALSE, pwzCopy, cchIn, nullptr, &cchProtected, nullptr))
    {
        hr = E_UNEXPECTED;
    }
    else if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || cchProtected == 0)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else
    {
        auto* pwzProtected = static_cast<PWSTR>(CoTaskMemAlloc(cchProtected * sizeof(wchar_t)));
        if (!pwzProtected)
        {
            hr = E_OUTOFMEMORY;
        }
        else if (CredProtectW(FALSE, pwzCopy, cchIn, pwzProtected, &cchProtected, nullptr))
        {
            *ppwzProtected = pwzProtected;
            hr = S_OK;
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            CoTaskMemFree(pwzProtected);
        }
    }

    SecureZeroMemory(pwzCopy, wcslen(pwzCopy) * sizeof(wchar_t));
    CoTaskMemFree(pwzCopy);
    return hr;
}

HRESULT ProtectIfNecessaryAndCopyPassword(
    _In_ PCWSTR pwzPassword,
    _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
    _Outptr_result_nullonfailure_ PWSTR* ppwzProtectedPassword)
{
    *ppwzProtectedPassword = nullptr;

    // An empty password doesn't need encrypting.
    if (!pwzPassword || !*pwzPassword)
    {
        return SHStrDupW(L"", ppwzProtectedPassword);
    }

    // CredIsProtected takes a non-const string.
    PWSTR pwzCopy;
    HRESULT hr = SHStrDupW(pwzPassword, &pwzCopy);
    if (FAILED(hr))
    {
        return hr;
    }

    // Don't encrypt twice, and don't encrypt for CredUI, whose caller may not
    // expect an encrypted password.
    CRED_PROTECTION_TYPE protectionType;
    bool alreadyProtected = CredIsProtectedW(pwzCopy, &protectionType) && protectionType != CredUnprotected;
    if (cpus == CPUS_CREDUI || alreadyProtected)
    {
        hr = SHStrDupW(pwzCopy, ppwzProtectedPassword);
    }
    else
    {
        hr = ProtectAndCopyString(pwzCopy, ppwzProtectedPassword);
    }

    SecureZeroMemory(pwzCopy, wcslen(pwzCopy) * sizeof(wchar_t));
    CoTaskMemFree(pwzCopy);
    return hr;
}

// A CoTaskMemAlloc copy of cch characters.
static HRESULT CoTaskStrDupN(PCWSTR source, size_t cch, PWSTR* ppsz)
{
    *ppsz = static_cast<PWSTR>(CoTaskMemAlloc(sizeof(wchar_t) * (cch + 1)));
    if (!*ppsz)
    {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = StringCchCopyNW(*ppsz, cch + 1, source, cch);
    if (FAILED(hr))
    {
        CoTaskMemFree(*ppsz);
        *ppsz = nullptr;
    }
    return hr;
}

HRESULT SplitDomainAndUsername(
    _In_ PCWSTR pszQualifiedUserName,
    _Outptr_result_nullonfailure_ PWSTR* ppszDomain,
    _Outptr_result_nullonfailure_ PWSTR* ppszUsername)
{
    *ppszDomain = nullptr;
    *ppszUsername = nullptr;
    const wchar_t* pchSlash = wcschr(pszQualifiedUserName, L'\\');
    if (!pchSlash)
    {
        return E_UNEXPECTED;
    }

    PWSTR pszDomain = nullptr;
    PWSTR pszUsername = nullptr;
    HRESULT hr = CoTaskStrDupN(pszQualifiedUserName, pchSlash - pszQualifiedUserName, &pszDomain);
    if (SUCCEEDED(hr))
    {
        hr = CoTaskStrDupN(pchSlash + 1, wcslen(pchSlash + 1), &pszUsername);
    }
    if (FAILED(hr))
    {
        CoTaskMemFree(pszDomain);
        return hr;
    }
    *ppszDomain = pszDomain;
    *ppszUsername = pszUsername;
    return S_OK;
}
