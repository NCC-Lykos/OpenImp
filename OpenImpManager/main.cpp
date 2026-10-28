// OpenImp Manager: installs the badge sign-in tile and links badges to Windows
// users. Runs as administrator (see the project's UAC setting), because both
// jobs write to HKLM and System32.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <lm.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <memory>
#include <string>
#include <vector>
#include "resource.h"
#include "..\shared\src\BadgeReader.h"
#include "..\shared\src\BadgeStore.h"
#include "..\shared\src\Branding.h"
#include "..\shared\src\Overlay.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t c_szClsid[] = L"{e241fbb7-0ad7-461d-b4c4-4a0a374d5f14}";
static const wchar_t c_szProviderKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\{e241fbb7-0ad7-461d-b4c4-4a0a374d5f14}";
static const wchar_t c_szClsidKey[] = L"SOFTWARE\\Classes\\CLSID\\{e241fbb7-0ad7-461d-b4c4-4a0a374d5f14}";
static const wchar_t* const c_rgInstallFiles[] = { L"OpenImpAuth.dll", L"pcProxAPI.dll", L"OpenImpService.exe" };
static const wchar_t c_szServiceName[] = L"OpenImp";
static const wchar_t c_szServiceDisplayName[] = L"OpenImp Badge Service";
static const wchar_t c_szServiceDescription[] = L"Locks the PC or switches users when a badge is tapped while someone is signed in.";

static const UINT_PTR c_readerTimer = 1;
static const UINT c_pollMs = 200;

static BadgeReader g_reader;
static std::wstring g_lastBadge;
static DWORD g_lastBadgeTick;

// ---- helpers ---------------------------------------------------------------

static std::wstring ErrorText(HRESULT hr)
{
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, hr, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring text = msg ? msg : L"Unknown error";
    LocalFree(msg);
    return text;
}

static void ShowError(HWND hwnd, const std::wstring& what, HRESULT hr)
{
    std::wstring text = what + L"\n\n" + ErrorText(hr);
    MessageBoxW(hwnd, text.c_str(), L"OpenImp", MB_OK | MB_ICONERROR);
}

static std::wstring GetText(HWND hwnd, int id)
{
    HWND hCtl = GetDlgItem(hwnd, id);
    std::wstring text(GetWindowTextLengthW(hCtl) + 1, L'\0');
    GetWindowTextW(hCtl, &text[0], static_cast<int>(text.size()));
    text.resize(wcslen(text.c_str()));
    return text;
}

static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\'));
}

static std::wstring System32Dir()
{
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, ARRAYSIZE(path));
    return path;
}

static std::wstring ComputerName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD cch = ARRAYSIZE(name);
    GetComputerNameW(name, &cch);
    return name;
}

// ---- install / uninstall ---------------------------------------------------

