// The badge sign-in tile; see COpenImpCredential.h.

#include "COpenImpCredential.h"
#include "guid.h"
#include "resource.h"
#include "..\shared\src\Account.h"
#include "..\shared\src\Log.h"
#include "..\shared\src\Util.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

static const wchar_t c_szTapPrompt[] = L"Tap your badge on the reader";

// Sign-in results that get a specific message.
static const NTSTATUS c_statusLogonFailure = static_cast<NTSTATUS>(0xC000006D);
static const NTSTATUS c_statusAccountDisabled = static_cast<NTSTATUS>(0xC0000072);
static const NTSTATUS c_statusAccountLockedOut = static_cast<NTSTATUS>(0xC0000234);

enum class Failure { WrongPassword, LockedOut, Disabled, Other };

// What to tell the user when a sign-in fails.
static std::wstring FailureMessage(Failure failure, bool fromBadge)
{
    switch (failure)
    {
    case Failure::LockedOut:
        return L"This account is locked out.";
    case Failure::Disabled:
        return L"This account is disabled.";
    default:
        if (!fromBadge)
        {
            return failure == Failure::WrongPassword ? L"The username or password is incorrect." : L"Sign-in failed.";
        }
        // Most likely the Windows password changed since the badge was enrolled.
        return L"Badge sign-in failed. If your password changed, sign in with your password and re-enroll your badge.";
    }
}

static Failure FailureFromStatus(NTSTATUS status)
{
    switch (status)
    {
    case c_statusLogonFailure:     return Failure::WrongPassword;
    case c_statusAccountLockedOut: return Failure::LockedOut;
    case c_statusAccountDisabled:  return Failure::Disabled;
    default:                       return Failure::Other;
    }
}

std::wstring COpenImpCredential::PrecheckSignIn(const std::wstring& user, const std::wstring& password, bool fromBadge)
{
    HRESULT hr = Account::CheckPassword(user, password);
    Failure failure;
    if (hr == HRESULT_FROM_WIN32(ERROR_LOGON_FAILURE))
    {
        failure = Failure::WrongPassword;
    }
    else if (hr == HRESULT_FROM_WIN32(ERROR_ACCOUNT_LOCKED_OUT))
    {
        failure = Failure::LockedOut;
    }
    else if (hr == HRESULT_FROM_WIN32(ERROR_ACCOUNT_DISABLED))
    {
        failure = Failure::Disabled;
    }
    else
    {
        // It worked, or failed in a way Windows should handle (such as a
        // password that must be changed): let the real sign-in go ahead.
        return L"";
    }
    Log(L"tile: password check for %s failed (0x%08X); not passing it to Windows", user.c_str(), static_cast<unsigned>(hr));
    return FailureMessage(failure, fromBadge);
}

// Packs an account (DOMAIN\name) and password into the Kerberos interactive
// (or unlock) logon buffer LogonUI passes to Windows.
static HRESULT Serialize(const std::wstring& user, const std::wstring& password,
    CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs)
{
    PWSTR pszDomain = nullptr;
    PWSTR pszUsername = nullptr;
    PWSTR pwzProtectedPassword = nullptr;
    HRESULT hr = SplitDomainAndUsername(user.c_str(), &pszDomain, &pszUsername);
    if (SUCCEEDED(hr))
    {
        hr = ProtectIfNecessaryAndCopyPassword(password.c_str(), cpus, &pwzProtectedPassword);
    }
    if (SUCCEEDED(hr))
    {
        KERB_INTERACTIVE_UNLOCK_LOGON kiul;
        hr = KerbInteractiveUnlockLogonInit(pszDomain, pszUsername, pwzProtectedPassword, cpus, &kiul);
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
    return hr;
}

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

void COpenImpCredential::SetPendingLogon(const std::wstring& user, const std::wstring& password, bool fromBadge)
{
    std::wstring status = L"Signing in as " + user + L"...";
    {
        std::lock_guard<std::mutex> guard(_lock);
        _pendingUser = user;
        _pendingPassword = password;
        _pendingFromBadge = fromBadge;
        _status = status;
    }
    _Notify(status, OverlayStatus::Info);
}

void COpenImpCredential::SetStatus(const std::wstring& status, bool isError)
{
    {
        std::lock_guard<std::mutex> guard(_lock);
        _status = status;
    }
    _Notify(status, isError ? OverlayStatus::Error : OverlayStatus::Info);
}

void COpenImpCredential::SetStatusListener(StatusListener listener)
{
    std::lock_guard<std::mutex> guard(_lock);
    _listener = std::move(listener);
}

void COpenImpCredential::_Notify(const std::wstring& status, OverlayStatus kind)
{
    StatusListener listener;
    {
        std::lock_guard<std::mutex> guard(_lock);
        listener = _listener;
    }
    if (listener)
    {
        listener(status, kind);
    }
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
    Log(L"tile: SetSelected autologon=%d", *pbAutoLogon);
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

// Hands LogonUI the pending account, if any. Each pending sign-in is used for
// one attempt only.
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
        _attemptFromBadge = _pendingFromBadge;
    }

    Log(L"tile: GetSerialization for %s", user.empty() ? L"(nobody: no badge tapped)" : user.c_str());
    if (user.empty())
    {
        if (!_overlayActive)
        {
            SHStrDupW(c_szTapPrompt, ppwszOptionalStatusText);
            *pcpsiOptionalStatusIcon = CPSI_WARNING;
        }
        SecureClear(password);
        return S_OK;
    }

    HRESULT hr = Serialize(user, password, _cpus, pcpcs);
    if (SUCCEEDED(hr))
    {
        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
    }
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
    Log(L"tile: ReportResult 0x%08X", static_cast<unsigned>(ntsStatus));

    if (NT_SUCCESS(ntsStatus))
    {
        SetStatus(c_szTapPrompt);
        return S_OK;
    }

    bool fromBadge;
    {
        std::lock_guard<std::mutex> guard(_lock);
        fromBadge = _attemptFromBadge;
    }

    std::wstring message = FailureMessage(FailureFromStatus(ntsStatus), fromBadge);
    SetStatus(message, true);

    // After a failure LogonUI shows its own screen (an error to dismiss, or a
    // password change) and takes no other sign-in until it's dealt with. The
    // provider's password check stops the usual failures before they get here,
    // so for the rest, close the branded screen and let the user see Windows'.
    if (_overlayActive)
    {
        _Notify(message, OverlayStatus::NeedsWindows);
    }
    SHStrDupW(message.c_str(), ppwszOptionalStatusText);
    *pcpsiOptionalStatusIcon = CPSI_ERROR;
    return S_OK;
}
