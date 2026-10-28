// OpenImp Manager: installs the badge sign-in tile and the badge service, links
// badges to Windows users, and sets up the branded sign-in screen. Runs as
// administrator (see the project's UAC setting), because all of that writes to
// HKLM, System32 and Program Files.

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
#include "..\shared\src\Account.h"
#include "..\shared\src\BadgeReader.h"
#include "..\shared\src\BadgeStore.h"
#include "..\shared\src\Branding.h"
#include "..\shared\src\Overlay.h"
#include "..\shared\src\Registry.h"
#include "..\shared\src\Util.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Registry::RegKey;

static const wchar_t c_szAppTitle[] = L"OpenImp";

// Registration of the sign-in tile (see OpenImpAuth\guid.h for the CLSID).
static const wchar_t c_szProviderKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\{e241fbb7-0ad7-461d-b4c4-4a0a374d5f14}";
static const wchar_t c_szClsidKey[] = L"SOFTWARE\\Classes\\CLSID\\{e241fbb7-0ad7-461d-b4c4-4a0a374d5f14}";

// What Install copies from next to this exe into System32.
static const wchar_t* const c_rgInstallFiles[] = { L"OpenImpAuth.dll", L"pcProxAPI.dll", L"OpenImpService.exe" };

static const wchar_t c_szServiceName[] = L"OpenImp";
static const wchar_t c_szServiceDisplayName[] = L"OpenImp Badge Service";
static const wchar_t c_szServiceDescription[] = L"Locks the PC or switches users when a badge is tapped while someone is signed in.";
static const int c_serviceStopWaits = 40;       // x 250 ms
static const DWORD c_serviceStopWaitMs = 250;

static const UINT_PTR c_readerTimer = 1;
static const UINT c_pollMs = 200;

static BadgeReader g_reader;
static TapFilter g_taps;

// ---- helpers ---------------------------------------------------------------

static void ShowMessage(HWND hwnd, const std::wstring& text, UINT icon = MB_ICONINFORMATION)
{
    MessageBoxW(hwnd, text.c_str(), c_szAppTitle, MB_OK | icon);
}

static void ShowError(HWND hwnd, const std::wstring& what, HRESULT hr)
{
    ShowMessage(hwnd, what + L"\n\n" + ErrorText(hr), MB_ICONERROR);
}

static bool Confirm(HWND hwnd, const std::wstring& question)
{
    return MessageBoxW(hwnd, question.c_str(), c_szAppTitle, MB_YESNO | MB_ICONQUESTION) == IDYES;
}

static std::wstring GetText(HWND hwnd, int id)
{
    return WindowText(GetDlgItem(hwnd, id));
}

static void SetText(HWND hwnd, int id, const std::wstring& text)
{
    SetDlgItemTextW(hwnd, id, text.c_str());
}

// The folder this exe runs from, which holds the files Install copies.
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

static bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---- service control -------------------------------------------------------

// A service control handle, closed automatically.
struct ScHandleCloser
{
    void operator()(SC_HANDLE h) const { CloseServiceHandle(h); }
};
using ScHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScHandleCloser>;

static ScHandle OpenBadgeService(DWORD scmAccess, DWORD serviceAccess)
{
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, scmAccess));
    return ScHandle(scm ? OpenServiceW(scm.get(), c_szServiceName, serviceAccess) : nullptr);
}

// Stops the badge service if it's installed and running, waiting up to 10 seconds.
static void StopBadgeService()
{
    ScHandle service = OpenBadgeService(SC_MANAGER_CONNECT, SERVICE_STOP | SERVICE_QUERY_STATUS);
    SERVICE_STATUS status = {};
    if (service && ControlService(service.get(), SERVICE_CONTROL_STOP, &status))
    {
        for (int i = 0; i < c_serviceStopWaits && QueryServiceStatus(service.get(), &status)
            && status.dwCurrentState != SERVICE_STOPPED; i++)
        {
            Sleep(c_serviceStopWaitMs);
        }
    }
}

