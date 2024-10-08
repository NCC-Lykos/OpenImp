#include "helpers.h"
#include <windows.h>
#include <strsafe.h>
#include <new>
#include <vector>
#include "COpenImpCredential.h"

class COpenImpProvider : public ICredentialProvider,
    public ICredentialProviderSetUserArray
{
public:
    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return ++_cRef;
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        long cRef = --_cRef;
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
            QITABENT(COpenImpProvider, ICredentialProvider), // IID_ICredentialProvider
            QITABENT(COpenImpProvider, ICredentialProviderSetUserArray), // IID_ICredentialProviderSetUserArray
            {0},
        };
        return QISearch(this, qit, riid, ppv);
    }

public:
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

    IFACEMETHODIMP SetUserArray(_In_ ICredentialProviderUserArray* users);

    friend HRESULT CSample_CreateInstance(_In_ REFIID riid, _Outptr_ void** ppv);

protected:
    COpenImpProvider();
    __override ~COpenImpProvider();

private:
    void _ReleaseEnumeratedCredentials();
    void _CreateEnumeratedCredentials();
    HRESULT _EnumerateEmpty();
    HRESULT _EnumerateCredentials();
    HRESULT _EnumerateEmptyTileCredential();
private:
    long                                    _cRef;            // Used for reference counting.
    std::vector<COpenImpCredential*>        _pCredentials;
    bool                                    _fRecreateEnumeratedCredentials;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO      _cpus;
    ICredentialProviderUserArray*           _pCredProviderUserArray;

};
