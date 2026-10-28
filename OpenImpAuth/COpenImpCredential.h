// COpenImpCredential is the single "tap your badge" tile. It has no input
// fields: the provider hands it an account and password (from a badge tap or the
// branded screen), and LogonUI then asks it to serialize them for sign-in.
//
// The tile also keeps the status line ("Signing in as...", errors) and passes
// every change to a listener, which the provider uses to update the branded
// screen.

#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include "common.h"
#include "Dll.h"
#include "..\shared\src\Overlay.h"

class COpenImpCredential : public ICredentialProviderCredential
{
public:
    using StatusListener = std::function<void(const std::wstring& status, OverlayStatus kind)>;

    COpenImpCredential(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus);

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        long cRef = InterlockedDecrement(&_cRef);
        if (!cRef)
        {
            delete this;
        }
        return cRef;
    }

    IFACEMETHODIMP QueryInterface(_In_ REFIID riid, _COM_Outptr_ void** ppv)
    {
        static const QITAB qit[] =
        {
            QITABENT(COpenImpCredential, ICredentialProviderCredential),
            {0},
        };
        return QISearch(this, qit, riid, ppv);
    }

    // ICredentialProviderCredential
    IFACEMETHODIMP Advise(_In_ ICredentialProviderCredentialEvents* pcpce);
    IFACEMETHODIMP UnAdvise();

    IFACEMETHODIMP SetSelected(_Out_ BOOL* pbAutoLogon);
    IFACEMETHODIMP SetDeselected();

    IFACEMETHODIMP GetFieldState(DWORD dwFieldID,
        _Out_ CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
        _Out_ CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis);

    IFACEMETHODIMP GetStringValue(DWORD dwFieldID, _Outptr_result_nullonfailure_ PWSTR* ppwsz);
    IFACEMETHODIMP GetBitmapValue(DWORD dwFieldID, _Outptr_result_nullonfailure_ HBITMAP* phbmp);
    IFACEMETHODIMP GetCheckboxValue(DWORD dwFieldID, _Out_ BOOL* pbChecked, _Outptr_result_nullonfailure_ PWSTR* ppwszLabel);
    IFACEMETHODIMP GetComboBoxValueCount(DWORD dwFieldID, _Out_ DWORD* pcItems, _Deref_out_range_(< , *pcItems) _Out_ DWORD* pdwSelectedItem);
    IFACEMETHODIMP GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, _Outptr_result_nullonfailure_ PWSTR* ppwszItem);
    IFACEMETHODIMP GetSubmitButtonValue(DWORD dwFieldID, _Out_ DWORD* pdwAdjacentTo);

    IFACEMETHODIMP SetStringValue(DWORD dwFieldID, _In_ PCWSTR pwz);
    IFACEMETHODIMP SetCheckboxValue(DWORD dwFieldID, BOOL bChecked);
    IFACEMETHODIMP SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem);
    IFACEMETHODIMP CommandLinkClicked(DWORD dwFieldID);

    IFACEMETHODIMP GetSerialization(_Out_ CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
        _Out_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
        _Outptr_result_maybenull_ PWSTR* ppwszOptionalStatusText,
        _Out_ CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon);
    IFACEMETHODIMP ReportResult(NTSTATUS ntsStatus,
        NTSTATUS ntsSubstatus,
        _Outptr_result_maybenull_ PWSTR* ppwszOptionalStatusText,
        _Out_ CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon);

    // The rest is called by the provider, from LogonUI's thread, the reader
    // thread, and the branded screen's thread.

    // Queues an account (qualified, see Account.h) for the next GetSerialization.
    // fromBadge picks the error message if the sign-in fails.
    void SetPendingLogon(const std::wstring& user, const std::wstring& password, bool fromBadge = true);
    bool HasPendingLogon();

    // Tries the password before it's handed to LogonUI, because a failed sign-in
    // leaves LogonUI on its own error screen until someone clicks OK, which the
    // branded screen would hide. Returns the message to show if the password is
    // wrong or the account is locked out or disabled, or "" to go ahead.
    static std::wstring PrecheckSignIn(const std::wstring& user, const std::wstring& password, bool fromBadge);
    void SetStatus(const std::wstring& status, bool isError = false);

    // The provider passes status changes on to the branded screen through this.
    void SetStatusListener(StatusListener listener);
    // While the branded screen covers LogonUI, errors go to it instead of to
    // LogonUI's own message screen, which nobody could see or dismiss.
    void SetOverlayActive(bool active) { _overlayActive = active; }

private:
    virtual ~COpenImpCredential();
    void _ClearPendingLogon();
    // Calls the status listener, outside the lock.
    void _Notify(const std::wstring& status, OverlayStatus kind);

    long                                _cRef;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO  _cpus;
    std::atomic<bool>                   _overlayActive{ false };

    std::mutex                          _lock;          // guards the members below
    std::wstring                        _status;
    std::wstring                        _pendingUser;   // DOMAIN\name
    std::wstring                        _pendingPassword;
    bool                                _pendingFromBadge = true;
    bool                                _attemptFromBadge = true;   // the attempt GetSerialization last handed to LogonUI
    StatusListener                      _listener;
};
