// String helpers; see Util.h.

#include "Util.h"

void SecureClear(std::wstring& s)
{
    if (!s.empty())
    {
        SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
    }
    s.clear();
}

std::wstring ErrorText(HRESULT hr)
{
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, hr, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text = message ? message : L"Unknown error";
    LocalFree(message);
    // FormatMessage ends its text with a line break.
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r'))
    {
        text.pop_back();
    }
    return text;
}

std::wstring WindowText(HWND hwnd)
{
    std::wstring text(GetWindowTextLengthW(hwnd) + 1, L'\0');
    GetWindowTextW(hwnd, &text[0], static_cast<int>(text.size()));
    text.resize(wcslen(text.c_str()));
    return text;
}