static bool IsInstalled()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, c_szProviderKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
    {
        return false;
    }
    RegCloseKey(hKey);
    return GetFileAttributesW((System32Dir() + L"\\OpenImpAuth.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}

static HRESULT SetDefaultValue(PCWSTR subKey, PCWSTR value, PCWSTR name = nullptr, PCWSTR data = nullptr)
{
    HKEY hKey;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (status == ERROR_SUCCESS)
    {
        status = RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
            static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
        if (status == ERROR_SUCCESS && name)
        {
            status = RegSetValueExW(hKey, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(data),
                static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(hKey);
    }
    return HRESULT_FROM_WIN32(status);
}

// LogonUI keeps the DLL loaded, so an existing copy can't be overwritten. It can
// be renamed, and the renamed copy is deleted at the next reboot.
static HRESULT ReplaceFile(const std::wstring& source, const std::wstring& dest)
{
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        std::wstring old = dest + L"." + std::to_wstring(GetTickCount64()) + L".old";
        if (!MoveFileExW(dest.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (!DeleteFileW(old.c_str()))
        {
            MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    return CopyFileW(source.c_str(), dest.c_str(), FALSE) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

// Stops the badge service if it's installed and running, waiting up to 10 seconds.
static void StopBadgeService()
{
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm)
    {
        return;
    }
    SC_HANDLE hService = OpenServiceW(hScm, c_szServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (hService)
    {
        SERVICE_STATUS status = {};
        if (ControlService(hService, SERVICE_CONTROL_STOP, &status))
        {
            for (int i = 0; i < 40 && QueryServiceStatus(hService, &status) && status.dwCurrentState != SERVICE_STOPPED; i++)
            {
                Sleep(250);
            }
        }
        CloseServiceHandle(hService);
    }
    CloseServiceHandle(hScm);
}

// Registers OpenImpService.exe (already copied to System32) to start with
// Windows, and starts it.
static HRESULT InstallBadgeService(const std::wstring& exePath)
{
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!hScm)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    std::wstring binPath = L"\"" + exePath + L"\"";
    SC_HANDLE hService = CreateServiceW(hScm, c_szServiceName, c_szServiceDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binPath.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!hService && GetLastError() == ERROR_SERVICE_EXISTS)
    {
        hService = OpenServiceW(hScm, c_szServiceName, SERVICE_ALL_ACCESS);
        if (hService && !ChangeServiceConfigW(hService, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, c_szServiceDisplayName))
        {
            DWORD error = GetLastError();
            CloseServiceHandle(hService);
            CloseServiceHandle(hScm);
            return HRESULT_FROM_WIN32(error);
        }
    }
    if (!hService)
    {
        DWORD error = GetLastError();
        CloseServiceHandle(hScm);
        return HRESULT_FROM_WIN32(error);
    }

    SERVICE_DESCRIPTIONW description = { const_cast<LPWSTR>(c_szServiceDescription) };
    ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &description);

    HRESULT hr = S_OK;
    if (!StartServiceW(hService, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    CloseServiceHandle(hService);
    CloseServiceHandle(hScm);
    return hr;
}

static void RemoveBadgeService()
{
    StopBadgeService();
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm)
    {
        return;
    }
    SC_HANDLE hService = OpenServiceW(hScm, c_szServiceName, DELETE);
    if (hService)
    {
        DeleteService(hService);
        CloseServiceHandle(hService);
    }
    CloseServiceHandle(hScm);
}

static HRESULT Install(std::wstring& failedStep)
{
    std::wstring exeDir = ExeDir();
    std::wstring sys32 = System32Dir();

    // Let go of OpenImpService.exe so it can be replaced.
    StopBadgeService();

    for (PCWSTR file : c_rgInstallFiles)
    {
        failedStep = std::wstring(L"Copying ") + file + L" to " + sys32;
        HRESULT hr = ReplaceFile(exeDir + L"\\" + file, sys32 + L"\\" + file);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    failedStep = L"Registering the sign-in tile";
    HRESULT hr = SetDefaultValue(c_szClsidKey, L"OpenImpAuth");
    if (SUCCEEDED(hr))
    {
        hr = SetDefaultValue((std::wstring(c_szClsidKey) + L"\\InprocServer32").c_str(), L"OpenImpAuth.dll", L"ThreadingModel", L"Apartment");
    }
    if (SUCCEEDED(hr))
    {
        hr = SetDefaultValue(c_szProviderKey, L"OpenImpAuth");
    }
    if (FAILED(hr))
    {
        return hr;
    }

    failedStep = L"Installing the badge service (tap to lock or switch users)";
    return InstallBadgeService(sys32 + L"\\OpenImpService.exe");
}

static HRESULT Uninstall()
{
    // Unregister first: once the provider key is gone LogonUI stops loading the tile.
    LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, c_szProviderKey);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
    {
        return HRESULT_FROM_WIN32(status);
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, c_szClsidKey);
    RemoveBadgeService();

    std::wstring sys32 = System32Dir();
    for (PCWSTR file : c_rgInstallFiles)
    {
        std::wstring path = sys32 + L"\\" + file;
        if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    return S_OK;
}

// ---- UI --------------------------------------------------------------------

static void RefreshInstallState(HWND hwnd)
{
    bool installed = IsInstalled();
    SetDlgItemTextW(hwnd, IDC_INSTALL_STATUS, installed
        ? L"Installed. Tap an enrolled badge to sign in, lock the PC, or switch users."
        : L"Not installed.");
    EnableWindow(GetDlgItem(hwnd, IDC_INSTALL), TRUE);
    SetDlgItemTextW(hwnd, IDC_INSTALL, installed ? L"Reinstall" : L"Install");
    EnableWindow(GetDlgItem(hwnd, IDC_UNINSTALL), installed);
}

static void RefreshBadges(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_BADGES);
    ListView_DeleteAllItems(hList);

    std::vector<BadgeEntry> entries;
    HRESULT hr = BadgeStore::List(entries);
    if (FAILED(hr))
    {
        ShowError(hwnd, L"Couldn't read the enrolled badges.", hr);
        return;
    }
    for (int i = 0; i < static_cast<int>(entries.size()); i++)
    {
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<LPWSTR>(entries[i].badgeId.c_str());
        ListView_InsertItem(hList, &item);
        ListView_SetItemText(hList, i, 1, const_cast<LPWSTR>(entries[i].user.c_str()));
    }
}

// Lists the local users. Called when the window opens and by the Refresh button,
// so users added or deleted since then show up. Keeps the current choice if
// that user still exists.
static void FillAccounts(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_ACCOUNT);
    std::wstring computer = ComputerName();
    std::wstring previous = GetText(hwnd, IDC_ACCOUNT);
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    // Enabled local accounts only.
    USER_INFO_1* pUsers = nullptr;
    DWORD read = 0, total = 0;
    if (NetUserEnum(nullptr, 1, FILTER_NORMAL_ACCOUNT, reinterpret_cast<BYTE**>(&pUsers),
        MAX_PREFERRED_LENGTH, &read, &total, nullptr) == NERR_Success)
    {
        for (DWORD i = 0; i < read; i++)
        {
            if (!(pUsers[i].usri1_flags & UF_ACCOUNTDISABLE))
            {
                std::wstring account = computer + L"\\" + pUsers[i].usri1_name;
                SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(account.c_str()));
            }
        }
        NetApiBufferFree(pUsers);
    }

    // Default to whoever is signed in at the console, falling back to the first entry.
    std::wstring current;
    DWORD session = WTSGetActiveConsoleSessionId();
    LPWSTR pName = nullptr;
    DWORD cb = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &pName, &cb) && pName && *pName)
    {
        current = computer + L"\\" + pName;
    }
    WTSFreeMemory(pName);
    LRESULT index = previous.empty() ? CB_ERR
        : SendMessageW(hCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(previous.c_str()));
    if (index == CB_ERR && !current.empty())
    {
        index = SendMessageW(hCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(current.c_str()));
    }
    SendMessageW(hCombo, CB_SETCURSEL, index == CB_ERR ? 0 : index, 0);
}

static void PollReader(HWND hwnd)
{
    if (!g_reader.Connect())
    {
        SetDlgItemTextW(hwnd, IDC_READER_STATUS, L"Badge reader not found. Check that it's plugged in.");
        return;
    }

    std::wstring badge = g_reader.ReadBadge();
    if (badge.empty())
    {
        if (GetText(hwnd, IDC_BADGE_ID).empty())
        {
            SetDlgItemTextW(hwnd, IDC_READER_STATUS, L"Reader ready. Tap a badge to fill in its ID.");
        }
        return;
    }

    DWORD now = GetTickCount();
    bool newTap = badge != g_lastBadge || now - g_lastBadgeTick > 3000;
    g_lastBadge = badge;
    g_lastBadgeTick = now;
    if (!newTap)
    {
        return;
    }

    SetDlgItemTextW(hwnd, IDC_BADGE_ID, badge.c_str());
    std::wstring user, password;
    if (SUCCEEDED(BadgeStore::Lookup(badge, user, password)))
    {
        SetDlgItemTextW(hwnd, IDC_READER_STATUS, (L"This badge is enrolled for " + user + L". Saving replaces it.").c_str());
    }
    else
    {
        SetDlgItemTextW(hwnd, IDC_READER_STATUS, L"Badge read. Choose the user, type their password, and click Save badge.");
    }
    SecureClear(password);
    SetFocus(GetDlgItem(hwnd, IDC_PASSWORD));
}

static void OnSave(HWND hwnd)
{
    std::wstring badge = GetText(hwnd, IDC_BADGE_ID);
    std::wstring account = GetText(hwnd, IDC_ACCOUNT);
    if (badge.empty())
    {
        MessageBoxW(hwnd, L"Tap a badge on the reader first.", L"OpenImp", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (account.empty())
    {
        MessageBoxW(hwnd, L"Choose the Windows user for this badge.", L"OpenImp", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (account.find(L'\\') == std::wstring::npos)
    {
        account = ComputerName() + L"\\" + account;
    }

    std::wstring password = GetText(hwnd, IDC_PASSWORD);
    size_t slash = account.find(L'\\');
    std::wstring domain = account.substr(0, slash);
    std::wstring name = account.substr(slash + 1);

    // Check the password now, so a typo doesn't turn into a failed sign-in later.
    HANDLE hToken = nullptr;
    if (!LogonUserW(name.c_str(), domain.c_str(), password.c_str(), LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hToken))
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        SecureClear(password);
        ShowError(hwnd, L"That password didn't work for " + account + L".", hr);
        return;
    }
    CloseHandle(hToken);

    HRESULT hr = BadgeStore::Save(badge, account, password);
    SecureClear(password);
    SetDlgItemTextW(hwnd, IDC_PASSWORD, L"");
    if (FAILED(hr))
    {
        ShowError(hwnd, L"Couldn't save the badge.", hr);
        return;
    }

    SetDlgItemTextW(hwnd, IDC_BADGE_ID, L"");
    SetDlgItemTextW(hwnd, IDC_READER_STATUS, (L"Saved. Badge " + badge + L" now signs in " + account + L".").c_str());
    RefreshBadges(hwnd);
}

static void OnRemove(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_BADGES);
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel < 0)
    {
        return;
    }
    wchar_t badge[256] = {};
    ListView_GetItemText(hList, sel, 0, badge, ARRAYSIZE(badge));
    std::wstring prompt = std::wstring(L"Remove badge ") + badge + L"?";
    if (MessageBoxW(hwnd, prompt.c_str(), L"OpenImp", MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        return;
    }
    HRESULT hr = BadgeStore::Remove(badge);
    if (FAILED(hr))
    {
        ShowError(hwnd, L"Couldn't remove the badge.", hr);
    }
    RefreshBadges(hwnd);
}

static void OnInstall(HWND hwnd)
{
    std::wstring failedStep;
    HRESULT hr = Install(failedStep);
    if (FAILED(hr))
    {
        ShowError(hwnd, failedStep + L" failed.", hr);
    }
    RefreshInstallState(hwnd);
}

static void OnUninstall(HWND hwnd)
{
    if (MessageBoxW(hwnd, L"Remove the badge sign-in tile? Enrolled badges are kept.", L"OpenImp",
        MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        return;
    }
    HRESULT hr = Uninstall();
    if (FAILED(hr))
    {
        ShowError(hwnd, L"Uninstall failed.", hr);
    }
    RefreshInstallState(hwnd);
}

// ---- sign-in screen branding ----------------------------------------------

struct BrandingDialog
{
    BrandingSettings settings;
    HBRUSH swatches[3] = {};    // accent, text, background
    std::unique_ptr<Overlay> preview;
};

static COLORREF* SwatchColor(BrandingSettings& s, int index)
{
    return index == 0 ? &s.accentColor : index == 1 ? &s.textColor : &s.backgroundColor;
}

static void UpdateSwatch(HWND hwnd, BrandingDialog& d, int index)
{
    static const int ids[] = { IDC_BR_ACCENT_SW, IDC_BR_TEXT_SW, IDC_BR_BGCOLOR_SW };
    if (d.swatches[index])
    {
        DeleteObject(d.swatches[index]);
    }
    d.swatches[index] = CreateSolidBrush(*SwatchColor(d.settings, index));
    InvalidateRect(GetDlgItem(hwnd, ids[index]), nullptr, TRUE);
}

// Reads the dialog into d.settings (colors are kept up to date as they're picked).
static void ReadBrandingControls(HWND hwnd, BrandingDialog& d)
{
    d.settings.enabled = IsDlgButtonChecked(hwnd, IDC_BR_ENABLED) == BST_CHECKED;
    d.settings.headline = GetText(hwnd, IDC_BR_HEADLINE);
    d.settings.message = GetText(hwnd, IDC_BR_MESSAGE);
    d.settings.backgroundImage = GetText(hwnd, IDC_BR_BACKGROUND);
    d.settings.logoImage = GetText(hwnd, IDC_BR_LOGO);
}

static void BrowseImage(HWND hwnd, int editId)
{
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Images (*.jpg;*.jpeg;*.png;*.bmp;*.gif)\0*.jpg;*.jpeg;*.png;*.bmp;*.gif\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAYSIZE(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn))
    {
        SetDlgItemTextW(hwnd, editId, path);
    }
}

static void PickColor(HWND hwnd, BrandingDialog& d, int index)
{
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner = hwnd;
    cc.rgbResult = *SwatchColor(d.settings, index);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc))
    {
        *SwatchColor(d.settings, index) = cc.rgbResult;
        UpdateSwatch(hwnd, d, index);
    }
}

// Images live in %ProgramFiles%\OpenImp\Branding, where only administrators can
// change them, since the sign-in screen (running as SYSTEM) displays them.
static std::wstring BrandingDir()
{
    PWSTR programFiles = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles)))
    {
        dir = std::wstring(programFiles) + L"\\OpenImp\\Branding";
    }
    CoTaskMemFree(programFiles);
    return dir;
}

