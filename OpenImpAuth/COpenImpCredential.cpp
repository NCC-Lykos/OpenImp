#include "COpenImpCredential.h"
#include "guid.h"
#include "resource.h"
#include "..\shared\src\BadgeStore.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

static const wchar_t c_szTapPrompt[] = L"Tap your badge on the reader";

COpenImpCredential::COpenImpCredential(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus) :
    _cRef(1),
    _cpus(cpus),
    _status(c_szTapPrompt)
{
    DllAddRef();
}

COpenImpCredential::~COpenImpCredential()
{
    _ClearPendingLogon();
    DllRelease();
}

void COpenImpCredential::_ClearPendingLogon()
{
    std::lock_guard<std::mutex> guard(_lock);
    SecureClear(_pendingUser);
    SecureClear(_pendingPassword);
}

void COpenImpCredential::SetPendingLogon(const std::wstring& user, const std::wstring& password)
{
    std::lock_guard<std::mutex> guard(_lock);
    _pendingUser = user;
    _pendingPassword = password;
    _status = L"Signing in as " + user;
}

void COpenImpCredential::SetStatus(const std::wstring& status)
{
    std::lock_guard<std::mutex> guard(_lock);
    _status = status;
}

bool COpenImpCredential::HasPendingLogon()
{
    std::lock_guard<std::mutex> guard(_lock);
    return !_pendingUser.empty();
}

HRESULT COpenImpCredential::Advise(_In_ ICredentialProviderCredentialEvents* /*pcpce*/)
{
    return S_OK;
}

HRESULT COpenImpCredential::UnAdvise()
{
    return S_OK;
}

HRESULT COpenImpCredential::SetSelected(_Out_ BOOL* pbAutoLogon)
{
    *pbAutoLogon = HasPendingLogon();
    return S_OK;
}

HRESULT COpenImpCredential::SetDeselected()
{
    return S_OK;
}

HRESULT COpenImpCredential::GetFieldState(DWORD dwFieldID,
    _Out_ CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
    _Out_ CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis)
{
    if (dwFieldID >= SFI_NUM_FIELDS)
    {
        return E_INVALIDARG;
    }
    *pcpfs = s_rgFieldStatePairs[dwFieldID].cpfs;
    *pcpfis = s_rgFieldStatePairs[dwFieldID].cpfis;
    return S_OK;
}

HRESULT COpenImpCredential::GetStringValue(DWORD dwFieldID, _Outptr_result_nullonfailure_ PWSTR* ppwsz)
{
    *ppwsz = nullptr;
    switch (dwFieldID)
    {
    case SFI_LABEL:
        return SHStrDupW(L"Badge sign-in", ppwsz);
    case SFI_STATUS:
    {
        std::lock_guard<std::mutex> guard(_lock);
        return SHStrDupW(_status.c_str(), ppwsz);
    }
    default:
        return E_INVALIDARG;
    }
}

