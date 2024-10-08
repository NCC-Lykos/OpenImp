#ifndef WIN32_NO_STATUS
#include <ntstatus.h>
#define WIN32_NO_STATUS
#endif
#include <unknwn.h>
#include <fstream>
#include <string>
#include <chrono>
#include <iomanip>
#include <ctime>
#include "COpenImpCredential.h"
#include "guid.h"


void LogMessage(const std::wstring& message)
{
    std::wofstream logFile(L"C:\\00_IT\\OpenImp\\debug_log.txt", std::ios_base::app);
    if (logFile.is_open())
    {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);

        logFile << std::put_time(std::localtime(&in_time_t), L"%Y-%m-%d %X") << L": " << message << std::endl;
    }
}

COpenImpCredential::COpenImpCredential() :
    _cRef(1),
    _pCredProvCredentialEvents(nullptr),
    _pszUserSid(nullptr),
    _pszQualifiedUserName(nullptr),
    _fIsLocalUser(false),
    _fChecked(false),
    _fShowControls(false),
    _dwComboIndex(0)
{
    DllAddRef();
    StartCardPolling();
    ZeroMemory(_rgCredProvFieldDescriptors, sizeof(_rgCredProvFieldDescriptors));
    ZeroMemory(_rgFieldStatePairs, sizeof(_rgFieldStatePairs));
    ZeroMemory(_rgFieldStrings, sizeof(_rgFieldStrings));
}

COpenImpCredential::~COpenImpCredential()
{
    if (_rgFieldStrings[SFI_PASSWORD])
    {
        size_t lenPassword = wcslen(_rgFieldStrings[SFI_PASSWORD]);
        SecureZeroMemory(_rgFieldStrings[SFI_PASSWORD], lenPassword * sizeof(*_rgFieldStrings[SFI_PASSWORD]));
    }
    for (int i = 0; i < ARRAYSIZE(_rgFieldStrings); i++)
    {
        CoTaskMemFree(_rgFieldStrings[i]);
        CoTaskMemFree(_rgCredProvFieldDescriptors[i].pszLabel);
    }
    CoTaskMemFree(_pszUserSid);
    CoTaskMemFree(_pszQualifiedUserName);
    _stopPolling = true;
    DllRelease();
}


// Initializes one credential with the field information passed in.
// Set the value of the SFI_LARGE_TEXT field to pwzUsername.
HRESULT COpenImpCredential::Initialize(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
    _In_ CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR const* rgcpfd,
    _In_ FIELD_STATE_PAIR const* rgfsp,
    _In_ ICredentialProviderUser* pcpUser)
{
    HRESULT hr = S_OK;
    _cpus = cpus;
   

    GUID guidProvider;
    if (pcpUser != nullptr) {
        pcpUser->GetProviderID(&guidProvider);
        _fIsLocalUser = (guidProvider == Identity_LocalUserProvider);
    }

    // Copy the field descriptors for each field. This is useful if you want to vary the field
    // descriptors based on what Usage scenario the credential was created for.
    for (DWORD i = 0; SUCCEEDED(hr) && i < ARRAYSIZE(_rgCredProvFieldDescriptors); i++)
    {
        _rgFieldStatePairs[i] = rgfsp[i];
        hr = FieldDescriptorCopy(rgcpfd[i], &_rgCredProvFieldDescriptors[i]);
    }

    // Initialize the String value of all the fields.
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"OpenImp Auth", &_rgFieldStrings[SFI_LABEL]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Scan your RFID Card", &_rgFieldStrings[SFI_LARGE_TEXT]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"", &_rgFieldStrings[SFI_PASSWORD]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Submit", &_rgFieldStrings[SFI_SUBMIT_BUTTON]);
    }
    if (SUCCEEDED(hr))
    {
        hr = pcpUser->GetStringValue(PKEY_Identity_QualifiedUserName, &_pszQualifiedUserName);
    }
    if (SUCCEEDED(hr))
    {
        hr = pcpUser->GetSid(&_pszUserSid);
    }
    return hr;
}