static bool IsInDir(const std::wstring& path, const std::wstring& dir)
{
    return path.size() > dir.size() && _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0 && path[dir.size()] == L'\\';
}

// Copies a picked image into the branding folder (unless it's already there)
// and returns its new path.
static HRESULT StoreImage(const std::wstring& source, const std::wstring& dir, PCWSTR baseName, std::wstring& stored)
{
    stored.clear();
    if (source.empty())
    {
        return S_OK;
    }
    if (IsInDir(source, dir))
    {
        stored = source;
        return S_OK;
    }
    size_t dot = source.find_last_of(L'.');
    size_t slash = source.find_last_of(L'\\');
    std::wstring ext = (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) ? source.substr(dot) : L"";
    std::wstring dest = dir + L"\\" + baseName + L"-" + std::to_wstring(GetTickCount64()) + ext;
    if (!CopyFileW(source.c_str(), dest.c_str(), FALSE))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    stored = dest;
    return S_OK;
}

// Deletes images in the branding folder that the settings no longer use.
static void RemoveUnusedImages(const std::wstring& dir, const BrandingSettings& s)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            continue;
        }
        std::wstring path = dir + L"\\" + fd.cFileName;
        if (_wcsicmp(path.c_str(), s.backgroundImage.c_str()) != 0 && _wcsicmp(path.c_str(), s.logoImage.c_str()) != 0)
        {
            DeleteFileW(path.c_str());
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

static bool SaveBranding(HWND hwnd, BrandingDialog& d)
{
    ReadBrandingControls(hwnd, d);
    BrandingSettings s = d.settings;

    std::wstring dir = BrandingDir();
    if (dir.empty())
    {
        ShowError(hwnd, L"Couldn't find the Program Files folder.", E_FAIL);
        return false;
    }
    CreateDirectoryW(dir.substr(0, dir.find_last_of(L'\\')).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    HRESULT hr = StoreImage(d.settings.backgroundImage, dir, L"background", s.backgroundImage);
    if (FAILED(hr))
    {
        ShowError(hwnd, L"Couldn't copy the background image.", hr);
        return false;
    }
    hr = StoreImage(d.settings.logoImage, dir, L"logo", s.logoImage);
    if (FAILED(hr))
    {
        ShowError(hwnd, L"Couldn't copy the logo.", hr);
        return false;
    }
    hr = s.Save();
    if (FAILED(hr))
    {
        ShowError(hwnd, L"Couldn't save the sign-in screen settings.", hr);
        return false;
    }
    RemoveUnusedImages(dir, s);
    return true;
}

static INT_PTR CALLBACK BrandingProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    BrandingDialog* d = reinterpret_cast<BrandingDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        d = reinterpret_cast<BrandingDialog*>(lParam);
        SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
        d->settings = BrandingSettings::Load();
        CheckDlgButton(hwnd, IDC_BR_ENABLED, d->settings.enabled ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(hwnd, IDC_BR_HEADLINE, d->settings.headline.c_str());
        SetDlgItemTextW(hwnd, IDC_BR_MESSAGE, d->settings.message.c_str());
        SetDlgItemTextW(hwnd, IDC_BR_BACKGROUND, d->settings.backgroundImage.c_str());
        SetDlgItemTextW(hwnd, IDC_BR_LOGO, d->settings.logoImage.c_str());
        for (int i = 0; i < 3; i++)
        {
            UpdateSwatch(hwnd, *d, i);
        }
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        int index = id == IDC_BR_ACCENT_SW ? 0 : id == IDC_BR_TEXT_SW ? 1 : id == IDC_BR_BGCOLOR_SW ? 2 : -1;
        if (d && index >= 0 && d->swatches[index])
        {
            return reinterpret_cast<INT_PTR>(d->swatches[index]);
        }
        break;
    }

    case WM_COMMAND:
        if (!d)
        {
            break;
        }
        switch (LOWORD(wParam))
        {
        case IDC_BR_BG_BROWSE:   BrowseImage(hwnd, IDC_BR_BACKGROUND); return TRUE;
        case IDC_BR_BG_CLEAR:    SetDlgItemTextW(hwnd, IDC_BR_BACKGROUND, L""); return TRUE;
        case IDC_BR_LOGO_BROWSE: BrowseImage(hwnd, IDC_BR_LOGO); return TRUE;
        case IDC_BR_LOGO_CLEAR:  SetDlgItemTextW(hwnd, IDC_BR_LOGO, L""); return TRUE;
        case IDC_BR_ACCENT:      PickColor(hwnd, *d, 0); return TRUE;
        case IDC_BR_TEXT:        PickColor(hwnd, *d, 1); return TRUE;
        case IDC_BR_BGCOLOR:     PickColor(hwnd, *d, 2); return TRUE;
        case IDC_BR_PREVIEW:
            ReadBrandingControls(hwnd, *d);
            d->preview.reset();
            d->preview = Overlay::Show(d->settings, nullptr, true);
            if (!d->preview)
            {
                MessageBoxW(hwnd, L"Couldn't open the preview.", L"OpenImp", MB_OK | MB_ICONERROR);
            }
            return TRUE;
        case IDOK:
            if (SaveBranding(hwnd, *d))
            {
                EndDialog(hwnd, IDOK);
            }
            return TRUE;
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (d)
        {
            d->preview.reset();
            for (HBRUSH& brush : d->swatches)
            {
                if (brush)
                {
                    DeleteObject(brush);
                    brush = nullptr;
                }
            }
        }
        break;
    }
    return FALSE;
}

static void OnBranding(HWND hwnd)
{
    BrandingDialog dialog;
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_BRANDING), hwnd, BrandingProc,
        reinterpret_cast<LPARAM>(&dialog));
}