HRESULT COpenImpCredential::GetBitmapValue(DWORD dwFieldID, _Outptr_result_nullonfailure_ HBITMAP* phbmp)
{
    *phbmp = nullptr;
    if (dwFieldID != SFI_TILEIMAGE)
    {
        return E_INVALIDARG;
    }
    *phbmp = LoadBitmap(HINST_THISDLL, MAKEINTRESOURCE(IDB_TILE_IMAGE));
    return *phbmp ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

// The tile has no checkboxes, combo boxes, buttons or editable fields.
HRESULT COpenImpCredential::GetCheckboxValue(DWORD, _Out_ BOOL* pbChecked, _Outptr_result_nullonfailure_ PWSTR* ppwszLabel)
{
    *pbChecked = FALSE;
    *ppwszLabel = nullptr;
    return E_NOTIMPL;
}

HRESULT COpenImpCredential::GetComboBoxValueCount(DWORD, _Out_ DWORD* pcItems, _Deref_out_range_(< , *pcItems) _Out_ DWORD* pdwSelectedItem)
{
    *pcItems = 0;
    *pdwSelectedItem = 0;
    return E_NOTIMPL;
}

HRESULT COpenImpCredential::GetComboBoxValueAt(DWORD, DWORD, _Outptr_result_nullonfailure_ PWSTR* ppwszItem)
{
    *ppwszItem = nullptr;
    return E_NOTIMPL;
}

HRESULT COpenImpCredential::GetSubmitButtonValue(DWORD, _Out_ DWORD* pdwAdjacentTo)
{
    *pdwAdjacentTo = 0;
    return E_NOTIMPL;
}

HRESULT COpenImpCredential::SetStringValue(DWORD, _In_ PCWSTR)
{
    return E_NOTIMPL;
}

HRESULT COpenImpCredential::SetCheckboxValue(DWORD, BOOL)
{
    return E_NOTIMPL;
}

HRESULT COpenImpCredential::SetComboBoxSelectedValue(DWORD, DWORD)
{
    return E_NOTIMPL;
}

HRESULT COpenImpCredential::CommandLinkClicked(DWORD)
{
    return E_NOTIMPL;
}

// Packs the account and password from the last badge tap into a Kerberos
// interactive (or unlock) logon buffer. Each tap is used for one attempt only.
HRESULT COpenImpCredential::GetSerialization(_Out_ CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
    _Out_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
    _Outptr_result_maybenull_ PWSTR* ppwszOptionalStatusText,
    _Out_ CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon)
{
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    ZeroMemory(pcpcs, sizeof(*pcpcs));

    std::wstring user, password;
    {
        std::lock_guard<std::mutex> guard(_lock);
        user.swap(_pendingUser);
        password.swap(_pendingPassword);
    }

    if (user.empty())
    {
        SHStrDupW(c_szTapPrompt, ppwszOptionalStatusText);
        *pcpsiOptionalStatusIcon = CPSI_WARNING;
        SecureClear(password);
        return S_OK;
    }

    PWSTR pszDomain = nullptr;
    PWSTR pszUsername = nullptr;
    PWSTR pwzProtectedPassword = nullptr;
    HRESULT hr = SplitDomainAndUsername(user.c_str(), &pszDomain, &pszUsername);
    if (SUCCEEDED(hr))
    {
        hr = ProtectIfNecessaryAndCopyPassword(password.c_str(), _cpus, &pwzProtectedPassword);
    }
    if (SUCCEEDED(hr))
    {
        KERB_INTERACTIVE_UNLOCK_LOGON kiul;
        hr = KerbInteractiveUnlockLogonInit(pszDomain, pszUsername, pwzProtectedPassword, _cpus, &kiul);
        if (SUCCEEDED(hr))
        {
            hr = KerbInteractiveUnlockLogonPack(kiul, &pcpcs->rgbSerialization, &pcpcs->cbSerialization);
        }
        if (SUCCEEDED(hr))
        {
            ULONG ulAuthPackage;
            hr = RetrieveNegotiateAuthPackage(&ulAuthPackage);
            if (SUCCEEDED(hr))
            {
                pcpcs->ulAuthenticationPackage = ulAuthPackage;
                pcpcs->clsidCredentialProvider = CLSID_OpenImp;
                *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
            }
            else
            {
                CoTaskMemFree(pcpcs->rgbSerialization);
                pcpcs->rgbSerialization = nullptr;
                pcpcs->cbSerialization = 0;
            }
        }
    }

    if (pwzProtectedPassword)
    {
        SecureZeroMemory(pwzProtectedPassword, wcslen(pwzProtectedPassword) * sizeof(wchar_t));
        CoTaskMemFree(pwzProtectedPassword);
    }
    CoTaskMemFree(pszDomain);
    CoTaskMemFree(pszUsername);
    SecureClear(password);
    return hr;
}

HRESULT COpenImpCredential::ReportResult(NTSTATUS ntsStatus,
    NTSTATUS /*ntsSubstatus*/,
    _Outptr_result_maybenull_ PWSTR* ppwszOptionalStatusText,
    _Out_ CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon)
{
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    _ClearPendingLogon();

    if (!NT_SUCCESS(ntsStatus))
    {
        // Most likely the Windows password changed since the badge was enrolled.
        SetStatus(L"Badge sign-in failed. If your password changed, re-enroll your badge.");
        SHStrDupW(L"Badge sign-in failed. If your password changed, sign in with your password and re-enroll your badge.",
            ppwszOptionalStatusText);
        *pcpsiOptionalStatusIcon = CPSI_ERROR;
    }
    else
    {
        SetStatus(c_szTapPrompt);
    }
    return S_OK;
}
