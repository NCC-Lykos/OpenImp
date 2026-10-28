// COpenImpProvider shows one "tap your badge" tile on the sign-in and unlock
// screens. While LogonUI is listening (between Advise and UnAdvise) a background
// thread polls the badge reader; when an enrolled badge is tapped it loads that
// badge's account into the tile and tells LogonUI to sign in with it.
//
// On the unlock screen, a badge enrolled for a different user switches users:
// the locked session is disconnected and the badge is handed to the sign-in
// screen that replaces it. OpenImpService hands badges over the same way when it
// locks or switches away from a signed-in session.
//
// When branding is turned on (OpenImp Manager > Sign-in screen), the provider
// also covers LogonUI with the branded screen (shared\src\Overlay.h), which
// takes badge taps or a typed username and password. Cancel reveals the normal
// Windows screen until LogonUI next starts (the next lock or sign-out).

#pragma once

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "helpers.h"
#include "COpenImpCredential.h"

class COpenImpProvider : public ICredentialProvider, private OverlayEvents
{
public:
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
            QITABENT(COpenImpProvider, ICredentialProvider),
            {0},
        };
        return QISearch(this, qit, riid, ppv);
    }

    // ICredentialProvider
    IFACEMETHODIMP SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags);
    IFACEMETHODIMP SetSerialization(_In_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION const* pcpcs);

    IFACEMETHODIMP Advise(_In_ ICredentialProviderEvents* pcpe, _In_ UINT_PTR upAdviseContext);
    IFACEMETHODIMP UnAdvise();

    IFACEMETHODIMP GetFieldDescriptorCount(_Out_ DWORD* pdwCount);
    IFACEMETHODIMP GetFieldDescriptorAt(DWORD dwIndex, _Outptr_result_nullonfailure_ CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd);

    IFACEMETHODIMP GetCredentialCount(_Out_ DWORD* pdwCount,
        _Out_ DWORD* pdwDefault,
        _Out_ BOOL* pbAutoLogonWithDefault);
    IFACEMETHODIMP GetCredentialAt(DWORD dwIndex,
        _Outptr_result_nullonfailure_ ICredentialProviderCredential** ppcpc);

    friend HRESULT OpenImpProvider_CreateInstance(_In_ REFIID riid, _Outptr_ void** ppv);

private:
    COpenImpProvider();
    ~COpenImpProvider();

    void _StartReader();
    void _StopReader();
    void _ReaderLoop();
    void _OnBadge(const std::wstring& badgeId);
    void _TakeHandoff();
    void _ShowOverlay();
    void _CloseOverlay();
    void _CredentialsChanged();

    // OverlayEvents (called on the branded screen's thread)
    void OnOverlaySignIn(const std::wstring& account, const std::wstring& password) override;
    void OnOverlayCancel() override;

    long                                _cRef;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO  _cpus;
    COpenImpCredential*                 _pCredential;

    std::mutex                          _pcpeLock;      // guards _pcpe and _upAdviseContext against the overlay thread
    ICredentialProviderEvents*          _pcpe;
    UINT_PTR                            _upAdviseContext;

    std::mutex                          _overlayLock;   // guards _overlay
    std::unique_ptr<Overlay>            _overlay;
    std::atomic<bool>                   _overlayDismissed{ false };   // Cancel was pressed in this LogonUI

    std::wstring                        _ignoreBadge;   // set before the reader thread starts
    std::thread                         _readerThread;
    std::mutex                          _stopLock;
    std::condition_variable             _stopSignal;
    bool                                _stop;
};