// Registers OpenImpService.exe (already copied to System32) to start with
// Windows, or updates an existing registration, and starts it.
static HRESULT InstallBadgeService(const std::wstring& exePath)
{
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scm)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    std::wstring binPath = L"\"" + exePath + L"\"";
    ScHandle service(CreateServiceW(scm.get(), c_szServiceName, c_szServiceDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binPath.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS)
    {
        service.reset(OpenServiceW(scm.get(), c_szServiceName, SERVICE_ALL_ACCESS));
        if (service && !ChangeServiceConfigW(service.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, c_szServiceDisplayName))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }
    if (!service)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    SERVICE_DESCRIPTIONW description = { const_cast<LPWSTR>(c_szServiceDescription) };
    ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &description);

    if (!StartServiceW(service.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

static void RemoveBadgeService()
{
    StopBadgeService();
    ScHandle service = OpenBadgeService(SC_MANAGER_CONNECT, DELETE);
    if (service)
    {
        DeleteService(service.get());
    }
}

// ---- install / uninstall ---------------------------------------------------

static bool IsInstalled()
{
    RegKey key;
    return key.Open(HKEY_LOCAL_MACHINE, c_szProviderKey, KEY_READ) == ERROR_SUCCESS
        && FileExists(System32Dir() + L"\\OpenImpAuth.dll");
}

// Sets a key's default value, and optionally one named value, creating the key.
static HRESULT SetDefaultValue(const std::wstring& subKey, PCWSTR value, PCWSTR name = nullptr, PCWSTR data = nullptr)
{
    RegKey key;
    HRESULT hr = HRESULT_FROM_WIN32(key.Create(HKEY_LOCAL_MACHINE, subKey.c_str(), KEY_WRITE));
    if (SUCCEEDED(hr))
    {
        hr = Registry::WriteString(key.Get(), nullptr, value);
    }
    if (SUCCEEDED(hr) && name)
    {
        hr = Registry::WriteString(key.Get(), name, data);
    }
    return hr;
}

// LogonUI keeps the DLL loaded, so an existing copy can't be overwritten. It can
// be renamed, and the renamed copy is deleted at the next reboot.
static HRESULT ReplaceFile(const std::wstring& source, const std::wstring& dest)
{
    if (FileExists(dest))
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

// Installs (or reinstalls) everything. On failure, failedStep says which step failed.
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
        hr = SetDefaultValue(std::wstring(c_szClsidKey) + L"\\InprocServer32", L"OpenImpAuth.dll", L"ThreadingModel", L"Apartment");
    }
    if (SUCCEEDED(hr))
    {
        hr = SetDefaultValue(c_szProviderKey, L"OpenImpAuth");
    }
    if (FAILED(hr))
    {
        return hr;
    }

    failedStep = L"Turning off the Windows lock screen for the branded sign-in screen";
    hr = BrandingSettings::SkipLockScreen(BrandingSettings::Load().enabled);
    if (FAILED(hr))
    {
        return hr;
    }

    failedStep = L"Installing the badge service (tap to lock or switch users)";
    return InstallBadgeService(sys32 + L"\\OpenImpService.exe");
}

// Removes the tile and the service. Enrolled badges and branding settings stay.
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
    // Without the tile there's no branded screen, so bring Windows' lock screen back.
    BrandingSettings::SkipLockScreen(false);

    // A file still loaded by LogonUI is deleted at the next reboot instead.
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

// ---- main window -----------------------------------------------------------

static void RefreshInstallState(HWND hwnd)
{
    bool installed = IsInstalled();
    SetText(hwnd, IDC_INSTALL_STATUS, installed
        ? L"Installed. Tap an enrolled badge to sign in, lock the PC, or switch users."
        : L"Not installed.");
    EnableWindow(GetDlgItem(hwnd, IDC_INSTALL), TRUE);
    SetText(hwnd, IDC_INSTALL, installed ? L"Reinstall" : L"Install");
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

// The user signed in at the console, as COMPUTER\name, or "" if nobody is.
static std::wstring ConsoleUser()
{
    std::wstring user;
    LPWSTR pName = nullptr;
    DWORD cb = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTSGetActiveConsoleSessionId(), WTSUserName, &pName, &cb)
        && pName && *pName)
    {
        user = Account::ComputerName() + L"\\" + pName;
    }
    WTSFreeMemory(pName);
    return user;
}

// Lists the local users. Called when the window opens and by the Refresh button,
// so users added or deleted since then show up. Keeps the current choice if
// that user still exists, otherwise picks whoever is signed in at the console.
static void FillAccounts(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_ACCOUNT);
    std::wstring computer = Account::ComputerName();
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

    auto find = [&](const std::wstring& account)
    {
        return account.empty() ? CB_ERR
            : SendMessageW(hCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(account.c_str()));
    };
    LRESULT index = find(previous);
    if (index == CB_ERR)
    {
        index = find(ConsoleUser());
    }
    SendMessageW(hCombo, CB_SETCURSEL, index == CB_ERR ? 0 : index, 0);
}