static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND hList = GetDlgItem(hwnd, IDC_BADGES);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT);
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 160;
        col.pszText = const_cast<LPWSTR>(L"Badge ID");
        ListView_InsertColumn(hList, 0, &col);
        col.cx = 260;
        col.pszText = const_cast<LPWSTR>(L"Windows user");
        ListView_InsertColumn(hList, 1, &col);

        RefreshInstallState(hwnd);
        RefreshBadges(hwnd);
        FillAccounts(hwnd);
        PollReader(hwnd);
        SetTimer(hwnd, c_readerTimer, c_pollMs, nullptr);
        return TRUE;
    }

    case WM_TIMER:
        if (wParam == c_readerTimer)
        {
            PollReader(hwnd);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_INSTALL:   OnInstall(hwnd);   return TRUE;
        case IDC_UNINSTALL: OnUninstall(hwnd); return TRUE;
        case IDC_SAVE:      OnSave(hwnd);      return TRUE;
        case IDC_REMOVE:    OnRemove(hwnd);    return TRUE;
        case IDC_REFRESH_USERS: FillAccounts(hwnd); return TRUE;
        case IDC_BRANDING:  OnBranding(hwnd);  return TRUE;
        case IDCANCEL:      EndDialog(hwnd, 0); return TRUE;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, c_readerTimer);
        g_reader.Disconnect();
        break;
    }
    return FALSE;
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    // Keep the badge service off the reader while this window is open, so tapping
    // a badge to enroll it doesn't lock the PC or switch users.
    ReaderPause pause;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    DialogBoxW(hInstance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DialogProc);
    return 0;
}