HRESULT COpenImpCredential::StartCardPolling()
{
    _stopPolling = false;

    std::thread pollingThread([this]()
        {
            PollForCardData();
        });
    pollingThread.detach(); // Detach the thread to run independently

    return S_OK;
}

void COpenImpCredential::PollForCardData()
{
    while (!_stopPolling)
    {
        unsigned short connectionResult = usbConnect();
        if (connectionResult == 1)
        {
            LogMessage(L"Connected to RFID USB device successfully.");

            short bufferSize = 64;
            short cardDataLengthBytes = static_cast<short>(getActiveID(bufferSize) / 8);

            if (cardDataLengthBytes > 0)
            {
                LogMessage(L"Card detected with data length: " + std::to_wstring(cardDataLengthBytes));

                std::wstring cardData;
                for (short i = 0; i < cardDataLengthBytes; ++i)
                {
                    cardData += std::to_wstring((int)getActiveID_byte(i));
                }

                // Process the card data
                if (_pCredProvCredentialEvents)
                {
                    _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT, cardData.c_str());
                    // Additional logic to trigger login if needed
                    // SignalCredentialChanged(); // Example function to notify LogonUI
                }
            }
        }
        else
        {
            LogMessage(L"Failed to connect to RFID USB device. Error code: " + std::to_wstring(connectionResult));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250)); // Poll every 250ms
    }
}


// LogonUI calls this in order to give us a callback in case we need to notify it of anything.
HRESULT COpenImpCredential::Advise(_In_ ICredentialProviderCredentialEvents* pcpce)
{
    if (_pCredProvCredentialEvents != nullptr)
    {
        _pCredProvCredentialEvents->Release();
    }
    return pcpce->QueryInterface(IID_PPV_ARGS(&_pCredProvCredentialEvents));
}

// LogonUI calls this to tell us to release the callback.
HRESULT COpenImpCredential::UnAdvise()
{
    if (_pCredProvCredentialEvents)
    {
        _pCredProvCredentialEvents->Release();
    }
    _pCredProvCredentialEvents = nullptr;
    return S_OK;
}

// LogonUI calls this function when our tile is selected (zoomed)
// If you simply want fields to show/hide based on the selected state,
// there's no need to do anything here - you can set that up in the
// field definitions. But if you want to do something
// more complicated, like change the contents of a field when the tile is
// selected, you would do it here.
HRESULT COpenImpCredential::SetSelected(_Out_ BOOL* pbAutoLogon)
{
    *pbAutoLogon = FALSE;
    return S_OK;
}

// Similarly to SetSelected, LogonUI calls this when your tile was selected
// and now no longer is. The most common thing to do here (which we do below)
// is to clear out the password field.
HRESULT COpenImpCredential::SetDeselected()
{
    HRESULT hr = S_OK;

    // Stop the polling when this credential is deselected
    _stopPolling = true;

    if (_rgFieldStrings[SFI_PASSWORD])
    {
        size_t lenPassword = wcslen(_rgFieldStrings[SFI_PASSWORD]);
        SecureZeroMemory(_rgFieldStrings[SFI_PASSWORD], lenPassword * sizeof(*_rgFieldStrings[SFI_PASSWORD]));

        CoTaskMemFree(_rgFieldStrings[SFI_PASSWORD]);
        hr = SHStrDupW(L"", &_rgFieldStrings[SFI_PASSWORD]);

        if (SUCCEEDED(hr) && _pCredProvCredentialEvents)
        {
            _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD, _rgFieldStrings[SFI_PASSWORD]);
        }
    }

    return hr;
}


// Get info for a particular field of a tile. Called by logonUI to get information
// to display the tile.
HRESULT COpenImpCredential::GetFieldState(DWORD dwFieldID,
    _Out_ CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
    _Out_ CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis)
{
    HRESULT hr;

    // Validate our parameters.
    if ((dwFieldID < ARRAYSIZE(_rgFieldStatePairs)))
    {
        *pcpfs = _rgFieldStatePairs[dwFieldID].cpfs;
        *pcpfis = _rgFieldStatePairs[dwFieldID].cpfis;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }
    return hr;
}

