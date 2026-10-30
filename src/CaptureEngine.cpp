// CaptureEngine.cpp - virtual screen geometry, DIB capture, guards, cursor.
#include "CaptureEngine.h"

void CapturedFrame::Reset()
{
    if (hMemDC != nullptr)
    {
        DeleteDC(hMemDC);
        hMemDC = nullptr;
    }
    if (hBitmap != nullptr)
    {
        DeleteObject(hBitmap);
        hBitmap = nullptr;
    }
    bits = nullptr;
    width = 0;
    height = 0;
    stride = 0;
}

CCaptureEngine::CCaptureEngine()
{
}

CCaptureEngine::~CCaptureEngine()
{
}

RECT CCaptureEngine::VirtualScreenRect()
{
    return snap::VirtualScreenRect();
}

bool CCaptureEngine::IsSecureDesktop()
{
    return snap::IsSecureDesktop();
}

void CCaptureEngine::EnsureDpiContext()
{
    if (m_dpiCtxApplied)
    {
        return;
    }
    // Only ever touches OUR worker thread, never the host UI thread.
    snap::SetThreadDpiAwareness();
    m_dpiCtxApplied = true;
    SNAP_LOG(L"DPI awareness context applied on capture thread");
}

CapturedFrame CCaptureEngine::CaptureFullScreen(bool includeCursor, SnapCode& outCode)
{
    return CaptureRect(VirtualScreenRect(), includeCursor, outCode, nullptr);
}

CapturedFrame CCaptureEngine::CaptureRegion(const RECT& rect, bool includeCursor, SnapCode& outCode)
{
    return CaptureRect(rect, includeCursor, outCode, nullptr);
}

CapturedFrame CCaptureEngine::CaptureActiveWindow(bool includeCursor, SnapCode& outCode, std::wstring& outErr)
{
    outErr.clear();
    outCode = kOk;

    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || !IsWindow(foreground))
    {
        outCode = kErrWindowNotFound;
        outErr = L"GetForegroundWindow";
        SNAP_LOGE(outCode, L"GetForegroundWindow returned no window");
        return CapturedFrame();
    }

    // Reject our own windows (overlay / sink / hotkey / worker / picker).
    wchar_t className[128] = {0};
    if (GetClassNameW(foreground, className, static_cast<int>(_countof(className))) > 0)
    {
        if (wcscmp(className, TMSNAP_OVERLAY_CLASS) == 0 || wcscmp(className, TMSNAP_SINK_CLASS) == 0 ||
            wcscmp(className, TMSNAP_HOTKEY_CLASS) == 0 || wcscmp(className, TMSNAP_WORKER_CLASS) == 0 ||
            wcscmp(className, TMSNAP_PICKER_CLASS) == 0)
        {
            outCode = kErrWindowNotFound;
            outErr = className;
            SNAP_LOGE(outCode, L"foreground window belongs to the plugin: %s", className);
            return CapturedFrame();
        }
    }

    return CaptureWindow(foreground, includeCursor, outCode, outErr);
}

CapturedFrame CCaptureEngine::CaptureWindow(HWND target, bool includeCursor, SnapCode& outCode, std::wstring& outErr)
{
    outErr.clear();
    outCode = kOk;

    if (target == nullptr || !IsWindow(target))
    {
        outCode = kErrWindowNotFound;
        outErr = L"target window";
        SNAP_LOGE(outCode, L"target window is no longer valid");
        return CapturedFrame();
    }

    if (IsIconic(target) != FALSE)
    {
        outCode = kErrWindowNotFound;
        outErr = L"minimized";
        SNAP_LOGE(outCode, L"target window is minimized");
        return CapturedFrame();
    }

    RECT bounds{};
    bool haveBounds = false;
    // DWMWA_EXTENDED_FRAME_BOUNDS strips the invisible DWM border and the drop
    // shadow so the captured image matches what the user perceives as "window".
    HRESULT hr = DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds));
    if (SUCCEEDED(hr) && bounds.right > bounds.left && bounds.bottom > bounds.top)
    {
        haveBounds = true;
    }
    if (!haveBounds)
    {
        if (GetWindowRect(target, &bounds) == FALSE || bounds.right <= bounds.left ||
            bounds.bottom <= bounds.top)
        {
            outCode = kErrWindowNotFound;
            outErr = L"GetWindowRect";
            SNAP_LOGE(outCode, L"cannot query the target window rectangle");
            return CapturedFrame();
        }
    }

    return CaptureRect(bounds, includeCursor, outCode, &outErr);
}

