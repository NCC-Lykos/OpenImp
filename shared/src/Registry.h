// Small helpers for the registry values OpenImp keeps under HKLM.
//
// RegKey closes its key when it goes out of scope. The Read* functions return
// false when the value is missing or has the wrong type, so callers can fall
// back to a default; the Write* functions return an HRESULT.

#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace Registry
{
    // An open registry key, closed automatically.
    class RegKey
    {
    public:
        RegKey() = default;
        ~RegKey() { Close(); }
        RegKey(const RegKey&) = delete;
        RegKey& operator=(const RegKey&) = delete;

        // Opens an existing key. Returns ERROR_FILE_NOT_FOUND when it doesn't exist.
        LSTATUS Open(HKEY root, PCWSTR path, REGSAM access);
        // Opens a key, creating it (and any missing parents) if needed. sa sets
        // the security of a newly created key; null means inherit.
        LSTATUS Create(HKEY root, PCWSTR path, REGSAM access, SECURITY_ATTRIBUTES* sa = nullptr);
        void Close();

        HKEY Get() const { return _hKey; }
        explicit operator bool() const { return _hKey != nullptr; }

    private:
        HKEY _hKey = nullptr;
    };

    bool ReadString(HKEY hKey, PCWSTR name, std::wstring& value);
    bool ReadDword(HKEY hKey, PCWSTR name, DWORD& value);
    bool ReadQword(HKEY hKey, PCWSTR name, ULONGLONG& value);
    bool ReadBinary(HKEY hKey, PCWSTR name, std::vector<BYTE>& value);

    // Convenience forms that return fallback when the value can't be read.
    std::wstring ReadStringOr(HKEY hKey, PCWSTR name, const std::wstring& fallback);
    DWORD ReadDwordOr(HKEY hKey, PCWSTR name, DWORD fallback);

    // name may be null for the key's default value.
    HRESULT WriteString(HKEY hKey, PCWSTR name, const std::wstring& value);
    HRESULT WriteDword(HKEY hKey, PCWSTR name, DWORD value);
    HRESULT WriteQword(HKEY hKey, PCWSTR name, ULONGLONG value);
    HRESULT WriteBinary(HKEY hKey, PCWSTR name, const BYTE* data, DWORD cb);
}
