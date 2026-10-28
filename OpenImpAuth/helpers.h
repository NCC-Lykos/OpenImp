// Helpers for building the credential buffer that GetSerialization hands to
// LogonUI, adapted from Microsoft's credential provider sample. Also pulls in
// the Windows headers every credential provider file needs.

#pragma once

#pragma warning(push)
#pragma warning(disable: 28251)
#include <credentialprovider.h>
#include <ntsecapi.h>
#pragma warning(pop)

#define SECURITY_WIN32
#include <security.h>
#include <intsafe.h>

#include <windows.h>
#include <strsafe.h>

#pragma warning(push)
#pragma warning(disable: 4995)
#include <shlwapi.h>
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable: 28301)
#include <wincred.h>
#pragma warning(pop)

// Copies a field descriptor into memory from CoTaskMemAlloc, as LogonUI expects.
HRESULT FieldDescriptorCoAllocCopy(
    _In_ const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR& rcpfd,
    _Outptr_result_nullonfailure_ CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd);

// Fills a KERB_INTERACTIVE_UNLOCK_LOGON that points at (doesn't copy) the
// given strings, ready for KerbInteractiveUnlockLogonPack.
HRESULT KerbInteractiveUnlockLogonInit(
    _In_ PWSTR pwzDomain,
    _In_ PWSTR pwzUsername,
    _In_ PWSTR pwzPassword,
    _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
    _Out_ KERB_INTERACTIVE_UNLOCK_LOGON* pkiul);

// Packs the logon into one CoTaskMemAlloc buffer in the layout Windows expects.
HRESULT KerbInteractiveUnlockLogonPack(
    _In_ const KERB_INTERACTIVE_UNLOCK_LOGON& rkiulIn,
    _Outptr_result_bytebuffer_(*pcb) BYTE** prgb,
    _Out_ DWORD* pcb);

// The ID of the Negotiate authentication package the logon goes to.
HRESULT RetrieveNegotiateAuthPackage(_Out_ ULONG* pulAuthPackage);

// Copies a password, encrypting it with CredProtect when the scenario allows.
HRESULT ProtectIfNecessaryAndCopyPassword(
    _In_ PCWSTR pwzPassword,
    _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
    _Outptr_result_nullonfailure_ PWSTR* ppwzProtectedPassword);

// Splits DOMAIN\name into two CoTaskMemAlloc strings. Fails if there's no backslash.
HRESULT SplitDomainAndUsername(
    _In_ PCWSTR pszQualifiedUserName,
    _Outptr_result_nullonfailure_ PWSTR* ppszDomain,
    _Outptr_result_nullonfailure_ PWSTR* ppszUsername);
