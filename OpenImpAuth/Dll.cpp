// COM plumbing for OpenImpAuth.dll: the class factory LogonUI uses to create
// the credential provider, and the DLL's reference count.

#include <windows.h>
#include <unknwn.h>
#include <new>
#include "Dll.h"
#include "helpers.h"
#include "guid.h"

static long g_cRef = 0;         // objects and server locks keeping the DLL loaded
HINSTANCE g_hinst = nullptr;    // this DLL's module handle

extern HRESULT OpenImpProvider_CreateInstance(_In_ REFIID riid, _Outptr_ void** ppv);

class CClassFactory : public IClassFactory
{
public:
    CClassFactory() : _cRef(1)
    {
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(_In_ REFIID riid, _COM_Outptr_ void** ppv)
    {
        static const QITAB qit[] =
        {
            QITABENT(CClassFactory, IClassFactory),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

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

    // IClassFactory
    IFACEMETHODIMP CreateInstance(_In_opt_ IUnknown* pUnkOuter, _In_ REFIID riid, _COM_Outptr_ void** ppv)
    {
        if (pUnkOuter)
        {
            *ppv = nullptr;
            return CLASS_E_NOAGGREGATION;
        }
        return OpenImpProvider_CreateInstance(riid, ppv);
    }

    IFACEMETHODIMP LockServer(BOOL bLock)
    {
        if (bLock)
        {
            DllAddRef();
        }
        else
        {
            DllRelease();
        }
        return S_OK;
    }

private:
    ~CClassFactory() = default;
    long _cRef;
};

void DllAddRef()
{
    InterlockedIncrement(&g_cRef);
}

void DllRelease()
{
    InterlockedDecrement(&g_cRef);
}

STDAPI DllCanUnloadNow()
{
    return g_cRef > 0 ? S_FALSE : S_OK;
}

STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ void** ppv)
{
    *ppv = nullptr;
    if (rclsid != CLSID_OpenImp)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    CClassFactory* pcf = new(std::nothrow) CClassFactory();
    if (!pcf)
    {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = pcf->QueryInterface(riid, ppv);
    pcf->Release();
    return hr;
}

STDAPI_(BOOL) DllMain(_In_ HINSTANCE hinstDll, _In_ DWORD dwReason, _In_opt_ void*)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        g_hinst = hinstDll;
        DisableThreadLibraryCalls(hinstDll);
    }
    return TRUE;
}