// Runs on a timer: fills in the badge ID when a badge is tapped.
static void PollReader(HWND hwnd)
{
    if (!g_reader.Connect())
    {
        SetText(hwnd, IDC_READER_STATUS, L"Badge reader not found. Check that it's plugged in.");
        return;
    }

    std::wstring badge = g_reader.ReadBadge();
    if (badge.empty() && GetText(hwnd, IDC_BADGE_ID).empty())
    {
        SetText(hwnd, IDC_READER_STATUS, L"Reader ready. Tap a badge to fill in its ID.");
    }
    if (!g_taps.IsNewTap(badge))
    {
        return;
    }

    SetText(hwnd, IDC_BADGE_ID, badge);
    std::wstring user, password;
    if (SUCCEEDED(BadgeStore::Lookup(badge, user, password)))
    {
        SetText(hwnd, IDC_READER_STATUS, L"This badge is enrolled for " + user + L". Saving replaces it.");
    }
    else
    {
        SetText(hwnd, IDC_READER_STATUS, L"Badge read. Choose the user, type their password, and click Save badge.");
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
        ShowMessage(hwnd, L"Tap a badge on the reader first.");
        return;
    }
    if (account.empty())
    {
        ShowMessage(hwnd, L"Choose the Windows user for this badge.");
        return;
    }
    account = Account::Qualify(account);

    std::wstring password = GetText(hwnd, IDC_PASSWORD);
    HRESULT hr = Account::CheckPassword(account, password);
    if (FAILED(hr))
    {
        SecureClear(password);
        ShowError(hwnd, L"That password didn't work for " + account + L".", hr);
        return;
    }

    hr = BadgeStore::Save(badge, account, password);
    SecureClear(password);
    SetText(hwnd, IDC_PASSWORD, L"");
    if (FAILED(hr))
    {
        ShowError(hwnd, L"Couldn't save the badge.", hr);
        return;
    }

    SetText(hwnd, IDC_BADGE_ID, L"");
    SetText(hwnd, IDC_READER_STATUS, L"Saved. Badge " + badge + L" now signs in " + account + L".");
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
    if (!Confirm(hwnd, std::wstring(L"Remove badge ") + badge + L"?"))
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
    if (!Confirm(hwnd, L"Remove the badge sign-in tile? Enrolled badges are kept."))
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

// ---- sign-in screen dialog -------------------------------------------------

// The three color pickers, in dialog order.
enum Swatch { SwatchAccent, SwatchText, SwatchBackground, SwatchCount };
static const int c_rgSwatchIds[SwatchCount] = { IDC_BR_ACCENT_SW, IDC_BR_TEXT_SW, IDC_BR_BGCOLOR_SW };

struct BrandingDialog
{
    BrandingSettings settings;
    HBRUSH swatches[SwatchCount] = {};      // fill for each color's sample square
    std::unique_ptr<Overlay> preview;
};

static COLORREF& SwatchColor(BrandingSettings& s, Swatch swatch)
{
    switch (swatch)
    {
    case SwatchAccent: return s.accentColor;
    case SwatchText:   return s.textColor;
    default:           return s.backgroundColor;
    }
}

// Repaints a color's sample square after the color changes.
static void UpdateSwatch(HWND hwnd, BrandingDialog& d, Swatch swatch)
{
    if (d.swatches[swatch])
    {
        DeleteObject(d.swatches[swatch]);
    }
    d.swatches[swatch] = CreateSolidBrush(SwatchColor(d.settings, swatch));
    InvalidateRect(GetDlgItem(hwnd, c_rgSwatchIds[swatch]), nullptr, TRUE);
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

static void FillBrandingControls(HWND hwnd, BrandingDialog& d)
{
    CheckDlgButton(hwnd, IDC_BR_ENABLED, d.settings.enabled ? BST_CHECKED : BST_UNCHECKED);
    SetText(hwnd, IDC_BR_HEADLINE, d.settings.headline);
    SetText(hwnd, IDC_BR_MESSAGE, d.settings.message);
    SetText(hwnd, IDC_BR_BACKGROUND, d.settings.backgroundImage);
    SetText(hwnd, IDC_BR_LOGO, d.settings.logoImage);
    for (int i = 0; i < SwatchCount; i++)
    {
        UpdateSwatch(hwnd, d, static_cast<Swatch>(i));
    }
}

// Lets the user pick an image file and puts its path in an edit box.
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
        SetText(hwnd, editId, path);
    }
}

