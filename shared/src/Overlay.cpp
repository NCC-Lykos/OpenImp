// The branded sign-in screen; see Overlay.h.
//
// One full-screen popup spans every monitor. It's drawn with GDI+ into two
// cached bitmaps: the background (drawn once) and the frame (background plus
// the sign-in panel, redrawn when the status or focus changes). The two text
// boxes are real EDIT controls placed over white boxes painted on the panel;
// the buttons are owner-drawn so they match the branding.

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <atomic>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include "Overlay.h"
#include "Util.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shcore.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    const wchar_t c_szClass[] = L"OpenImpBrandedSignIn";
    const wchar_t c_szFont[] = L"Segoe UI";
    const wchar_t c_szDefaultStatus[] = L"Tap your badge, or sign in with your username and password.";
    const wchar_t c_szSignIn[] = L"Sign in";
    const wchar_t c_szCancel[] = L"Cancel and use Windows sign-in";

    const UINT WM_APP_STATUS = WM_APP + 1;      // the status text changed; redraw
    const UINT_PTR c_topmostTimer = 1;
    const UINT c_topmostMs = 1000;
    const DWORD c_startTimeoutMs = 10000;
    const LONGLONG c_maxImageBytes = 64LL * 1024 * 1024;

    // Text sizes, in pixels at 100% scaling.
    const int c_headlinePx = 28;
    const int c_messagePx = 15;
    const int c_labelPx = 13;
    const int c_editPx = 16;
    const int c_signInPx = 16;
    const int c_cancelPx = 14;

    const Gdiplus::Color c_black(255, 0, 0, 0);
    const Gdiplus::Color c_white(255, 255, 255, 255);
    const Gdiplus::Color c_panel(205, 14, 16, 22);      // translucent near-black
    const Gdiplus::Color c_errorText(255, 255, 140, 140);

    enum { IDC_USER = 101, IDC_PASS, IDC_SIGNIN, IDC_CANCEL };

    Gdiplus::Color ToColor(COLORREF c, BYTE alpha = 255)
    {
        return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    Gdiplus::RectF ToRectF(const RECT& r)
    {
        return Gdiplus::RectF(static_cast<Gdiplus::REAL>(r.left), static_cast<Gdiplus::REAL>(r.top),
            static_cast<Gdiplus::REAL>(r.right - r.left), static_cast<Gdiplus::REAL>(r.bottom - r.top));
    }

    void AddRoundRect(Gdiplus::GraphicsPath& path, const RECT& r, int radius)
    {
        int d = radius * 2;
        path.AddArc(r.left, r.top, d, d, 180, 90);
        path.AddArc(r.right - d, r.top, d, d, 270, 90);
        path.AddArc(r.right - d, r.bottom - d, d, d, 0, 90);
        path.AddArc(r.left, r.bottom - d, d, d, 90, 90);
        path.CloseFigure();
    }

    void MoveWindowTo(HWND hwnd, const RECT& r)
    {
        MoveWindow(hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, FALSE);
    }

    // The main monitor, where the panel sits.
    HMONITOR PrimaryMonitor()
    {
        return MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    }

    // Text centered horizontally, and optionally vertically too.
    struct CenteredFormat : Gdiplus::StringFormat
    {
        explicit CenteredFormat(bool middle = false)
        {
            SetAlignment(Gdiplus::StringAlignmentCenter);
            if (middle)
            {
                SetLineAlignment(Gdiplus::StringAlignmentCenter);
            }
        }
    };

    // Loads an image into memory, so the file isn't kept open (OpenImp Manager
    // may want to replace it).
    class Image
    {
    public:
        ~Image() { Free(); }

        bool Load(const std::wstring& path)
        {
            Free();
            std::vector<BYTE> data;
            if (path.empty() || !ReadAll(path, data))
            {
                return false;
            }
            _stream = SHCreateMemStream(data.data(), static_cast<UINT>(data.size()));
            if (!_stream)
            {
                return false;
            }
            _bitmap = Gdiplus::Bitmap::FromStream(_stream);
            if (!_bitmap || _bitmap->GetLastStatus() != Gdiplus::Ok || _bitmap->GetWidth() == 0 || _bitmap->GetHeight() == 0)
            {
                Free();
                return false;
            }
            return true;
        }

        void Free()
        {
            delete _bitmap;
            _bitmap = nullptr;
            if (_stream)
            {
                _stream->Release();
                _stream = nullptr;
            }
        }

        Gdiplus::Bitmap* Get() const { return _bitmap; }

    private:
        // Reads a whole file of a sensible size.
        static bool ReadAll(const std::wstring& path, std::vector<BYTE>& data)
        {
            HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hFile == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            LARGE_INTEGER size = {};
            DWORD read = 0;
            bool ok = GetFileSizeEx(hFile, &size) && size.QuadPart > 0 && size.QuadPart <= c_maxImageBytes;
            if (ok)
            {
                data.resize(static_cast<size_t>(size.QuadPart));
                ok = ReadFile(hFile, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size();
            }
            CloseHandle(hFile);
            return ok;
        }

        IStream* _stream = nullptr;
        Gdiplus::Bitmap* _bitmap = nullptr;
    };

    // An off-screen bitmap the size of the window.
    struct Surface
    {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ old = nullptr;

        bool Create(HWND hwnd, SIZE size)
        {
            if (dc)
            {
                return true;
            }
            HDC screen = GetDC(hwnd);
            dc = CreateCompatibleDC(screen);
            bitmap = CreateCompatibleBitmap(screen, size.cx, size.cy);
            ReleaseDC(hwnd, screen);
            if (!dc || !bitmap)
            {
                return false;
            }
            old = SelectObject(dc, bitmap);
            return true;
        }

        void Free()
        {
            if (dc)
            {
                SelectObject(dc, old);
                DeleteDC(dc);
            }
            if (bitmap)
            {
                DeleteObject(bitmap);
            }
            *this = {};
        }
    };

    class OverlayWindow : public Overlay
    {
    public:
        OverlayWindow(const BrandingSettings& settings, OverlayEvents* events, bool preview) :
            _settings(settings), _events(events), _preview(preview), _status(c_szDefaultStatus)
        {
        }

        ~OverlayWindow() override
        {
            _closing = true;
            HWND hwnd = _hwnd.load();
            if (hwnd)
            {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            if (_thread.joinable())
            {
                _thread.join();
            }
        }

        // Starts the window's thread and waits until the window is up (or failed).
        bool Start()
        {
            HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!hReady)
            {
                return false;
            }
            // The new thread must use the same desktop as the caller: on the
            // sign-in screen that's the secure Winlogon desktop.
            HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
            _thread = std::thread(&OverlayWindow::_Run, this, desktop, hReady);
            WaitForSingleObject(hReady, c_startTimeoutMs);
            CloseHandle(hReady);
            return _open;
        }

        void SetStatus(const std::wstring& text, bool isError) override
        {
            {
                std::lock_guard<std::mutex> guard(_statusLock);
                _status = text.empty() ? c_szDefaultStatus : text;
                _statusIsError = isError;
            }
            HWND hwnd = _hwnd.load();
            if (hwnd)
            {
                PostMessageW(hwnd, WM_APP_STATUS, 0, 0);
            }
        }

        bool IsOpen() const override { return _open; }

    private:
        // Where everything goes, in window coordinates.
        struct Layout
        {
            RECT panel, logo, headline, message, status, userLabel, userBox, passLabel, passBox, signIn, cancel;
        };

        // Scales a size at 100% to the main monitor's scaling.
        int _S(int px) const { return MulDiv(px, _dpi, 96); }
        Gdiplus::REAL _SF(int px) const { return static_cast<Gdiplus::REAL>(_S(px)); }

        // The main monitor's rectangle, in window coordinates.
        RECT _PrimaryRect() const
        {
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(PrimaryMonitor(), &mi);
            RECT r = mi.rcMonitor;
            OffsetRect(&r, -_origin.x, -_origin.y);
            return r;
        }

        // The window thread: creates the window, runs its messages, cleans up.
        void _Run(HDESK desktop, HANDLE hReady)
        {
            if (desktop)
            {
                SetThreadDesktop(desktop);
            }

            Gdiplus::GdiplusStartupInput input;
            ULONG_PTR token = 0;
            if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
            {
                SetEvent(hReady);
                return;
            }

            HINSTANCE hInst = reinterpret_cast<HINSTANCE>(&__ImageBase);
            _open = _Create(hInst);
            SetEvent(hReady);
            if (_open)
            {
                _MessageLoop();
            }
            _open = false;
            _Cleanup();
            Gdiplus::GdiplusShutdown(token);
            UnregisterClassW(c_szClass, hInst);
        }

        bool _Create(HINSTANCE hInst)
        {
            // Work in real screen pixels, whatever the host process declared;
            // otherwise Windows scales the window and the background image
            // comes out stretched or cropped when display scaling is above 100%.
            if (!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            {
                SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
            }

            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = s_WndProc;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = c_szClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                return false;
            }

            _background.Load(_settings.backgroundImage);
            _logo.Load(_settings.logoImage);

            // Cover every monitor.
            _origin = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) };
            _size = { GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN) };
            HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, c_szClass, L"OpenImp sign-in",
                WS_POPUP | WS_CLIPCHILDREN, _origin.x, _origin.y, _size.cx, _size.cy, nullptr, nullptr, hInst, this);
            if (!hwnd)
            {
                return false;
            }
            _hwnd = hwnd;

            // Size the panel for the main monitor's scaling, since that's where it sits.
            UINT dpiX = 0, dpiY = 0;
            if (FAILED(GetDpiForMonitor(PrimaryMonitor(), MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0)
            {
                dpiX = GetDpiForWindow(hwnd);
            }
            _dpi = dpiX ? dpiX : 96;

            if (!_CreateControls(hInst))
            {
                DestroyWindow(hwnd);
                return false;
            }

            _Layout();
            _RenderBackground();
            _RenderFrame();

            if (_closing)
            {
                DestroyWindow(hwnd);
                return false;
            }
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            SetFocus(_hUser);
            SetTimer(hwnd, c_topmostTimer, c_topmostMs, nullptr);
            return true;
        }

        // The username and password boxes and the two buttons. _Layout places them.
        bool _CreateControls(HINSTANCE hInst)
        {
            HWND hwnd = _hwnd.load();
            auto create = [&](PCWSTR cls, PCWSTR text, DWORD style, int id)
            {
                return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                    0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
            };
            _hUser = create(L"EDIT", L"", ES_AUTOHSCROLL, IDC_USER);
            _hPass = create(L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD, IDC_PASS);
            _hSignIn = create(L"BUTTON", c_szSignIn, BS_OWNERDRAW, IDC_SIGNIN);
            _hCancel = create(L"BUTTON", c_szCancel, BS_OWNERDRAW, IDC_CANCEL);
            if (!_hUser || !_hPass || !_hSignIn || !_hCancel)
            {
                return false;
            }

            _hEditFont = CreateFontW(-_S(c_editPx), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, c_szFont);
            SendMessageW(_hUser, WM_SETFONT, reinterpret_cast<WPARAM>(_hEditFont), FALSE);
            SendMessageW(_hPass, WM_SETFONT, reinterpret_cast<WPARAM>(_hEditFont), FALSE);
            return true;
        }

        // Handles Esc and Enter before the controls see them, and Tab through
        // IsDialogMessage.
        void _MessageLoop()
        {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                HWND hwnd = _hwnd.load();
                if (hwnd && msg.message == WM_KEYDOWN)
                {
                    if (msg.wParam == VK_ESCAPE)
                    {
                        _Cancel();
                        continue;
                    }
                    if (msg.wParam == VK_RETURN && (msg.hwnd == _hUser || msg.hwnd == _hPass))
                    {
                        // Enter in the username box moves on to the password, unless one is already typed.
                        if (msg.hwnd == _hUser && GetWindowTextLengthW(_hPass) == 0)
                        {
                            SetFocus(_hPass);
                        }
                        else
                        {
                            _SignIn();
                        }
                        continue;
                    }
                }
                if (hwnd && IsDialogMessageW(hwnd, &msg))
                {
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        void _Cleanup()
        {
            _background.Free();
            _logo.Free();
            _bg.Free();
            _frame.Free();
            if (_hEditFont)
            {
                DeleteObject(_hEditFont);
                _hEditFont = nullptr;
            }
        }

        // Measures the panel's contents, centers the panel on the main monitor,
        // and moves the controls into place.
        void _Layout()
        {
            Layout& L = _layout;
            RECT mon = _PrimaryRect();
            int panelWidth = (std::min)(_S(440), static_cast<int>(mon.right - mon.left) - _S(32));
            int pad = _S(40);
            int contentWidth = panelWidth - 2 * pad;

            // How tall the headline and message are once wrapped to the panel's width.
            HDC hdc = GetDC(_hwnd);
            Gdiplus::Graphics g(hdc);
            Gdiplus::FontFamily family(c_szFont);
            Gdiplus::Font headlineFont(&family, _SF(c_headlinePx), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font messageFont(&family, _SF(c_messagePx), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            CenteredFormat center;
            auto measure = [&](const std::wstring& text, const Gdiplus::Font& font) -> int
            {
                if (text.empty())
                {
                    return 0;
                }
                Gdiplus::RectF box;
                g.MeasureString(text.c_str(), -1, &font, Gdiplus::RectF(0, 0, static_cast<Gdiplus::REAL>(contentWidth), 100000.0f), &center, &box);
                return static_cast<int>(box.Height + 0.999f);
            };
            int headlineHeight = measure(_settings.headline, headlineFont);
            int messageHeight = measure(_settings.message, messageFont);
            ReleaseDC(_hwnd, hdc);

            // The logo fits the content width and at most 96px tall, keeping its shape.
            int logoHeight = 0, logoWidth = 0;
            if (_logo.Get())
            {
                double w = _logo.Get()->GetWidth(), h = _logo.Get()->GetHeight();
                double scale = (std::min)(contentWidth / w, _S(96) / h);
                logoWidth = static_cast<int>(w * scale);
                logoHeight = static_cast<int>(h * scale);
            }

            // Stack the pieces from the top of the panel (y relative to the panel).
            // Empty pieces (no logo, no message) take no space and no gap.
            int y = _S(36);
            auto place = [&](RECT& r, int height, int gapAfter)
            {
                r = { pad, y, pad + contentWidth, y + height };
                y += height + (height ? gapAfter : 0);
            };
            place(L.logo, logoHeight, _S(20));
            place(L.headline, headlineHeight, _S(8));
            place(L.message, messageHeight, _S(20));
            place(L.status, _S(44), _S(12));
            place(L.userLabel, _S(20), _S(4));
            place(L.userBox, _S(36), _S(12));
            place(L.passLabel, _S(20), _S(4));
            place(L.passBox, _S(36), _S(20));
            place(L.signIn, _S(40), _S(10));
            place(L.cancel, _S(30), 0);
            int panelHeight = y + _S(28);

            if (logoWidth)
            {
                L.logo.left = pad + (contentWidth - logoWidth) / 2;
                L.logo.right = L.logo.left + logoWidth;
            }

            // Center the panel on the main monitor (top-aligned if it's too tall).
            int left = mon.left + (mon.right - mon.left - panelWidth) / 2;
            int top = (std::max)(static_cast<int>(mon.top), static_cast<int>(mon.top + (mon.bottom - mon.top - panelHeight) / 2));
            L.panel = { 0, 0, panelWidth, panelHeight };
            for (RECT* r : { &L.logo, &L.headline, &L.message, &L.status, &L.userLabel, &L.userBox, &L.passLabel, &L.passBox, &L.signIn, &L.cancel, &L.panel })
            {
                OffsetRect(r, left, top);
            }

            // The edit controls sit inside the white boxes drawn on the panel.
            int editHeight = _S(22);
            auto placeEdit = [&](HWND hEdit, const RECT& box)
            {
                int editTop = box.top + (box.bottom - box.top - editHeight) / 2;
                MoveWindow(hEdit, box.left + _S(10), editTop, box.right - box.left - _S(20), editHeight, FALSE);
            };
            placeEdit(_hUser, L.userBox);
            placeEdit(_hPass, L.passBox);
            MoveWindowTo(_hSignIn, L.signIn);
            MoveWindowTo(_hCancel, L.cancel);
        }

        // The background, drawn once: the main monitor gets the background color
        // and image, the image cropped to fill it while keeping its shape. Any
        // other monitors are plain black.
        void _RenderBackground()
        {
            if (!_bg.Create(_hwnd, _size))
            {
                return;
            }
            Gdiplus::Graphics g(_bg.dc);
            g.Clear(c_black);
            RECT m = _PrimaryRect();
            Gdiplus::SolidBrush fill(ToColor(_settings.backgroundColor));
            g.FillRectangle(&fill, ToRectF(m));

            Gdiplus::Bitmap* image = _background.Get();
            if (!image)
            {
                return;
            }
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            double iw = image->GetWidth(), ih = image->GetHeight();
            double mw = m.right - m.left, mh = m.bottom - m.top;
            double scale = (std::max)(mw / iw, mh / ih);
            double w = iw * scale, h = ih * scale;
            g.SetClip(ToRectF(m));
            g.DrawImage(image, Gdiplus::RectF(static_cast<Gdiplus::REAL>(m.left + (mw - w) / 2), static_cast<Gdiplus::REAL>(m.top + (mh - h) / 2),
                static_cast<Gdiplus::REAL>(w), static_cast<Gdiplus::REAL>(h)));
            g.ResetClip();
        }

        // Background plus the panel. Redrawn when the status or focus changes.
        void _RenderFrame()
        {
            if (!_frame.Create(_hwnd, _size) || !_bg.dc)
            {
                return;
            }
            BitBlt(_frame.dc, 0, 0, _size.cx, _size.cy, _bg.dc, 0, 0, SRCCOPY);

            const Layout& L = _layout;
            Gdiplus::Graphics g(_frame.dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

            // The panel, with an accent strip along its top edge.
            Gdiplus::GraphicsPath panel;
            AddRoundRect(panel, L.panel, _S(12));
            Gdiplus::SolidBrush panelBrush(c_panel);
            g.FillPath(&panelBrush, &panel);
            g.SetClip(&panel);
            Gdiplus::SolidBrush accent(ToColor(_settings.accentColor));
            g.FillRectangle(&accent, L.panel.left, L.panel.top, L.panel.right - L.panel.left, _S(6));
            g.ResetClip();

            if (_logo.Get())
            {
                g.DrawImage(_logo.Get(), ToRectF(L.logo));
            }

            Gdiplus::FontFamily family(c_szFont);
            Gdiplus::Font headlineFont(&family, _SF(c_headlinePx), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font messageFont(&family, _SF(c_messagePx), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font labelFont(&family, _SF(c_labelPx), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            CenteredFormat center;
            CenteredFormat centerMiddle(true);
            Gdiplus::SolidBrush text(ToColor(_settings.textColor));
            Gdiplus::SolidBrush softText(ToColor(_settings.textColor, 210));
            Gdiplus::SolidBrush errorText(c_errorText);

            g.DrawString(_settings.headline.c_str(), -1, &headlineFont, ToRectF(L.headline), &center, &text);
            g.DrawString(_settings.message.c_str(), -1, &messageFont, ToRectF(L.message), &center, &softText);

            std::wstring status;
            bool isError;
            {
                std::lock_guard<std::mutex> guard(_statusLock);
                status = _status;
                isError = _statusIsError;
            }
            g.DrawString(status.c_str(), -1, &messageFont, ToRectF(L.status), &centerMiddle, isError ? &errorText : &text);

            g.DrawString(L"Username", -1, &labelFont, ToRectF(L.userLabel), nullptr, &softText);
            g.DrawString(L"Password", -1, &labelFont, ToRectF(L.passLabel), nullptr, &softText);

            // White boxes behind the edit controls; the focused one gets an accent outline.
            Gdiplus::SolidBrush white(c_white);
            Gdiplus::Pen focusPen(ToColor(_settings.accentColor), _SF(2));
            for (auto [box, edit] : { std::make_pair(L.userBox, _hUser), std::make_pair(L.passBox, _hPass) })
            {
                Gdiplus::GraphicsPath path;
                AddRoundRect(path, box, _S(6));
                g.FillPath(&white, &path);
                if (_focused == edit)
                {
                    g.DrawPath(&focusPen, &path);
                }
            }

            InvalidateRect(_hwnd, nullptr, FALSE);
            InvalidateRect(_hSignIn, nullptr, FALSE);
            InvalidateRect(_hCancel, nullptr, FALSE);
        }

        // Draws a button over its patch of the frame, so it blends into the panel.
        void _DrawButton(const DRAWITEMSTRUCT* dis)
        {
            RECT r = dis->rcItem;
            int w = r.right - r.left, h = r.bottom - r.top;
            const RECT& place = dis->CtlID == IDC_SIGNIN ? _layout.signIn : _layout.cancel;
            BitBlt(dis->hDC, 0, 0, w, h, _frame.dc, place.left, place.top, SRCCOPY);

            Gdiplus::Graphics g(dis->hDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            Gdiplus::FontFamily family(c_szFont);
            CenteredFormat centerMiddle(true);
            Gdiplus::RectF bounds(0, 0, static_cast<Gdiplus::REAL>(w), static_cast<Gdiplus::REAL>(h));
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            bool focused = (dis->itemState & ODS_FOCUS) != 0;

            if (dis->CtlID == IDC_SIGNIN)
            {
                // A filled accent-colored button, darker while pressed.
                COLORREF c = _settings.accentColor;
                if (pressed)
                {
                    c = RGB(GetRValue(c) * 3 / 4, GetGValue(c) * 3 / 4, GetBValue(c) * 3 / 4);
                }
                Gdiplus::GraphicsPath path;
                AddRoundRect(path, { 0, 0, w - 1, h - 1 }, _S(6));
                Gdiplus::SolidBrush fill(ToColor(c));
                g.FillPath(&fill, &path);
                if (focused)
                {
                    Gdiplus::Pen pen(c_white, _SF(2));
                    g.DrawPath(&pen, &path);
                }
                Gdiplus::Font font(&family, _SF(c_signInPx), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush white(c_white);
                g.DrawString(c_szSignIn, -1, &font, bounds, &centerMiddle, &white);
            }
            else
            {
                // A text link, underlined while focused.
                Gdiplus::Font font(&family, _SF(c_cancelPx), focused ? Gdiplus::FontStyleUnderline : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush text(ToColor(_settings.textColor, pressed ? 150 : 210));
                g.DrawString(c_szCancel, -1, &font, bounds, &centerMiddle, &text);
            }
        }

        void _SignIn()
        {
            std::wstring account = WindowText(_hUser);
            std::wstring password = WindowText(_hPass);
            SetWindowTextW(_hPass, L"");
            if (account.empty())
            {
                SetStatus(L"Type your username.", true);
                SetFocus(_hUser);
            }
            else if (_preview || !_events)
            {
                SetStatus(L"Preview only: signing in is turned off here.", false);
            }
            else
            {
                _events->OnOverlaySignIn(account, password);
            }
            SecureClear(password);
        }

        void _Cancel()
        {
            HWND hwnd = _hwnd.load();
            if (!hwnd)
            {
                return;
            }
            if (_events)
            {
                _events->OnOverlayCancel();
            }
            DestroyWindow(hwnd);
        }

        // Routes window messages to the OverlayWindow passed to CreateWindowEx.
        static LRESULT CALLBACK s_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            OverlayWindow* self;
            if (msg == WM_NCCREATE)
            {
                self = static_cast<OverlayWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            else
            {
                self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            }
            return self ? self->_WndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        LRESULT _WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg)
            {
            case WM_ERASEBKGND:
                return 1;   // WM_PAINT covers everything

            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                if (_frame.dc)
                {
                    BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                        ps.rcPaint.bottom - ps.rcPaint.top, _frame.dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                }
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_CTLCOLOREDIT:
                SetTextColor(reinterpret_cast<HDC>(wParam), RGB(20, 20, 20));
                SetBkColor(reinterpret_cast<HDC>(wParam), RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));

            case WM_DRAWITEM:
                _DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
                return TRUE;

            case WM_COMMAND:
                switch (LOWORD(wParam))
                {
                case IDC_SIGNIN:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        _SignIn();
                    }
                    return 0;
                case IDC_CANCEL:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        _Cancel();
                    }
                    return 0;
                case IDC_USER:
                case IDC_PASS:
                    // Redraw so the focus outline follows the caret.
                    if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS)
                    {
                        _focused = HIWORD(wParam) == EN_SETFOCUS ? reinterpret_cast<HWND>(lParam) : nullptr;
                        _RenderFrame();
                    }
                    return 0;
                }
                break;

            case WM_APP_STATUS:
                _RenderFrame();
                return 0;

            case WM_TIMER:
                if (wParam == c_topmostTimer)
                {
                    // Stay above the Windows sign-in screen if it reorders itself.
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                return 0;

            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;

            case WM_DESTROY:
                KillTimer(hwnd, c_topmostTimer);
                _hwnd = nullptr;
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        BrandingSettings _settings;
        OverlayEvents* _events;
        bool _preview;

        std::thread _thread;
        std::atomic<HWND> _hwnd{ nullptr };
        std::atomic<bool> _open{ false };
        std::atomic<bool> _closing{ false };    // the owner is destroying us; don't show the window

        std::mutex _statusLock;     // guards the two members below
        std::wstring _status;
        bool _statusIsError = false;

        // Window thread only.
        UINT _dpi = 96;
        POINT _origin = {};     // top-left of the virtual screen
        SIZE _size = {};        // size of the virtual screen
        Layout _layout = {};
        Image _background, _logo;
        Surface _bg, _frame;
        HFONT _hEditFont = nullptr;
        HWND _hUser = nullptr, _hPass = nullptr, _hSignIn = nullptr, _hCancel = nullptr;
        HWND _focused = nullptr;    // the edit box with the focus, for its outline
    };
}

std::unique_ptr<Overlay> Overlay::Show(const BrandingSettings& settings, OverlayEvents* events, bool preview)
{
    std::unique_ptr<OverlayWindow> window(new(std::nothrow) OverlayWindow(settings, events, preview));
    if (!window || !window->Start())
    {
        return nullptr;
    }
    return window;
}
