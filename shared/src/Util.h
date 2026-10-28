// Small string helpers used across OpenImp.

#pragma once

#include <windows.h>
#include <string>

// Overwrites a string's characters before clearing it, so a password doesn't
// linger in freed memory.
void SecureClear(std::wstring& s);

// The system's message for an error code, for example "Access is denied."
std::wstring ErrorText(HRESULT hr);

// The text of a window or control.
std::wstring WindowText(HWND hwnd);
