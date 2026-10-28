#include <new>
#include "COpenImpProvider.h"
#include <wtsapi32.h>
#include "guid.h"
#include "..\shared\src\BadgeReader.h"
#include "..\shared\src\BadgeStore.h"
#include "..\shared\src\Branding.h"
#include "..\shared\src\Log.h"
#include "..\shared\src\Session.h"

static const auto c_pollInterval = std::chrono::milliseconds(250);
static const auto c_reconnectInterval = std::chrono::seconds(2);

COpenImpProvider::COpenImpProvider() :
    _cRef(1),
    _cpus(CPUS_INVALID),
    _pCredential(nullptr),
    _pcpe(nullptr),
    _upAdviseContext(0),
    _stop(false)
{
    DllAddRef();
}

COpenImpProvider::~COpenImpProvider()
{
    _CloseOverlay();
    _StopReader();
    if (_pcpe)
    {
        _pcpe->Release();
    }
    if (_pCredential)
    {
        _pCredential->SetStatusListener(nullptr);
        _pCredential->Release();
    }
    DllRelease();
}

// Turns what was typed into DOMAIN\name: "name" becomes COMPUTER\name, and
// "name@domain" keeps an empty domain (Windows resolves the UPN).
static std::wstring QualifyAccount(const std::wstring& account)
{
    if (account.find(L'\\') != std::wstring::npos)
    {
        return account;
    }
    if (account.find(L'@') != std::wstring::npos)
    {
        return L"\\" + account;
    }
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD cch = ARRAYSIZE(computer);
    GetComputerNameW(computer, &cch);
    return std::wstring(computer) + L"\\" + account;
}

HRESULT COpenImpProvider::SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD /*dwFlags*/)
{
    switch (cpus)
    {
    case CPUS_LOGON:
    case CPUS_UNLOCK_WORKSTATION:
        Log(L"tile: scenario %s", cpus == CPUS_LOGON ? L"sign-in" : L"unlock");
        _cpus = cpus;
        if (!_pCredential)
        {
            _pCredential = new(std::nothrow) COpenImpCredential(cpus);
            if (_pCredential)
            {
                _pCredential->SetStatusListener([this](const std::wstring& text, OverlayStatus kind)
                {
                    if (kind == OverlayStatus::NeedsWindows)
                    {
                        Log(L"tile: Windows needs its own screen; closing the branded screen");
                        _overlayDismissed = true;
                        _CloseOverlay();
                        return;
                    }
                    std::lock_guard<std::mutex> guard(_overlayLock);
                    if (_overlay)
                    {
                        _overlay->SetStatus(text, kind == OverlayStatus::Error);
                    }
                });
            }
        }
        return _pCredential ? S_OK : E_OUTOFMEMORY;

    case CPUS_CHANGE_PASSWORD:
    case CPUS_CREDUI:
        return E_NOTIMPL;

    default:
        return E_INVALIDARG;
    }
}

HRESULT COpenImpProvider::SetSerialization(_In_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION const* /*pcpcs*/)
{
    return E_NOTIMPL;
}

HRESULT COpenImpProvider::Advise(_In_ ICredentialProviderEvents* pcpe, _In_ UINT_PTR upAdviseContext)
{
    _StopReader();
    {
        std::lock_guard<std::mutex> guard(_pcpeLock);
        if (_pcpe)
        {
            _pcpe->Release();
        }
        _pcpe = pcpe;
        _pcpe->AddRef();
        _upAdviseContext = upAdviseContext;
    }
    Log(L"tile: Advise");
    // LogonUI asks for GetCredentialCount after Advise, so a handed-over badge
    // signs in as soon as the screen is up.
    _TakeHandoff();
    _ShowOverlay();
    _StartReader();
    return S_OK;
}

void COpenImpProvider::_ShowOverlay()
{
    if (_overlayDismissed || !_pCredential)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(_overlayLock);
        if (_overlay && _overlay->IsOpen())
        {
            return;
        }
    }
    BrandingSettings settings = BrandingSettings::Load();
    if (!settings.ShouldShow())
    {
        return;
    }

    std::unique_ptr<Overlay> overlay = Overlay::Show(settings, this, false);
    if (!overlay)
    {
        Log(L"tile: the branded screen couldn't open; showing the normal screen");
        return;
    }
    Log(L"tile: branded screen shown");
    if (_pCredential->HasPendingLogon())
    {
        overlay->SetStatus(L"Signing in...", false);
    }
    std::unique_ptr<Overlay> old;
    {
        std::lock_guard<std::mutex> guard(_overlayLock);
        old = std::move(_overlay);
        _overlay = std::move(overlay);
    }
    _pCredential->SetOverlayActive(true);
}