bool CCaptureEngine::LooksAllBlack(const CapturedFrame& frame)
{
    if (!frame.IsValid())
    {
        return false;
    }
    const int grid = 32;
    const unsigned char* base = static_cast<const unsigned char*>(frame.bits);
    unsigned long first = 0;
    bool firstSet = false;
    for (int gy = 0; gy < grid; ++gy)
    {
        int y = static_cast<int>(static_cast<long long>(gy) * (frame.height - 1) / grid);
        for (int gx = 0; gx < grid; ++gx)
        {
            int x = static_cast<int>(static_cast<long long>(gx) * (frame.width - 1) / grid);
            const unsigned char* pixel = base + static_cast<size_t>(y) * static_cast<size_t>(frame.stride) +
                                         static_cast<size_t>(x) * 4;
            unsigned long value = static_cast<unsigned long>(pixel[0]) |
                                  (static_cast<unsigned long>(pixel[1]) << 8) |
                                  (static_cast<unsigned long>(pixel[2]) << 16);
            if (!firstSet)
            {
                first = value;
                firstSet = true;
                if (first != 0)
                {
                    return false;
                }
            }
            else if (value != first)
            {
                return false;
            }
        }
    }
    return firstSet;
}

void CCaptureEngine::DrawCursorOnFrame(HDC memDC, int originX, int originY, int width, int height)
{
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (GetCursorInfo(&cursor) == FALSE || (cursor.flags & CURSOR_SHOWING) == 0)
    {
        return;
    }

    ICONINFO iconInfo{};
    if (GetIconInfo(cursor.hCursor, &iconInfo) == FALSE)
    {
        return;
    }

    int x = cursor.ptScreenPos.x - originX - static_cast<int>(iconInfo.xHotspot);
    int y = cursor.ptScreenPos.y - originY - static_cast<int>(iconInfo.yHotspot);

    HRGN clip = CreateRectRgn(0, 0, width, height);
    if (clip != nullptr)
    {
        SelectClipRgn(memDC, clip);
    }
    DrawIconEx(memDC, x, y, cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    if (clip != nullptr)
    {
        SelectClipRgn(memDC, nullptr);
        DeleteObject(clip);
    }

    if (iconInfo.hbmMask != nullptr)
    {
        DeleteObject(iconInfo.hbmMask);
    }
    if (iconInfo.hbmColor != nullptr)
    {
        DeleteObject(iconInfo.hbmColor);
    }
}

CapturedFrame CCaptureEngine::CaptureRect(const RECT& rect, bool includeCursor, SnapCode& outCode,
                                          std::wstring* outErr)
{
    CapturedFrame frame;
    outCode = kOk;
    EnsureDpiContext();

    // Guard 1: secure desktop (lock screen / UAC).
    if (IsSecureDesktop())
    {
        outCode = kErrBlackFrame;
        if (outErr != nullptr)
        {
            *outErr = L"secure desktop";
        }
        SNAP_LOGE(outCode, L"secure desktop detected, capture aborted");
        return frame;
    }

    // Guard 2: rectangle validity, clipped to the virtual screen.
    RECT virtualScreen = VirtualScreenRect();
    RECT target{};
    if (rect.right <= rect.left || rect.bottom <= rect.top ||
        IntersectRect(&target, &rect, &virtualScreen) == FALSE)
    {
        outCode = kErrWindowNotFound;
        if (outErr != nullptr)
        {
            *outErr = snap::FormatString(L"%d,%d,%d,%d", static_cast<int>(rect.left), static_cast<int>(rect.top),
                                         static_cast<int>(rect.right), static_cast<int>(rect.bottom));
        }
        SNAP_LOGE(outCode, L"invalid capture rectangle");
        return frame;
    }

    const int width = target.right - target.left;
    const int height = target.bottom - target.top;

    // Guard 3: size / memory.
    if (width <= 0 || height <= 0 || width > snap::kMaxImageSide || height > snap::kMaxImageSide ||
        static_cast<SIZE_T>(width) * static_cast<SIZE_T>(height) * 4 > snap::kMaxImageBytes)
    {
        outCode = kErrTooLarge;
        if (outErr != nullptr)
        {
            *outErr = snap::FormatString(L"%dx%d", width, height);
        }
        SNAP_LOGE(outCode, L"image too large: %dx%d", width, height);
        return frame;
    }

    HDC screenDC = GetDC(nullptr);
    if (screenDC == nullptr)
    {
        outCode = kErrCaptureDC;
        if (outErr != nullptr)
        {
            *outErr = L"GetDC(NULL)";
        }
        SNAP_LOGE(outCode, L"GetDC(NULL) failed");
        return frame;
    }

    frame.hMemDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = 0;

    if (frame.hMemDC != nullptr)
    {
        frame.hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &frame.bits, nullptr, 0);
    }

    // Guard 4: GDI allocation.
    if (frame.hMemDC == nullptr || frame.hBitmap == nullptr || frame.bits == nullptr)
    {
        if (frame.hMemDC != nullptr)
        {
            DeleteDC(frame.hMemDC);
            frame.hMemDC = nullptr;
        }
        if (frame.hBitmap != nullptr)
        {
            DeleteObject(frame.hBitmap);
            frame.hBitmap = nullptr;
        }
        frame.bits = nullptr;
        ReleaseDC(nullptr, screenDC);
        outCode = kErrNoMemory;
        if (outErr != nullptr)
        {
            *outErr = snap::FormatString(L"%dx%d", width, height);
        }
        SNAP_LOGE(outCode, L"CreateDIBSection failed for %dx%d", width, height);
        return frame;
    }

    frame.width = width;
    frame.height = height;
    frame.stride = width * 4;

    HGDIOBJ oldObject = SelectObject(frame.hMemDC, frame.hBitmap);

    // Guard 5: BitBlt.
    BOOL blt = BitBlt(frame.hMemDC, 0, 0, width, height, screenDC, target.left, target.top,
                      SRCCOPY | CAPTUREBLT);
    int bltError = (blt == FALSE) ? static_cast<int>(GetLastError()) : 0;

    if (blt != FALSE && includeCursor)
    {
        DrawCursorOnFrame(frame.hMemDC, target.left, target.top, width, height);
    }

    if (oldObject != nullptr)
    {
        SelectObject(frame.hMemDC, oldObject);
    }
    ReleaseDC(nullptr, screenDC);

    if (blt == FALSE)
    {
        frame.Reset();
        outCode = kErrBitBlt;
        if (outErr != nullptr)
        {
            *outErr = snap::FormatString(L"0x%08X", static_cast<unsigned int>(bltError));
        }
        SNAP_LOGE(outCode, L"BitBlt failed (%d)", bltError);
        return frame;
    }

    // BitBlt leaves the alpha channel at 0.  Force it to 255 so the frame can
    // be handed to WIC / the clipboard as an opaque image.
    unsigned char* pixels = static_cast<unsigned char*>(frame.bits);
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixelCount; ++i)
    {
        pixels[i * 4 + 3] = 0xFF;
    }

    // Guard 6: all black sampling (lock screen / UAC fallback).
    if (LooksAllBlack(frame))
    {
        frame.Reset();
        outCode = kErrBlackFrame;
        if (outErr != nullptr)
        {
            *outErr = L"all black";
        }
        SNAP_LOGE(outCode, L"captured frame is entirely black");
        return frame;
    }

    DWORD gdiObjects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    if (gdiObjects > 5000)
    {
        SNAP_LOGE(kErrInternal, L"GDI object count is high: %u", static_cast<unsigned int>(gdiObjects));
    }

    return frame;
}
