// Registry helpers; see Registry.h.

#include "Registry.h"

#pragma comment(lib, "advapi32.lib")

namespace Registry
{
    LSTATUS RegKey::Open(HKEY root, PCWSTR path, REGSAM access)
    {
        Close();
        return RegOpenKeyExW(root, path, 0, access, &_hKey);
    }

    LSTATUS RegKey::Create(HKEY root, PCWSTR path, REGSAM access, SECURITY_ATTRIBUTES* sa)
    {
        Close();
        return RegCreateKeyExW(root, path, 0, nullptr, 0, access, sa, &_hKey, nullptr);
    }

    void RegKey::Close()
    {
        if (_hKey)
        {
            RegCloseKey(_hKey);
            _hKey = nullptr;
        }
    }

    // Reads a value of the expected type into a byte buffer.
    static bool ReadRaw(HKEY hKey, PCWSTR name, DWORD expectedType, std::vector<BYTE>& data)
    {
        DWORD type = 0, cb = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS || type != expectedType)
        {
            return false;
        }
        data.resize(cb);
        if (RegQueryValueExW(hKey, name, nullptr, nullptr, data.data(), &cb) != ERROR_SUCCESS)
        {
            return false;
        }
        data.resize(cb);
        return true;
    }

    bool ReadString(HKEY hKey, PCWSTR name, std::wstring& value)
    {
        std::vector<BYTE> data;
        if (!ReadRaw(hKey, name, REG_SZ, data))
        {
            return false;
        }
        // The stored string may or may not include its terminator.
        value.assign(reinterpret_cast<const wchar_t*>(data.data()), data.size() / sizeof(wchar_t));
        value.resize(wcsnlen(value.c_str(), value.size()));
        return true;
    }

    bool ReadDword(HKEY hKey, PCWSTR name, DWORD& value)
    {
        std::vector<BYTE> data;
        if (!ReadRaw(hKey, name, REG_DWORD, data) || data.size() != sizeof(DWORD))
        {
            return false;
        }
        value = *reinterpret_cast<const DWORD*>(data.data());
        return true;
    }

    bool ReadQword(HKEY hKey, PCWSTR name, ULONGLONG& value)
    {
        std::vector<BYTE> data;
        if (!ReadRaw(hKey, name, REG_QWORD, data) || data.size() != sizeof(ULONGLONG))
        {
            return false;
        }
        value = *reinterpret_cast<const ULONGLONG*>(data.data());
        return true;
    }

    bool ReadBinary(HKEY hKey, PCWSTR name, std::vector<BYTE>& value)
    {
        return ReadRaw(hKey, name, REG_BINARY, value);
    }

    std::wstring ReadStringOr(HKEY hKey, PCWSTR name, const std::wstring& fallback)
    {
        std::wstring value;
        return ReadString(hKey, name, value) ? value : fallback;
    }

    DWORD ReadDwordOr(HKEY hKey, PCWSTR name, DWORD fallback)
    {
        DWORD value = 0;
        return ReadDword(hKey, name, value) ? value : fallback;
    }

    static HRESULT WriteRaw(HKEY hKey, PCWSTR name, DWORD type, const void* data, DWORD cb)
    {
        return HRESULT_FROM_WIN32(RegSetValueExW(hKey, name, 0, type, static_cast<const BYTE*>(data), cb));
    }

    HRESULT WriteString(HKEY hKey, PCWSTR name, const std::wstring& value)
    {
        return WriteRaw(hKey, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    HRESULT WriteDword(HKEY hKey, PCWSTR name, DWORD value)
    {
        return WriteRaw(hKey, name, REG_DWORD, &value, sizeof(value));
    }

    HRESULT WriteQword(HKEY hKey, PCWSTR name, ULONGLONG value)
    {
        return WriteRaw(hKey, name, REG_QWORD, &value, sizeof(value));
    }

    HRESULT WriteBinary(HKEY hKey, PCWSTR name, const BYTE* data, DWORD cb)
    {
        return WriteRaw(hKey, name, REG_BINARY, data, cb);
    }
}