void COpenImpProvider::_CloseOverlay()
{
    std::unique_ptr<Overlay> overlay;
    {
        std::lock_guard<std::mutex> guard(_overlayLock);
        overlay = std::move(_overlay);
    }
    if (_pCredential)
    {
        _pCredential->SetOverlayActive(false);
    }
    // Destroyed outside the lock: this waits for the overlay's thread, which may
    // be passing a status through the listener above.
    overlay.reset();
}

void COpenImpProvider::_CredentialsChanged()
{
    ICredentialProviderEvents* pcpe;
    UINT_PTR context;
    {
        std::lock_guard<std::mutex> guard(_pcpeLock);
        pcpe = _pcpe;
        context = _upAdviseContext;
        if (pcpe)
        {
            pcpe->AddRef();
        }
    }
    if (pcpe)
    {
        pcpe->CredentialsChanged(context);
        pcpe->Release();
    }
}

void COpenImpProvider::OnOverlaySignIn(const std::wstring& account, const std::wstring& password)
{
    if (!_pCredential)
    {
        return;
    }
    std::wstring qualified = QualifyAccount(account);
    if (_cpus == CPUS_UNLOCK_WORKSTATION)
    {
        DWORD sessionId = 0;
        std::wstring lookupName = qualified[0] == L'\\' ? account : qualified;
        if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) && Session::IsOtherUser(sessionId, lookupName))
        {
            Log(L"tile: password sign-in for %s on another user's lock screen", qualified.c_str());
            _pCredential->SetStatus(L"This PC is locked by someone else. Choose Cancel, then Switch user.", true);
            return;
        }
    }
    Log(L"tile: password sign-in for %s", qualified.c_str());
    _pCredential->SetPendingLogon(qualified, password, false);
    _CredentialsChanged();
}

void COpenImpProvider::OnOverlayCancel()
{
    Log(L"tile: branded screen cancelled");
    _overlayDismissed = true;
    if (_pCredential)
    {
        _pCredential->SetOverlayActive(false);
    }
}

void COpenImpProvider::_TakeHandoff()
{
    _ignoreBadge.clear();
    std::wstring badgeId;
    HandoffAction action;
    if (!_pCredential || !BadgeStore::TakeHandoff(badgeId, action))
    {
        return;
    }

    Log(L"tile: took handoff %s (%s)", badgeId.c_str(), action == HandoffAction::Switch ? L"switch" : L"lock");

    // The badge may still be on the reader; don't act on it twice.
    _ignoreBadge = badgeId;

    if (action == HandoffAction::Switch && _cpus == CPUS_LOGON)
    {
        std::wstring user, password;
        if (SUCCEEDED(BadgeStore::Lookup(badgeId, user, password)))
        {
            _pCredential->SetPendingLogon(user, password);
        }
        SecureClear(password);
    }
}

HRESULT COpenImpProvider::UnAdvise()
{
    Log(L"tile: UnAdvise");
    _CloseOverlay();
    _StopReader();
    std::lock_guard<std::mutex> guard(_pcpeLock);
    if (_pcpe)
    {
        _pcpe->Release();
        _pcpe = nullptr;
    }
    return S_OK;
}

HRESULT COpenImpProvider::GetFieldDescriptorCount(_Out_ DWORD* pdwCount)
{
    *pdwCount = SFI_NUM_FIELDS;
    return S_OK;
}

HRESULT COpenImpProvider::GetFieldDescriptorAt(DWORD dwIndex,
    _Outptr_result_nullonfailure_ CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd)
{
    *ppcpfd = nullptr;
    if (dwIndex >= SFI_NUM_FIELDS)
    {
        return E_INVALIDARG;
    }
    return FieldDescriptorCoAllocCopy(s_rgCredProvFieldDescriptors[dwIndex], ppcpfd);
}