// Sets ppwsz to the string value of the field at the index dwFieldID
HRESULT COpenImpCredential::GetStringValue(DWORD dwFieldID, _Outptr_result_nullonfailure_ PWSTR* ppwsz)
{
    HRESULT hr;
    *ppwsz = nullptr;

    // Check to make sure dwFieldID is a legitimate index
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors))
    {
        // Make a copy of the string and return that. The caller
        // is responsible for freeing it.
        hr = SHStrDupW(_rgFieldStrings[dwFieldID], ppwsz);
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Get the image to show in the user tile
HRESULT COpenImpCredential::GetBitmapValue(DWORD dwFieldID, _Outptr_result_nullonfailure_ HBITMAP* phbmp)
{
    HRESULT hr;
    *phbmp = nullptr;

    if ((SFI_TILEIMAGE == dwFieldID))
    {
        HBITMAP hbmp = LoadBitmap(HINST_THISDLL, MAKEINTRESOURCE(IDB_TILE_IMAGE));
        if (hbmp != nullptr)
        {
            hr = S_OK;
            *phbmp = hbmp;
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Sets pdwAdjacentTo to the index of the field the submit button should be
// adjacent to. We recommend that the submit button is placed next to the last
// field which the user is required to enter information in. Optional fields
// should be below the submit button.
HRESULT COpenImpCredential::GetSubmitButtonValue(DWORD dwFieldID, _Out_ DWORD* pdwAdjacentTo)
{
    HRESULT hr;

    if (SFI_SUBMIT_BUTTON == dwFieldID)
    {
        // pdwAdjacentTo is a pointer to the fieldID you want the submit button to
        // appear next to.
        *pdwAdjacentTo = SFI_PASSWORD;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }
    return hr;
}

// Sets the value of a field which can accept a string as a value.
// This is called on each keystroke when a user types into an edit field
HRESULT COpenImpCredential::SetStringValue(DWORD dwFieldID, _In_ PCWSTR pwz)
{
    HRESULT hr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_EDIT_TEXT == _rgCredProvFieldDescriptors[dwFieldID].cpft ||
            CPFT_PASSWORD_TEXT == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        PWSTR* ppwszStored = &_rgFieldStrings[dwFieldID];
        CoTaskMemFree(*ppwszStored);
        hr = SHStrDupW(pwz, ppwszStored);
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Returns whether a checkbox is checked or not as well as its label.
HRESULT COpenImpCredential::GetCheckboxValue(DWORD dwFieldID, _Out_ BOOL* pbChecked, _Outptr_result_nullonfailure_ PWSTR* ppwszLabel)
{
    HRESULT hr;
    *ppwszLabel = nullptr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_CHECKBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        *pbChecked = _fChecked;
        hr = E_INVALIDARG;
        //hr = SHStrDupW(_rgFieldStrings[SFI_CHECKBOX], ppwszLabel);
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Sets whether the specified checkbox is checked or not.
HRESULT COpenImpCredential::SetCheckboxValue(DWORD dwFieldID, BOOL bChecked)
{
    HRESULT hr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_CHECKBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        _fChecked = bChecked;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Returns the number of items to be included in the combobox (pcItems), as well as the
// currently selected item (pdwSelectedItem).
HRESULT COpenImpCredential::GetComboBoxValueCount(DWORD dwFieldID, _Out_ DWORD* pcItems, _Deref_out_range_(< , *pcItems) _Out_ DWORD* pdwSelectedItem)
{
    HRESULT hr;
    *pcItems = 0;
    *pdwSelectedItem = 0;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_COMBOBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        *pcItems = ARRAYSIZE(s_rgComboBoxStrings);
        *pdwSelectedItem = 0;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Called iteratively to fill the combobox with the string (ppwszItem) at index dwItem.
HRESULT COpenImpCredential::GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, _Outptr_result_nullonfailure_ PWSTR* ppwszItem)
{
    HRESULT hr;
    *ppwszItem = nullptr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_COMBOBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        hr = SHStrDupW(s_rgComboBoxStrings[dwItem], ppwszItem);
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Called when the user changes the selected item in the combobox.
HRESULT COpenImpCredential::SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem)
{
    HRESULT hr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_COMBOBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        _dwComboIndex = dwSelectedItem;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Called when the user clicks a command link.
HRESULT COpenImpCredential::CommandLinkClicked(DWORD dwFieldID)
{
    HRESULT hr = S_OK;

    CREDENTIAL_PROVIDER_FIELD_STATE cpfsShow = CPFS_HIDDEN;

    // Validate parameter.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_COMMAND_LINK == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        HWND hwndOwner = nullptr;
        hr = E_INVALIDARG;

        //switch (dwFieldID)
        //{
        //case SFI_LAUNCHWINDOW_LINK:
        //    if (_pCredProvCredentialEvents)
        //    {
        //        _pCredProvCredentialEvents->OnCreatingWindow(&hwndOwner);
        //    }

        //    // Pop a messagebox indicating the click.
        //    ::MessageBox(hwndOwner, L"Command link clicked", L"Click!", 0);
        //    break;
        //case SFI_HIDECONTROLS_LINK:
        //    _pCredProvCredentialEvents->BeginFieldUpdates();
        //    cpfsShow = _fShowControls ? CPFS_DISPLAY_IN_SELECTED_TILE : CPFS_HIDDEN;
        //    _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_FULLNAME_TEXT, cpfsShow);
        //    _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_DISPLAYNAME_TEXT, cpfsShow);
        //    _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_LOGONSTATUS_TEXT, cpfsShow);
        //    _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_CHECKBOX, cpfsShow);
        //    _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_EDIT_TEXT, cpfsShow);
        //    _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_COMBOBOX, cpfsShow);
        //    _pCredProvCredentialEvents->SetFieldString(nullptr, SFI_HIDECONTROLS_LINK, _fShowControls ? L"Hide additional controls" : L"Show additional controls");
        //    _pCredProvCredentialEvents->EndFieldUpdates();
        //    _fShowControls = !_fShowControls;
        //    break;
        //default:
        //    hr = E_INVALIDARG;
        //}

    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

HRESULT COpenImpCredential::ReadCredentialsFromFile(const std::wstring& cardData, std::wstring& username, std::wstring& password) {
    std::wifstream file(L"RFIDCredentials.txt");
    std::wstring line;

    if (!file.is_open()) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    while (std::getline(file, line)) {
        size_t pos1 = line.find(L'|');
        size_t pos2 = line.find(L'|', pos1 + 1);

        if (pos1 != std::wstring::npos && pos2 != std::wstring::npos) {
            std::wstring fileCardData = line.substr(0, pos1);
            if (fileCardData == cardData) {
                username = line.substr(pos1 + 1, pos2 - pos1 - 1);
                password = line.substr(pos2 + 1);
                return S_OK;
            }
        }
    }

    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

// Collect the username and password into a serialized credential for the correct usage scenario
// (logon/unlock is what's demonstrated in this sample).  LogonUI then passes these credentials
// back to the system to log on.
HRESULT COpenImpCredential::GetSerialization(_Out_ CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
    _Out_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
    _Outptr_result_maybenull_ PWSTR* ppwszOptionalStatusText,
    _Out_ CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon)
{
    HRESULT hr = E_UNEXPECTED;
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    ZeroMemory(pcpcs, sizeof(*pcpcs));

    // For local user, the domain and user name can be split from _pszQualifiedUserName (domain\username).
    // CredPackAuthenticationBuffer() cannot be used because it won't work with unlock scenario.
    if (_fIsLocalUser)
    {
        PWSTR pwzProtectedPassword;
        std::wstring cardData;
        std::wstring username;
        std::wstring password;
        short bufferSize = 64;
        short cardDataLengthBytes = static_cast<short>(getActiveID(bufferSize) / 8);

        if (cardDataLengthBytes > 1) {
            for (short i = 0; i < cardDataLengthBytes; ++i) {
                cardData += std::to_wstring((int)getActiveID_byte(i));
            }

            HRESULT hr = ReadCredentialsFromFile(cardData, username, password);
            if (SUCCEEDED(hr)) {
                hr = ProtectIfNecessaryAndCopyPassword(password.c_str(), _cpus, &pwzProtectedPassword);
                if (SUCCEEDED(hr))
                {
                    PWSTR pszDomain;
                    PWSTR pszUsername;
                    hr = SplitDomainAndUsername(_pszQualifiedUserName, &pszDomain, &pszUsername);
                    if (SUCCEEDED(hr))
                    {
                        KERB_INTERACTIVE_UNLOCK_LOGON kiul;
                        hr = KerbInteractiveUnlockLogonInit(pszDomain, pszUsername, pwzProtectedPassword, _cpus, &kiul);
                        if (SUCCEEDED(hr))
                        {
                            // We use KERB_INTERACTIVE_UNLOCK_LOGON in both unlock and logon scenarios.  It contains a
                            // KERB_INTERACTIVE_LOGON to hold the creds plus a LUID that is filled in for us by Winlogon
                            // as necessary.
                            hr = KerbInteractiveUnlockLogonPack(kiul, &pcpcs->rgbSerialization, &pcpcs->cbSerialization);
                            if (SUCCEEDED(hr))
                            {
                                ULONG ulAuthPackage;
                                hr = RetrieveNegotiateAuthPackage(&ulAuthPackage);
                                if (SUCCEEDED(hr))
                                {
                                    pcpcs->ulAuthenticationPackage = ulAuthPackage;
                                    pcpcs->clsidCredentialProvider = CLSID_OpenImp;
                                    // At this point the credential has created the serialized credential used for logon
                                    // By setting this to CPGSR_RETURN_CREDENTIAL_FINISHED we are letting logonUI know
                                    // that we have all the information we need and it should attempt to submit the
                                    // serialized credential.
                                    *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
                                }
                            }
                        }
                        CoTaskMemFree(pszDomain);
                        CoTaskMemFree(pszUsername);
                    }
                    CoTaskMemFree(pwzProtectedPassword);
                }
            }
        }
        else {
            *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
            *ppwszOptionalStatusText = nullptr;
            *pcpsiOptionalStatusIcon = CPSI_WARNING;
        }
    }
    else
    {
        DWORD dwAuthFlags = CRED_PACK_PROTECTED_CREDENTIALS | CRED_PACK_ID_PROVIDER_CREDENTIALS;

        // First get the size of the authentication buffer to allocate
        if (!CredPackAuthenticationBuffer(dwAuthFlags, _pszQualifiedUserName, const_cast<PWSTR>(_rgFieldStrings[SFI_PASSWORD]), nullptr, &pcpcs->cbSerialization) &&
            (GetLastError() == ERROR_INSUFFICIENT_BUFFER))
        {
            pcpcs->rgbSerialization = static_cast<byte*>(CoTaskMemAlloc(pcpcs->cbSerialization));
            if (pcpcs->rgbSerialization != nullptr)
            {
                hr = S_OK;

                // Retrieve the authentication buffer
                if (CredPackAuthenticationBuffer(dwAuthFlags, _pszQualifiedUserName, const_cast<PWSTR>(_rgFieldStrings[SFI_PASSWORD]), pcpcs->rgbSerialization, &pcpcs->cbSerialization))
                {
                    ULONG ulAuthPackage;
                    hr = RetrieveNegotiateAuthPackage(&ulAuthPackage);
                    if (SUCCEEDED(hr))
                    {
                        pcpcs->ulAuthenticationPackage = ulAuthPackage;
                        pcpcs->clsidCredentialProvider = CLSID_OpenImp;

                        // At this point the credential has created the serialized credential used for logon
                        // By setting this to CPGSR_RETURN_CREDENTIAL_FINISHED we are letting logonUI know
                        // that we have all the information we need and it should attempt to submit the
                        // serialized credential.
                        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
                    }
                }
                else
                {
                    hr = HRESULT_FROM_WIN32(GetLastError());
                    if (SUCCEEDED(hr))
                    {
                        hr = E_FAIL;
                    }
                }

                if (FAILED(hr))
                {
                    CoTaskMemFree(pcpcs->rgbSerialization);
                }
            }
            else
            {
                hr = E_OUTOFMEMORY;
            }
        }
    }
    return hr;
}

struct REPORT_RESULT_STATUS_INFO
{
    NTSTATUS ntsStatus;
    NTSTATUS ntsSubstatus;
    PWSTR     pwzMessage;
    CREDENTIAL_PROVIDER_STATUS_ICON cpsi;
};

static const REPORT_RESULT_STATUS_INFO s_rgLogonStatusInfo[] =
{
    { STATUS_LOGON_FAILURE, STATUS_SUCCESS, L"Incorrect password or username.", CPSI_ERROR, },
    { STATUS_ACCOUNT_RESTRICTION, STATUS_ACCOUNT_DISABLED, L"The account is disabled.", CPSI_WARNING },
};

// ReportResult is completely optional.  Its purpose is to allow a credential to customize the string
// and the icon displayed in the case of a logon failure.  For example, we have chosen to
// customize the error shown in the case of bad username/password and in the case of the account
// being disabled.
HRESULT COpenImpCredential::ReportResult(NTSTATUS ntsStatus,
    NTSTATUS ntsSubstatus,
    _Outptr_result_maybenull_ PWSTR* ppwszOptionalStatusText,
    _Out_ CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon)
{
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    DWORD dwStatusInfo = (DWORD)-1;

    // Look for a match on status and substatus.
    for (DWORD i = 0; i < ARRAYSIZE(s_rgLogonStatusInfo); i++)
    {
        if (s_rgLogonStatusInfo[i].ntsStatus == ntsStatus && s_rgLogonStatusInfo[i].ntsSubstatus == ntsSubstatus)
        {
            dwStatusInfo = i;
            break;
        }
    }

    if ((DWORD)-1 != dwStatusInfo)
    {
        if (SUCCEEDED(SHStrDupW(s_rgLogonStatusInfo[dwStatusInfo].pwzMessage, ppwszOptionalStatusText)))
        {
            *pcpsiOptionalStatusIcon = s_rgLogonStatusInfo[dwStatusInfo].cpsi;
        }
    }

    // If we failed the logon, try to erase the password field.
    if (FAILED(HRESULT_FROM_NT(ntsStatus)))
    {
        if (_pCredProvCredentialEvents)
        {
            _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD, L"");
        }
    }

    // Since nullptr is a valid value for *ppwszOptionalStatusText and *pcpsiOptionalStatusIcon
    // this function can't fail.
    return S_OK;
}

// Gets the SID of the user corresponding to the credential.
HRESULT COpenImpCredential::GetUserSid(_Outptr_result_nullonfailure_ PWSTR* ppszSid)
{
    *ppszSid = nullptr;
    HRESULT hr = E_UNEXPECTED;
    if (_pszUserSid != nullptr)
    {
        hr = SHStrDupW(_pszUserSid, ppszSid);
    }
    // Return S_FALSE with a null SID in ppszSid for the
    // credential to be associated with an empty user tile.

    return hr;
}

// GetFieldOptions to enable the password reveal button and touch keyboard auto-invoke in the password field.
HRESULT COpenImpCredential::GetFieldOptions(DWORD dwFieldID,
    _Out_ CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS* pcpcfo)
{
    *pcpcfo = CPCFO_NONE;

    if (dwFieldID == SFI_PASSWORD)
    {
        *pcpcfo = CPCFO_ENABLE_PASSWORD_REVEAL;
    }
    else if (dwFieldID == SFI_TILEIMAGE)
    {
        *pcpcfo = CPCFO_ENABLE_TOUCH_KEYBOARD_AUTO_INVOKE;
    }

    return S_OK;
}