static void PickColor(HWND hwnd, BrandingDialog& d, Swatch swatch)
{
    static COLORREF custom[16] = {};    // the color dialog's custom colors, kept while the app runs
    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner = hwnd;
    cc.rgbResult = SwatchColor(d.settings, swatch);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc))
    {
        SwatchColor(d.settings, swatch) = cc.rgbResult;
        UpdateSwatch(hwnd, d, swatch);
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

// The extension of a file path, including the dot, or "" if it has none.
static std::wstring Extension(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L'\\');
    return dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? path.substr(dot) : L"";
}

// Copies a picked image into the branding folder (unless it's already there)
// and returns its new path. Each copy gets a new name, so the sign-in screen
// never sees a half-written file.
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
    std::wstring dest = dir + L"\\" + baseName + L"-" + std::to_wstring(GetTickCount64()) + Extension(source);
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

// Copies the images into place and saves the settings. Shows any error itself.
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

static void ShowPreview(HWND hwnd, BrandingDialog& d)
{
    ReadBrandingControls(hwnd, d);
    d.preview.reset();
    d.preview = Overlay::Show(d.settings, nullptr, true);
    if (!d.preview)
    {
        ShowMessage(hwnd, L"Couldn't open the preview.", MB_ICONERROR);
    }
}

static INT_PTR CALLBACK BrandingProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    BrandingDialog* d = reinterpret_cast<BrandingDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (msg)
    {
    case WM_INITDIALOG:
        d = reinterpret_cast<BrandingDialog*>(lParam);
        SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
        d->settings = BrandingSettings::Load();
        FillBrandingControls(hwnd, *d);
        return TRUE;

    case WM_CTLCOLORSTATIC:
    {
        // Paint each color's sample square in that color.
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        for (int i = 0; d && i < SwatchCount; i++)
        {
            if (id == c_rgSwatchIds[i] && d->swatches[i])
            {
                return reinterpret_cast<INT_PTR>(d->swatches[i]);
            }
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
        case IDC_BR_BG_CLEAR:    SetText(hwnd, IDC_BR_BACKGROUND, L""); return TRUE;
        case IDC_BR_LOGO_BROWSE: BrowseImage(hwnd, IDC_BR_LOGO); return TRUE;
        case IDC_BR_LOGO_CLEAR:  SetText(hwnd, IDC_BR_LOGO, L""); return TRUE;
        case IDC_BR_ACCENT:      PickColor(hwnd, *d, SwatchAccent); return TRUE;
        case IDC_BR_TEXT:        PickColor(hwnd, *d, SwatchText); return TRUE;
        case IDC_BR_BGCOLOR:     PickColor(hwnd, *d, SwatchBackground); return TRUE;
        case IDC_BR_PREVIEW:     ShowPreview(hwnd, *d); return TRUE;
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

// ---- main dialog -----------------------------------------------------------

// Gives the window the OpenImp icon in its title bar and on the taskbar.
static void SetWindowIcon(HWND hwnd)
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    auto load = [&](int cx, int cy)
    {
        return reinterpret_cast<LPARAM>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    };
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, load(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON)));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, load(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON)));
}

static void InitBadgeList(HWND hwnd)
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
}

static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowIcon(hwnd);
        InitBadgeList(hwnd);
        RefreshInstallState(hwnd);
        RefreshBadges(hwnd);
        FillAccounts(hwnd);
        PollReader(hwnd);
        SetTimer(hwnd, c_readerTimer, c_pollMs, nullptr);
        return TRUE;

    case WM_TIMER:
        if (wParam == c_readerTimer)
        {
            PollReader(hwnd);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_INSTALL:       OnInstall(hwnd);    return TRUE;
        case IDC_UNINSTALL:     OnUninstall(hwnd);  return TRUE;
        case IDC_SAVE:          OnSave(hwnd);       return TRUE;
        case IDC_REMOVE:        OnRemove(hwnd);     return TRUE;
        case IDC_REFRESH_USERS: FillAccounts(hwnd); return TRUE;
        case IDC_BRANDING:      OnBranding(hwnd);   return TRUE;
        case IDCANCEL:          EndDialog(hwnd, 0); return TRUE;
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