// One tile. After an enrolled badge is tapped it becomes the default tile with
// auto-logon, so LogonUI signs in straight away.
HRESULT COpenImpProvider::GetCredentialCount(_Out_ DWORD* pdwCount,
    _Out_ DWORD* pdwDefault,
    _Out_ BOOL* pbAutoLogonWithDefault)
{
    *pdwCount = _pCredential ? 1 : 0;
    *pdwDefault = CREDENTIAL_PROVIDER_NO_DEFAULT;
    *pbAutoLogonWithDefault = FALSE;

    if (_pCredential && _pCredential->HasPendingLogon())
    {
        *pdwDefault = 0;
        *pbAutoLogonWithDefault = TRUE;
    }
    Log(L"tile: GetCredentialCount autologon=%d", *pbAutoLogonWithDefault);
    return S_OK;
}

HRESULT COpenImpProvider::GetCredentialAt(DWORD dwIndex,
    _Outptr_result_nullonfailure_ ICredentialProviderCredential** ppcpc)
{
    *ppcpc = nullptr;
    if (dwIndex != 0 || !_pCredential)
    {
        return E_INVALIDARG;
    }
    return _pCredential->QueryInterface(IID_PPV_ARGS(ppcpc));
}

void COpenImpProvider::_StartReader()
{
    {
        std::lock_guard<std::mutex> guard(_stopLock);
        _stop = false;
    }
    _readerThread = std::thread(&COpenImpProvider::_ReaderLoop, this);
}

void COpenImpProvider::_StopReader()
{
    {
        std::lock_guard<std::mutex> guard(_stopLock);
        _stop = true;
    }
    _stopSignal.notify_all();
    if (_readerThread.joinable())
    {
        _readerThread.join();
    }
}

void COpenImpProvider::_ReaderLoop()
{
    BadgeReader reader;
    TapFilter taps;
    if (!_ignoreBadge.empty())
    {
        taps.Ignore(_ignoreBadge);
    }

    // A handed-over badge set up a sign-in; make sure LogonUI notices it even if
    // it counted the tiles before Advise.
    if (_pCredential && _pCredential->HasPendingLogon())
    {
        _CredentialsChanged();
    }

    std::unique_lock<std::mutex> lock(_stopLock);
    while (!_stop)
    {
        lock.unlock();
        std::chrono::milliseconds wait = c_pollInterval;
        if (!reader.Connect())
        {
            wait = c_reconnectInterval;
        }
        else
        {
            std::wstring badge = reader.ReadBadge();
            if (taps.IsNewTap(badge))
            {
                Log(L"tile: tap %s", badge.c_str());
                _OnBadge(badge);
            }
        }
        lock.lock();
        _stopSignal.wait_for(lock, wait, [this] { return _stop; });
    }
}

void COpenImpProvider::_OnBadge(const std::wstring& badgeId)
{
    if (!_pCredential)
    {
        return;
    }

    std::wstring user, password;
    DWORD sessionId = 0;
    if (SUCCEEDED(BadgeStore::Lookup(badgeId, user, password)))
    {
        if (_cpus == CPUS_UNLOCK_WORKSTATION
            && ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)
            && Session::IsOtherUser(sessionId, user))
        {
            // Someone else's badge on this lock screen: leave the locked session
            // running (like "Switch user") and sign them in on the next screen.
            Log(L"tile: %s is another user, switching from session %lu", user.c_str(), sessionId);
            _pCredential->SetStatus(L"Switching to " + user);
            if (FAILED(BadgeStore::SaveHandoff(badgeId, HandoffAction::Switch))
                || !WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, sessionId, FALSE))
            {
                Log(L"tile: switch failed (%lu)", GetLastError());
                _pCredential->SetStatus(L"Couldn't switch to " + user + L". Use Switch user, then tap the badge again.", true);
            }
        }
        else
        {
            Log(L"tile: signing in %s", user.c_str());
            _pCredential->SetPendingLogon(user, password);
        }
    }
    else
    {
        Log(L"tile: badge %s not enrolled", badgeId.c_str());
        _pCredential->SetStatus(L"This badge isn't enrolled. Badge ID " + badgeId, true);
    }
    SecureClear(password);

    // Makes LogonUI re-query GetCredentialCount (and the tile's text).
    _CredentialsChanged();
}

HRESULT OpenImpProvider_CreateInstance(_In_ REFIID riid, _Outptr_ void** ppv)
{
    *ppv = nullptr;
    COpenImpProvider* pProvider = new(std::nothrow) COpenImpProvider();
    if (!pProvider)
    {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = pProvider->QueryInterface(riid, ppv);
    pProvider->Release();
    return hr;
}
