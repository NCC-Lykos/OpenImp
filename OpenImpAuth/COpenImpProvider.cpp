#include <new>
#include "COpenImpProvider.h"
#include "guid.h"
#include "..\shared\src\BadgeReader.h"
#include "..\shared\src\BadgeStore.h"

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
    _StopReader();
    if (_pcpe)
    {
        _pcpe->Release();
    }
    if (_pCredential)
    {
        _pCredential->Release();
    }
    DllRelease();
}

HRESULT COpenImpProvider::SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD /*dwFlags*/)
{
    switch (cpus)
    {
    case CPUS_LOGON:
    case CPUS_UNLOCK_WORKSTATION:
        _cpus = cpus;
        if (!_pCredential)
        {
            _pCredential = new(std::nothrow) COpenImpCredential(cpus);
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
    if (_pcpe)
    {
        _pcpe->Release();
    }
    _pcpe = pcpe;
    _pcpe->AddRef();
    _upAdviseContext = upAdviseContext;
    _StartReader();
    return S_OK;
}

HRESULT COpenImpProvider::UnAdvise()
{
    _StopReader();
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
    std::wstring lastBadge;

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
            // React once per tap: only when a badge arrives, not while it rests on the reader.
            std::wstring badge = reader.ReadBadge();
            if (!badge.empty() && badge != lastBadge)
            {
                _OnBadge(badge);
            }
            lastBadge = badge;
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
    if (SUCCEEDED(BadgeStore::Lookup(badgeId, user, password)))
    {
        _pCredential->SetPendingLogon(user, password);
    }
    else
    {
        _pCredential->SetStatus(L"This badge isn't enrolled. Badge ID " + badgeId);
    }
    SecureClear(password);

    // Makes LogonUI re-query GetCredentialCount (and the tile's text).
    if (_pcpe)
    {
        _pcpe->CredentialsChanged(_upAdviseContext);
    }
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
